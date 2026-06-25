#include "test.hpp"

#include <chrono>
#include <cstddef>
#include <string_view>
#include <variant>

#include <uvpp/protocols/http.hpp>

using namespace std::chrono_literals;

UVP_TEST_CASE("http options expose configured limits") {
  auto options = uvp::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_body_bytes(1024 * 1024)
    .max_pending_responses_per_connection(8)
    .idle_timeout(30s)
    .server_header(false);

  UVP_CHECK_EQ(options.max_header_bytes(), 32 * 1024U);
  UVP_CHECK_EQ(options.max_body_bytes(), 1024 * 1024U);
  UVP_CHECK_EQ(options.max_pending_responses_per_connection(), 8U);
  UVP_CHECK(options.idle_timeout() == 30s);
  UVP_CHECK(!options.server_header());
}

UVP_TEST_CASE("http headers are case insensitive") {
  uvp::http::headers headers;
  headers.set("Content-Type", "text/plain");

  UVP_CHECK(headers.contains("content-type"));
  UVP_CHECK_EQ(headers.get("CONTENT-TYPE"), "text/plain");
}

UVP_TEST_CASE("http connection info preserves endpoint variants") {
  auto connection = uvp::http::connection_info{
    uvp::io::tcp_endpoint{"127.0.0.1", 8080},
    uvp::io::pipe_endpoint{"/tmp/uvpp.sock"},
  };

  UVP_CHECK(std::holds_alternative<uvp::io::tcp_endpoint>(connection.local_endpoint()));
  UVP_CHECK(std::holds_alternative<uvp::io::pipe_endpoint>(connection.remote_endpoint()));
}

UVP_TEST_CASE("http request exposes raw and parsed query parameters") {
  auto connection = uvp::http::connection_info{
    uvp::io::tcp_endpoint{"127.0.0.1", 8080},
    uvp::io::tcp_endpoint{"127.0.0.1", 4242},
  };
  uvp::http::request request(
    uvp::http::method::get,
    "/search?tag=cxx&tag=networking&q=&encoded=a+b%2Fc&bad=%zz",
    "/search",
    "tag=cxx&tag=networking&q=&encoded=a+b%2Fc&bad=%zz",
    uvp::http::headers{},
    {},
    uvp::http::route_params{},
    connection);

  UVP_CHECK_EQ(request.query(), "tag=cxx&tag=networking&q=&encoded=a+b%2Fc&bad=%zz");
  const auto first_tag = request.query("tag");
  UVP_REQUIRE(first_tag);
  UVP_CHECK_EQ(*first_tag, "cxx");

  const auto tags = request.query_all("tag");
  UVP_REQUIRE(tags.size() == 2);
  UVP_CHECK_EQ(tags[0], "cxx");
  UVP_CHECK_EQ(tags[1], "networking");

  const auto q = request.query("q");
  UVP_REQUIRE(q);
  UVP_CHECK(q->empty());
  UVP_CHECK(!request.query("missing"));
  UVP_CHECK_EQ(request.query_or("missing", "fallback"), "fallback");
  UVP_CHECK_EQ(request.query_or("encoded"), "a b/c");
  UVP_CHECK_EQ(request.query_or("bad"), "%zz");
  UVP_CHECK(request.query_params().contains("tag"));
}

UVP_TEST_CASE("http request exposes borrowed body views") {
  auto body = std::vector<std::byte>{
    std::byte{'t'},
    std::byte{'e'},
    std::byte{'s'},
    std::byte{'t'},
  };
  uvp::http::request request(
    uvp::http::method::post,
    "/submit",
    "/submit",
    "",
    uvp::http::headers{},
    std::move(body),
    uvp::http::route_params{},
    uvp::http::connection_info{});

  UVP_CHECK_EQ(request.body(), "test");
  UVP_CHECK_EQ(request.body_bytes().size(), 4U);
}
