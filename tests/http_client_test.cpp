#include "test.hpp"

#include <uvpp/protocols/http.hpp>
#include <uvpp/uv.hpp>

#include <string>
#include <variant>

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
