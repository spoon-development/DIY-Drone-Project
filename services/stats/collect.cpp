#include "collect.hpp"

#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace dronebros::stats {

namespace {

std::string getenv_or(const char* key, std::string_view fallback) {
  const char* v = std::getenv(key);
  return (v && *v) ? std::string(v) : std::string(fallback);
}

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void trim_inplace(std::string& s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
}

std::string first_line(std::string_view s) {
  auto pos = s.find('\n');
  std::string out(pos == std::string_view::npos ? std::string(s) : std::string(s.substr(0, pos)));
  trim_inplace(out);
  return out;
}

nlohmann::json cpu_temp_c(const std::string& host_sys) {
  auto raw = read_file(host_sys + "/class/thermal/thermal_zone0/temp");
  if (!raw) return nlohmann::json(nullptr);
  try {
    long v = std::stol(*raw);
    return std::round((v / 1000.0) * 100.0) / 100.0;
  } catch (...) {
    return nlohmann::json(nullptr);
  }
}

nlohmann::json loadavg(const std::string& host_proc) {
  auto line = read_file(host_proc + "/loadavg");
  if (!line || line->empty()) return nlohmann::json::object();
  std::istringstream iss(*line);
  std::string p0, p1, p2;
  if (!(iss >> p0 >> p1 >> p2)) {
    trim_inplace(*line);
    return {{"raw", *line}};
  }
  try {
    return nlohmann::json{{"1m", std::stod(p0)}, {"5m", std::stod(p1)}, {"15m", std::stod(p2)}};
  } catch (...) {
    trim_inplace(*line);
    return {{"raw", *line}};
  }
}

std::optional<int> grab_mem_kb(const std::string& text, const char* label) {
  std::string pat = std::string("^") + label + R"(:\s+(\d+)\s+kB$)";
  std::regex re(pat, std::regex_constants::ECMAScript | std::regex_constants::multiline);
  std::smatch m;
  if (!std::regex_search(text, m, re)) return std::nullopt;
  return std::stoi(m[1].str()) / 1024;
}

nlohmann::json memory_mb(const std::string& host_proc) {
  auto text = read_file(host_proc + "/meminfo");
  if (!text) return nlohmann::json::object();
  nlohmann::json out = nlohmann::json::object();
  auto total = grab_mem_kb(*text, "MemTotal");
  auto avail = grab_mem_kb(*text, "MemAvailable");
  auto free = grab_mem_kb(*text, "MemFree");
  if (total) out["mem_total_mb"] = *total;
  if (avail) out["mem_available_mb"] = *avail;
  if (free) out["mem_free_mb"] = *free;
  if (total && avail) {
    int used = std::max(*total - *avail, 0);
    out["mem_used_mb"] = used;
    out["mem_used_pct"] = std::round(100.0 * static_cast<double>(used) / static_cast<double>(*total) * 10.0) / 10.0;
  }
  return out;
}

nlohmann::json uptime_json(const std::string& host_proc) {
  auto line = read_file(host_proc + "/uptime");
  if (!line || line->empty()) return nlohmann::json::object();
  std::istringstream iss(*line);
  std::string up_s, idle_s;
  if (!(iss >> up_s >> idle_s)) {
    trim_inplace(*line);
    return {{"raw", *line}};
  }
  try {
    return nlohmann::json{{"uptime_seconds", std::stod(up_s)}, {"idle_seconds", std::stod(idle_s)}};
  } catch (...) {
    trim_inplace(*line);
    return {{"raw", *line}};
  }
}

nlohmann::json disk_root(const std::string& host_root) {
  if (!fs::is_directory(host_root)) return {{"mounted", false}};
  struct statvfs st {};
  if (statvfs(host_root.c_str(), &st) != 0) {
    return nlohmann::json{{"mounted", true}, {"error", std::string(std::strerror(errno))}};
  }
  const auto total = static_cast<double>(st.f_frsize) * static_cast<double>(st.f_blocks);
  const auto free = static_cast<double>(st.f_frsize) * static_cast<double>(st.f_bavail);
  const auto used = total - free;
  return nlohmann::json{{"mounted", true},
                        {"path", host_root},
                        {"total_mb", std::round(total / (1024.0 * 1024.0) * 10.0) / 10.0},
                        {"used_mb", std::round(used / (1024.0 * 1024.0) * 10.0) / 10.0},
                        {"avail_mb", std::round(free / (1024.0 * 1024.0) * 10.0) / 10.0},
                        {"used_pct", total > 0 ? std::round(100.0 * used / total * 10.0) / 10.0 : 0.0}};
}

std::optional<std::string> hostname(const std::string& host_root, const std::string& host_proc) {
  for (const auto* rel : {"/etc/hostname", "/sys/kernel/hostname"}) {
    std::string path = (std::string_view(rel).find("etc") != std::string_view::npos) ? host_root + rel : host_proc + rel;
    auto h = read_file(path);
    if (!h) continue;
    auto line = first_line(*h);
    if (!line.empty()) return line;
  }
  return std::nullopt;
}

std::vector<std::string> proc_route_paths(const std::string& host_proc) {
  return {host_proc + "/1/net/route", host_proc + "/net/route"};
}

std::optional<std::string> decode_route_ipv4(std::string_view hex8) {
  std::string h(hex8);
  trim_inplace(h);
  if (h.size() != 8) return std::nullopt;
  try {
    unsigned long a = std::stoul(h, nullptr, 16);
    std::ostringstream oss;
    for (int i = 0; i < 4; ++i) {
      if (i) oss << '.';
      oss << ((a >> (8 * i)) & 0xFF);
    }
    return oss.str();
  } catch (...) {
    return std::nullopt;
  }
}

bool is_virtual_iface(std::string_view name) {
  if (name.empty() || name == "lo") return true;
  std::string nl;
  nl.reserve(name.size());
  for (unsigned char c : name) nl.push_back(static_cast<char>(std::tolower(c)));
  if (nl.rfind("veth", 0) == 0 || nl.rfind("br-", 0) == 0 || nl.rfind("virbr", 0) == 0 ||
      nl.rfind("docker", 0) == 0 || nl.rfind("lxc", 0) == 0 || nl.rfind("zt", 0) == 0 ||
      nl.rfind("tun", 0) == 0 || nl.rfind("tap", 0) == 0)
    return true;
  return nl == "docker0" || nl == "cni0" || nl == "flannel.1" || nl == "cilium_host" || nl == "cilium_net";
}

nlohmann::json default_routes_from_proc(const std::string& host_proc) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& path : proc_route_paths(host_proc)) {
    auto text = read_file(path);
    if (!text) continue;
    std::istringstream iss(*text);
    std::string line;
    if (!std::getline(iss, line)) continue;
    while (std::getline(iss, line)) {
      std::istringstream ls(line);
      std::vector<std::string> parts;
      std::string w;
      while (ls >> w) parts.push_back(w);
      if (parts.size() < 8) continue;
      const auto& iface = parts[0];
      const auto& dest = parts[1];
      const auto& gw_raw = parts[2];
      const auto& mask = parts[7];
      if (dest != "00000000" || mask != "00000000") continue;
      int metric = 1'000'000;
      try {
        metric = std::stoi(parts[6]);
      } catch (...) {
      }
      auto g = decode_route_ipv4(gw_raw);
      if (g && *g == "0.0.0.0") g = std::nullopt;
      out.push_back({{"iface", iface},
                     {"gateway", g ? nlohmann::json(*g) : nlohmann::json(nullptr)},
                     {"metric", metric},
                     {"source", path}});
    }
  }
  return out;
}

