#include "test.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include <uvpp/protocols/http.hpp>

UVP_TEST_CASE("http status enum covers common response codes") {
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::moved_permanently), 301U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::found), 302U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::not_modified), 304U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::unauthorized), 401U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::forbidden), 403U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::request_timeout), 408U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::conflict), 409U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::unprocessable_content), 422U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::unprocessable_entity), 422U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::too_many_requests), 429U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::bad_gateway), 502U);
  UVP_CHECK_EQ(static_cast<unsigned int>(uvp::http::status::service_unavailable), 503U);

  UVP_CHECK_EQ(uvp::http::reason_phrase(uvp::http::status::moved_permanently), "Moved Permanently");
  UVP_CHECK_EQ(uvp::http::reason_phrase(uvp::http::status::unprocessable_content), "Unprocessable Content");
  UVP_CHECK_EQ(uvp::http::reason_phrase(uvp::http::status::too_many_requests), "Too Many Requests");
  UVP_CHECK_EQ(uvp::http::reason_phrase(uvp::http::status::service_unavailable), "Service Unavailable");
}

UVP_TEST_CASE("http response serializes common body helpers") {
  uvp::http::response response;
  response.json(uvp::json{{"status", "ok"}});

  UVP_CHECK(response.ended());
  UVP_CHECK_EQ(response.status_code(), static_cast<unsigned int>(uvp::http::status::ok));
  UVP_CHECK_EQ(response.headers().get("content-type"), "application/json");
  UVP_CHECK_EQ(response.body(), "{\"status\":\"ok\"}");
}

UVP_TEST_CASE("http response validates status and committed headers") {
  uvp::http::response response;

  UVP_CHECK_THROWS(response.status(99), std::invalid_argument);
  UVP_CHECK_THROWS(response.status(1000), std::invalid_argument);

  response.status(201).header("x-test", "before");
  response.end();

  UVP_CHECK(response.ended());
  UVP_CHECK_EQ(response.status_code(), 201U);
  UVP_CHECK_EQ(response.headers().get("x-test"), "before");
}

UVP_TEST_CASE("http deferred response completes the owner response") {
  uvp::http::response response;
  auto reply = response.defer();
  bool cancelled = false;
  reply.on_cancel([&] {
    cancelled = true;
  });

  UVP_CHECK(reply.active());
  UVP_CHECK(response.deferred());

  reply.status(uvp::http::status::created).text("later");

  UVP_CHECK(!reply.active());
  UVP_CHECK(!response.deferred());
  UVP_CHECK(response.ended());
  UVP_CHECK_EQ(response.status_code(), static_cast<unsigned int>(uvp::http::status::created));
  UVP_CHECK_EQ(response.body(), "later");
  UVP_CHECK(!cancelled);
}

UVP_TEST_CASE("http response prevents mixing buffered and streaming modes") {
  uvp::http::response response;
  auto stream = response.stream();

  UVP_CHECK(response.streaming());
  UVP_CHECK(stream.active());
  UVP_CHECK_THROWS(response.text("not allowed"), std::logic_error);
  UVP_CHECK_THROWS(response.json("{}"), std::logic_error);
  UVP_CHECK_THROWS(response.bytes({}), std::logic_error);
  UVP_CHECK_THROWS(response.end(), std::logic_error);
}

UVP_TEST_CASE("http streaming response rejects unattached writes") {
  uvp::http::response response;
  auto stream = response.stream();
  bool stream_cancelled = false;
  bool stream_drained = false;
  bool stream_errored = false;

  stream
    .type("application/x-ndjson")
    .on_cancel([&] {
      stream_cancelled = true;
    })
    .on_drain([&] {
      stream_drained = true;
    })
    .on_error([&](std::error_code) {
      stream_errored = true;
    });

  const auto unattached_write = stream.write(std::string{"line\n"});
  UVP_CHECK(!unattached_write.accepted());
  UVP_CHECK(!unattached_write);
  UVP_CHECK(!stream_cancelled);
  UVP_CHECK(!stream_drained);
  UVP_CHECK(!stream_errored);
}

UVP_TEST_CASE("http stream write result reports accepted backpressure and rejection") {
  const auto ready = uvp::http::stream_write_result::ready();
  UVP_CHECK(ready.accepted());
  UVP_CHECK(ready.should_continue());
  UVP_CHECK(ready);

  const auto backpressure = uvp::http::stream_write_result::backpressure();
  UVP_CHECK(backpressure.accepted());
  UVP_CHECK(!backpressure.should_continue());
  UVP_CHECK(!backpressure);

  const auto rejected = uvp::http::stream_write_result::rejected(std::make_error_code(std::errc::not_connected));
  UVP_CHECK(!rejected.accepted());
  UVP_CHECK(!rejected.should_continue());
  UVP_CHECK(!rejected);
  UVP_CHECK(rejected.error() == std::make_error_code(std::errc::not_connected));
}
