#include "test.hpp"

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

using namespace std::chrono_literals;

UVP_TEST_CASE("http options expose configured limits") {
  auto options = uvp::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_header_count(64)
    .max_body_bytes(1024 * 1024)
    .max_pending_responses_per_connection(8)
    .idle_timeout(30s)
    .route_path_matching(uvp::http::route_path_matching::raw)
    .server_header(false);

  UVP_CHECK_EQ(options.max_header_bytes(), 32 * 1024U);
  UVP_CHECK_EQ(options.max_header_count(), 64U);
  UVP_CHECK_EQ(options.max_body_bytes(), 1024 * 1024U);
  UVP_CHECK_EQ(options.max_pending_responses_per_connection(), 8U);
  UVP_CHECK(options.idle_timeout() == 30s);
  UVP_CHECK(options.route_path_matching() == uvp::http::route_path_matching::raw);
  UVP_CHECK(!options.server_header());
}

UVP_TEST_CASE("http server options reject zero body limit") {
  UVP_CHECK_THROWS(uvp::http::server_options{}.max_body_bytes(0), std::invalid_argument);
  UVP_CHECK_THROWS(uvp::http::server_options{}.max_header_count(0), std::invalid_argument);
}

UVP_TEST_CASE("http headers are case insensitive") {
  uvp::http::headers headers;
  headers.set("Content-Type", "text/plain");

  UVP_CHECK(headers.contains("content-type"));
  UVP_CHECK_EQ(headers.get("CONTENT-TYPE"), "text/plain");
  UVP_CHECK(uvp::http::headers::names_equal("Content-Length", "content-length"));
  UVP_CHECK(!uvp::http::headers::names_equal("Content-Length", "content-type"));
}

UVP_TEST_CASE("http headers reject invalid outbound syntax") {
  uvp::http::headers headers;
  headers.add("x-safe", "value");

  UVP_CHECK_THROWS(headers.set("bad name", "value"), std::invalid_argument);
  UVP_CHECK_THROWS(headers.add("x-injected", "value\r\nx-injected: yes"), std::invalid_argument);
  UVP_CHECK_THROWS(headers.add("x-injected", "value\nnext"), std::invalid_argument);
  const auto nul_value = std::string{"value\0next", 10};
  UVP_CHECK_THROWS(headers.add("x-injected", nul_value), std::invalid_argument);

  UVP_CHECK_EQ(headers.size(), 1U);
  UVP_CHECK_EQ(headers.get("x-safe"), "value");
  UVP_CHECK(uvp::http::headers::is_valid_name("x-token_123"));
  UVP_CHECK(!uvp::http::headers::is_valid_name("x token"));
  UVP_CHECK(uvp::http::headers::is_valid_value("safe value"));
  UVP_CHECK(!uvp::http::headers::is_valid_value(nul_value));
}

UVP_TEST_CASE("http client rejects injected streaming headers and proxy authorization") {
  uv::loop loop;
  auto options = uvp::http::client_options{};
  options.proxy.url = "http://127.0.0.1:8080";
  options.proxy.authorization = "Basic safe\r\nX-Injected: yes";
  uvp::http::client client(loop, std::move(options));

  auto request = client.request(uvp::http::method::get, "http://example.test/");
  UVP_CHECK_THROWS(request.header("x-test", "value\r\nX-Injected: yes"), std::invalid_argument);

  auto completed = false;
  auto operation = client.get("http://example.test/", [&](uvp::result<uvp::http::response> result) {
    completed = true;
    UVP_CHECK(!result);
    UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_proxy_failed);
  });
  UVP_CHECK(operation.valid());
  UVP_CHECK(completed);
  loop.close();
}

UVP_TEST_CASE("http static file cache control rejects header injection") {
  UVP_CHECK_THROWS(
    uvp::http::static_file_options{}.cache_control("public\r\nX-Injected: yes"),
    std::invalid_argument);
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
    connection,
    std::vector<std::string>{"search"});

  UVP_CHECK_EQ(request.query(), "tag=cxx&tag=networking&q=&encoded=a+b%2Fc&bad=%zz");
  const auto path_segments = request.decoded_path_segments();
  UVP_REQUIRE(path_segments.size() == 1U);
  UVP_CHECK_EQ(path_segments[0], "search");
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

UVP_TEST_CASE("http server exposes explicit convenience methods for every common verb") {
  uv::loop loop;
  uvp::http::server server(loop);

  server
    .get("/verbs/get", [](uvp::http::request&, uvp::http::response&) {})
    .post("/verbs/post", [](uvp::http::request&, uvp::http::response&) {})
    .put("/verbs/put", [](uvp::http::request&, uvp::http::response&) {})
    .patch("/verbs/patch", [](uvp::http::request&, uvp::http::response&) {})
    .delete_("/verbs/delete", [](uvp::http::request&, uvp::http::response&) {})
    .head("/verbs/head", [](uvp::http::request&, uvp::http::response&) {})
    .options("/verbs/options", [](uvp::http::request&, uvp::http::response&) {});

  UVP_CHECK(server.routes().find(uvp::http::method::get, "/verbs/get") != nullptr);
  UVP_CHECK(server.routes().find(uvp::http::method::post, "/verbs/post") != nullptr);
  UVP_CHECK(server.routes().find(uvp::http::method::put, "/verbs/put") != nullptr);
  UVP_CHECK(server.routes().find(uvp::http::method::patch, "/verbs/patch") != nullptr);
  UVP_CHECK(server.routes().find(uvp::http::method::delete_, "/verbs/delete") != nullptr);
  UVP_CHECK(server.routes().find(uvp::http::method::head, "/verbs/head") != nullptr);
  UVP_CHECK(server.routes().find(uvp::http::method::options, "/verbs/options") != nullptr);
}