nlohmann::json default_ipv4_route(const std::string& host_proc) {
  auto arr = default_routes_from_proc(host_proc);
  if (!arr.is_array() || arr.empty()) return nlohmann::json::object();
  std::vector<nlohmann::json> candidates;
  for (const auto& c : arr) candidates.push_back(c);

  std::vector<nlohmann::json> real;
  for (const auto& c : candidates) {
    std::string iface = c.value("iface", "");
    if (!is_virtual_iface(iface)) real.push_back(c);
  }
  auto& pool = real.empty() ? candidates : real;
  std::sort(pool.begin(), pool.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    int ma = a.value("metric", 1'000'000);
    int mb = b.value("metric", 1'000'000);
    if (ma != mb) return ma < mb;
    return a.value("iface", "") < b.value("iface", "");
  });
  const auto& best = pool.front();
  return nlohmann::json{{"iface", best["iface"]},
                        {"gateway", best["gateway"]},
                        {"metric", best["metric"]},
                        {"source", best["source"]}};
}

struct IfaceRow {
  std::string name;
  std::optional<std::string> operstate;
  std::optional<std::string> mac;
  std::optional<bool> carrier;
};

std::tuple<int, int, std::string> sort_key_for_iface(const IfaceRow& x) {
  const std::string& name = x.name;
  if (name.empty() || name == "lo" || is_virtual_iface(name)) return {9, 9, name};
  bool carrier_ok = x.carrier && *x.carrier;
  bool up = x.operstate && *x.operstate == "up";
  bool stale = x.operstate && *x.operstate == "unknown" && !x.carrier;
  int link_score = (carrier_ok || up || stale) ? 0 : 1;
  int medium = 2;
  std::string nl = name;
  for (auto& c : nl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (nl.rfind("en", 0) == 0 || nl.rfind("end", 0) == 0 || nl.rfind("eth", 0) == 0 || nl.rfind("usb", 0) == 0)
    medium = 0;
  else if (nl.rfind("wl", 0) == 0)
    medium = 1;
  return {link_score, medium, name};
}

std::string suggested_static_iface(const nlohmann::json& route, const std::vector<IfaceRow>& ifaces) {
  if (ifaces.empty()) {
    std::string cand = route.value("iface", "");
    trim_inplace(cand);
    if (!cand.empty() && !is_virtual_iface(cand)) return cand;
    return "eth0";
  }
  std::vector<IfaceRow> phys = ifaces;
  std::sort(phys.begin(), phys.end(), [](const IfaceRow& a, const IfaceRow& b) {
    return sort_key_for_iface(a) < sort_key_for_iface(b);
  });
  for (const auto& x : phys) {
    if (x.name == "lo" || is_virtual_iface(x.name)) continue;
    return x.name;
  }
  std::string cand = route.value("iface", "");
  trim_inplace(cand);
  if (!cand.empty() && !is_virtual_iface(cand)) return cand;
  return "eth0";
}

std::vector<IfaceRow> list_sysfs_net_ifaces(const std::string& host_sys) {
  std::vector<IfaceRow> out;
  const std::string base = host_sys + "/class/net";
  DIR* d = opendir(base.c_str());
  if (!d) return out;
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == ".." || n == "lo") continue;
    names.push_back(std::move(n));
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const std::string pfx = base + "/" + name;
    IfaceRow row;
    row.name = name;
    if (auto st = read_file(pfx + "/operstate")) {
      trim_inplace(*st);
      row.operstate = st->empty() ? std::nullopt : std::optional<std::string>(*st);
    }
    if (auto mac = read_file(pfx + "/address")) {
      trim_inplace(*mac);
      if (!mac->empty() && *mac != "00:00:00:00:00:00") row.mac = *mac;
    }
    if (auto car = read_file(pfx + "/carrier")) {
      trim_inplace(*car);
      if (*car == "0" || *car == "1") row.carrier = (*car == "1");
    }
    out.push_back(std::move(row));
  }
  return out;
}

