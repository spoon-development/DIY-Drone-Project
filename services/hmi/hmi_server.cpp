#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <optional>
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

void apply_netplan_on_host(const std::string& yaml_text, const std::filesystem::path& host_file,
                           const std::string& netplan_cmd) {
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

  std::vector<std::string> args = {"nsenter", "-t", "1", "-m", "-u", "-i", "-n", "-p", netplan_cmd, "apply"};
  auto rr = run_command_argv(args);
  if (rr.exit_code != 0) {
    std::string err = rr.combined_output;
    trim_inplace(err);
    if (err.empty()) err = "netplan exit " + std::to_string(rr.exit_code);
    throw std::runtime_error(err);
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
    res.set_content(cur.dump(), "application/json; charset=utf-8");
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
    try {
      std::filesystem::path host_netplan(getenv_or("HMI_HOST_NETPLAN_FILE", "/etc/netplan/99-drone-hmi.yaml"));
      std::string netplan_cmd = getenv_or("HMI_NETPLAN_CMD", "/usr/sbin/netplan");
      trim_inplace(netplan_cmd);
      if (netplan_cmd.empty()) netplan_cmd = "/usr/sbin/netplan";
      apply_netplan_on_host(yaml_text, host_netplan, netplan_cmd);
    } catch (const std::exception& e) {
      json j = {{"ok", false}, {"error", e.what()}};
      res.status = 500;
      res.set_content(j.dump(), "application/json; charset=utf-8");
      return;
    }

    bool reboot_scheduled = false;
    if (!env_flag_off("HMI_REBOOT_AFTER_NETPLAN", "1")) {
      if (access("/usr/bin/nsenter", X_OK) == 0 || access("/bin/nsenter", X_OK) == 0) {
        schedule_host_reboot();
        reboot_scheduled = true;
      }
    }
    json j = {{"ok", true}, {"rebootScheduled", reboot_scheduled}};
    res.set_content(j.dump(), "application/json; charset=utf-8");
  });

  svr.Get("/api/stats", [&](const httplib::Request&, httplib::Response& res) {
    proxy_stats_to_response(stats_urls, stats_timeout, res);
  });

  auto serve_index = [](const httplib::Request&, httplib::Response& res) {
    auto raw = read_text_file("./static/index.html");
    if (!raw) {
      res.status = 500;
      res.set_content("missing index.html", "text/plain; charset=utf-8");
      return;
    }
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
