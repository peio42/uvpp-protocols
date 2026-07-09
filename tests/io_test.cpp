#include "test.hpp"

#include <optional>
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

UVP_TEST_CASE("io tcp connector connects to tcp listener") {
  uv::loop loop;
  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto endpoint = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint());

  std::optional<uvp::io::byte_stream> accepted_stream;
  std::optional<uvp::io::byte_stream> connected_stream;
  auto accepted = false;
  auto connected = false;
  auto connect_completions = 0;
  auto accepted_closed = false;
  auto connected_closed = false;

  auto maybe_close_listener = [&]() {
    if (accepted_closed && connected_closed) {
      listener.close();
    }
  };

  listener.listen([&](uvp::io::accept_result result) {
    accepted = true;
    UVP_REQUIRE(result);
    accepted_stream.emplace(std::move(result).stream());
    accepted_stream->close([&]() {
      accepted_closed = true;
      accepted_stream.reset();
      maybe_close_listener();
    });
  });

  uvp::io::tcp_connector connector(loop);
  auto op = connector.connect(endpoint, [&](uvp::result<uvp::io::byte_stream> result) {
    ++connect_completions;
    connected = true;
    UVP_REQUIRE(result);
    connected_stream.emplace(std::move(result).value());
    connected_stream->close([&]() {
      connected_closed = true;
      connected_stream.reset();
      maybe_close_listener();
    });
  });

  UVP_CHECK(op.valid());
  loop.run();
  op.cancel();
  loop.close();

  UVP_CHECK(accepted);
  UVP_CHECK(connected);
  UVP_CHECK_EQ(connect_completions, 1);
}

UVP_TEST_CASE("io byte stream forwards handle reference controls") {
  uv::loop loop;
  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto endpoint = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint());

  std::optional<uvp::io::byte_stream> accepted_stream;
  std::optional<uvp::io::byte_stream> connected_stream;
  auto accepted_closed = false;
  auto connected_closed = false;

  auto maybe_close_listener = [&]() {
    if (accepted_closed && connected_closed) {
      listener.close();
    }
  };

  listener.listen([&](uvp::io::accept_result result) {
    UVP_REQUIRE(result);
    accepted_stream.emplace(std::move(result).stream());
    accepted_stream->close([&]() {
      accepted_closed = true;
      accepted_stream.reset();
      maybe_close_listener();
    });
  });

  uvp::io::tcp_connector connector(loop);
  auto op = connector.connect(endpoint, [&](uvp::result<uvp::io::byte_stream> result) {
    UVP_REQUIRE(result);
    connected_stream.emplace(std::move(result).value());

    UVP_CHECK(connected_stream->has_ref());
    connected_stream->unref();
    UVP_CHECK(!connected_stream->has_ref());
    connected_stream->ref();
    UVP_CHECK(connected_stream->has_ref());

    uvp::io::byte_stream invalid;
    invalid.unref();
    invalid.ref();
    UVP_CHECK(!invalid.has_ref());

    connected_stream->close([&]() {
      connected_closed = true;
      connected_stream.reset();
      maybe_close_listener();
    });
  });

  UVP_CHECK(op.valid());
  loop.run();
  loop.close();

  UVP_CHECK(accepted_closed);
  UVP_CHECK(connected_closed);
}

UVP_TEST_CASE("io tcp connector rejects empty address list") {
  uv::loop loop;
  uvp::io::tcp_connector connector(loop);

  auto completed = false;
  auto completions = 0;
  auto op = connector.connect(uvp::dns::address_list{}, [&](uvp::result<uvp::io::byte_stream> result) {
    ++completions;
    completed = true;
    UVP_CHECK(!result);
    UVP_CHECK_EQ(result.error().code, uvp::io::connect_errc::no_addresses);
  });

  UVP_CHECK(op.valid());
  op.cancel();
  loop.run();
  loop.close();

  UVP_CHECK(completed);
  UVP_CHECK_EQ(completions, 1);
}