std::optional<std::string> read_devicetree_model(const std::string& host_root) {
  auto raw = read_file(host_root + "/sys/firmware/devicetree/base/model");
  if (!raw) return std::nullopt;
  std::string s;
  s.reserve(raw->size());
  for (char c : *raw)
    if (c != '\0') s.push_back(c);
  trim_inplace(s);
  if (s.empty()) return std::nullopt;
  return s;
}

nlohmann::json parse_cpuinfo_board(const std::string& host_proc) {
  auto text = read_file(host_proc + "/cpuinfo");
  if (!text) return nlohmann::json::object();
  nlohmann::json out = nlohmann::json::object();
  std::istringstream iss(*text);
  std::string line;
  while (std::getline(iss, line)) {
    auto pos = line.find(':');
    if (pos == std::string::npos) continue;
    std::string key = line.substr(0, pos);
    std::string val = line.substr(pos + 1);
    trim_inplace(key);
    trim_inplace(val);
    if (key == "Hardware" || key == "Revision" || key == "Model" || key == "model name") {
      std::string nk;
      for (char c : key) {
        if (c == ' ')
          nk.push_back('_');
        else
          nk.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      out[nk] = val;
    }
  }
  return out;
}

nlohmann::json iface_rows_to_json(const std::vector<IfaceRow>& rows) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& r : rows) {
    nlohmann::json o{{"name", r.name}};
    if (r.operstate) o["operstate"] = *r.operstate;
    else
      o["operstate"] = nullptr;
    if (r.mac) o["mac"] = *r.mac;
    if (r.carrier) o["carrier"] = *r.carrier;
    arr.push_back(std::move(o));
  }
  return arr;
}

