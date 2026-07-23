#include "test.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "src/http/detail/http1_state_machine.hpp"

namespace {

struct event_collector {
  bool operator()(const uvp::http::detail::http1_event& event) {
    event_types.push_back(event.event_type());
    switch (event.event_type()) {
    case uvp::http::detail::http1_event::type::headers:
      current = event.message();
      current.body.clear();
      headers.push_back(event.message());
      break;
    case uvp::http::detail::http1_event::type::body:
      body_chunks.emplace_back(event.body());
      current.body.append(event.body());
      break;
    case uvp::http::detail::http1_event::type::complete:
      current.keep_alive = event.message().keep_alive;
      current.upgrade = event.message().upgrade;
      messages.push_back(current);
      break;
    }
    return true;
  }

  std::vector<uvp::http::detail::http1_event::type> event_types;
  std::vector<std::string> body_chunks;
  std::vector<uvp::http::detail::http1_message> headers;
  std::vector<uvp::http::detail::http1_message> messages;
  uvp::http::detail::http1_message current;
};

} // namespace

UVP_TEST_CASE("http1 parser parses a complete request") {
  uvp::http::detail::http1_state_machine parser;
  event_collector collector;
  const auto result = parser.parse(
    "POST /config?dry_run=1 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "test",
    [&collector](const auto& event) { return collector(event); });

  UVP_REQUIRE(result.ok());
  UVP_REQUIRE(collector.messages.size() == 1);

  const auto& message = collector.messages.front();
  UVP_CHECK(message.method == uvp::http::method::post);
  UVP_CHECK_EQ(message.target, "/config?dry_run=1");
  UVP_CHECK_EQ(message.headers.get("host"), "example.test");
  UVP_CHECK_EQ(message.headers.get("content-type"), "text/plain");
  UVP_CHECK_EQ(message.body, "test");
  UVP_CHECK_EQ(message.http_major, 1U);
  UVP_CHECK_EQ(message.http_minor, 1U);
  UVP_CHECK(message.keep_alive);
}

UVP_TEST_CASE("http1 parser handles requests split across multiple parses") {
  uvp::http::detail::http1_state_machine parser;
  event_collector collector;
  const auto on_event = [&collector](const auto& event) { return collector(event); };

  UVP_CHECK(parser.parse("POST /items HTTP/1.1\r\nHo", on_event).ok());
  UVP_CHECK(parser.parse("st: example.test\r\nContent-Length: 5\r\n\r\nhe", on_event).ok());
  UVP_CHECK(parser.parse("llo", on_event).ok());

  UVP_REQUIRE(collector.messages.size() == 1);
  const auto& message = collector.messages.front();
  UVP_CHECK(message.method == uvp::http::method::post);
  UVP_CHECK_EQ(message.target, "/items");
  UVP_CHECK_EQ(message.headers.get("host"), "example.test");
  UVP_CHECK_EQ(message.body, "hello");
}

UVP_TEST_CASE("http1 parser emits header body and complete events inline") {
  uvp::http::detail::http1_state_machine parser;
  event_collector collector;
  const auto result = parser.parse(
    "POST /events HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "test",
    [&collector](const auto& event) { return collector(event); });

  UVP_REQUIRE(result.ok());
  UVP_REQUIRE(collector.event_types.size() == 3);
  UVP_CHECK(collector.event_types[0] == uvp::http::detail::http1_event::type::headers);
  UVP_CHECK(collector.event_types[1] == uvp::http::detail::http1_event::type::body);
  UVP_CHECK_EQ(collector.body_chunks[0], "test");
  UVP_CHECK(collector.event_types[2] == uvp::http::detail::http1_event::type::complete);
}

UVP_TEST_CASE("http1 parser pauses and resumes body delivery without retaining the input") {
  uvp::http::detail::http1_state_machine parser;
  std::string body;
  std::size_t complete_events = 0;
  bool pause_on_body = true;
  const auto on_event = [&](const uvp::http::detail::http1_event& event) {
    if (event.event_type() == uvp::http::detail::http1_event::type::body) {
      body.append(event.body());
      if (pause_on_body) {
        pause_on_body = false;
        return false;
      }
    } else if (event.event_type() == uvp::http::detail::http1_event::type::complete) {
      ++complete_events;
    }
    return true;
  };
  const std::string input =
    "POST /pause HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "test"
    "GET /next HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "\r\n";

  const auto paused = parser.parse(input, on_event);
  UVP_CHECK(paused.code == uvp::http::detail::http1_parse_result::status::paused);
  UVP_CHECK_EQ(body, "test");
  UVP_CHECK_EQ(complete_events, 0U);
  UVP_CHECK(paused.parsed_bytes < input.size());

  parser.resume();
  const auto resumed = parser.parse(std::string_view{input}.substr(paused.parsed_bytes), on_event);
  UVP_CHECK(resumed.ok());
  UVP_CHECK_EQ(body, "test");
  UVP_CHECK_EQ(complete_events, 2U);
}

UVP_TEST_CASE("http1 parser decodes chunked request bodies") {
  uvp::http::detail::http1_state_machine parser;
  event_collector collector;
  const auto result = parser.parse(
    "POST /chunked HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "4\r\nWiki\r\n"
    "5\r\npedia\r\n"
    "0\r\n\r\n",
    [&collector](const auto& event) { return collector(event); });

  UVP_REQUIRE(result.ok());
  UVP_REQUIRE(collector.messages.size() == 1);
  UVP_CHECK_EQ(collector.messages.front().body, "Wikipedia");
}

UVP_TEST_CASE("http1 parser reports malformed requests") {
  uvp::http::detail::http1_state_machine parser;
  const auto result = parser.parse(
    "GET / HTTP/1.1\r\nBad Header\r\n\r\n",
    [](const auto&) { return true; });

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
      "\r\n",
      [](const auto&) { return true; });

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
      "\r\n",
      [](const auto&) { return true; });

    UVP_CHECK(result.code == uvp::http::detail::http1_parse_result::status::error);
    UVP_CHECK_EQ(result.error, "request has too many headers");
  }
}

UVP_TEST_CASE("http1 parser reports websocket upgrade pause") {
  uvp::http::detail::http1_state_machine parser;
  event_collector collector;
  const auto result = parser.parse(
    "GET /echo HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\nextra",
    [&collector](const auto& event) { return collector(event); });

  UVP_CHECK(result.code == uvp::http::detail::http1_parse_result::status::upgrade);
  UVP_REQUIRE(collector.headers.size() == 1);
  UVP_CHECK(collector.headers.front().upgrade);
  UVP_REQUIRE(collector.event_types.size() == 1);
  UVP_CHECK(collector.event_types.front() == uvp::http::detail::http1_event::type::headers);
  UVP_CHECK(result.parsed_bytes < std::string_view{
    "GET /echo HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\nextra"}.size());
}
