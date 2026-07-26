#include "test.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stdexcept>
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
  UVP_REQUIRE(payload.size() <= 125U);

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

std::string masked_frame(unsigned char first, std::span<const std::byte> payload, std::array<unsigned char, 4> mask) {
  UVP_REQUIRE(payload.size() <= 125U);

  std::string frame;
  frame.reserve(payload.size() + 6U);
  frame.push_back(static_cast<char>(first));
  frame.push_back(static_cast<char>(0x80U | payload.size()));
  for (auto byte : mask) {
    frame.push_back(static_cast<char>(byte));
  }
  for (std::size_t index = 0; index < payload.size(); ++index) {
    const auto value = static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()];
    frame.push_back(static_cast<char>(value));
  }
  return frame;
}

std::string masked_close_frame(unsigned short code, std::array<unsigned char, 4> mask, std::string_view reason = {}) {
  std::vector<std::byte> payload;
  payload.reserve(2U + reason.size());
  payload.push_back(std::byte{static_cast<unsigned char>((code >> 8U) & 0xffU)});
  payload.push_back(std::byte{static_cast<unsigned char>(code & 0xffU)});
  for (const char byte : reason) {
    payload.push_back(std::byte{static_cast<unsigned char>(byte)});
  }
  return masked_frame(0x88U, payload, mask);
}

std::string websocket_handshake(
  std::string_view path = "/ws",
  std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==",
  std::string_view subprotocols = {}) {
  auto handshake = std::string{"GET "} + std::string{path} + " HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "uPgRaDe: WebSocket\r\n"
    "cOnNeCtIoN: keep-alive, UpGrAdE\r\n"
    "sEc-wEbSoCkEt-kEy: " + std::string{key} + "\r\n"
    "sEc-wEbSoCkEt-vErSiOn: 13\r\n";
  if (!subprotocols.empty()) {
    handshake += "Sec-WebSocket-Protocol: ";
    handshake += subprotocols;
    handshake += "\r\n";
  }
  handshake += "\r\n";
  return handshake;
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
    .close_timeout(std::chrono::milliseconds{250})
    .subprotocol("chat")
    .auto_pong(false);

  UVP_CHECK_EQ(websocket_options.max_message_bytes(), 64 * 1024U);
  UVP_CHECK_EQ(websocket_options.max_pending_write_bytes(), 64 * 1024U);
  UVP_CHECK(websocket_options.close_timeout() == std::chrono::milliseconds{250});
  UVP_CHECK_EQ(websocket_options.subprotocol(), "chat");
  UVP_CHECK(!websocket_options.auto_pong());
  UVP_CHECK_THROWS(
    uvp::websocket::accept_options{}.close_timeout(std::chrono::milliseconds{0}),
    std::invalid_argument);
  UVP_CHECK_THROWS(
    uvp::websocket::accept_options{}.subprotocol("chat\r\nX-Injected: yes"),
    std::invalid_argument);
  UVP_CHECK_THROWS(
    uvp::websocket::accept_options{}.subprotocol("chat, superchat"),
    std::invalid_argument);
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

UVP_TEST_CASE("websocket validates outbound text and control frame arguments") {
  auto websocket = uvp::websocket::session{};
  const std::array<std::byte, 126> oversized{};
  const std::string invalid_utf8{"\xc3\x28", 2U};
  const std::string long_reason(124U, 'x');

  UVP_CHECK_THROWS(websocket.text(invalid_utf8), std::invalid_argument);
  UVP_CHECK_THROWS(websocket.ping(oversized), std::invalid_argument);
  UVP_CHECK_THROWS(websocket.pong(oversized), std::invalid_argument);
  UVP_CHECK_THROWS(websocket.close(uvp::websocket::close_code::normal, invalid_utf8), std::invalid_argument);
  UVP_CHECK_THROWS(websocket.close(uvp::websocket::close_code::normal, long_reason), std::invalid_argument);
  UVP_CHECK_THROWS(websocket.close(static_cast<uvp::websocket::close_code>(1005)), std::invalid_argument);
}

UVP_TEST_CASE("websocket rejects malformed handshake nonces") {
  uv::loop loop;
  uv::tcp client(loop);
  uv::connect_request connect_request;
  uv::write_request handshake_write;
  uv::timer timeout(loop);

  uvp::http::server server(loop);
  server.upgrade("/ws", [](uvp::http::upgrade_request& req) {
    (void)uvp::websocket::accept_detached(req, uvp::websocket::accept_options{}.subprotocol("chat"));
  });

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto stream_listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto port = std::get<uvp::io::tcp_endpoint>(stream_listener.local_endpoint()).port;
  server.listen(std::move(stream_listener));

  auto handshake = websocket_handshake("/ws", "not-base64", "chat");
  std::string response;
  bool client_closed = false;
  bool rejected = false;
  bool timed_out = false;

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
          stream.read_stop();
          timeout.close();
          stream.close([&](uv::tcp&) {
            client_closed = true;
          });
          server.close();
          return;
        }
        UVP_REQUIRE(read.ok());
        const auto bytes = read.bytes();
        response.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (response.find("\r\n\r\n") != std::string::npos) {
          rejected = response.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos;
        }
      });
    });
  });

  loop.run();

  UVP_CHECK(!timed_out);
  UVP_CHECK(rejected);
  UVP_CHECK(client_closed);

  loop.close();
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
    (void)uvp::websocket::accept_detached(req, uvp::websocket::accept_options{}.subprotocol("chat"))
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

  std::string handshake = websocket_handshake("/ws/a%2Fb", "dGhlIHNhbXBsZSBub25jZQ==", "superchat, chat");
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
          UVP_CHECK(response.find("sec-websocket-protocol: chat\r\n") != std::string::npos);

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

