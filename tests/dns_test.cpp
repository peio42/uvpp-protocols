#include "test.hpp"

#include <uvpp/protocols/dns.hpp>
#include <uvpp/uv.hpp>

UVP_TEST_CASE("dns resolver resolves localhost service") {
  uv::loop loop;
  uvp::dns::resolver resolver(loop);

  auto completed = false;
  auto saw_loopback = false;

  auto op = resolver.resolve(
    uvp::dns::query{}.host("localhost").service("80"),
    [&](uvp::result<uvp::dns::address_list> result) {
      completed = true;
      UVP_REQUIRE(result);
      UVP_CHECK(!result.value().empty());
      for (const auto& address : result.value()) {
        UVP_CHECK(address.port() == 80U);
        if (address.host() == "127.0.0.1" || address.host() == "::1") {
          saw_loopback = true;
        }
      }
    });

  UVP_CHECK(op.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
  UVP_CHECK(saw_loopback);
}

UVP_TEST_CASE("dns resolver supports numeric port and ipv4 family") {
  uv::loop loop;
  uvp::dns::resolver resolver(loop);

  auto completed = false;

  auto op = resolver.resolve(
    uvp::dns::query{}.host("127.0.0.1").port(443).family(uvp::dns::address_family::ipv4),
    [&](uvp::result<uvp::dns::address_list> result) {
      completed = true;
      UVP_REQUIRE(result);
      UVP_REQUIRE(!result.value().empty());
      UVP_CHECK_EQ(result.value()[0].family(), uvp::dns::address_family::ipv4);
      UVP_CHECK_EQ(result.value()[0].host(), "127.0.0.1");
      UVP_CHECK_EQ(result.value()[0].port(), 443U);
    });

  UVP_CHECK(op.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("dns resolver rejects invalid queries") {
  uv::loop loop;
  uvp::dns::resolver resolver(loop);

  auto completed = false;
  auto op = resolver.resolve(uvp::dns::query{}.host("localhost"), [&](uvp::result<uvp::dns::address_list> result) {
    completed = true;
    UVP_CHECK(!result);
    UVP_CHECK_EQ(result.error().code, uvp::dns::errc::invalid_query);
  });

  UVP_CHECK(op.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("dns resolve operation cancel completes once") {
  uv::loop loop;
  uvp::dns::resolver resolver(loop);

  auto completions = 0;
  auto op = resolver.resolve(
    uvp::dns::query{}.host("localhost").service("80"),
    [&](uvp::result<uvp::dns::address_list> result) {
      ++completions;
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::dns::errc::cancelled);
    });

  op.cancel();
  loop.run();
  loop.close();

  UVP_CHECK_EQ(completions, 1);
}