nlohmann::json compute_profile(const std::string& host_proc, const std::string& host_root) {
  utsname u {};
  uname(&u);
  auto board = parse_cpuinfo_board(host_proc);
  auto model = read_devicetree_model(host_root);
  std::string kind = "unknown";
  std::vector<std::string> label_parts;
  if (model) {
    kind = "embedded";
    label_parts.push_back(*model);
  } else if (board.contains("hardware") || board.contains("revision")) {
    kind = "embedded";
    if (board.contains("hardware")) label_parts.push_back(board["hardware"].get<std::string>());
    if (board.contains("revision")) label_parts.push_back("rev " + board["revision"].get<std::string>());
  } else if (board.contains("model_name")) {
    kind = "general";
    label_parts.push_back(board["model_name"].get<std::string>());
  }
  std::string label;
  for (size_t i = 0; i < label_parts.size(); ++i) {
    if (i) label += " · ";
    label += label_parts[i];
  }
  nlohmann::json cpuinfo = board.empty() ? nullptr : board;
  return nlohmann::json{{"kind", kind},
                        {"label", label.empty() ? nullptr : nlohmann::json(label)},
                        {"kernel", std::string(u.release)},
                        {"machine", std::string(u.machine)},
                        {"cpuinfo", cpuinfo},
                        {"devicetree_model", model ? nlohmann::json(*model) : nlohmann::json(nullptr)}};
}

std::vector<std::string> sorted_netplan_paths(const std::string& host_root) {
  std::vector<std::string> paths;
  const std::string dir = host_root + "/etc/netplan";
  if (!fs::is_directory(dir)) return paths;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    auto ext = e.path().extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".yaml" || ext == ".yml") paths.push_back(e.path().string());
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

nlohmann::json grep_netplan_hints(const std::string& host_root) {
  nlohmann::json out = nlohmann::json::array();
  std::vector<std::string> paths = sorted_netplan_paths(host_root);
  std::regex line_re(
      R"(^\s*(?:addresses|gateway4|gateway6|routes|nameservers|dhcp4)\s*:.*$)",
      std::regex_constants::ECMAScript | std::regex_constants::icase | std::regex_constants::multiline);
  for (const auto& path : paths) {
    auto text = read_file(path);
    if (!text) continue;
    nlohmann::json hints = nlohmann::json::array();
    for (auto it = std::sregex_iterator(text->begin(), text->end(), line_re); it != std::sregex_iterator();
         ++it) {
      std::string line = it->str();
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
      hints.push_back(line);
      if (hints.size() >= 24) break;
    }
    std::string disp = path;
    const std::string hr = host_root;
    if (disp.rfind(hr, 0) == 0) {
      disp = disp.substr(hr.size());
      if (disp.empty()) disp = "/";
      else if (disp.front() != '/')
        disp = "/" + disp;
    }
    out.push_back(nlohmann::json{{"path", disp}, {"hints", std::move(hints)}});
  }
  return out;
}

nlohmann::json grep_dhcpcd_hints(const std::string& host_root) {
  nlohmann::json out = nlohmann::json::array();
  auto text = read_file(host_root + "/etc/dhcpcd.conf");
  if (!text) return out;
  std::istringstream iss(*text);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string s = line;
    trim_inplace(s);
    if (s.empty() || s.front() == '#') continue;
    auto sl = s;
    for (auto& c : sl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (sl.rfind("interface ", 0) == 0 || sl.rfind("static ip_", 0) == 0 || sl.rfind("static routers", 0) == 0 ||
        sl.rfind("static domain_", 0) == 0 || sl.rfind("nogateway", 0) == 0 || sl.rfind("fallback", 0) == 0) {
      out.push_back(s);
      if (out.size() >= 40) break;
    }
  }
  return out;
}

