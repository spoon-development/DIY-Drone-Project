#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <sys/wait.h>

namespace {

using json = nlohmann::json;

std::string getenv_or(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  return (v && *v) ? std::string(v) : fallback;
}

void trim_inplace(std::string& s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
}

void strip_network_draft_ips(json& draft) {
  if (!draft.is_object()) return;
  for (const char* section : {"lan", "wifi"}) {
    if (!draft.contains(section) || !draft[section].is_object()) continue;
    draft[section].erase("ip");
    draft[section].erase("gateway");
  }
}

std::string to_lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool env_flag_off(const char* key, const char* default_value) {
  std::string s = getenv_or(key, default_value);
  trim_inplace(s);
  s = to_lower(std::move(s));
  return s == "0" || s == "false" || s == "no" || s == "off";
}

std::filesystem::path data_dir() {
  return std::filesystem::path(getenv_or("DATA_DIR", "/data"));
}

std::filesystem::path settings_path() { return data_dir() / "hmi-settings.json"; }

std::optional<std::string> read_text_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool read_settings(json& out) {
  out = json::object();
  auto p = settings_path();
  if (!std::filesystem::is_regular_file(p)) return true;
  auto raw = read_text_file(p);
  if (!raw) return false;
  try {
    auto j = json::parse(*raw);
    if (j.is_object()) out = std::move(j);
  } catch (...) {
    out = json::object();
  }
  return true;
}

bool write_settings_atomic(const json& data) {
  std::error_code ec;
  std::filesystem::create_directories(data_dir(), ec);
  if (ec) return false;
  const auto tmp = settings_path();
  auto tmp_path = tmp;
  tmp_path += ".json.tmp";
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << data.dump(2);
    if (!out.flush()) return false;
  }
  std::filesystem::rename(tmp_path, tmp, ec);
  return !ec;
}

struct CmdResult {
  int exit_code = -1;
  std::string combined_output;
};

