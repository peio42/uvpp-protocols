#include "test.hpp"

#include <variant>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

UVP_TEST_CASE("io tcp listener exposes selected ephemeral port") {
  uv::loop loop;
  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto endpoint = listener.local_endpoint();

  UVP_REQUIRE(std::holds_alternative<uvp::io::tcp_endpoint>(endpoint));
  UVP_CHECK(std::get<uvp::io::tcp_endpoint>(endpoint).port != 0);

  listener.close();
  loop.run();
  loop.close();
}