nlohmann::json network_summary(const std::string& host_proc, const std::string& host_sys, const std::string& host_root) {
  auto ifaces_rows = list_sysfs_net_ifaces(host_sys);
  auto all_defaults = default_routes_from_proc(host_proc);
  auto route = default_ipv4_route(host_proc);
  auto netplan = grep_netplan_hints(host_root);
  auto dhcpcd_arr = grep_dhcpcd_hints(host_root);
  std::vector<std::string> dhcpcd;
  if (dhcpcd_arr.is_array()) {
    for (const auto& x : dhcpcd_arr) {
      if (x.is_string()) dhcpcd.push_back(x.get<std::string>());
    }
  }

  std::optional<std::string> primary;
  std::regex ip_re(R"(\b((?:\d{1,3}\.){3}\d{1,3})(?:/\d{1,2})?\b)");
  if (netplan.is_array()) {
    for (const auto& block : netplan) {
      if (!block.contains("hints")) continue;
      for (const auto& h : block["hints"]) {
        if (!h.is_string()) continue;
        const std::string hint = h.get<std::string>();
        std::smatch m;
        if (std::regex_search(hint, m, ip_re)) {
          primary = m[1].str();
          break;
        }
      }
      if (primary) break;
    }
  }
  if (!primary) {
    std::regex ip_eq(R"(=\s*((?:\d{1,3}\.){3}\d{1,3}))");
    for (const auto& line : dhcpcd) {
      std::smatch m;
      if (std::regex_search(line, m, ip_eq)) {
        primary = m[1].str();
        break;
      }
    }
  }

  std::vector<nlohmann::json> sorted_defaults;
  if (all_defaults.is_array()) {
    for (const auto& c : all_defaults) sorted_defaults.push_back(c);
    std::sort(sorted_defaults.begin(), sorted_defaults.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
      int ma = a.value("metric", 1'000'000);
      int mb = b.value("metric", 1'000'000);
      if (ma != mb) return ma < mb;
      return a.value("iface", "") < b.value("iface", "");
    });
    if (sorted_defaults.size() > 12) sorted_defaults.resize(12);
  }

  nlohmann::json dhcpcd_json = nlohmann::json::array();
  for (const auto& s : dhcpcd) dhcpcd_json.push_back(s);

  return nlohmann::json{{"interfaces", iface_rows_to_json(ifaces_rows)},
                        {"default_route", route.empty() ? nullptr : route},
                        {"default_routes", nlohmann::json(sorted_defaults)},
                        {"static_iface_hint", suggested_static_iface(route.empty() ? nlohmann::json::object() : route, ifaces_rows)},
                        {"config_hints", nlohmann::json{{"netplan", netplan}, {"dhcpcd", dhcpcd_json}}},
                        {"primary_ipv4_hint", primary ? nlohmann::json(*primary) : nlohmann::json(nullptr)}};
}

}  // namespace

nlohmann::json collect() {
  const std::string host_proc = getenv_or("HOST_PROC", "/host/proc");
  const std::string host_sys = getenv_or("HOST_SYS", "/host/sys");
  const std::string host_root = getenv_or("HOST_ROOT", "/host_root");

  nlohmann::json j;
  auto hn = hostname(host_root, host_proc);
  j["hostname"] = hn ? nlohmann::json(*hn) : nlohmann::json(nullptr);
  j["cpu_temp_c"] = cpu_temp_c(host_sys);
  j["loadavg"] = loadavg(host_proc);
  j["memory"] = memory_mb(host_proc);
  j["uptime"] = uptime_json(host_proc);
  j["disk_root"] = disk_root(host_root);
  j["compute"] = compute_profile(host_proc, host_root);
  j["network"] = network_summary(host_proc, host_sys, host_root);
  return j;
}

}  // namespace dronebros::stats