CmdResult run_command_argv(const std::vector<std::string>& args) {
  CmdResult r;
  int pipefd[2];
  if (pipe(pipefd) != 0) return r;
  pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    std::vector<char*> argv;
    std::vector<std::string> mut = args;
    for (auto& s : mut) argv.push_back(s.data());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(pipefd[1]);
  char buf[4096];
  ssize_t n;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) r.combined_output.append(buf, static_cast<size_t>(n));
  close(pipefd[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  r.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

CmdResult run_on_host_shell(const std::string& script) {
  return run_command_argv({"nsenter", "-t", "1", "-m", "-u", "-i", "-n", "-p", "sh", "-c", script});
}

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

struct EthApplySpec {
  std::string iface;
  std::string address_cidr;
  std::string gateway;
  bool never_default = true;
};

int yaml_line_indent_local(const std::string& line) {
  int indent = 0;
  for (char c : line) {
    if (c == ' ') indent++;
    else break;
  }
  return indent;
}

std::optional<EthApplySpec> parse_eth_from_netplan_yaml(const std::string& text) {
  std::istringstream iss(text);
  std::string line;
  bool in_ethernets = false;
  int eth_indent = -1;
  std::string cur_iface;
  bool iface_dhcp4 = false;
  EthApplySpec building;
  bool in_addresses = false;
  int addresses_indent = -1;
  std::optional<EthApplySpec> result;

  auto commit_iface = [&]() {
    if (!cur_iface.empty() && !iface_dhcp4 && !building.address_cidr.empty()) {
      building.iface = cur_iface;
      result = building;
    }
    cur_iface.clear();
    iface_dhcp4 = false;
    building = EthApplySpec{};
    in_addresses = false;
    addresses_indent = -1;
  };

  std::regex cidr_re(R"(^\-\s*((?:\d{1,3}\.){3}\d{1,3})/(\d{1,2})\s*$)");
  std::regex via_re(R"(^\s*via:\s*((?:\d{1,3}\.){3}\d{1,3})\s*$)", std::regex_constants::icase);

  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const int indent = yaml_line_indent_local(line);
    std::string trimmed = line.substr(static_cast<size_t>(indent));
    if (trimmed.empty() || trimmed.front() == '#') continue;
    std::string tl = trimmed;
    for (auto& c : tl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (tl == "ethernets:") {
      commit_iface();
      in_ethernets = true;
      eth_indent = indent;
      continue;
    }
    if (!in_ethernets) continue;
    if (indent <= eth_indent) {
      commit_iface();
      in_ethernets = false;
      continue;
    }

    if (cur_iface.empty() && indent == eth_indent + 2) {
      const auto colon = trimmed.find(':');
      if (colon != std::string::npos && colon + 1 == trimmed.size()) {
        cur_iface = trimmed.substr(0, colon);
        continue;
      }
    }
    if (cur_iface.empty()) continue;

    if (indent <= eth_indent + 2) {
      commit_iface();
      if (indent == eth_indent + 2) {
        const auto colon = trimmed.find(':');
        if (colon != std::string::npos && colon + 1 == trimmed.size()) cur_iface = trimmed.substr(0, colon);
      }
      continue;
    }

    if (tl.rfind("dhcp4:", 0) == 0) {
      std::string v = trimmed.substr(trimmed.find(':') + 1);
      trim_inplace(v);
      for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      iface_dhcp4 = (v == "true" || v == "yes" || v == "1");
      continue;
    }
    if (tl == "addresses:") {
      in_addresses = true;
      addresses_indent = indent;
      continue;
    }
    if (in_addresses && indent > addresses_indent) {
      std::smatch m;
      if (std::regex_match(trimmed, m, cidr_re)) {
        building.address_cidr = m[1].str() + "/" + m[2].str();
      }
      continue;
    }
    std::smatch m;
    if (std::regex_match(trimmed, m, via_re)) {
      building.gateway = m[1].str();
      building.never_default = false;
    }
  }
  commit_iface();
  return result;
}

bool host_netplan_uses_networkmanager() {
  const std::filesystem::path dir("/etc/netplan");
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return false;
  bool saw_nm = false;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (ec || !e.is_regular_file()) continue;
    const std::string name = e.path().filename().string();
    if (name.rfind("90-NM-", 0) == 0) saw_nm = true;
  }
  return saw_nm;
}

std::string nmcli_apply_eth_script(const EthApplySpec& spec) {
  std::ostringstream ss;
  ss << "set -e\n"
     << "command -v nmcli >/dev/null 2>&1 || { echo 'nmcli not found on host' >&2; exit 43; }\n"
     << "IF=" << shell_quote(spec.iface) << "\n"
     << "ADDR=" << shell_quote(spec.address_cidr) << "\n"
     << "UUID=$(nmcli -g GENERAL.CON-UUID device show \"$IF\" 2>/dev/null | head -n1)\n"
     << "if [ -z \"$UUID\" ] || [ \"$UUID\" = \"--\" ]; then\n"
     << "  UUID=$(nmcli -t -f UUID,DEVICE connection show 2>/dev/null | "
        "awk -F: -v d=\"$IF\" '$2==d{print $1; exit}')\n"
     << "fi\n"
     << "if [ -z \"$UUID\" ]; then echo \"no NetworkManager profile for $IF\" >&2; exit 42; fi\n"
     << "nmcli connection modify \"$UUID\" ipv6.method ignore ipv4.method manual "
        "ipv4.addresses \"$ADDR\"";
  if (spec.never_default || spec.gateway.empty()) {
    ss << " ipv4.gateway '' ipv4.never-default yes";
  } else {
    ss << " ipv4.gateway " << shell_quote(spec.gateway) << " ipv4.never-default no";
  }
  ss << "\nnmcli connection up \"$UUID\" 2>/dev/null || nmcli device reapply \"$IF\"\n";
  return ss.str();
}

void apply_netplan_on_host(const std::string& yaml_text, const std::filesystem::path& host_file,
                           const std::string& netplan_cmd, std::string& apply_method_out) {
  apply_method_out.clear();
  std::error_code ec;
  std::filesystem::create_directories(host_file.parent_path(), ec);
  auto tmp = host_file;
  tmp += ".yaml.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write netplan temp file");
    out << yaml_text;
    if (!out.flush()) throw std::runtime_error("cannot flush netplan temp file");
  }
  std::filesystem::rename(tmp, host_file, ec);
  if (ec) throw std::runtime_error(ec.message());

  const bool nm_managed = host_netplan_uses_networkmanager();
  const auto eth = parse_eth_from_netplan_yaml(yaml_text);

  if (eth && nm_managed) {
    auto rr = run_on_host_shell(nmcli_apply_eth_script(*eth));
    if (rr.exit_code != 0) {
      std::string err = rr.combined_output;
      trim_inplace(err);
      if (err.empty()) err = "nmcli exit " + std::to_string(rr.exit_code);
      throw std::runtime_error(err);
    }
    apply_method_out = "nmcli";
  }

  const bool force_netplan = !env_flag_off("HMI_NETPLAN_APPLY", nm_managed ? "0" : "1");
  if (force_netplan) {
    std::vector<std::string> args = {"nsenter", "-t", "1", "-m", "-u", "-i", "-n", "-p", netplan_cmd, "apply"};
    auto rr = run_command_argv(args);
    if (rr.exit_code != 0) {
      std::string err = rr.combined_output;
      trim_inplace(err);
      if (err.empty()) err = "netplan exit " + std::to_string(rr.exit_code);
      if (apply_method_out == "nmcli") {
        apply_method_out += "+netplan_warn";
      } else {
        throw std::runtime_error(err);
      }
    } else if (apply_method_out.empty()) {
      apply_method_out = "netplan";
    } else {
      apply_method_out += "+netplan";
    }
  } else if (apply_method_out.empty()) {
    if (!eth) throw std::runtime_error("No Ethernet section in saved netplan text.");
    throw std::runtime_error("NetworkManager netplan detected but no Ethernet block to apply.");
  }

  if (!env_flag_off("HMI_RESTART_NETWORK_MANAGER", "1")) {
    run_on_host_shell("systemctl restart NetworkManager 2>/dev/null || "
                      "service NetworkManager restart 2>/dev/null || true");
  }
}

