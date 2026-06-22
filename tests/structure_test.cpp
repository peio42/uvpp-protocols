#include <cassert>
#include <chrono>
#include <cstddef>
#include <exception>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>
#include <uvpp/protocols/websocket.hpp>

#include "../src/http/detail/http1_state_machine.hpp"

using namespace std::chrono_literals;

int main() {
  auto options = uvp::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_body_bytes(1024 * 1024)
    .max_pending_responses_per_connection(8)
    .idle_timeout(30s)
    .server_header(false);

  assert(options.max_header_bytes() == 32 * 1024);
  assert(options.max_body_bytes() == 1024 * 1024);
  assert(options.max_pending_responses_per_connection() == 8);
  assert(options.idle_timeout() == 30s);
  assert(!options.server_header());

  uvp::http::headers headers;
  headers.set("Content-Type", "text/plain");
  assert(headers.contains("content-type"));
  assert(headers.get("CONTENT-TYPE") == "text/plain");

  auto connection = uvp::http::connection_info{
    uvp::io::tcp_endpoint{"127.0.0.1", 8080},
    uvp::io::pipe_endpoint{"/tmp/uvpp.sock"},
  };
  assert(std::holds_alternative<uvp::io::tcp_endpoint>(connection.local_endpoint()));
  assert(std::holds_alternative<uvp::io::pipe_endpoint>(connection.remote_endpoint()));

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

  router.post("/echo", [](uvp::http::request&, uvp::http::response&, std::span<const std::byte>) {});
  auto bytes_match = router.match(uvp::http::method::post, "/echo");
  assert(bytes_match);
  assert(bytes_match.body == uvp::http::body_mode::bytes);

  router.post("/message", [](uvp::http::request&, uvp::http::response&, std::string_view) {});
  auto text_match = router.match(uvp::http::method::post, "/message");
  assert(text_match);
  assert(text_match.body == uvp::http::body_mode::text);

  router.post("/upload", [](uvp::http::request&, uvp::http::response&, uvp::http::request_body_stream&) {});
  auto stream_match = router.match(uvp::http::method::post, "/upload");
  assert(stream_match);
  assert(stream_match.body == uvp::http::body_mode::stream);

  router.post("/empty", uvp::http::body::none{}, [](uvp::http::request&, uvp::http::response&) {});
  auto none_match = router.match(uvp::http::method::post, "/empty");
  assert(none_match);
  assert(none_match.body == uvp::http::body_mode::none);

  uvp::http::response response;
  response.json(uvp::json{{"status", "ok"}});
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

  uvp::http::response streaming_response;
  auto stream = streaming_response.stream();
  bool stream_cancelled = false;
  bool stream_drained = false;
  bool stream_errored = false;
  stream
    .type("application/x-ndjson")
    .on_cancel([&] {
      stream_cancelled = true;
    })
    .on_drain([&] {
      stream_drained = true;
    })
    .on_error([&](std::error_code) {
      stream_errored = true;
    });
  assert(streaming_response.streaming());
  auto unattached_write = stream.write(std::string{"line\n"});
  assert(!unattached_write.accepted());
  assert(!unattached_write);
  assert(!stream_cancelled);
  assert(!stream_drained);
  assert(!stream_errored);

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
  server.upgrade("/echo", [](uvp::http::upgrade_request& req) {
    (void)req;
  });
  assert(server.routes().size() == 1);

  assert(uvp::websocket::accept_options{}.auto_pong());

  auto websocket_options = uvp::websocket::accept_options{}
    .max_message_bytes(64 * 1024)
    .max_pending_write_bytes(64 * 1024)
    .subprotocol("chat")
    .auto_pong(false)
    .on_text([](uvp::websocket::session& ws, std::string_view message) {
      ws.text(message);
    })
    .on_binary([](uvp::websocket::session& ws, std::span<const std::byte> message) {
      ws.binary(message);
    })
    .on_ping([](uvp::websocket::session& ws, std::span<const std::byte> payload) {
      ws.pong(payload);
    })
    .on_close([](uvp::websocket::session&, uvp::websocket::close_code, std::string_view) {})
    .on_error([](uvp::websocket::session&, std::error_code) {});
  assert(websocket_options.max_message_bytes() == 64 * 1024);
  assert(websocket_options.max_pending_write_bytes() == 64 * 1024);
  assert(websocket_options.subprotocol() == "chat");
  assert(!websocket_options.auto_pong());

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

  uvp::http::detail::http1_state_machine upgrade_parser;
  const auto upgrade_result = upgrade_parser.parse(
    "GET /echo HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n");
  assert(upgrade_result.code == uvp::http::detail::http1_parse_result::status::upgrade);
}
