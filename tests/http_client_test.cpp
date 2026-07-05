#include "test.hpp"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <uvpp/protocols/http.hpp>
#include <uvpp/uv.hpp>

#include <string>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view text) {
  auto out = std::vector<std::byte>(text.size());
  std::memcpy(out.data(), text.data(), text.size());
  return out;
}

} // namespace

UVP_TEST_CASE("http client performs a plain get request") {
  uv::loop loop;

  uvp::http::server server(loop);
  server.get("/hello", [](uvp::http::request&, uvp::http::response& res) {
    res.header("x-test", "ok");
    res.text("hello client");
  });

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp)};
  const auto port = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint()).port;
  server.listen(std::move(listener));

  uvp::http::client client(loop);
  auto completed = false;
  auto request = client.get(
    "http://127.0.0.1:" + std::to_string(port) + "/hello",
    [&](uvp::result<uvp::http::response> result) {
      completed = true;
      UVP_REQUIRE(result);
      UVP_CHECK_EQ(result.value().status_code(), 200U);
      UVP_CHECK_EQ(result.value().headers().get("x-test"), "ok");
      UVP_CHECK_EQ(result.value().body(), "hello client");
      server.close();
    });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("http client rejects unsupported schemes") {
  uv::loop loop;
  uvp::http::client client(loop);

  auto completed = false;
  auto request = client.get("https://example.com/", [&](uvp::result<uvp::http::response> result) {
    completed = true;
    UVP_CHECK(!result);
    UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_unsupported_scheme);
  });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("http client times out waiting for response headers") {
  uv::loop loop;

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp)};
  const auto port = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint()).port;
  std::optional<uvp::io::byte_stream> accepted;

  listener.listen([&](uvp::io::accept_result result) {
    UVP_REQUIRE(result);
    accepted.emplace(std::move(result).stream());
  });

  uvp::http::client client(
    loop,
    uvp::http::client_options{
      .response_header_timeout = std::chrono::milliseconds{10},
    });

  auto completed = false;
  auto request = client.get(
    "http://127.0.0.1:" + std::to_string(port) + "/slow",
    [&](uvp::result<uvp::http::response> result) {
      completed = true;
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_timeout);
      if (accepted) {
        accepted->close([&] {
          accepted.reset();
          listener.close();
        });
      } else {
        listener.close();
      }
    });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("http client times out waiting for response body") {
  uv::loop loop;

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp)};
  const auto port = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint()).port;
  std::optional<uvp::io::byte_stream> accepted;
  auto header_bytes = bytes("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n");

  listener.listen([&](uvp::io::accept_result result) {
    UVP_REQUIRE(result);
    accepted.emplace(std::move(result).stream());
    accepted->write(header_bytes, [](uvp::io::stream_error error) {
      UVP_CHECK(!error);
    });
  });

  uvp::http::client client(
    loop,
    uvp::http::client_options{
      .response_body_timeout = std::chrono::milliseconds{10},
    });

  auto completed = false;
  auto request = client.get(
    "http://127.0.0.1:" + std::to_string(port) + "/slow-body",
    [&](uvp::result<uvp::http::response> result) {
      completed = true;
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_timeout);
      if (accepted) {
        accepted->close([&] {
          accepted.reset();
          listener.close();
        });
      } else {
        listener.close();
      }
    });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}