UVP_TEST_CASE("websocket closes after close handshake timeout") {
  uv::loop loop;
  uv::tcp client(loop);
  uv::connect_request connect_request;
  uv::write_request handshake_write;
  uv::write_request close_write;
  uv::timer timeout(loop);

  uvp::http::server server(loop);
  bool on_close_called = false;
  bool on_error_called = false;
  bool client_closed = false;
  bool eof_seen = false;
  bool close_sent = false;
  bool timed_out = false;

  server.upgrade("/ws", [&](uvp::http::upgrade_request& req) {
    (void)uvp::websocket::accept_detached(
      req,
      uvp::websocket::accept_options{}.close_timeout(std::chrono::milliseconds{20}))
      .on_close([&](uvp::websocket::session&, uvp::websocket::close_code code, std::string_view) {
        on_close_called = code == uvp::websocket::close_code::normal;
      })
      .on_error([&](uvp::websocket::session&, std::error_code) {
        on_error_called = true;
      });
  });

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto stream_listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto port = std::get<uvp::io::tcp_endpoint>(stream_listener.local_endpoint()).port;
  server.listen(std::move(stream_listener));

  auto handshake = websocket_handshake();
  auto close_frame = masked_close_frame(1000, {0x01, 0x02, 0x03, 0x04});
  std::string response;

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
          eof_seen = true;
          stream.read_stop();
          timeout.close();
          stream.close([&](uv::tcp&) {
            client_closed = true;
          });
          server.close();
          return;
        }
        UVP_REQUIRE(read.ok());

        const auto bytes = read.bytes();
        response.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!close_sent && response.find("\r\n\r\n") != std::string::npos) {
          close_sent = true;
          auto input = uv::buffer_view{close_frame.data(), close_frame.size()};
          stream.write(close_write, input, [](uv::write_request&, uv::result close_status) {
            UVP_REQUIRE(close_status);
          });
        }
      });
    });
  });

  loop.run();

  std::string expected_close;
  expected_close.push_back(static_cast<char>(0x88U));
  expected_close.push_back(static_cast<char>(0x02U));
  expected_close.push_back(static_cast<char>(0x03U));
  expected_close.push_back(static_cast<char>(0xe8U));

  UVP_CHECK(!timed_out);
  UVP_CHECK(close_sent);
  UVP_CHECK(eof_seen);
  UVP_CHECK(client_closed);
  UVP_CHECK(on_close_called);
  UVP_CHECK(!on_error_called);
  UVP_CHECK(response.find(expected_close) != std::string::npos);

  loop.close();
}