void schedule_host_reboot() {
  pid_t pid = fork();
  if (pid != 0) return;
  setsid();
  const char* script =
      "nohup sh -c 'sleep 3; systemctl reboot 2>/dev/null || /sbin/reboot' >/dev/null 2>&1 &";
  execlp("nsenter", "nsenter", "-t", "1", "-m", "-u", "-i", "-n", "-p", "sh", "-c", script, nullptr);
  _exit(1);
}

struct ParsedUrl {
  std::string host;
  int port = 80;
  std::string path = "/";
};

bool parse_http_url(const std::string& url, ParsedUrl& out) {
  if (url.rfind("http://", 0) != 0) return false;
  std::string rest = url.substr(7);
  auto slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : rest.substr(slash);
  auto colon = hostport.find(':');
  if (colon == std::string::npos) {
    out.host = hostport;
    out.port = 80;
  } else {
    out.host = hostport.substr(0, colon);
    try {
      out.port = std::stoi(hostport.substr(colon + 1));
    } catch (...) {
      return false;
    }
  }
  trim_inplace(out.host);
  return !out.host.empty() && out.port > 0 && out.port <= 65535;
}

std::vector<std::string> stats_url_candidates() {
  std::vector<std::string> urls;
  const std::string raw = getenv_or("STATS_METRICS_URLS", "");
  if (!raw.empty()) {
    std::istringstream iss(raw);
    std::string item;
    while (std::getline(iss, item, ',')) {
      trim_inplace(item);
      if (!item.empty()) urls.push_back(item);
    }
  } else {
    urls.push_back(getenv_or("STATS_METRICS_URL", "http://stats:9101/metrics"));
    urls.push_back("http://stats:9101/metrics");
    urls.push_back("http://dronebros-stats:9101/metrics");
  }
  std::vector<std::string> out;
  for (const auto& u : urls) {
    if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
  }
  return out;
}

bool fetch_stats_once(const std::string& url, int timeout_sec, int& status, std::string& body,
                      std::string& content_type) {
  ParsedUrl pu;
  if (!parse_http_url(url, pu)) return false;
  httplib::Client cli(pu.host, pu.port);
  cli.set_read_timeout(timeout_sec, 0);
  cli.set_connection_timeout(5, 0);
  auto res = cli.Get(pu.path.c_str());
  if (!res) return false;
  status = res->status;
  body = res->body;
  auto it = res->headers.find("Content-Type");
  content_type = it != res->headers.end() ? it->second : "application/json";
  return true;
}

ParsedUrl video_service_base_url() {
  const std::string raw = getenv_or("VIDEO_SERVICE_URL", "http://video:8765");
  ParsedUrl pu;
  if (!parse_http_url(raw, pu)) {
    pu.host = "video";
    pu.port = 8765;
    pu.path = "/";
  }
  return pu;
}

