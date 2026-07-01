#include "test.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>
#include <uvpp/protocols/websocket.hpp>

#include "src/websocket/detail/handshake.hpp"

namespace {

uv::buffer_view websocket_alloc(uv::tcp&, std::size_t) {
  static std::array<char, 4096> storage{};
  return uv::buffer_view{storage.data(), storage.size()};
}

std::string masked_text_frame(std::string_view payload, std::array<unsigned char, 4> mask) {
  std::string frame;
  frame.reserve(payload.size() + 6U);
  frame.push_back(static_cast<char>(0x81U));
  frame.push_back(static_cast<char>(0x80U | payload.size()));
  for (auto byte : mask) {
    frame.push_back(static_cast<char>(byte));
  }
  for (std::size_t index = 0; index < payload.size(); ++index) {
    frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]));
  }
  return frame;
}

} // namespace

UVP_TEST_CASE("websocket accept value matches RFC 6455 example") {
  const auto accept = uvp::websocket::detail::websocket_accept_value("dGhlIHNhbXBsZSBub25jZQ==");
  UVP_CHECK_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

UVP_TEST_CASE("websocket accept options expose configured limits") {
  UVP_CHECK(uvp::websocket::accept_options{}.auto_pong());

  auto websocket_options = uvp::websocket::accept_options{}
    .max_message_bytes(64 * 1024)
    .max_pending_write_bytes(64 * 1024)
    .subprotocol("chat")
    .auto_pong(false);

  UVP_CHECK_EQ(websocket_options.max_message_bytes(), 64 * 1024U);
  UVP_CHECK_EQ(websocket_options.max_pending_write_bytes(), 64 * 1024U);
  UVP_CHECK_EQ(websocket_options.subprotocol(), "chat");
  UVP_CHECK(!websocket_options.auto_pong());
}

UVP_TEST_CASE("websocket session exposes callback setters") {
  auto websocket = uvp::websocket::session{};
  websocket
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
}

UVP_TEST_CASE("websocket parses coalesced client frames without compacting per frame") {
  uv::loop loop;
  uv::tcp client(loop);
  uv::connect_request connect_request;
  uv::write_request handshake_write;
  uv::write_request frames_write;
  uv::timer timeout(loop);

  uvp::http::server server(loop);
  std::vector<std::string> messages;
  std::string observed_name;
  std::vector<std::string> observed_segments;
  bool client_closed = false;
  bool timed_out = false;

  server.upgrade("/ws/:name", [&](uvp::http::upgrade_request& req) {
    observed_name = req.params().get("name");
    observed_segments.assign(req.decoded_path_segments().begin(), req.decoded_path_segments().end());
    (void)uvp::websocket::accept_detached(req)
      .on_text([&](uvp::websocket::session&, std::string_view message) {
        messages.emplace_back(message);
        if (messages.size() == 2U) {
          timeout.close();
          if (!client_closed) {
            client.close([&](uv::tcp&) {
              client_closed = true;
            });
          }
          server.close();
        }
      });
  });

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto stream_listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto endpoint = stream_listener.local_endpoint();
  const auto port = std::get<uvp::io::tcp_endpoint>(endpoint).port;
  server.listen(std::move(stream_listener));

  std::string handshake =
    "GET /ws/a%2Fb HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";
  std::string response;
  std::string frames = masked_text_frame("one", {0x01, 0x02, 0x03, 0x04});
  frames += masked_text_frame("two", {0x05, 0x06, 0x07, 0x08});
  bool frames_sent = false;

  timeout.start(std::chrono::seconds{2}, [&](uv::timer& timer) {
    timed_out = true;
    server.close();
    if (!client_closed) {
      client.close([&](uv::tcp&) {
        client_closed = true;
      });
    }
    timer.close();
  });

  client.connect(connect_request, uv::ipv4{"127.0.0.1", static_cast<int>(port)}, [&](uv::connect_request&, uv::result status) {
    UVP_REQUIRE(status);
    auto output = uv::buffer_view{handshake.data(), handshake.size()};
    client.write(handshake_write, output, [&](uv::write_request&, uv::result write_status) {
      UVP_REQUIRE(write_status);

      client.read_start(websocket_alloc, [&](uv::tcp& stream, uv::read_result read) {
        if (read.eof()) {
          return;
        }
        UVP_REQUIRE(read.ok());

        const auto bytes = read.bytes();
        response.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!frames_sent && response.find("\r\n\r\n") != std::string::npos) {
          UVP_CHECK(response.find("HTTP/1.1 101 Switching Protocols\r\n") != std::string::npos);
          UVP_CHECK(response.find("sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);

          frames_sent = true;
          auto input = uv::buffer_view{frames.data(), frames.size()};
          stream.write(frames_write, input, [](uv::write_request&, uv::result frame_write_status) {
            UVP_REQUIRE(frame_write_status);
          });
        }
      });
    });
  });

  loop.run();

  UVP_CHECK(!timed_out);
  UVP_CHECK(frames_sent);
  UVP_CHECK(client_closed);
  UVP_REQUIRE(messages.size() == 2U);
  UVP_CHECK_EQ(messages[0], "one");
  UVP_CHECK_EQ(messages[1], "two");
  UVP_CHECK_EQ(observed_name, "a/b");
  UVP_REQUIRE(observed_segments.size() == 2U);
  UVP_CHECK_EQ(observed_segments[0], "ws");
  UVP_CHECK_EQ(observed_segments[1], "a/b");

  loop.close();
}
