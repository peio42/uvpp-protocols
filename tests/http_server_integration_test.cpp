#include "test.hpp"

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

namespace {

uv::buffer_view integration_alloc(uv::tcp&, std::size_t) {
  static std::array<char, 4096> storage{};
  return uv::buffer_view{storage.data(), storage.size()};
}

template<class Configure>
std::string perform_http_request(Configure configure, std::string request, std::string_view response_marker) {
  uv::loop loop;
  uvp::http::server server(loop);
  configure(server);

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto stream_listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto endpoint = stream_listener.local_endpoint();
  const auto port = std::get<uvp::io::tcp_endpoint>(endpoint).port;
  server.listen(std::move(stream_listener));

  uv::tcp client(loop);
  uv::connect_request connect_request;
  uv::write_request write_request;
  uv::timer timeout(loop);

  std::string received;
  bool client_closed = false;
  bool timed_out = false;

  timeout.start(std::chrono::seconds{2}, [&](uv::timer& timer) {
    timed_out = true;
    server.close();
    if (!client_closed) {
      client.close([&](uv::tcp&) {
        client_closed = true;
      });
    }
    timer.close();
  });

  client.connect(connect_request, uv::ipv4{"127.0.0.1", static_cast<int>(port)}, [&](uv::connect_request&, uv::result status) {
    UVP_REQUIRE(status);

    auto output = uv::buffer_view{request.data(), request.size()};
    client.write(write_request, output, [&](uv::write_request&, uv::result write_status) {
      UVP_REQUIRE(write_status);

      client.read_start(integration_alloc, [&](uv::tcp& stream, uv::read_result read) {
        if (read.eof()) {
          return;
        }
        UVP_REQUIRE(read.ok());

        const auto bytes = read.bytes();
        received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (received.find(response_marker) != std::string::npos) {
          stream.read_stop();
          timeout.close();
          stream.close([&](uv::tcp&) {
            client_closed = true;
          });
          server.close();
        }
      });
    });
  });

  loop.run();

  UVP_CHECK(!timed_out);
  UVP_CHECK(client_closed);
  loop.close();
  return received;
}

} // namespace

UVP_TEST_CASE("http server handles a real tcp request") {
  uv::loop loop;
  uvp::http::server server(loop);

  server.get("/hello/:name", [](uvp::http::request& req, uvp::http::response& res) {
    res.type("text/plain").text(std::string{"hello "} + std::string(req.params().get("name")));
  });

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto stream_listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto endpoint = stream_listener.local_endpoint();
  const auto port = std::get<uvp::io::tcp_endpoint>(endpoint).port;
  server.listen(std::move(stream_listener));

  uv::tcp client(loop);
  uv::connect_request connect_request;
  uv::write_request write_request;
  uv::timer timeout(loop);

  std::string request =
    "GET /hello/ada HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n";
  std::string received;
  bool connected = false;
  bool write_done = false;
  bool response_seen = false;
  bool client_closed = false;
  bool timed_out = false;

  timeout.start(std::chrono::seconds{2}, [&](uv::timer& timer) {
    timed_out = true;
    server.close();
    if (!client_closed) {
      client.close([&](uv::tcp&) {
        client_closed = true;
      });
    }
    timer.close();
  });

  client.connect(connect_request, uv::ipv4{"127.0.0.1", static_cast<int>(port)}, [&](uv::connect_request&, uv::result status) {
    UVP_REQUIRE(status);
    connected = true;

    auto output = uv::buffer_view{request.data(), request.size()};
    client.write(write_request, output, [&](uv::write_request&, uv::result write_status) {
      UVP_REQUIRE(write_status);
      write_done = true;

      client.read_start(integration_alloc, [&](uv::tcp& stream, uv::read_result read) {
        if (read.eof()) {
          return;
        }
        UVP_REQUIRE(read.ok());

        const auto bytes = read.bytes();
        received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (received.find("\r\n\r\n") != std::string::npos && received.find("hello ada") != std::string::npos) {
          response_seen = true;
          stream.read_stop();
          timeout.close();
          stream.close([&](uv::tcp&) {
            client_closed = true;
          });
          server.close();
        }
      });
    });
  });

  loop.run();

  UVP_CHECK(!timed_out);
  UVP_CHECK(connected);
  UVP_CHECK(write_done);
  UVP_CHECK(response_seen);
  UVP_CHECK(client_closed);
  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-type: text/plain; charset=utf-8\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-length: 9\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nhello ada") != std::string::npos);

  loop.close();
}

UVP_TEST_CASE("http server falls back from HEAD to GET without sending a body") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.get("/hello/:name", [](uvp::http::request& req, uvp::http::response& res) {
        res.type("text/plain").text(std::string{"hello "} + std::string(req.params().get("name")));
      });
    },
    "HEAD /hello/ada HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-type: text/plain; charset=utf-8\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-length: 9\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nhello ada") == std::string::npos);
}

