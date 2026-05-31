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

  uvp::http::server srv(
    loop,
    uvp::http::server_options{}
      .max_header_bytes(32 * 1024)
      .max_body_bytes(1024 * 1024)
      .idle_timeout(30s));

  srv.get("/health", [](uvp::http::request& req, uvp::http::response& res) {
    (void)req;
    res.json({{"status", "ok"}});
  });

  srv.get("/logs", [](uvp::http::request& req, uvp::http::response& res) {
    (void)req;
    res.text(read_recent_logs());
  });

  srv.post("/config", [](uvp::http::request& req, uvp::http::response& res) {
    auto cfg = parse_config(req.body());
    apply_config(cfg);
    res.status(uvp::http::status::no_content).end();
  });

  srv.listen("127.0.0.1", 8080);
  std::cout << "listening on http://127.0.0.1:8080 with "
            << srv.routes().size() << " routes\n";

  loop.run();
}
