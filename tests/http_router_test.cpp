#include "test.hpp"

#include <chrono>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
  UVP_CHECK_EQ(user_match.pattern, "/users/:id");
  UVP_CHECK_EQ(user_match.params.get("id"), "alice");

  auto static_match = router.match(uvp::http::method::get, "/static/css/app.css");
  UVP_REQUIRE(static_match);
  UVP_CHECK_EQ(static_match.pattern, "/static/*path");
  UVP_CHECK_EQ(static_match.params.get("path"), "css/app.css");

  auto empty_wildcard_match = router.match(uvp::http::method::get, "/static");
  UVP_REQUIRE(empty_wildcard_match);
  UVP_CHECK_EQ(empty_wildcard_match.params.get("path"), "");
}

UVP_TEST_CASE("http router matches percent decoded path segments by default") {
  uvp::http::router router;
  router.get("/users/:id", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/files/:name", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/static/*path", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/literal/a+b", [](uvp::http::request&, uvp::http::response&) {});

  auto user_match = router.match(uvp::http::method::get, "/users/alice%20smith");
  UVP_REQUIRE(user_match);
  UVP_CHECK_EQ(user_match.params.get("id"), "alice smith");

  auto file_match = router.match(uvp::http::method::get, "/files/a%2Fb");
  UVP_REQUIRE(file_match);
  UVP_CHECK_EQ(file_match.params.get("name"), "a/b");
  UVP_CHECK(!router.match(uvp::http::method::get, "/files/a/b"));

  auto wildcard_match = router.match(uvp::http::method::get, "/static/a%2Fb/c%20d");
  UVP_REQUIRE(wildcard_match);
  UVP_CHECK_EQ(wildcard_match.params.get("path"), "a/b/c d");

  UVP_CHECK(router.match(uvp::http::method::get, "/literal/a+b"));
  UVP_CHECK(!router.match(uvp::http::method::get, "/literal/a%20b"));

  auto invalid_match = router.match(uvp::http::method::get, "/users/%zz");
  UVP_CHECK(!invalid_match);
  UVP_CHECK(invalid_match.invalid_path);
}

UVP_TEST_CASE("http router can match raw path segments while still validating encoding") {
  uvp::http::router router{uvp::http::route_path_matching::raw};
  router.get("/users/:id", [](uvp::http::request&, uvp::http::response&) {});
  router.get("/literal/alice%20smith", [](uvp::http::request&, uvp::http::response&) {});

  auto param_match = router.match(uvp::http::method::get, "/users/alice%20smith");
  UVP_REQUIRE(param_match);
  UVP_CHECK_EQ(param_match.params.get("id"), "alice%20smith");

  UVP_CHECK(router.match(uvp::http::method::get, "/literal/alice%20smith"));
  UVP_CHECK(!router.match(uvp::http::method::get, "/literal/alice smith"));

  auto invalid_match = router.match(uvp::http::method::get, "/users/%");
  UVP_CHECK(!invalid_match);
  UVP_CHECK(invalid_match.invalid_path);
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
  UVP_CHECK_THROWS(router.get("/bad/%zz", [](uvp::http::request&, uvp::http::response&) {}), std::invalid_argument);
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

UVP_TEST_CASE("http router stores explicit json body policies") {
  uvp::http::router router;
  router.post(
    "/items",
    uvp::http::body::json<int>{},
    [](uvp::http::request&, uvp::http::response&, int) {});
  router.post(
    "/raw",
    uvp::http::body::json<>{},
    [](uvp::http::request&, uvp::http::response&, const uvp::json&) {});

  auto item_match = router.match(uvp::http::method::post, "/items");
  UVP_REQUIRE(item_match);
  UVP_CHECK(item_match.body == uvp::http::detail::body_mode::json);

  auto raw_match = router.match(uvp::http::method::post, "/raw");
  UVP_REQUIRE(raw_match);
  UVP_CHECK(raw_match.body == uvp::http::detail::body_mode::json);
}

UVP_TEST_CASE("http router stores route options body limits") {
  uvp::http::router router;

  router.post(
    "/upload",
    uvp::http::route_options{}
      .max_body_bytes(32)
      .body_timeout(std::chrono::milliseconds{250}),
    uvp::http::body::stream{},
    [](uvp::http::request&, uvp::http::response&, uvp::http::request_body_stream&) {});

  auto upload_match = router.match(uvp::http::method::post, "/upload");
  UVP_REQUIRE(upload_match);
  UVP_CHECK(upload_match.body == uvp::http::detail::body_mode::stream);
  UVP_CHECK_EQ(upload_match.max_body_bytes, 32U);
  UVP_CHECK(upload_match.body_timeout == std::chrono::milliseconds{250});

  router.group("/api")
    .resource("/items")
    .post(
      uvp::http::route_options{}
        .max_body_bytes(24)
        .body_timeout(std::chrono::milliseconds{500}),
      uvp::http::body::text{},
      [](uvp::http::request&, uvp::http::response&, std::string_view) {});

  auto resource_match = router.match(uvp::http::method::post, "/api/items");
  UVP_REQUIRE(resource_match);
  UVP_CHECK(resource_match.body == uvp::http::detail::body_mode::text);
  UVP_CHECK_EQ(resource_match.max_body_bytes, 24U);
  UVP_CHECK(resource_match.body_timeout == std::chrono::milliseconds{500});
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

UVP_TEST_CASE("http router reports methods allowed for a matched path") {
  uvp::http::router router;
  router.get("/items/:id", [](uvp::http::request&, uvp::http::response&) {});
  router.post("/items/:id", [](uvp::http::request&, uvp::http::response&) {});
  router.delete_("/items/:id", [](uvp::http::request&, uvp::http::response&) {});
  router.patch("/names/:name", [](uvp::http::request&, uvp::http::response&) {});

  const auto allowed = router.allowed_methods("/items/42");
  UVP_REQUIRE(allowed.size() == 3U);
  UVP_CHECK(allowed[0] == uvp::http::method::get);
  UVP_CHECK(allowed[1] == uvp::http::method::post);
  UVP_CHECK(allowed[2] == uvp::http::method::delete_);
  const auto decoded_allowed = router.allowed_methods("/names/alice%20smith");
  UVP_REQUIRE(decoded_allowed.size() == 1U);
  UVP_CHECK(decoded_allowed[0] == uvp::http::method::patch);
  UVP_CHECK(router.allowed_methods("/missing").empty());
}

UVP_TEST_CASE("http router groups prefix routes") {
  uvp::http::router router;

  auto api = router.group("/api");
  api.get("/health", [](uvp::http::request&, uvp::http::response&) {});

  auto v1 = api.group("v1");
  v1.post("items", [](uvp::http::request&, uvp::http::response&, std::string_view) {});

  UVP_CHECK(router.find(uvp::http::method::get, "/api/health") != nullptr);
  UVP_CHECK(router.find(uvp::http::method::get, "/health") == nullptr);

  auto match = router.match(uvp::http::method::post, "/api/v1/items");
  UVP_REQUIRE(match);
  UVP_CHECK(match.body == uvp::http::detail::body_mode::text);
  UVP_CHECK_EQ(match.pattern, "/api/v1/items");
}

UVP_TEST_CASE("http router resources register multiple methods on the same path") {
  uvp::http::router router;

  router.resource("/items/:id")
    .get([](uvp::http::request&, uvp::http::response&) {})
    .put(uvp::http::body::text{}, [](uvp::http::request&, uvp::http::response&, std::string_view) {})
    .delete_([](uvp::http::request&, uvp::http::response&) {});

  UVP_CHECK(router.find(uvp::http::method::get, "/items/42") != nullptr);
  auto put_match = router.match(uvp::http::method::put, "/items/42");
  UVP_REQUIRE(put_match);
  UVP_CHECK(put_match.body == uvp::http::detail::body_mode::text);
  UVP_CHECK_EQ(put_match.pattern, "/items/:id");
  UVP_CHECK_EQ(put_match.params.get("id"), "42");
  UVP_CHECK(router.find(uvp::http::method::delete_, "/items/42") != nullptr);
}

UVP_TEST_CASE("http router group resources use the group prefix") {
  uvp::http::router router;

  router.group("/api/v1")
    .resource("/items/:id")
    .get([](uvp::http::request&, uvp::http::response&) {})
    .patch(uvp::http::body::text{}, [](uvp::http::request&, uvp::http::response&, std::string_view) {});

  UVP_CHECK(router.find(uvp::http::method::get, "/api/v1/items/42") != nullptr);
  auto patch_match = router.match(uvp::http::method::patch, "/api/v1/items/42");
  UVP_REQUIRE(patch_match);
  UVP_CHECK(patch_match.body == uvp::http::detail::body_mode::text);
  UVP_CHECK_EQ(patch_match.pattern, "/api/v1/items/:id");
  UVP_CHECK_EQ(patch_match.params.get("id"), "42");
  UVP_CHECK(router.find(uvp::http::method::get, "/items/42") == nullptr);
}

UVP_TEST_CASE("http router mounts another router under a prefix") {
  uvp::http::router api;
  api.get("/items/:id", [](uvp::http::request&, uvp::http::response&) {});
  api.post("/items/:id", uvp::http::body::text{}, [](uvp::http::request&, uvp::http::response&, std::string_view) {});

  uvp::http::router router;
  router.mount("/api/v1", std::move(api));

  UVP_CHECK_EQ(router.size(), 2U);
  UVP_CHECK(api.empty());

  auto get_match = router.match(uvp::http::method::get, "/api/v1/items/42");
  UVP_REQUIRE(get_match);
  UVP_CHECK_EQ(get_match.pattern, "/api/v1/items/:id");
  UVP_CHECK_EQ(get_match.params.get("id"), "42");

  auto post_match = router.match(uvp::http::method::post, "/api/v1/items/42");
  UVP_REQUIRE(post_match);
  UVP_CHECK(post_match.body == uvp::http::detail::body_mode::text);
  UVP_CHECK_EQ(post_match.pattern, "/api/v1/items/:id");
  UVP_CHECK(router.find(uvp::http::method::get, "/items/42") == nullptr);
}

UVP_TEST_CASE("http router rejects mounting routers with different path matching modes") {
  uvp::http::router decoded;
  uvp::http::router raw{uvp::http::route_path_matching::raw};

  UVP_CHECK_THROWS(decoded.mount("/raw", std::move(raw)), std::invalid_argument);
}

UVP_TEST_CASE("http router groups mount routers relative to their prefix") {
  uvp::http::router api;
  api.get("/items", [](uvp::http::request&, uvp::http::response&) {});

  uvp::http::router router;
  router.group("/api").mount("v1", std::move(api));

  UVP_CHECK(router.find(uvp::http::method::get, "/api/v1/items") != nullptr);
  UVP_CHECK(router.find(uvp::http::method::get, "/v1/items") == nullptr);
}

UVP_TEST_CASE("http router mounted hooks compose with destination hooks") {
  uvp::http::router mounted;
  std::vector<std::string> order;

  mounted.on_request([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("mounted-request");
  });
  mounted.pre_handler([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("mounted-pre");
  });
  mounted.on_response([&](const uvp::http::response_info&) {
    order.push_back("mounted-response");
  });

  auto mounted_v1 = mounted.group("/v1");
  mounted_v1.on_request([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("mounted-v1-request");
  });
  mounted_v1.pre_handler([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("mounted-v1-pre");
  });
  mounted_v1.on_response([&](const uvp::http::response_info&) {
    order.push_back("mounted-v1-response");
  });
  mounted_v1.get("/items", [](uvp::http::request&, uvp::http::response&) {});

  uvp::http::router router;
  router.on_request([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("root-request");
  });
  router.pre_handler([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("root-pre");
  });
  router.on_response([&](const uvp::http::response_info&) {
    order.push_back("root-response");
  });

  auto api = router.group("/api");
  api.on_request([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("api-request");
  });
  api.pre_handler([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("api-pre");
  });
  api.on_response([&](const uvp::http::response_info&) {
    order.push_back("api-response");
  });

  router.mount("/api", std::move(mounted));

  auto match = router.match(uvp::http::method::get, "/api/v1/items");
  UVP_REQUIRE(match);
  UVP_CHECK_EQ(match.on_request_hooks.size(), 4U);
  UVP_CHECK_EQ(match.pre_handler_hooks.size(), 4U);
  UVP_CHECK_EQ(match.on_response_hooks.size(), 4U);

  uvp::http::request req;
  uvp::http::response res;
  for (const auto* hook : match.on_request_hooks) {
    UVP_CHECK((*hook)(req, res) == uvp::http::hook_result::next);
  }
  for (const auto* hook : match.pre_handler_hooks) {
    UVP_CHECK((*hook)(req, res) == uvp::http::hook_result::next);
  }
  uvp::http::request_snapshot snapshot;
  uvp::http::headers headers;
  const auto info = uvp::http::response_info{
    snapshot,
    200,
    headers,
    0,
    uvp::http::response_outcome::completed,
  };
  for (auto hook = match.on_response_hooks.rbegin(); hook != match.on_response_hooks.rend(); ++hook) {
    (**hook)(info);
  }

  UVP_REQUIRE(order.size() == 12U);
  UVP_CHECK_EQ(order[0], "root-request");
  UVP_CHECK_EQ(order[1], "api-request");
  UVP_CHECK_EQ(order[2], "mounted-request");
  UVP_CHECK_EQ(order[3], "mounted-v1-request");
  UVP_CHECK_EQ(order[4], "root-pre");
  UVP_CHECK_EQ(order[5], "api-pre");
  UVP_CHECK_EQ(order[6], "mounted-pre");
  UVP_CHECK_EQ(order[7], "mounted-v1-pre");
  UVP_CHECK_EQ(order[8], "mounted-v1-response");
  UVP_CHECK_EQ(order[9], "mounted-response");
  UVP_CHECK_EQ(order[10], "api-response");
  UVP_CHECK_EQ(order[11], "root-response");
}

UVP_TEST_CASE("http router rejects conflicting mounted routes") {
  uvp::http::router router;
  router.get("/api/items", [](uvp::http::request&, uvp::http::response&) {});

  uvp::http::router conflicting;
  conflicting.get("/items", [](uvp::http::request&, uvp::http::response&) {});

  UVP_CHECK_THROWS(router.mount("/api", std::move(conflicting)), std::invalid_argument);
  UVP_CHECK_EQ(router.size(), 1U);
  UVP_CHECK(router.find(uvp::http::method::get, "/api/items") != nullptr);
}

UVP_TEST_CASE("http router rejects conflicting mounted parameter names") {
  uvp::http::router router;
  router.get("/api/users/:id", [](uvp::http::request&, uvp::http::response&) {});

  uvp::http::router conflicting;
  conflicting.get("/users/:name", [](uvp::http::request&, uvp::http::response&) {});

  UVP_CHECK_THROWS(router.mount("/api", std::move(conflicting)), std::invalid_argument);
  UVP_CHECK_EQ(router.size(), 1U);
}

UVP_TEST_CASE("http router matches inherited group hooks from root to leaf") {
  uvp::http::router router;
  std::vector<std::string> order;

  router.on_request([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("root-request");
  });
  router.pre_handler([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("root-pre");
  });
  router.on_response([&](const uvp::http::response_info&) {
    order.push_back("root-response");
  });

  auto api = router.group("/api");
  api.on_request([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("api-request");
    return uvp::http::hook_result::next;
  });
  api.pre_handler([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("api-pre");
    return uvp::http::hook_result::next;
  });
  api.on_response([&](const uvp::http::response_info&) {
    order.push_back("api-response");
  });

  auto v1 = api.group("/v1");
  v1.on_request([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("v1-request");
  });
  v1.pre_handler([&](uvp::http::request&, uvp::http::response&) {
    order.push_back("v1-pre");
  });
  v1.on_response([&](const uvp::http::response_info&) {
    order.push_back("v1-response");
  });
  v1.get("/items/:id", [](uvp::http::request&, uvp::http::response&) {});

  auto match = router.match(uvp::http::method::get, "/api/v1/items/42");
  UVP_REQUIRE(match);
  UVP_CHECK_EQ(match.on_request_hooks.size(), 3U);
  UVP_CHECK_EQ(match.pre_handler_hooks.size(), 3U);
  UVP_CHECK_EQ(match.on_response_hooks.size(), 3U);
  UVP_CHECK_EQ(match.params.get("id"), "42");

  uvp::http::request req;
  uvp::http::response res;
  for (const auto* hook : match.on_request_hooks) {
    UVP_CHECK((*hook)(req, res) == uvp::http::hook_result::next);
  }
  for (const auto* hook : match.pre_handler_hooks) {
    UVP_CHECK((*hook)(req, res) == uvp::http::hook_result::next);
  }
  uvp::http::request_snapshot snapshot;
  uvp::http::headers headers;
  const auto info = uvp::http::response_info{
    snapshot,
    200,
    headers,
    0,
    uvp::http::response_outcome::completed,
  };
  for (auto hook = match.on_response_hooks.rbegin(); hook != match.on_response_hooks.rend(); ++hook) {
    (**hook)(info);
  }

  UVP_REQUIRE(order.size() == 9U);
  UVP_CHECK_EQ(order[0], "root-request");
  UVP_CHECK_EQ(order[1], "api-request");
  UVP_CHECK_EQ(order[2], "v1-request");
  UVP_CHECK_EQ(order[3], "root-pre");
  UVP_CHECK_EQ(order[4], "api-pre");
  UVP_CHECK_EQ(order[5], "v1-pre");
  UVP_CHECK_EQ(order[6], "v1-response");
  UVP_CHECK_EQ(order[7], "api-response");
  UVP_CHECK_EQ(order[8], "root-response");
}