UVP_TEST_CASE("websocket rejects invalid UTF-8 text with close code 1007") {
  uv::loop loop;
  uv::tcp client(loop);
  uv::connect_request connect_request;
  uv::write_request handshake_write;
  uv::write_request text_write;
  uv::timer timeout(loop);

  uvp::http::server server(loop);
  bool on_text_called = false;
  bool on_close_called = false;
  bool invalid_payload_error = false;
  bool client_closed = false;
  bool invalid_text_sent = false;
  bool invalid_payload_close_seen = false;
  bool timed_out = false;

  server.upgrade("/ws", [&](uvp::http::upgrade_request& req) {
    (void)uvp::websocket::accept_detached(
      req,
      uvp::websocket::accept_options{}.close_timeout(std::chrono::milliseconds{50}))
      .on_text([&](uvp::websocket::session&, std::string_view) {
        on_text_called = true;
      })
      .on_close([&](uvp::websocket::session&, uvp::websocket::close_code, std::string_view) {
        on_close_called = true;
      })
      .on_error([&](uvp::websocket::session&, std::error_code error) {
        invalid_payload_error = error == std::make_error_code(std::errc::illegal_byte_sequence);
      });
  });

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto stream_listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto port = std::get<uvp::io::tcp_endpoint>(stream_listener.local_endpoint()).port;
  server.listen(std::move(stream_listener));

  auto handshake = websocket_handshake();
  const std::string invalid_text{"\xc3\x28", 2U};
  auto invalid_frame = masked_text_frame(invalid_text, {0x05, 0x06, 0x07, 0x08});
  std::string response;
  const std::string expected_invalid_payload_close{"\x88\x02\x03\xef", 4U};

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
        if (!invalid_text_sent && response.find("\r\n\r\n") != std::string::npos) {
          invalid_text_sent = true;
          auto input = uv::buffer_view{invalid_frame.data(), invalid_frame.size()};
          stream.write(text_write, input, [](uv::write_request&, uv::result frame_write_status) {
            UVP_REQUIRE(frame_write_status);
          });
        }
        if (!invalid_payload_close_seen && response.find(expected_invalid_payload_close) != std::string::npos) {
          invalid_payload_close_seen = true;
          stream.read_stop();
          timeout.close();
          stream.close([&](uv::tcp&) {
            client_closed = true;
          });
          server.close();
        }
      });
    });
  });

  loop.run();

  UVP_CHECK(!timed_out);
  UVP_CHECK(invalid_text_sent);
  UVP_CHECK(invalid_payload_close_seen);
  UVP_CHECK(client_closed);
  UVP_CHECK(!on_text_called);
  UVP_CHECK(!on_close_called);
  UVP_CHECK(invalid_payload_error);

  loop.close();
}

UVP_TEST_CASE("websocket rejects invalid UTF-8 close reasons with close code 1007") {
  uv::loop loop;
  uv::tcp client(loop);
  uv::connect_request connect_request;
  uv::write_request handshake_write;
  uv::write_request close_write;
  uv::timer timeout(loop);

  uvp::http::server server(loop);
  bool on_close_called = false;
  bool on_error_called = false;
  bool client_closed = false;
  bool invalid_close_sent = false;
  bool protocol_close_seen = false;
  bool timed_out = false;

  server.upgrade("/ws", [&](uvp::http::upgrade_request& req) {
    (void)uvp::websocket::accept_detached(
      req,
      uvp::websocket::accept_options{}.close_timeout(std::chrono::milliseconds{50}))
      .on_close([&](uvp::websocket::session&, uvp::websocket::close_code, std::string_view) {
        on_close_called = true;
      })
      .on_error([&](uvp::websocket::session&, std::error_code error) {
        on_error_called = error == std::make_error_code(std::errc::illegal_byte_sequence);
      });
  });

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto stream_listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto port = std::get<uvp::io::tcp_endpoint>(stream_listener.local_endpoint()).port;
  server.listen(std::move(stream_listener));

  auto handshake = websocket_handshake();
  const std::string invalid_reason{"\xc3\x28", 2U};
  auto invalid_close = masked_close_frame(1000, {0x05, 0x06, 0x07, 0x08}, invalid_reason);
  std::string response;
  const std::string expected_invalid_payload_close{"\x88\x02\x03\xef", 4U};

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
        if (!invalid_close_sent && response.find("\r\n\r\n") != std::string::npos) {
          invalid_close_sent = true;
          auto input = uv::buffer_view{invalid_close.data(), invalid_close.size()};
          stream.write(close_write, input, [](uv::write_request&, uv::result close_status) {
            UVP_REQUIRE(close_status);
          });
        }
        if (!protocol_close_seen && response.find(expected_invalid_payload_close) != std::string::npos) {
          protocol_close_seen = true;
          stream.read_stop();
          timeout.close();
          stream.close([&](uv::tcp&) {
            client_closed = true;
          });
          server.close();
        }
      });
    });
  });

  loop.run();

  UVP_CHECK(!timed_out);
  UVP_CHECK(invalid_close_sent);
  UVP_CHECK(protocol_close_seen);
  UVP_CHECK(client_closed);
  UVP_CHECK(!on_close_called);
  UVP_CHECK(on_error_called);

  loop.close();
}
