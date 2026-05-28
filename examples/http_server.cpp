#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

using namespace std::chrono_literals;

namespace {

std::string read_recent_logs() {
  return "no logs yet\n";
}

struct config {
  std::string raw;
};

config parse_config(std::string_view body) {
  return config{std::string(body)};
}

void apply_config(const config&) {}

} // namespace

int main() {
  uv::loop loop;

  uv::http::server srv(
    loop,
    uv::http::server_options{}
      .max_header_bytes(32 * 1024)
      .max_body_bytes(1024 * 1024)
      .idle_timeout(30s));

  srv.get("/health", [](uv::http::request& req, uv::http::response& res) {
    (void)req;
    res.json({{"status", "ok"}});
  });

  srv.get("/logs", [](uv::http::request& req, uv::http::response& res) {
    (void)req;
    res.text(read_recent_logs());
  });

  srv.post("/config", [](uv::http::request& req, uv::http::response& res) {
    auto cfg = parse_config(req.body());
    apply_config(cfg);
    res.status(uv::http::status::no_content).end();
  });

  std::cout << "registered routes: " << srv.routes().size() << '\n';

  try {
    srv.listen("127.0.0.1", 8080);
  } catch (const std::logic_error& error) {
    std::cout << "listen reserved for milestone 1: " << error.what() << '\n';
  }
}
