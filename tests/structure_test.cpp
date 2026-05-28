#include <cassert>
#include <chrono>
#include <stdexcept>

namespace uv {
class loop {};
} // namespace uv

#include <uvpp/protocols/http.hpp>

#include "../src/http/detail/http1_state_machine.hpp"

using namespace std::chrono_literals;

int main() {
  auto options = uv::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_body_bytes(1024 * 1024)
    .idle_timeout(30s)
    .server_header(false);

  assert(options.max_header_bytes_ == 32 * 1024);
  assert(options.max_body_bytes_ == 1024 * 1024);
  assert(options.idle_timeout_ == 30s);
  assert(!options.server_header_);

  uv::http::headers headers;
  headers.set("Content-Type", "text/plain");
  assert(headers.contains("content-type"));
  assert(headers.get("CONTENT-TYPE") == "text/plain");

  uv::http::router router;
  router.get("/health", [](uv::http::request&, uv::http::response& res) {
    res.text("ok");
  });

  assert(router.size() == 1);
  assert(router.find(uv::http::method::get, "/health") != nullptr);
  assert(router.find(uv::http::method::post, "/health") == nullptr);

  uv::http::response response;
  response.json({{"status", "ok"}});
  assert(response.ended());
  assert(response.body() == "{\"status\":\"ok\"}");

  uv::loop loop;
  uv::http::server server(loop, options);
  server.get("/health", [](uv::http::request&, uv::http::response& res) {
    res.text("ok");
  });
  assert(server.routes().size() == 1);

  bool listen_failed = false;
  try {
    server.listen("127.0.0.1", 8080);
  } catch (const std::logic_error&) {
    listen_failed = true;
  }
  assert(listen_failed);

  uv::http::detail::http1_state_machine parser;
  const auto result = parser.parse(
    "POST /config?dry_run=1 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "test");

  assert(result.ok());
  assert(parser.completed_messages().size() == 1);

  const auto& message = parser.completed_messages().front();
  assert(message.method == uv::http::method::post);
  assert(message.target == "/config?dry_run=1");
  assert(message.headers.get("host") == "example.test");
  assert(message.headers.get("content-type") == "text/plain");
  assert(message.body == "test");
  assert(message.http_major == 1);
  assert(message.http_minor == 1);
}