bool safe_profile_id(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (unsigned char c : s) {
    if (std::isalnum(c) != 0 || c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

bool fetch_video_upstream_path(const ParsedUrl& base, const std::string& path_query, int timeout_sec, int& status,
                               std::string& body, std::string& content_type) {
  httplib::Client cli(base.host, base.port);
  cli.set_read_timeout(timeout_sec, 0);
  cli.set_connection_timeout(3, 0);
  auto res = cli.Get(path_query.c_str());
  if (!res) return false;
  status = res->status;
  body = res->body;
  auto it = res->headers.find("Content-Type");
  content_type = it != res->headers.end() ? it->second : "application/json";
  return true;
}

ParsedUrl telemetry_service_base_url() {
  const std::string raw = getenv_or("TELEMETRY_SERVICE_URL", "http://telemetry:9102");
  ParsedUrl pu;
  if (!parse_http_url(raw, pu)) {
    pu.host = "telemetry";
    pu.port = 9102;
    pu.path = "/";
  }
  return pu;
}

bool fetch_telemetry_upstream(const ParsedUrl& base, const std::string& path_query, int timeout_sec, int& status,
                              std::string& body, std::string& content_type) {
  return fetch_video_upstream_path(base, path_query, timeout_sec, status, body, content_type);
}

bool valid_hotspot_ssid(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  for (unsigned char c : s) {
    if (c < 32 || c > 126) return false;
  }
  return true;
}

bool valid_hotspot_psk(const std::string& s) {
  return s.size() >= 8 && s.size() <= 63;
}

bool valid_ipv4(const std::string& s) {
  static const std::regex re(R"(^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$)");
  std::smatch m;
  if (!std::regex_match(s, m, re)) return false;
  for (int i = 1; i <= 4; ++i) {
    try { if (std::stoi(m[i].str()) > 255) return false; } catch (...) { return false; }
  }
  return true;
}

bool valid_hotspot_channel(int ch) {
  if (ch >= 1 && ch <= 14) return true;
  static const int ch5[] = {36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,149,153,157,161,165};
  for (int c : ch5) if (c == ch) return true;
  return false;
}

bool valid_wlan_iface(const std::string& s) {
  if (s.size() < 3 || s.size() > 16) return false;
  if (s.rfind("wlan", 0) != 0) return false;
  for (unsigned char c : s) {
    if (!std::isalnum(c) && c != '_') return false;
  }
  return true;
}

std::string hotspot_apply_script(const std::string& iface, const std::string& ssid,
                                  const std::string& psk, const std::string& ip, int channel) {
  std::ostringstream ss;
  ss << "set -e\n"
     << "command -v nmcli >/dev/null 2>&1 || { echo 'nmcli not found' >&2; exit 43; }\n"
     << "CON=DroneHotspot\n"
     << "IF=" << shell_quote(iface) << "\n"
     << "if nmcli connection show \"$CON\" >/dev/null 2>&1; then\n"
     << "  nmcli connection modify \"$CON\""
     << " 802-11-wireless.ssid " << shell_quote(ssid)
     << " wifi-sec.psk " << shell_quote(psk)
     << " ipv4.addresses " << shell_quote(ip + "/24")
     << " 802-11-wireless.channel " << channel << "\n"
     << "else\n"
     << "  nmcli connection add type wifi ifname \"$IF\" con-name \"$CON\" autoconnect yes"
     << " ssid " << shell_quote(ssid)
     << " wifi-sec.key-mgmt wpa-psk"
     << " wifi-sec.psk " << shell_quote(psk)
     << " 802-11-wireless.mode ap"
     << " 802-11-wireless.band bg"
     << " 802-11-wireless.channel " << channel
     << " ipv4.method shared"
     << " ipv4.addresses " << shell_quote(ip + "/24") << "\n"
     << "fi\n"
     << "nmcli connection up \"$CON\"\n";
  return ss.str();
}

bool valid_serial_device(const std::string& s) {
  if (s.rfind("/dev/", 0) != 0 || s.size() < 6 || s.size() > 128) return false;
  for (unsigned char c : s) {
    if (std::isalnum(c) != 0 || c == '/' || c == '_' || c == '-' || c == '.') continue;
    return false;
  }
  return true;
}

bool valid_serial_baud(int baud) {
  static const int kRates[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
  for (int r : kRates) {
    if (r == baud) return true;
  }
  return false;
}

struct MjpegBridge {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> q;
  bool upstream_done = false;
  std::atomic<bool> abort{false};
  std::thread th;
};

bool proxy_stats_to_response(const std::vector<std::string>& candidates, int timeout_sec, httplib::Response& res) {
  std::string last_what;
  for (const auto& url : candidates) {
    for (int attempt = 0; attempt < 6; ++attempt) {
      int st = 0;
      std::string body;
      std::string ct;
      try {
        if (!fetch_stats_once(url, timeout_sec, st, body, ct)) {
          throw std::runtime_error("connection failed");
        }
        if (st != 200) throw std::runtime_error("stats returned HTTP " + std::to_string(st));
        auto semi = ct.find(';');
        std::string main_type = semi == std::string::npos ? ct : ct.substr(0, semi);
        trim_inplace(main_type);
        if (main_type.empty()) main_type = "application/json";
        res.status = st;
        res.set_content(body, main_type.c_str());
        res.set_header("Cache-Control", "no-store");
        return true;
      } catch (const std::exception& e) {
        last_what = e.what();
        std::cerr << "[hmi] proxy_stats url=" << url << " attempt=" << attempt << ": " << last_what << "\n"
                  << std::flush;
        if (attempt < 5)
          std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
      }
    }
  }
  json err = {{"error", last_what.empty() ? "unknown" : last_what},
              {"hint", std::string("Tried: ") + [&] {
                 std::ostringstream o;
                 for (size_t i = 0; i < candidates.size(); ++i) {
                   if (i) o << ", ";
                   o << candidates[i];
                 }
                 return o.str();
               }() + " — on the Pi: docker compose logs stats hmi && docker compose ps"}};
  res.status = 502;
  res.set_content(err.dump(), "application/json; charset=utf-8");
  return false;
}

}  // namespace

int main() {
  const std::string host = getenv_or("HMI_BIND", "0.0.0.0");
  int port = 80;
  try {
    port = std::stoi(getenv_or("HMI_PORT", "80"));
  } catch (...) {
    port = 80;
  }
  size_t max_body = 12u * 1024u * 1024u;
  try {
    max_body = static_cast<size_t>(std::stoll(getenv_or("HMI_MAX_SETTINGS_BYTES", std::to_string(12 * 1024 * 1024))));
  } catch (...) {
  }
  const int stats_timeout = [] {
    try {
      return static_cast<int>(std::lround(std::stod(getenv_or("STATS_FETCH_TIMEOUT", "35"))));
    } catch (...) {
      return 35;
    }
  }();
  const auto stats_urls = stats_url_candidates();

  httplib::Server svr;
  svr.set_payload_max_length(max_body);
  svr.new_task_queue = [] {
    const unsigned n = std::thread::hardware_concurrency();
    return new httplib::ThreadPool(n ? n : 8u);
  };

  if (!svr.set_mount_point("/images", "./static/images")) {
    std::cerr << "[hmi] failed to mount /images\n";
    return 1;
  }

  svr.Get("/api/host-meta", [](const httplib::Request&, httplib::Response& res) {
    json j = {{"displayHostname", getenv_or("DISPLAY_HOSTNAME", "")}};
    res.set_content(j.dump(), "application/json; charset=utf-8");
  });

  svr.Get("/api/hmi-settings", [](const httplib::Request&, httplib::Response& res) {
    json cur;
    if (!read_settings(cur)) {
      res.status = 500;
      res.set_content("{}", "application/json");
      return;
    }
    json response = cur;
    if (response.contains("networkDraft")) strip_network_draft_ips(response["networkDraft"]);
    res.set_content(response.dump(), "application/json; charset=utf-8");
  });

  svr.Post("/api/hmi-settings", [](const httplib::Request& req, httplib::Response& res) {
    json incoming;
    try {
      incoming = json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content("Invalid JSON", "text/plain; charset=utf-8");
      return;
    }
    if (!incoming.is_object()) {
      res.status = 400;
      res.set_content("Expected a JSON object", "text/plain; charset=utf-8");
      return;
    }
    json cur;
    if (!read_settings(cur)) {
      res.status = 500;
      res.set_content("cannot read settings", "text/plain; charset=utf-8");
      return;
    }
    if (incoming.contains("background")) cur["background"] = incoming["background"];
    if (incoming.contains("networkDraft")) cur["networkDraft"] = incoming["networkDraft"];
    if (incoming.contains("netplanYaml")) cur["netplanYaml"] = incoming["netplanYaml"];
    if (incoming.contains("videoCameraProfile") && incoming["videoCameraProfile"].is_string())
      cur["videoCameraProfile"] = incoming["videoCameraProfile"];
    if (incoming.contains("fcSerialDevice") && incoming["fcSerialDevice"].is_string())
      cur["fcSerialDevice"] = incoming["fcSerialDevice"];
    if (incoming.contains("fcSerialBaud") && incoming["fcSerialBaud"].is_number_integer())
      cur["fcSerialBaud"] = incoming["fcSerialBaud"];
    if (incoming.contains("hotspotConfig") && incoming["hotspotConfig"].is_object())
      cur["hotspotConfig"] = incoming["hotspotConfig"];
    if (incoming.contains("fcCardLayout") && incoming["fcCardLayout"].is_array()) {
      json layout = json::array();
      for (const auto& item : incoming["fcCardLayout"]) {
        if (item.is_string()) layout.push_back(item.get<std::string>());
      }
      cur["fcCardLayout"] = std::move(layout);
    }
    if (!write_settings_atomic(cur)) {
      res.status = 500;
      res.set_content("cannot write settings", "text/plain; charset=utf-8");
      return;
    }
    res.set_content(cur.dump(), "application/json; charset=utf-8");
  });

  svr.Post("/api/apply-network", [&](const httplib::Request&, httplib::Response& res) {
    if (env_flag_off("HMI_APPLY_NETPLAN", "1")) {
      json j = {{"ok", false}, {"error", "Host netplan apply is disabled on this container."}};
      res.status = 503;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    json cur;
    if (!read_settings(cur)) {
      json j = {{"ok", false}, {"error", "cannot read settings"}};
      res.status = 500;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    std::string yaml_text;
    if (cur.contains("netplanYaml") && cur["netplanYaml"].is_string())
      yaml_text = cur["netplanYaml"].get<std::string>();
    trim_inplace(yaml_text);
    if (yaml_text.empty()) {
      json j = {{"ok", false},
                {"error", "No netplan text saved yet — fill the network form and save first."}};
      res.status = 400;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    std::string apply_method;
    try {
      std::filesystem::path host_netplan(getenv_or("HMI_HOST_NETPLAN_FILE", "/etc/netplan/99-drone-hmi.yaml"));
      std::string netplan_cmd = getenv_or("HMI_NETPLAN_CMD", "/usr/sbin/netplan");
      trim_inplace(netplan_cmd);
      if (netplan_cmd.empty()) netplan_cmd = "/usr/sbin/netplan";
      apply_netplan_on_host(yaml_text, host_netplan, netplan_cmd, apply_method);
    } catch (const std::exception& e) {
      json j = {{"ok", false}, {"error", e.what()}};
      res.status = 500;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }

    bool reboot_scheduled = false;
    if (!env_flag_off("HMI_REBOOT_AFTER_NETPLAN", "0")) {
      if (access("/usr/bin/nsenter", X_OK) == 0 || access("/bin/nsenter", X_OK) == 0) {
        schedule_host_reboot();
        reboot_scheduled = true;
      }
    }
    json j = {{"ok", true}, {"rebootScheduled", reboot_scheduled}, {"applyMethod", apply_method}};
    res.set_content(j.dump(), "application/json; charset=utf-8");
  });

  svr.Get("/api/hotspot", [](const httplib::Request&, httplib::Response& res) {
    json cur;
    read_settings(cur);
    const json& cfg = cur.contains("hotspotConfig") ? cur["hotspotConfig"] : json::object();

    auto sr = run_on_host_shell(
        "nmcli -t -f DEVICE,STATE,CONNECTION device status 2>/dev/null | "
        "awk -F: '$1==\"wlan1\"{print $2\"|\"$3}'");
    bool active = sr.exit_code == 0 &&
                  sr.combined_output.find("connected|DroneHotspot") != std::string::npos;

    int clients = 0;
    if (active) {
      auto cr = run_on_host_shell(
          "iw dev wlan1 station dump 2>/dev/null | grep -c '^Station' || echo 0");
      if (cr.exit_code == 0) {
        try { clients = std::stoi(cr.combined_output); } catch (...) {}
      }
    }

    json j = {
        {"active", active},
        {"clients", clients},
        {"ssid", cfg.value("ssid", "DroneNet")},
        {"ip", cfg.value("ip", "10.0.69.1")},
        {"iface", cfg.value("iface", "wlan1")},
        {"channel", cfg.value("channel", 6)},
    };
    res.set_content(j.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
  });

  svr.Post("/api/hotspot", [](const httplib::Request& req, httplib::Response& res) {
    json incoming;
    try {
      incoming = json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"ok":false,"error":"invalid JSON"})", "application/json; charset=utf-8");
      return;
    }

    const bool enable = incoming.value("enabled", true);
    if (!enable) {
      run_on_host_shell("nmcli connection down DroneHotspot 2>/dev/null || true");
      json cur; read_settings(cur);
      if (cur.contains("hotspotConfig")) cur["hotspotConfig"]["enabled"] = false;
      write_settings_atomic(cur);
      res.set_content(R"({"ok":true,"active":false})", "application/json; charset=utf-8");
      return;
    }

    const std::string ssid  = incoming.value("ssid", "");
    const std::string psk   = incoming.value("psk", "");
    const std::string ip    = incoming.value("ip", "");
    const std::string iface = incoming.value("iface", "wlan1");
    const int channel       = incoming.value("channel", 6);

    if (!valid_hotspot_ssid(ssid)) {
      res.status = 400;
      res.set_content(R"x({"ok":false,"error":"invalid SSID (1-32 printable characters)"})x", "application/json; charset=utf-8");
      return;
    }
    if (!valid_hotspot_psk(psk)) {
      res.status = 400;
      res.set_content(R"({"ok":false,"error":"password must be 8-63 characters"})", "application/json; charset=utf-8");
      return;
    }
    if (!valid_ipv4(ip)) {
      res.status = 400;
      res.set_content(R"({"ok":false,"error":"invalid gateway IP address"})", "application/json; charset=utf-8");
      return;
    }
    if (!valid_wlan_iface(iface)) {
      res.status = 400;
      res.set_content(R"({"ok":false,"error":"invalid interface name"})", "application/json; charset=utf-8");
      return;
    }
    if (!valid_hotspot_channel(channel)) {
      res.status = 400;
      res.set_content(R"x({"ok":false,"error":"invalid channel (use 1-14 for 2.4 GHz)"})x", "application/json; charset=utf-8");
      return;
    }

    auto r = run_on_host_shell(hotspot_apply_script(iface, ssid, psk, ip, channel));
    if (r.exit_code != 0) {
      std::string err = r.combined_output;
      trim_inplace(err);
      json j = {{"ok", false}, {"error", err.empty() ? "nmcli failed" : err}};
      res.status = 500;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }

    json cur; read_settings(cur);
    cur["hotspotConfig"] = {{"ssid", ssid}, {"psk", psk}, {"ip", ip},
                             {"iface", iface}, {"channel", channel}, {"enabled", true}};
    write_settings_atomic(cur);

    res.set_content(R"({"ok":true,"active":true})", "application/json; charset=utf-8");
  });

  svr.Get("/api/stats", [&](const httplib::Request&, httplib::Response& res) {
    proxy_stats_to_response(stats_urls, stats_timeout, res);
  });

  const int video_fetch_timeout = [] {
    try {
      return static_cast<int>(std::lround(std::stod(getenv_or("VIDEO_FETCH_TIMEOUT", "12"))));
    } catch (...) {
      return 12;
    }
  }();

  svr.Get("/api/video/profiles", [&](const httplib::Request&, httplib::Response& res) {
    const ParsedUrl vbase = video_service_base_url();
    int st = 0;
    std::string body;
    std::string ct;
    if (!fetch_video_upstream_path(vbase, "/api/profiles", video_fetch_timeout, st, body, ct)) {
      json j = {{"error", "video service unreachable"}, {"profiles", json::array()}};
      res.status = 502;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    if (st != 200) {
      json j = {{"error", "video service returned HTTP " + std::to_string(st)}, {"profiles", json::array()}};
      res.status = static_cast<int>(st == 404 ? 502 : st);
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    auto semi = ct.find(';');
    std::string main_type = semi == std::string::npos ? ct : ct.substr(0, semi);
    trim_inplace(main_type);
    if (main_type.empty()) main_type = "application/json";
    res.status = st;
    res.set_content(body, main_type.c_str());
    res.set_header("Cache-Control", "no-store");
  });

  svr.Get("/api/video/ready", [&](const httplib::Request& req, httplib::Response& res) {
    const ParsedUrl vbase = video_service_base_url();
    std::string qprof = req.get_param_value("profile");
    trim_inplace(qprof);
    std::string path = "/api/ready";
    if (!qprof.empty() && safe_profile_id(qprof)) path += "?profile=" + qprof;
    int st = 0;
    std::string body;
    std::string ct;
    if (!fetch_video_upstream_path(vbase, path, video_fetch_timeout, st, body, ct)) {
      json j = {{"ok", false}, {"reason", "video service unreachable"}};
      res.status = 502;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    auto semi = ct.find(';');
    std::string main_type = semi == std::string::npos ? ct : ct.substr(0, semi);
    trim_inplace(main_type);
    if (main_type.empty()) main_type = "application/json";
    res.status = st;
    res.set_content(body, main_type.c_str());
    res.set_header("Cache-Control", "no-store");
  });

  svr.Get("/api/video/stream", [&](const httplib::Request& req, httplib::Response& res) {
    std::string prof = req.get_param_value("profile");
    trim_inplace(prof);
    if (prof.empty()) prof = "uc60";
    if (!safe_profile_id(prof)) prof = "uc60";

    const ParsedUrl vbase = video_service_base_url();
    const std::string upstream_path = std::string("/stream?profile=") + prof;

    auto bridge = std::make_shared<MjpegBridge>();
    bridge->th = std::thread([bridge, vbase, upstream_path]() {
      httplib::Client cli(vbase.host, vbase.port);
      // rpicam-vid (chroot) can take several seconds before the first MJPEG byte.
      cli.set_read_timeout(120, 0);
      cli.set_connection_timeout(10, 0);
      auto result = cli.Get(upstream_path.c_str(), [&](const char* data, size_t len) {
        if (bridge->abort.load()) return false;
        {
          std::lock_guard<std::mutex> lk(bridge->mu);
          bridge->q.emplace_back(data, len);
          while (bridge->q.size() > 64) bridge->q.pop_front();
        }
        bridge->cv.notify_one();
        return true;
      });
      std::lock_guard<std::mutex> lk(bridge->mu);
      bridge->upstream_done = true;
      (void)result;
      bridge->cv.notify_all();
    });

    res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
    res.set_content_provider(
        "multipart/x-mixed-replace; boundary=frame",
        [bridge](size_t /*offset*/, httplib::DataSink& sink) -> bool {
          std::unique_lock<std::mutex> lk(bridge->mu);
          for (;;) {
            if (bridge->abort.load()) return false;
            while (!bridge->q.empty()) {
              std::string chunk = std::move(bridge->q.front());
              bridge->q.pop_front();
              lk.unlock();
              if (!sink.write(chunk.data(), chunk.size())) return false;
              lk.lock();
            }
            if (bridge->upstream_done) return false;
            bridge->cv.wait_for(lk, std::chrono::milliseconds(400));
          }
        },
        [bridge](bool /*success*/) {
          bridge->abort = true;
          bridge->cv.notify_all();
          if (bridge->th.joinable()) bridge->th.join();
        });
  });

  const int telemetry_fetch_timeout = [] {
    try {
      return static_cast<int>(std::lround(std::stod(getenv_or("TELEMETRY_FETCH_TIMEOUT", "8"))));
    } catch (...) {
      return 8;
    }
  }();

  svr.Get("/api/telemetry", [&](const httplib::Request&, httplib::Response& res) {
    const ParsedUrl tbase = telemetry_service_base_url();
    int st = 0;
    std::string body;
    std::string ct;
    if (!fetch_telemetry_upstream(tbase, "/api/telemetry", telemetry_fetch_timeout, st, body, ct)) {
      json j = {{"error", "telemetry service unreachable"}, {"connected", false}};
      res.status = 502;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    auto semi = ct.find(';');
    std::string main_type = semi == std::string::npos ? ct : ct.substr(0, semi);
    trim_inplace(main_type);
    if (main_type.empty()) main_type = "application/json";
    res.status = st;
    res.set_content(body, main_type.c_str());
    res.set_header("Cache-Control", "no-store");
  });

  svr.Get("/api/telemetry/config", [&](const httplib::Request&, httplib::Response& res) {
    const ParsedUrl tbase = telemetry_service_base_url();
    int st = 0;
    std::string body;
    std::string ct;
    if (!fetch_telemetry_upstream(tbase, "/api/config", telemetry_fetch_timeout, st, body, ct)) {
      json j = {{"error", "telemetry service unreachable"}};
      res.status = 502;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }
    auto semi = ct.find(';');
    std::string main_type = semi == std::string::npos ? ct : ct.substr(0, semi);
    trim_inplace(main_type);
    if (main_type.empty()) main_type = "application/json";
    res.status = st;
    res.set_content(body, main_type.c_str());
    res.set_header("Cache-Control", "no-store");
  });

  svr.Post("/api/telemetry/config", [&](const httplib::Request& req, httplib::Response& res) {
    json incoming;
    try {
      incoming = json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"ok":false,"error":"invalid JSON"})", "application/json; charset=utf-8");
      return;
    }
    json upstream = json::object();
    if (incoming.contains("device") && incoming["device"].is_string()) {
      const std::string dev = incoming["device"].get<std::string>();
      if (!valid_serial_device(dev)) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"invalid serial device path"})", "application/json; charset=utf-8");
        return;
      }
      upstream["device"] = dev;
    }
    if (incoming.contains("baud")) {
      if (!incoming["baud"].is_number_integer()) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"baud must be an integer"})", "application/json; charset=utf-8");
        return;
      }
      const int baud = incoming["baud"].get<int>();
      if (!valid_serial_baud(baud)) {
        res.status = 400;
        res.set_content(R"({"ok":false,"error":"unsupported baud rate"})", "application/json; charset=utf-8");
        return;
      }
      upstream["baud"] = baud;
    }
    if (upstream.empty()) {
      res.status = 400;
      res.set_content(R"({"ok":false,"error":"provide baud and/or device"})", "application/json; charset=utf-8");
      return;
    }

    const ParsedUrl tbase = telemetry_service_base_url();
    httplib::Client cli(tbase.host, tbase.port);
    cli.set_read_timeout(telemetry_fetch_timeout, 0);
    cli.set_connection_timeout(3, 0);
    auto upstream_res = cli.Post("/api/config", upstream.dump(), "application/json; charset=utf-8");
    if (!upstream_res) {
      json j = {{"ok", false}, {"error", "telemetry service unreachable"}};
      res.status = 502;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }

    json cur;
    if (read_settings(cur)) {
      if (upstream.contains("device")) cur["fcSerialDevice"] = upstream["device"];
      if (upstream.contains("baud")) cur["fcSerialBaud"] = upstream["baud"];
      write_settings_atomic(cur);
    }

    res.status = upstream_res->status;
    res.set_content(upstream_res->body, "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
  });

  auto serve_index = [](const httplib::Request&, httplib::Response& res) {
    auto raw = read_text_file("./static/index.html");
    if (!raw) {
      res.status = 500;
      res.set_content("missing index.html", "text/plain; charset=utf-8");
      return;
    }
    res.set_header("Cache-Control", "no-store, no-cache, must-revalidate");
    res.set_content(*raw, "text/html; charset=utf-8");
  };
  svr.Get("/", serve_index);
  svr.Get("/index.html", serve_index);

  std::cout << "[hmi] listening on http://" << host << ":" << port << "\n" << std::flush;
  if (!svr.listen(host.c_str(), port)) {
    std::cerr << "[hmi] listen failed\n";
    return 1;
  }
  return 0;
}
