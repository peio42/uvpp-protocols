#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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

  srv.get("/health", [](uvp::http::request&, uvp::http::response& res) {
    res.json(uvp::json{{"status", "ok"}});
  });

  srv.get("/logs", [](uvp::http::request&, uvp::http::response& res) {
    res.text(read_recent_logs());
  });

  srv.post("/config", uvp::http::body::text{}, [](uvp::http::request&, uvp::http::response& res, std::string_view body) {
    auto cfg = parse_config(body);
    apply_config(cfg);
    res.status(uvp::http::status::no_content).end();
  });

  srv.post("/upload", uvp::http::body::stream{}, [](uvp::http::request&, uvp::http::response& res, uvp::http::request_body_stream& body) {
    auto total = std::make_shared<std::size_t>(0);
    auto reply = std::make_shared<uvp::http::deferred_response>(res.defer());

    body.on_data([total](std::span<const std::byte> chunk) {
      *total += chunk.size();
    });
    body.on_end([total, reply] {
      if (reply->active()) {
        reply->status(uvp::http::status::created).json(uvp::json{{"bytes", *total}});
      }
    });
    body.on_error([reply](std::error_code) {
      if (reply->active()) {
        reply->status(400).json(uvp::json{{"error", "upload failed"}});
      }
    });
  });

  srv.listen("127.0.0.1", 8080);
  std::cout << "listening on http://127.0.0.1:8080 with "
            << srv.routes().size() << " routes\n";

  loop.run();
}