UVP_TEST_CASE("http server returns 405 and allow for a known path with another method") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.get("/items", [](uvp::http::request&, uvp::http::response& res) {
        res.text("items");
      });
      server.post("/items", [](uvp::http::request&, uvp::http::response& res) {
        res.status(uvp::http::status::created).text("created");
      });
    },
    "PUT /items HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "method not allowed\n");

  UVP_CHECK(received.find("HTTP/1.1 405 Method Not Allowed\r\n") != std::string::npos);
  UVP_CHECK(received.find("allow: GET, HEAD, POST, OPTIONS\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nmethod not allowed\n") != std::string::npos);
}

UVP_TEST_CASE("http server answers automatic OPTIONS for a known path") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.get("/items", [](uvp::http::request&, uvp::http::response& res) {
        res.text("items");
      });
      server.post("/items", [](uvp::http::request&, uvp::http::response& res) {
        res.status(uvp::http::status::created).text("created");
      });
    },
    "OPTIONS /items HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\n");

  UVP_CHECK(received.find("HTTP/1.1 204 No Content\r\n") != std::string::npos);
  UVP_CHECK(received.find("allow: GET, HEAD, POST, OPTIONS\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-length: 0\r\n") != std::string::npos);
}

UVP_TEST_CASE("http server applies route group pre handlers") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      auto api = server.group("/api");
      api.pre_handler([](uvp::http::request&, uvp::http::response& res) {
        res.header("x-hook", "pre");
      });
      api.get("/health", [](uvp::http::request&, uvp::http::response& res) {
        res.text("ok\n");
      });
    },
    "GET /api/health HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "ok\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("x-hook: pre\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nok\n") != std::string::npos);
}

UVP_TEST_CASE("http server route group on_request can short circuit before handler") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      auto api = server.group("/api");
      api.on_request([](uvp::http::request&, uvp::http::response& res) {
        res.status(uvp::http::status::unauthorized).text("blocked\n");
        return uvp::http::hook_result::stop;
      });
      api.post("/items", uvp::http::body::text{}, [](uvp::http::request&, uvp::http::response& res, std::string_view) {
        res.text("handler\n");
      });
    },
    "POST /api/items HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "Content-Length: 7\r\n"
    "\r\n"
    "payload",
    "blocked\n");

  UVP_CHECK(received.find("HTTP/1.1 401 Unauthorized\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nblocked\n") != std::string::npos);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
}

UVP_TEST_CASE("http server uses scoped not_found fallback") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.not_found([](uvp::http::request&, uvp::http::response& res) {
        res.status(uvp::http::status::not_found).text("global missing\n");
      });

      auto api = server.group("/api");
      api.not_found([](uvp::http::request& req, uvp::http::response& res) {
        res.status(uvp::http::status::not_found).text(std::string{"api missing "} + std::string(req.path()) + "\n");
      });
    },
    "GET /api/missing HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "api missing /api/missing\n");

  UVP_CHECK(received.find("HTTP/1.1 404 Not Found\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\napi missing /api/missing\n") != std::string::npos);
  UVP_CHECK(received.find("global missing\n") == std::string::npos);
}

UVP_TEST_CASE("http server on_exception handles application exceptions") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.on_exception([](uvp::http::request&, uvp::http::response& res, std::exception_ptr error) {
        try {
          std::rethrow_exception(error);
        } catch (const std::runtime_error& ex) {
          res.status(uvp::http::status::internal_server_error).text(std::string{"caught "} + ex.what() + "\n");
        }
      });
      server.get("/boom", [](uvp::http::request&, uvp::http::response&) {
        throw std::runtime_error{"boom"};
      });
    },
    "GET /boom HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "caught boom\n");

  UVP_CHECK(received.find("HTTP/1.1 500 Internal Server Error\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\ncaught boom\n") != std::string::npos);
}

UVP_TEST_CASE("http server uses scoped on_exception for matched routes") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.on_exception([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
        res.status(uvp::http::status::internal_server_error).text("global exception\n");
      });

      auto api = server.group("/api");
      api.on_exception([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
        res.status(uvp::http::status::internal_server_error).text("api exception\n");
      });
      api.get("/boom", [](uvp::http::request&, uvp::http::response&) {
        throw std::runtime_error{"api boom"};
      });
    },
    "GET /api/boom HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "api exception\n");

  UVP_CHECK(received.find("HTTP/1.1 500 Internal Server Error\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\napi exception\n") != std::string::npos);
  UVP_CHECK(received.find("global exception\n") == std::string::npos);
}

UVP_TEST_CASE("http server resources register multiple methods on one endpoint") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.group("/api")
        .resource("/items/:id")
        .get([](uvp::http::request& req, uvp::http::response& res) {
          res.text(std::string{"show "} + std::string(req.params().get("id")) + "\n");
        })
        .put(uvp::http::body::text{}, [](uvp::http::request& req, uvp::http::response& res, std::string_view body) {
          res.text(std::string{"update "} + std::string(req.params().get("id")) + " " + std::string(body) + "\n");
        });
    },
    "PUT /api/items/42 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "Content-Length: 3\r\n"
    "\r\n"
    "new",
    "update 42 new\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nupdate 42 new\n") != std::string::npos);
}

