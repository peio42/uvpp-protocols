#include "test.hpp"

#include <cstddef>
#include <span>
#include <string_view>

#include <uvpp/protocols/websocket.hpp>

UVP_TEST_CASE("websocket accept options expose configured callbacks and limits") {
  UVP_CHECK(uvp::websocket::accept_options{}.auto_pong());

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

  UVP_CHECK_EQ(websocket_options.max_message_bytes(), 64 * 1024U);
  UVP_CHECK_EQ(websocket_options.max_pending_write_bytes(), 64 * 1024U);
  UVP_CHECK_EQ(websocket_options.subprotocol(), "chat");
  UVP_CHECK(!websocket_options.auto_pong());
  UVP_CHECK(static_cast<bool>(websocket_options.on_text()));
  UVP_CHECK(static_cast<bool>(websocket_options.on_binary()));
  UVP_CHECK(static_cast<bool>(websocket_options.on_ping()));
  UVP_CHECK(static_cast<bool>(websocket_options.on_close()));
  UVP_CHECK(static_cast<bool>(websocket_options.on_error()));
}

