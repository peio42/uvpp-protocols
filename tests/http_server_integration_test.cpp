#include "test.hpp"

#include <array>
#include <chrono>
#include <string>
#include <variant>

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
