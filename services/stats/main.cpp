#include <httplib.h>

#include <cstdlib>
#include <iostream>
#include <thread>

#include "collect.hpp"

int main() {
  int port = 9101;
  if (const char* p = std::getenv("STATS_PORT")) {
    try {
      port = std::stoi(p);
    } catch (...) {
    }
  }

  httplib::Server svr;
  svr.new_task_queue = [] {
    const unsigned n = std::thread::hardware_concurrency();
    return new httplib::ThreadPool(n ? n : 4u);
  };

  auto send_json = [](httplib::Response& res, const std::string& body) {
    res.set_content(body, "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
  };

  auto metrics = [&](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j = dronebros::stats::collect();
    send_json(res, j.dump(2) + "\n");
  };

  svr.Get("/", metrics);
  svr.Get("/metrics", metrics);
  svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
    send_json(res, "{\"status\":\"ok\"}\n");
  });

  std::cout << "[stats] listening on http://0.0.0.0:" << port << "/metrics\n" << std::flush;
  if (!svr.listen("0.0.0.0", port)) {
    std::cerr << "[stats] listen failed\n";
    return 1;
  }
  return 0;
}
