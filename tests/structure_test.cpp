#include <cassert>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

#include "../src/http/detail/http1_state_machine.hpp"

using namespace std::chrono_literals;

int main() {
  auto options = uvp::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_body_bytes(1024 * 1024)
    .max_pending_responses_per_connection(8)
    .idle_timeout(30s)
    .server_header(false);

  assert(options.max_header_bytes_ == 32 * 1024);
  assert(options.max_body_bytes_ == 1024 * 1024);
  assert(options.max_pending_responses_per_connection_ == 8);
  assert(options.idle_timeout_ == 30s);
  assert(!options.server_header_);

  uvp::http::headers headers;
  headers.set("Content-Type", "text/plain");
  assert(headers.contains("content-type"));
  assert(headers.get("CONTENT-TYPE") == "text/plain");

  uvp::http::router router;
  router.get("/health", [](uvp::http::request&, uvp::http::response& res) {
    res.text("ok");
  });

  assert(router.size() == 1);
  assert(router.find(uvp::http::method::get, "/health") != nullptr);
  assert(router.find(uvp::http::method::post, "/health") == nullptr);

  router.get("/users/:id", [](uvp::http::request&, uvp::http::response&) {});
  auto user_match = router.match(uvp::http::method::get, "/users/alice");
  assert(user_match);
  assert(user_match.params.get("id") == "alice");

  router.get("/static/*path", [](uvp::http::request&, uvp::http::response&) {});
  auto static_match = router.match(uvp::http::method::get, "/static/css/app.css");
  assert(static_match);
  assert(static_match.params.get("path") == "css/app.css");

  uvp::http::response response;
  response.json({{"status", "ok"}});
  assert(response.ended());
  assert(response.body() == "{\"status\":\"ok\"}");

  uvp::http::response deferred_response;
  auto reply = deferred_response.defer();
  bool cancelled = false;
  reply.on_cancel([&] {
    cancelled = true;
  });
  assert(reply.active());
  assert(deferred_response.deferred());
  reply.status(uvp::http::status::created).text("later");
  assert(!reply.active());
  assert(!deferred_response.deferred());
  assert(deferred_response.ended());
  assert(deferred_response.status_code() == static_cast<unsigned int>(uvp::http::status::created));
  assert(deferred_response.body() == "later");
  assert(!cancelled);

  uv::loop loop;
  uvp::http::server server(loop, options);
  server.get("/health", [](uvp::http::request&, uvp::http::response& res) {
    res.text("ok");
  });
  server.not_found([](uvp::http::request&, uvp::http::response& res) {
    res.status(uvp::http::status::not_found).text("custom not found\n");
  });
  server.on_error([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
    res.status(uvp::http::status::internal_server_error).text("custom error\n");
  });
  assert(server.routes().size() == 1);

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto endpoint = listener.local_endpoint();
  assert(std::holds_alternative<uvp::io::tcp_endpoint>(endpoint));
  assert(std::get<uvp::io::tcp_endpoint>(endpoint).port != 0);
  listener.close();

  uvp::http::detail::http1_state_machine parser;
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
  assert(message.method == uvp::http::method::post);
  assert(message.target == "/config?dry_run=1");
  assert(message.headers.get("host") == "example.test");
  assert(message.headers.get("content-type") == "text/plain");
  assert(message.body == "test");
  assert(message.http_major == 1);
  assert(message.http_minor == 1);
}
