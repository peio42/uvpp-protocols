#include "test.hpp"

#include <uvpp/protocols/url.hpp>

#include <system_error>

UVP_TEST_CASE("url parses http origin with default path and port") {
  auto parsed = uvp::parse_url("http://example.com");

  UVP_REQUIRE(parsed);
  UVP_CHECK_EQ(parsed.value().href(), "http://example.com/");
  UVP_CHECK_EQ(parsed.value().scheme(), "http");
  UVP_CHECK_EQ(parsed.value().host(), "example.com");
  UVP_CHECK_EQ(parsed.value().hostname(), "example.com");
  UVP_CHECK_EQ(parsed.value().path(), "/");
  UVP_CHECK(!parsed.value().has_port());
  UVP_CHECK_EQ(uvp::scheme_id(parsed.value()), uvp::url_scheme::http);
  UVP_CHECK_EQ(uvp::effective_port(parsed.value()).value(), 80);
  UVP_CHECK_EQ(uvp::origin_form_target(parsed.value()), "/");
}

UVP_TEST_CASE("url builds http request target without fragment") {
  auto parsed = uvp::parse_url("https://example.com:8443/a/b?q=1#frag");

  UVP_REQUIRE(parsed);
  UVP_CHECK_EQ(parsed.value().scheme(), "https");
  UVP_CHECK_EQ(parsed.value().port(), "8443");
  UVP_CHECK_EQ(parsed.value().path(), "/a/b");
  UVP_CHECK_EQ(parsed.value().query(), "q=1");
  UVP_CHECK_EQ(parsed.value().fragment(), "frag");
  UVP_CHECK_EQ(uvp::effective_port(parsed.value()).value(), 8443);
  UVP_CHECK_EQ(uvp::origin_form_target(parsed.value()), "/a/b?q=1");
  UVP_CHECK_EQ(uvp::absolute_form_target(parsed.value()), "https://example.com:8443/a/b?q=1");
}

UVP_TEST_CASE("url rejects invalid and missing host inputs") {
  auto missing_scheme = uvp::parse_url("example.com/path");
  UVP_CHECK(!missing_scheme);
  UVP_CHECK_EQ(missing_scheme.error().code, uvp::url_errc::missing_scheme);

  auto missing_host = uvp::parse_url("https:///path");
  UVP_CHECK(!missing_host);
  UVP_CHECK_EQ(missing_host.error().code, uvp::url_errc::missing_host);

  auto invalid = uvp::parse_url("http://exa mple.com");
  UVP_CHECK(!invalid);
  UVP_CHECK_EQ(invalid.error().code, uvp::url_errc::invalid);
}

UVP_TEST_CASE("url classifies known and unknown schemes") {
  auto ws = uvp::parse_url("ws://example.com/socket");
  auto other = uvp::parse_url("custom://example.com/resource");

  UVP_REQUIRE(ws);
  UVP_REQUIRE(other);
  UVP_CHECK_EQ(uvp::scheme_id(ws.value()), uvp::url_scheme::ws);
  UVP_CHECK_EQ(uvp::effective_port(ws.value()).value(), 80);
  UVP_CHECK_EQ(uvp::scheme_id(other.value()), uvp::url_scheme::other);
  UVP_CHECK(!uvp::effective_port(other.value()));
}

UVP_TEST_CASE("url handles ipv6 literals and authority endpoints") {
  auto parsed = uvp::parse_url("https://[2001:db8::1]:9443/path");

  UVP_REQUIRE(parsed);
  UVP_CHECK_EQ(parsed.value().host(), "[2001:db8::1]");
  UVP_CHECK_EQ(parsed.value().hostname(), "2001:db8::1");
  UVP_CHECK(uvp::host_is_ip_literal(parsed.value()));

  auto endpoint = uvp::authority_endpoint(parsed.value());
  UVP_REQUIRE(endpoint);
  UVP_CHECK_EQ(endpoint.value().host, "2001:db8::1");
  UVP_CHECK_EQ(endpoint.value().port, 9443);
  UVP_CHECK(endpoint.value().host_is_ip_literal);
}

UVP_TEST_CASE("url preserves credentials but excludes them from origin") {
  auto parsed = uvp::parse_url("https://user:pass@example.com:443/private");

  UVP_REQUIRE(parsed);
  UVP_CHECK(parsed.value().has_credentials());
  UVP_CHECK_EQ(parsed.value().username(), "user");
  UVP_CHECK_EQ(parsed.value().password(), "pass");

  auto origin = uvp::origin_from_url(parsed.value());
  UVP_REQUIRE(origin);
  UVP_CHECK_EQ(origin.value().scheme, uvp::url_scheme::https);
  UVP_CHECK_EQ(origin.value().hostname, "example.com");
  UVP_CHECK_EQ(origin.value().port, 443);
}

UVP_TEST_CASE("url resolves simple relative references against a base") {
  auto absolute = uvp::parse_url("users/42?active=1", "https://api.example.com/v1/index.html");
  auto root_relative = uvp::parse_url("/health", "https://api.example.com/v1/index.html");

  UVP_REQUIRE(absolute);
  UVP_REQUIRE(root_relative);
  UVP_CHECK_EQ(absolute.value().href(), "https://api.example.com/v1/users/42?active=1");
  UVP_CHECK_EQ(root_relative.value().href(), "https://api.example.com/health");
}
