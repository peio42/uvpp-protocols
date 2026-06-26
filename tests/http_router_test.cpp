#include "test.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>

#include <uvpp/protocols/http.hpp>

UVP_TEST_CASE("http router finds routes by method and path") {
  uvp::http::router router;
  router.get("/health", [](uvp::http::request&, uvp::http::response& res) {
    res.text("ok");
  });

  UVP_CHECK_EQ(router.size(), 1U);
  UVP_CHECK(router.find(uvp::http::method::get, "/health") != nullptr);
  UVP_CHECK(router.find(uvp::http::method::post, "/health") == nullptr);
  UVP_CHECK(!router.match(uvp::http::method::post, "/health"));
  UVP_CHECK(!router.match(uvp::http::method::get, "/missing"));
}

UVP_TEST_CASE("http router captures parameters and wildcards") {
  uvp::http::router router;
  router.get("/users/:id", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/static/*path", [](uvp::http::request&, uvp::http::response&) {});

  auto user_match = router.match(uvp::http::method::get, "/users/alice");
  UVP_REQUIRE(user_match);
  UVP_CHECK_EQ(user_match.params.get("id"), "alice");

  auto static_match = router.match(uvp::http::method::get, "/static/css/app.css");
  UVP_REQUIRE(static_match);
  UVP_CHECK_EQ(static_match.params.get("path"), "css/app.css");

  auto empty_wildcard_match = router.match(uvp::http::method::get, "/static");
  UVP_REQUIRE(empty_wildcard_match);
  UVP_CHECK_EQ(empty_wildcard_match.params.get("path"), "");
}

UVP_TEST_CASE("http router prefers static routes over parameters and wildcards") {
  uvp::http::router router;
  router.get("/priority/:id", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/priority/*path", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/priority/me", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/priority/me/details", [](uvp::http::request&, uvp::http::response&) {});

  auto priority_static_match = router.match(uvp::http::method::get, "/priority/me");
  UVP_REQUIRE(priority_static_match);
  UVP_CHECK(priority_static_match.params.get("id").empty());
  UVP_CHECK(priority_static_match.params.get("path").empty());

  auto priority_param_match = router.match(uvp::http::method::get, "/priority/you");
  UVP_REQUIRE(priority_param_match);
  UVP_CHECK_EQ(priority_param_match.params.get("id"), "you");

  auto priority_static_deep_match = router.match(uvp::http::method::get, "/priority/me/details");
  UVP_REQUIRE(priority_static_deep_match);
  UVP_CHECK(priority_static_deep_match.params.get("path").empty());

  auto priority_wildcard_match = router.match(uvp::http::method::get, "/priority/me/extra");
  UVP_REQUIRE(priority_wildcard_match);
  UVP_CHECK_EQ(priority_wildcard_match.params.get("path"), "me/extra");
}

UVP_TEST_CASE("http router rejects ambiguous or invalid patterns") {
  uvp::http::router router;
  router.get("/health", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/priority/:id", [](uvp::http::request&, uvp::http::response&) {});

  UVP_CHECK_THROWS(router.get("/health", [](uvp::http::request&, uvp::http::response&) {}), std::invalid_argument);
  UVP_CHECK_THROWS(router.get("/priority/:name", [](uvp::http::request&, uvp::http::response&) {}), std::invalid_argument);
  UVP_CHECK_THROWS(router.get("/assets/*", [](uvp::http::request&, uvp::http::response&) {}), std::invalid_argument);
  UVP_CHECK_THROWS(router.get("/assets/*path/thumb", [](uvp::http::request&, uvp::http::response&) {}), std::invalid_argument);
  UVP_CHECK_THROWS(router.get("/empty/:", [](uvp::http::request&, uvp::http::response&) {}), std::invalid_argument);
}

UVP_TEST_CASE("http router infers body policies from handler signatures") {
  uvp::http::router router;

  router.post("/echo", [](uvp::http::request&, uvp::http::response&, std::span<const std::byte>) {});
  auto bytes_match = router.match(uvp::http::method::post, "/echo");
  UVP_REQUIRE(bytes_match);
  UVP_CHECK(bytes_match.body == uvp::http::detail::body_mode::bytes);

  router.post("/message", [](uvp::http::request&, uvp::http::response&, std::string_view) {});
  auto text_match = router.match(uvp::http::method::post, "/message");
  UVP_REQUIRE(text_match);
  UVP_CHECK(text_match.body == uvp::http::detail::body_mode::text);

  router.post("/upload", [](uvp::http::request&, uvp::http::response&, uvp::http::request_body_stream&) {});
  auto stream_match = router.match(uvp::http::method::post, "/upload");
  UVP_REQUIRE(stream_match);
  UVP_CHECK(stream_match.body == uvp::http::detail::body_mode::stream);

  router.post("/empty", uvp::http::body::none{}, [](uvp::http::request&, uvp::http::response&) {});
  auto none_match = router.match(uvp::http::method::post, "/empty");
  UVP_REQUIRE(none_match);
  UVP_CHECK(none_match.body == uvp::http::detail::body_mode::none);
}

UVP_TEST_CASE("http router exposes explicit convenience methods for every common verb") {
  uvp::http::router router;

  router.put("/resource", [](uvp::http::request&, uvp::http::response&) {});
  router.patch("/resource", [](uvp::http::request&, uvp::http::response&) {});
  router.delete_("/resource", [](uvp::http::request&, uvp::http::response&) {});
  router.head("/resource", [](uvp::http::request&, uvp::http::response&) {});
  router.options("/resource", [](uvp::http::request&, uvp::http::response&) {});

  UVP_CHECK(router.find(uvp::http::method::put, "/resource") != nullptr);
  UVP_CHECK(router.find(uvp::http::method::patch, "/resource") != nullptr);
  UVP_CHECK(router.find(uvp::http::method::delete_, "/resource") != nullptr);
  UVP_CHECK(router.find(uvp::http::method::head, "/resource") != nullptr);
  UVP_CHECK(router.find(uvp::http::method::options, "/resource") != nullptr);
}