UVP_TEST_CASE("http server applies route options body limits") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::route_options{}.max_body_bytes(3),
        uvp::http::body::text{},
        [](uvp::http::request&, uvp::http::response& res, std::string_view) {
          res.text("accepted\n");
        });
    },
    "POST /upload HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "tool",
    "Payload Too Large\n");

  UVP_CHECK(received.find("HTTP/1.1 413 Payload Too Large\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nPayload Too Large\n") != std::string::npos);
  UVP_CHECK(received.find("accepted\n") == std::string::npos);
}

UVP_TEST_CASE("http server mounts an independently declared router") {
  std::string observed_pattern;

  const auto received = perform_http_request(
    [&](uvp::http::server& server) {
      uvp::http::router api;
      api.get("/items/:id", [&](uvp::http::request& req, uvp::http::response& res) {
        observed_pattern = req.matched_pattern();
        res.text(std::string{"item "} + std::string(req.params().get("id")) + "\n");
      });

      server.mount("/api/v1", std::move(api));
    },
    "GET /api/v1/items/42 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "item 42\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nitem 42\n") != std::string::npos);
  UVP_CHECK_EQ(observed_pattern, "/api/v1/items/:id");
}

UVP_TEST_CASE("http server runs response hooks with response snapshots") {
  std::vector<std::string> events;
  unsigned int observed_status = 0;
  std::size_t observed_body_size = 0;
  std::string observed_path;
  std::string observed_pattern;
  std::string observed_request_pattern;
  uvp::http::response_outcome observed_outcome = uvp::http::response_outcome::error;

  const auto received = perform_http_request(
    [&](uvp::http::server& server) {
      server.on_response([&](const uvp::http::response_info& info) {
        events.push_back("server");
        observed_status = info.status;
        observed_body_size = info.response_body_size;
        observed_path = info.request.path;
        observed_pattern = info.request.matched_pattern;
        observed_outcome = info.outcome;
      });

      auto api = server.group("/api");
      api.on_request([&](uvp::http::request& req, uvp::http::response&) {
        observed_request_pattern = req.matched_pattern();
      });
      api.on_response([&](const uvp::http::response_info&) {
        events.push_back("api");
      });
      api.get("/items/:id", [](uvp::http::request& req, uvp::http::response& res) {
        res.text(std::string{"item "} + std::string(req.params().get("id")) + "\n");
      });
    },
    "GET /api/items/42 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "item 42\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_REQUIRE(events.size() == 2U);
  UVP_CHECK_EQ(events[0], "api");
  UVP_CHECK_EQ(events[1], "server");
  UVP_CHECK_EQ(observed_status, 200U);
  UVP_CHECK_EQ(observed_body_size, 8U);
  UVP_CHECK_EQ(observed_path, "/api/items/42");
  UVP_CHECK_EQ(observed_pattern, "/api/items/:id");
  UVP_CHECK_EQ(observed_request_pattern, "/api/items/:id");
  UVP_CHECK(observed_outcome == uvp::http::response_outcome::completed);
}

UVP_TEST_CASE("http server runs response hooks for short circuited requests") {
  unsigned int observed_status = 0;
  uvp::http::response_outcome observed_outcome = uvp::http::response_outcome::error;

  const auto received = perform_http_request(
    [&](uvp::http::server& server) {
      auto api = server.group("/api");
      api.on_request([](uvp::http::request&, uvp::http::response& res) {
        res.status(uvp::http::status::unauthorized).text("blocked\n");
        return uvp::http::hook_result::stop;
      });
      api.on_response([&](const uvp::http::response_info& info) {
        observed_status = info.status;
        observed_outcome = info.outcome;
      });
      api.get("/secure", [](uvp::http::request&, uvp::http::response& res) {
        res.text("handler\n");
      });
    },
    "GET /api/secure HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "blocked\n");

  UVP_CHECK(received.find("HTTP/1.1 401 Unauthorized\r\n") != std::string::npos);
  UVP_CHECK_EQ(observed_status, 401U);
  UVP_CHECK(observed_outcome == uvp::http::response_outcome::completed);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
}

UVP_TEST_CASE("http server runs response hooks for deferred responses") {
  unsigned int observed_status = 0;
  std::size_t observed_body_size = 0;
  uvp::http::response_outcome observed_outcome = uvp::http::response_outcome::error;

  const auto received = perform_http_request(
    [&](uvp::http::server& server) {
      server.on_response([&](const uvp::http::response_info& info) {
        observed_status = info.status;
        observed_body_size = info.response_body_size;
        observed_outcome = info.outcome;
      });
      server.get("/later", [](uvp::http::request&, uvp::http::response& res) {
        auto reply = res.defer();
        reply.status(uvp::http::status::created).text("later\n");
      });
    },
    "GET /later HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "later\n");

  UVP_CHECK(received.find("HTTP/1.1 201 Created\r\n") != std::string::npos);
  UVP_CHECK_EQ(observed_status, 201U);
  UVP_CHECK_EQ(observed_body_size, 6U);
  UVP_CHECK(observed_outcome == uvp::http::response_outcome::completed);
}
