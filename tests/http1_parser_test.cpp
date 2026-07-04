#include "test.hpp"

#include <string_view>

#include "src/http/detail/http1_state_machine.hpp"

UVP_TEST_CASE("http1 parser parses a complete request") {
  uvp::http::detail::http1_state_machine parser;
  const auto result = parser.parse(
    "POST /config?dry_run=1 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "test");

  UVP_REQUIRE(result.ok());
  UVP_REQUIRE(parser.completed_messages().size() == 1);

  const auto& message = parser.completed_messages().front();
  UVP_CHECK(message.method == uvp::http::method::post);
  UVP_CHECK_EQ(message.target, "/config?dry_run=1");
  UVP_CHECK_EQ(message.headers.get("host"), "example.test");
  UVP_CHECK_EQ(message.headers.get("content-type"), "text/plain");
  UVP_CHECK_EQ(message.body, "test");
  UVP_CHECK_EQ(message.http_major, 1U);
  UVP_CHECK_EQ(message.http_minor, 1U);
}

UVP_TEST_CASE("http1 parser handles requests split across multiple parses") {
  uvp::http::detail::http1_state_machine parser;

  UVP_CHECK(parser.parse("POST /items HTTP/1.1\r\nHo").ok());
  UVP_CHECK(parser.parse("st: example.test\r\nContent-Length: 5\r\n\r\nhe").ok());
  UVP_CHECK(parser.parse("llo").ok());

  UVP_REQUIRE(parser.completed_messages().size() == 1);
  const auto& message = parser.completed_messages().front();
  UVP_CHECK(message.method == uvp::http::method::post);
  UVP_CHECK_EQ(message.target, "/items");
  UVP_CHECK_EQ(message.headers.get("host"), "example.test");
  UVP_CHECK_EQ(message.body, "hello");
}

UVP_TEST_CASE("http1 parser emits header body and complete events") {
  uvp::http::detail::http1_state_machine parser;
  const auto result = parser.parse(
    "POST /events HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "test");

  UVP_REQUIRE(result.ok());
  UVP_REQUIRE(parser.events().size() == 3);
  UVP_CHECK(parser.events()[0].event_type() == uvp::http::detail::http1_event::type::headers);
  UVP_CHECK(parser.events()[1].event_type() == uvp::http::detail::http1_event::type::body);
  UVP_CHECK_EQ(parser.events()[1].body(), "test");
  UVP_CHECK(parser.events()[2].event_type() == uvp::http::detail::http1_event::type::complete);
}

UVP_TEST_CASE("http1 parser decodes chunked request bodies") {
  uvp::http::detail::http1_state_machine parser;
  const auto result = parser.parse(
    "POST /chunked HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "4\r\nWiki\r\n"
    "5\r\npedia\r\n"
    "0\r\n\r\n");

  UVP_REQUIRE(result.ok());
  UVP_REQUIRE(parser.completed_messages().size() == 1);
  UVP_CHECK_EQ(parser.completed_messages().front().body, "Wikipedia");
}

UVP_TEST_CASE("http1 parser reports malformed requests") {
  uvp::http::detail::http1_state_machine parser;
  const auto result = parser.parse("GET / HTTP/1.1\r\nBad Header\r\n\r\n");

  UVP_CHECK(result.code == uvp::http::detail::http1_parse_result::status::error);
  UVP_CHECK(!result.error.empty());
}

UVP_TEST_CASE("http1 parser enforces request header limits") {
  {
    uvp::http::detail::http1_state_machine parser;
    parser.limits(uvp::http::detail::http1_limits{15, 128});
    const auto result = parser.parse(
      "GET / HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "\r\n");

    UVP_CHECK(result.code == uvp::http::detail::http1_parse_result::status::error);
    UVP_CHECK_EQ(result.error, "request headers are too large");
  }

  {
    uvp::http::detail::http1_state_machine parser;
    parser.limits(uvp::http::detail::http1_limits{16 * 1024, 1});
    const auto result = parser.parse(
      "GET / HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "X-Test: ok\r\n"
      "\r\n");

    UVP_CHECK(result.code == uvp::http::detail::http1_parse_result::status::error);
    UVP_CHECK_EQ(result.error, "request has too many headers");
  }
}

UVP_TEST_CASE("http1 parser reports websocket upgrade pause") {
  uvp::http::detail::http1_state_machine parser;
  const auto result = parser.parse(
    "GET /echo HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\nextra");

  UVP_CHECK(result.code == uvp::http::detail::http1_parse_result::status::upgrade);
  UVP_REQUIRE(parser.completed_messages().size() == 1);
  UVP_CHECK(parser.completed_messages().front().upgrade);
  UVP_CHECK(result.parsed_bytes < std::string_view{
    "GET /echo HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\nextra"}.size());
}
