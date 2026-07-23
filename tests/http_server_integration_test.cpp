#include "test.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

namespace {

struct json_widget {
  std::string name;
  int count = 0;
};

struct json_throwing_widget {
  std::string name;
};

void from_json(const uvp::json& value, json_widget& widget) {
  widget.name = value.at("name").get<std::string>();
  widget.count = value.at("count").get<int>();
}

void from_json(const uvp::json& value, json_throwing_widget& widget) {
  widget.name = value.at("name").get<std::string>();
  throw std::runtime_error("domain validation failed");
}

class temporary_directory {
public:
  temporary_directory() {
    const auto base = std::filesystem::temp_directory_path();
    for (auto attempt = 0; attempt < 100; ++attempt) {
      path_ = base / ("uvpp-protocols-static-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(attempt));
      std::error_code ec;
      if (std::filesystem::create_directory(path_, ec)) {
        return;
      }
    }
    throw std::runtime_error("failed to create temporary directory");
  }

  ~temporary_directory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  temporary_directory(const temporary_directory&) = delete;
  temporary_directory& operator=(const temporary_directory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

class scoped_permissions {
public:
  scoped_permissions(std::filesystem::path path, std::filesystem::perms replacement)
      : path_(std::move(path)),
        original_(std::filesystem::status(path_).permissions()) {
    std::filesystem::permissions(path_, replacement, std::filesystem::perm_options::replace);
  }

  ~scoped_permissions() {
    std::error_code ec;
    std::filesystem::permissions(path_, original_, std::filesystem::perm_options::replace, ec);
  }

  scoped_permissions(const scoped_permissions&) = delete;
  scoped_permissions& operator=(const scoped_permissions&) = delete;

private:
  std::filesystem::path path_;
  std::filesystem::perms original_;
};

void write_file(const std::filesystem::path& path, std::string_view body) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << body;
}

uv::buffer_view integration_alloc(uv::tcp&, std::size_t) {
  static std::array<char, 4096> storage{};
  return uv::buffer_view{storage.data(), storage.size()};
}

template<class Configure>
std::string perform_http_request(
  uvp::http::server_options options,
  Configure configure,
  std::string request,
  std::string_view response_marker) {
  uv::loop loop;
  uvp::http::server server(loop, options);
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

template<class Configure>
std::string perform_http_request(Configure configure, std::string request, std::string_view response_marker) {
  return perform_http_request(uvp::http::server_options{}, std::move(configure), std::move(request), response_marker);
}

template<class Configure>
std::string perform_http_request_chunks(
  Configure configure,
  std::vector<std::string> request_chunks,
  std::string_view response_marker) {
  uv::loop loop;
  uvp::http::server server(loop);
  if constexpr (std::is_invocable_v<Configure&, uvp::http::server&, uv::loop&>) {
    configure(server, loop);
  } else {
    configure(server);
  }

  auto tcp_listener = uvp::io::tcp_listener{loop};
  tcp_listener.bind("127.0.0.1", 0);
  auto stream_listener = uvp::io::stream_listener{std::move(tcp_listener)};
  const auto endpoint = stream_listener.local_endpoint();
  const auto port = std::get<uvp::io::tcp_endpoint>(endpoint).port;
  server.listen(std::move(stream_listener));

  uv::tcp client(loop);
  uv::connect_request connect_request;
  std::vector<uv::write_request> write_requests(request_chunks.size());
  uv::timer timeout(loop);

  std::string received;
  std::size_t next_write = 0;
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

    auto write_next = std::make_shared<std::function<void()>>();
    *write_next = [&, write_next] {
      if (next_write >= request_chunks.size()) {
        return;
      }
      const auto index = next_write++;
      auto output = uv::buffer_view{request_chunks[index].data(), request_chunks[index].size()};
      client.write(write_requests[index], output, [&, write_next](uv::write_request&, uv::result write_status) {
        UVP_REQUIRE(write_status);
        (*write_next)();
      });
    };
    (*write_next)();
  });

  loop.run();

  UVP_CHECK(!timed_out);
  UVP_CHECK(client_closed);
  loop.close();
  return received;
}

template<class Configure>
std::string perform_http_request_until_close(
  uvp::http::server_options options,
  Configure configure,
  std::string request) {
  uv::loop loop;
  uvp::http::server server(loop, options);
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
          stream.read_stop();
          timeout.close();
          stream.close([&](uv::tcp&) {
            client_closed = true;
          });
          server.close();
          return;
        }
        UVP_REQUIRE(read.ok());

        const auto bytes = read.bytes();
        received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
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

UVP_TEST_CASE("http server rejects requests exceeding header limits") {
  const auto too_many = perform_http_request(
    uvp::http::server_options{}.max_header_count(1),
    [](uvp::http::server& server) {
      server.get("/", [](uvp::http::request&, uvp::http::response& res) {
        res.text("ok\n");
      });
    },
    "GET / HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "X-Test: ok\r\n"
    "Connection: close\r\n"
    "\r\n",
    "HTTP/1.1 400 Bad Request\r\n");

  UVP_CHECK(too_many.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);

  const auto too_large = perform_http_request(
    uvp::http::server_options{}.max_header_bytes(15),
    [](uvp::http::server& server) {
      server.get("/", [](uvp::http::request&, uvp::http::response& res) {
        res.text("ok\n");
      });
    },
    "GET / HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "HTTP/1.1 400 Bad Request\r\n");

  UVP_CHECK(too_large.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
}

UVP_TEST_CASE("http server serves static files with metadata") {
  temporary_directory root;
  write_file(root.path() / "app.js", "console.log('ok');\n");

  auto received = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()).cache_control("public, max-age=60"));
    },
    "GET /assets/app.js HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\nconsole.log('ok');\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-type: text/javascript; charset=utf-8\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-length: 19\r\n") != std::string::npos);
  UVP_CHECK(received.find("cache-control: public, max-age=60\r\n") != std::string::npos);
  UVP_CHECK(received.find("etag: W/\"") != std::string::npos);
  UVP_CHECK(received.find("last-modified: ") != std::string::npos);
  UVP_CHECK(received.find("x-content-type-options: nosniff\r\n") != std::string::npos);
  UVP_CHECK(received.find("transfer-encoding: chunked\r\n") == std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nconsole.log('ok');\n") != std::string::npos);
}

UVP_TEST_CASE("http server serves static index files and head metadata") {
  temporary_directory root;
  write_file(root.path() / "index.html", "<h1>Home</h1>\n");

  auto get_received = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "GET /assets HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\n<h1>Home</h1>\n");

  UVP_CHECK(get_received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(get_received.find("content-type: text/html; charset=utf-8\r\n") != std::string::npos);
  UVP_CHECK(get_received.find("content-length: 14\r\n") != std::string::npos);

  auto head_received = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "HEAD /assets HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\n");

  UVP_CHECK(head_received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(head_received.find("content-length: 14\r\n") != std::string::npos);
  UVP_CHECK(head_received.find("<h1>Home</h1>") == std::string::npos);
}

UVP_TEST_CASE("http server rejects unsafe static paths") {
  temporary_directory root;
  write_file(root.path() / "public.txt", "public\n");
  write_file(root.path() / ".secret", "secret\n");

  auto traversal = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "GET /assets/../uvpp-protocols-outside-secret.txt HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\nNot Found\n");

  UVP_CHECK(traversal.find("HTTP/1.1 404 Not Found\r\n") != std::string::npos);
  UVP_CHECK(traversal.find("outside") == std::string::npos);

  auto encoded_slash = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "GET /assets/a%2Fb.txt HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\nNot Found\n");

  UVP_CHECK(encoded_slash.find("HTTP/1.1 404 Not Found\r\n") != std::string::npos);

  auto hidden = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "GET /assets/.secret HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\nNot Found\n");

  UVP_CHECK(hidden.find("HTTP/1.1 404 Not Found\r\n") != std::string::npos);
  UVP_CHECK(hidden.find("secret") == std::string::npos);
}

UVP_TEST_CASE("http server confines static symlinks to the root") {
  temporary_directory root;
  temporary_directory outside;
  write_file(outside.path() / "secret.txt", "secret\n");

  std::error_code ec;
  std::filesystem::create_symlink(outside.path() / "secret.txt", root.path() / "outside-link.txt", ec);
  if (ec) {
    return;
  }

  auto received = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "GET /assets/outside-link.txt HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\nNot Found\n");

  UVP_CHECK(received.find("HTTP/1.1 404 Not Found\r\n") != std::string::npos);
  UVP_CHECK(received.find("secret") == std::string::npos);
}

UVP_TEST_CASE("http server rejects static symlinks when configured") {
  temporary_directory root;
  write_file(root.path() / "public.txt", "public\n");

  std::error_code ec;
  std::filesystem::create_symlink(root.path() / "public.txt", root.path() / "public-link.txt", ec);
  if (ec) {
    return;
  }

  auto received = perform_http_request(
    [&](uvp::http::server& server) {
      server.get(
        "/assets/*path",
        uvp::http::static_files(root.path()).symlinks(uvp::http::symlink_policy::reject));
    },
    "GET /assets/public-link.txt HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\nNot Found\n");

  UVP_CHECK(received.find("HTTP/1.1 404 Not Found\r\n") != std::string::npos);
  UVP_CHECK(received.find("public\n") == std::string::npos);
}

UVP_TEST_CASE("http server reports permission denied static index files as internal errors") {
#ifdef _WIN32
  return;
#else
  temporary_directory root;
  const auto private_dir = root.path() / "private";
  write_file(private_dir / "index.html", "secret\n");

  auto permissions = scoped_permissions{private_dir, std::filesystem::perms::none};

  std::error_code status_ec;
  (void)std::filesystem::status(private_dir / "index.html", status_ec);
  if (status_ec != std::errc::permission_denied) {
    return;
  }

  auto received = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "GET /assets/private HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\nInternal Server Error\n");

  UVP_CHECK(received.find("HTTP/1.1 500 Internal Server Error\r\n") != std::string::npos);
  UVP_CHECK(received.find("secret") == std::string::npos);
#endif
}

UVP_TEST_CASE("http server supports static file conditional etags") {
  temporary_directory root;
  write_file(root.path() / "data.json", "{\"ok\":true}\n");

  auto first = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "GET /assets/data.json HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\n{\"ok\":true}\n");

  const auto etag_name = std::string{"etag: "};
  const auto etag_begin = first.find(etag_name);
  UVP_REQUIRE(etag_begin != std::string::npos);
  const auto etag_value_begin = etag_begin + etag_name.size();
  const auto etag_end = first.find("\r\n", etag_value_begin);
  UVP_REQUIRE(etag_end != std::string::npos);
  const auto etag = first.substr(etag_value_begin, etag_end - etag_value_begin);
  const auto modified_name = std::string{"last-modified: "};
  const auto modified_begin = first.find(modified_name);
  UVP_REQUIRE(modified_begin != std::string::npos);
  const auto modified_value_begin = modified_begin + modified_name.size();
  const auto modified_end = first.find("\r\n", modified_value_begin);
  UVP_REQUIRE(modified_end != std::string::npos);
  const auto modified = first.substr(modified_value_begin, modified_end - modified_value_begin);

  auto second = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()));
    },
    "GET /assets/data.json HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "If-None-Match: " + etag + "\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\n");

  UVP_CHECK(second.find("HTTP/1.1 304 Not Modified\r\n") != std::string::npos);
  UVP_CHECK(second.find("etag: " + etag + "\r\n") != std::string::npos);
  UVP_CHECK(second.find("{\"ok\":true}") == std::string::npos);

  auto third = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/assets/*path", uvp::http::static_files(root.path()).etag(false));
    },
    "GET /assets/data.json HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "If-Modified-Since: " + modified + "\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\n");

  UVP_CHECK(third.find("HTTP/1.1 304 Not Modified\r\n") != std::string::npos);
  UVP_CHECK(third.find("etag: ") == std::string::npos);
  UVP_CHECK(third.find("{\"ok\":true}") == std::string::npos);
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

UVP_TEST_CASE("http server serializes custom numeric status without a reason phrase") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.get("/custom-status", [](uvp::http::request&, uvp::http::response& res) {
        res.status(299).text("custom\n");
      });
    },
    "GET /custom-status HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "custom\n");

  UVP_CHECK(received.find("HTTP/1.1 299 \r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\ncustom\n") != std::string::npos);
}

UVP_TEST_CASE("http server sends server sent events over chunked streaming") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.get("/events", [](uvp::http::request&, uvp::http::response& res) {
        auto sse = res.sse();
        (void)sse.send(uvp::http::sse_event{
          .event = "bad\nevent",
          .data = "ignored",
        });
        (void)sse.retry(std::chrono::seconds{5});
        (void)sse.comment("ping\n");
        (void)sse.send(uvp::http::sse_event{});
        (void)sse.send(uvp::http::sse_event{
          .data = "tail\r",
        });
        (void)sse.send(uvp::http::sse_event{
          .data = "\nhead",
        });
        (void)sse.send(uvp::http::sse_event{
          .event = "ready",
          .id = "1",
          .data = "alpha\r\nbeta\n",
        });
        sse.close();
      });
    },
    "GET /events HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "0\r\n\r\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-type: text/event-stream; charset=utf-8\r\n") != std::string::npos);
  UVP_CHECK(received.find("cache-control: no-cache\r\n") != std::string::npos);
  UVP_CHECK(received.find("x-accel-buffering: no\r\n") != std::string::npos);
  UVP_CHECK(received.find("transfer-encoding: chunked\r\n") != std::string::npos);
  UVP_CHECK(received.find("retry: 5000\n\n") != std::string::npos);
  UVP_CHECK(received.find(": ping\n:\n\n") != std::string::npos);
  UVP_CHECK(received.find("data:\n\n") != std::string::npos);
  UVP_CHECK(received.find("data: tail\ndata:\n\n") != std::string::npos);
  UVP_CHECK(received.find("data:\ndata: head\n\n") != std::string::npos);
  UVP_CHECK(
    received.find("event: ready\nid: 1\ndata: alpha\ndata: beta\ndata:\n\n") != std::string::npos);
  UVP_CHECK(received.find("ignored") == std::string::npos);
}

UVP_TEST_CASE("http server suppresses transfer encoding when streaming with content length") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.get("/stream", [](uvp::http::request&, uvp::http::response& res) {
        auto stream = res.stream();
        stream.header("content-length", "5");
        stream.header("transfer-encoding", "chunked");
        const auto write = stream.write(std::string_view{"hello"});
        UVP_CHECK(write.accepted());
        stream.end();
      });
    },
    "GET /stream HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "\r\n\r\nhello");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-length: 5\r\n") != std::string::npos);
  UVP_CHECK(received.find("transfer-encoding:") == std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nhello") != std::string::npos);
}

UVP_TEST_CASE("http server routes on decoded path segments") {
  std::vector<std::string> observed_segments;

  const auto received = perform_http_request(
    [&](uvp::http::server& server) {
      server.get("/files/:name", [&](uvp::http::request& req, uvp::http::response& res) {
        observed_segments.assign(req.decoded_path_segments().begin(), req.decoded_path_segments().end());
        res.text(std::string{"file "} + std::string(req.params().get("name")) + "\n");
      });
    },
    "GET /files/a%2Fb+c%20d HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "file a/b+c d\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nfile a/b+c d\n") != std::string::npos);
  UVP_REQUIRE(observed_segments.size() == 2U);
  UVP_CHECK_EQ(observed_segments[0], "files");
  UVP_CHECK_EQ(observed_segments[1], "a/b+c d");
}

UVP_TEST_CASE("http server rejects invalid path percent encoding") {
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.get("/files/:name", [](uvp::http::request&, uvp::http::response& res) {
        res.text("should not run\n");
      });
    },
    "GET /files/%zz HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "Bad Request\n");

  UVP_CHECK(received.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nBad Request\n") != std::string::npos);
  UVP_CHECK(received.find("should not run\n") == std::string::npos);
}

UVP_TEST_CASE("http server rejects invalid upgrade route percent encoding") {
  uv::loop loop;
  uvp::http::server server(loop);

  UVP_CHECK_THROWS(
    server.upgrade("/ws/%zz", [](uvp::http::upgrade_request&) {}),
    std::invalid_argument);
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

UVP_TEST_CASE("http server parses raw json request bodies") {
  const std::string body = R"({"name":"ada"})";
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/json",
        uvp::http::body::json<>{},
        [](uvp::http::request&, uvp::http::response& res, const uvp::json& body) {
          res.text(body.at("name").get<std::string>() + "\n");
        });
    },
    std::string{
      "POST /json HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "ada\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nada\n") != std::string::npos);
}

UVP_TEST_CASE("http server converts typed json request bodies") {
  const std::string body = R"({"name":"bolt","count":3})";
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/widgets",
        uvp::http::body::json<json_widget>{},
        [](uvp::http::request&, uvp::http::response& res, const json_widget& body) {
          res.text(body.name + " " + std::to_string(body.count) + "\n");
        });
    },
    std::string{
      "POST /widgets HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: Application/Problem+JSON; charset=utf-8\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "bolt 3\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nbolt 3\n") != std::string::npos);
}

UVP_TEST_CASE("http server rejects json bodies with non json content types") {
  unsigned int observed_status = 0;
  const std::string body = R"({"name":"ada"})";
  const auto received = perform_http_request(
    [&](uvp::http::server& server) {
      auto api = server.group("/api");
      api.pre_handler([](uvp::http::request&, uvp::http::response& res) {
        res.header("x-pre-handler", "ran");
      });
      api.on_response([&](const uvp::http::response_info& info) {
        observed_status = info.status;
      });
      api.post(
        "/json",
        uvp::http::body::json<>{},
        [](uvp::http::request&, uvp::http::response& res, const uvp::json&) {
          res.text("handler\n");
        });
    },
    std::string{
      "POST /api/json HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "unsupported media type\n");

  UVP_CHECK(received.find("HTTP/1.1 415 Unsupported Media Type\r\n") != std::string::npos);
  UVP_CHECK(received.find("x-pre-handler: ran\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nunsupported media type\n") != std::string::npos);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
  UVP_CHECK_EQ(observed_status, 415U);
}

UVP_TEST_CASE("http server rejects malformed json request bodies") {
  const std::string body = R"({"name":)";
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/json",
        uvp::http::body::json<>{},
        [](uvp::http::request&, uvp::http::response& res, const uvp::json&) {
          res.text("handler\n");
        });
    },
    std::string{
      "POST /json HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "invalid json\n");

  UVP_CHECK(received.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\ninvalid json\n") != std::string::npos);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
}

UVP_TEST_CASE("http server rejects json conversion failures") {
  const std::string body = R"({"name":"bolt","count":"bad"})";
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/widgets",
        uvp::http::body::json<json_widget>{},
        [](uvp::http::request&, uvp::http::response& res, const json_widget&) {
          res.text("handler\n");
        });
    },
    std::string{
      "POST /widgets HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "invalid json body\n");

  UVP_CHECK(received.find("HTTP/1.1 422 Unprocessable Content\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\ninvalid json body\n") != std::string::npos);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
}

UVP_TEST_CASE("http server maps standard typed json exceptions to unprocessable content") {
  const std::string body = R"({"name":"bolt"})";
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/widgets",
        uvp::http::body::json<json_throwing_widget>{},
        [](uvp::http::request&, uvp::http::response& res, const json_throwing_widget&) {
          res.text("handler\n");
        });
    },
    std::string{
      "POST /widgets HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "invalid json body\n");

  UVP_CHECK(received.find("HTTP/1.1 422 Unprocessable Content\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\ninvalid json body\n") != std::string::npos);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
}

UVP_TEST_CASE("http server applies route body limits before json parsing") {
  const std::string body = R"({"x":1})";
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/json",
        uvp::http::route_options{}.max_body_bytes(2),
        uvp::http::body::json<>{},
        [](uvp::http::request&, uvp::http::response& res, const uvp::json&) {
          res.text("handler\n");
        });
    },
    std::string{
      "POST /json HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "Payload Too Large\n");

  UVP_CHECK(received.find("HTTP/1.1 413 Payload Too Large\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nPayload Too Large\n") != std::string::npos);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
}

UVP_TEST_CASE("http server streams multipart form-data parts") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"title\"\r\n"
    "\r\n"
    "hello\r\n"
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"../note.txt\"\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "abc123\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          auto title = std::make_shared<std::string>();
          auto file_name = std::make_shared<std::string>();
          auto file_body = std::make_shared<std::string>();

          multipart
            .on_part([title, file_name, file_body](uvp::http::multipart_part& part) {
              if (part.name() == "title") {
                part.text(1024, [title](uvp::result<std::string> value) {
                  UVP_REQUIRE(value);
                  *title = std::move(value).value();
                });
                return;
              }
              if (part.name() == "file") {
                *file_name = part.safe_filename();
                part.stream()
                  .on_data([file_body](std::span<const std::byte> chunk) {
                    file_body->append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                  });
                return;
              }
              part.discard();
            })
            .on_end([&res, title, file_name, file_body] {
              res.text(*title + ":" + *file_name + ":" + *file_body + "\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=\"AaB03x\"\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "hello:..note.txt:abc123\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nhello:..note.txt:abc123\n") != std::string::npos);
}

UVP_TEST_CASE("http server streams multipart boundaries fragmented across writes") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "abcdef\r\n"
    "--AaB03x--\r\n";
  const std::string head =
    "POST /upload HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
    "Content-Length: " + std::to_string(body.size()) + "\r\n"
    "\r\n";

  const auto received = perform_http_request_chunks(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          auto field = std::make_shared<std::string>();
          multipart
            .on_part([field](uvp::http::multipart_part& part) {
              part.stream()
                .on_data([field](std::span<const std::byte> chunk) {
                  field->append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                });
            })
            .on_end([&res, field] {
              res.text(*field + "\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    {
      head + "--Aa",
      "B03x\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\nabc",
      "def\r\n--Aa",
      "B03x--\r\n",
    },
    "abcdef\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nabcdef\n") != std::string::npos);
}

UVP_TEST_CASE("http server supports multipart pause resume in the middle of a part") {
  const std::string payload = std::string(40, 'x') + std::string(40, 'y');
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n" +
    payload +
    "\r\n"
    "--AaB03x--\r\n";
  const std::string head =
    "POST /upload HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
    "Content-Length: " + std::to_string(body.size()) + "\r\n"
    "\r\n";

  const auto received = perform_http_request_chunks(
    [](uvp::http::server& server, uv::loop& loop) {
      auto resume_timer = std::make_shared<uv::timer>(loop);
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [resume_timer](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          auto size = std::make_shared<std::size_t>(0);
          auto paused_once = std::make_shared<bool>(false);
          multipart
            .on_part([resume_timer, size, paused_once](uvp::http::multipart_part& part) {
              auto stream = std::make_shared<uvp::http::multipart_part_stream>(part.stream());
              stream->on_data([resume_timer, stream, size, paused_once](std::span<const std::byte> chunk) {
                *size += chunk.size();
                if (!*paused_once) {
                  *paused_once = true;
                  stream->pause();
                  resume_timer->start(std::chrono::milliseconds{1}, [stream](uv::timer& timer) {
                    stream->resume();
                    timer.close();
                  });
                }
              });
            })
            .on_end([&res, size, paused_once] {
              UVP_CHECK(*paused_once);
              res.text(std::to_string(*size) + "\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    {
      head + "--AaB03x\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\n" + payload.substr(0, 40),
      payload.substr(40) + "\r\n--AaB03x--\r\n",
    },
    "80\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\n80\n") != std::string::npos);
}

UVP_TEST_CASE("http server resumes a paused stream before parsing pipelined input") {
  const std::string request =
    "POST /upload HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Length: 4\r\n"
    "\r\n"
    "data"
    "GET /next HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n";

  const auto received = perform_http_request_chunks(
    [](uvp::http::server& server, uv::loop& loop) {
      auto resume_timer = std::make_shared<uv::timer>(loop);
      server.post(
        "/upload",
        uvp::http::body::stream{},
        [resume_timer](uvp::http::request&, uvp::http::response& res, uvp::http::request_body_stream& body) {
          body
            .on_data([resume_timer, &body](std::span<const std::byte>) {
              body.pause();
              resume_timer->start(std::chrono::milliseconds{1}, [&body](uv::timer& timer) {
                body.resume();
                timer.close();
              });
            })
            .on_end([&res] {
              res.text("upload\n");
            });
        });
      server.get("/next", [](uvp::http::request&, uvp::http::response& res) {
        res.text("next\n");
      });
    },
    {request},
    "next\n");

  UVP_CHECK(received.find("\r\n\r\nupload\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nnext\n") != std::string::npos);
}

UVP_TEST_CASE("http server rejects multipart stream routes with non multipart content types") {
  const std::string body = "not multipart";
  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream&) {
          res.text("handler\n");
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "expected multipart/form-data\n");

  UVP_CHECK(received.find("HTTP/1.1 415 Unsupported Media Type\r\n") != std::string::npos);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
}

UVP_TEST_CASE("http server reports malformed multipart bodies to stream handlers") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"a\"\r\n"
    "Content-Disposition: form-data; name=\"b\"\r\n"
    "\r\n"
    "bad\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.discard();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "duplicate content-disposition\n");

  UVP_CHECK(received.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server rejects deferred multipart streams without error handlers") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "value\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream&) {
          [[maybe_unused]] auto reply = res.defer();
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "multipart on_error handler required\n");

  UVP_CHECK(received.find("HTTP/1.1 500 Internal Server Error\r\n") != std::string::npos);
}

UVP_TEST_CASE("http server reports duplicate multipart part consumers") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "value\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          auto text_error_seen = std::make_shared<bool>(false);
          multipart
            .on_part([text_error_seen](uvp::http::multipart_part& part) {
              part.text(1024, [text_error_seen](uvp::result<std::string> value) {
                if (!value) {
                  *text_error_seen = true;
                }
              });
              part.discard();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res, text_error_seen](uvp::error error) {
              UVP_CHECK(*text_error_seen);
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "multipart part consumer already selected\n");

  UVP_CHECK(received.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server reports stream after multipart text consumer") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "value\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.text(1024, [](uvp::result<std::string>) {});
              part.stream()
                .on_data([](std::span<const std::byte>) {});
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "multipart part consumer already selected\n");

  UVP_CHECK(received.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server reports missing multipart content disposition to stream handlers") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "bad\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.discard();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "missing content-disposition\n");

  UVP_CHECK(received.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server reports unsupported multipart transfer encoding to stream handlers") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "dmFsdWU=\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.discard();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "unsupported content-transfer-encoding\n");

  UVP_CHECK(received.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server reports multipart stream part header byte limits") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "value\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      uvp::http::multipart_limits limits;
      limits.max_part_header_bytes = 8;
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{uvp::http::multipart_stream_options{}.limits(limits)},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.discard();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              const auto status = error.code == uvp::http::make_error_code(uvp::http::errc::multipart_limit_exceeded)
                ? uvp::http::status::payload_too_large
                : uvp::http::status::bad_request;
              res.status(status).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "multipart part headers are too large\n");

  UVP_CHECK(received.find("HTTP/1.1 413 Payload Too Large\r\n") != std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server streams delimiter-like multipart data with invalid suffix") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "alpha\r\n"
    "--AaB03x_NOT_A_BOUNDARY\r\n"
    "omega\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          auto field = std::make_shared<std::string>();
          multipart
            .on_part([field](uvp::http::multipart_part& part) {
              part.stream()
                .on_data([field](std::span<const std::byte> chunk) {
                  field->append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                });
            })
            .on_end([&res, field] {
              res.text(*field + "\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "alpha\r\n--AaB03x_NOT_A_BOUNDARY\r\nomega\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("invalid multipart delimiter suffix") == std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nalpha\r\n--AaB03x_NOT_A_BOUNDARY\r\nomega\n") != std::string::npos);
}

UVP_TEST_CASE("http server maps multipart limit errors to payload too large") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"big.txt\"\r\n"
    "\r\n"
    "abcdef\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{}.max_file_bytes(3),
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.stream();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              const auto status = error.code == uvp::http::make_error_code(uvp::http::errc::multipart_limit_exceeded)
                ? uvp::http::status::payload_too_large
                : uvp::http::status::bad_request;
              res.status(status).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "multipart file is too large\n");

  UVP_CHECK(received.find("HTTP/1.1 413 Payload Too Large\r\n") != std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server leaves multipart stream route body limit responses to handlers") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "abcdef\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::route_options{}.max_body_bytes(24),
        uvp::http::body::multipart_stream{},
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.discard();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              UVP_CHECK(error.code == std::make_error_code(std::errc::message_size));
              res.status(uvp::http::status::unprocessable_content).text("application-owned body limit\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "application-owned body limit\n");

  UVP_CHECK(received.find("HTTP/1.1 422 Unprocessable Content\r\n") != std::string::npos);
  UVP_CHECK(received.find("HTTP/1.1 413 Payload Too Large\r\n") == std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server applies multipart stream parser total limits independently from route limits") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "abcdef\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::route_options{}.max_body_bytes(1024),
        uvp::http::body::multipart_stream{}.max_total_bytes(24),
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.discard();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              const auto status = error.code == uvp::http::make_error_code(uvp::http::errc::multipart_limit_exceeded)
                ? uvp::http::status::payload_too_large
                : uvp::http::status::bad_request;
              res.status(status).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "multipart body is too large\n");

  UVP_CHECK(received.find("HTTP/1.1 413 Payload Too Large\r\n") != std::string::npos);
  UVP_CHECK(received.find("ok\n") == std::string::npos);
}

UVP_TEST_CASE("http server uses multipart stream total limits instead of server body default") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "abcdef\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    uvp::http::server_options{}.max_body_bytes(24),
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_stream{}.max_total_bytes(1024),
        [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
          multipart
            .on_part([](uvp::http::multipart_part& part) {
              part.discard();
            })
            .on_end([&res] {
              res.text("ok\n");
            })
            .on_error([&res](uvp::error error) {
              res.status(uvp::http::status::bad_request).text(error.detail + "\n");
            });
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "ok\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("HTTP/1.1 413 Payload Too Large\r\n") == std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nok\n") != std::string::npos);
}

UVP_TEST_CASE("http server collects multipart form-data bodies") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"title\"\r\n"
    "\r\n"
    "hello\r\n"
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"tag\"\r\n"
    "\r\n"
    "one\r\n"
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"tag\"\r\n"
    "\r\n"
    "two\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_form{},
        [](uvp::http::request&, uvp::http::response& res, const uvp::http::multipart_form& form) {
          const auto title = form.single_field("title");
          UVP_REQUIRE(title);
          const auto tags = form.fields("tag");
          UVP_REQUIRE(tags.size() == 2U);
          res.text(std::string{title.value().text()} + ":" + std::string{tags[0].text()} + "," + std::string{tags[1].text()} + "\n");
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "hello:one,two\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nhello:one,two\n") != std::string::npos);
}

UVP_TEST_CASE("http server rejects multipart form files by default") {
  const std::string body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"note.txt\"\r\n"
    "\r\n"
    "abc\r\n"
    "--AaB03x--\r\n";

  const auto received = perform_http_request(
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::body::multipart_form{},
        [](uvp::http::request&, uvp::http::response& res, const uvp::http::multipart_form&) {
          res.text("handler\n");
        });
    },
    std::string{
      "POST /upload HTTP/1.1\r\n"
      "Host: example.test\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/form-data; boundary=AaB03x\r\n"
      "Content-Length: "} + std::to_string(body.size()) + "\r\n"
      "\r\n" + body,
    "multipart files are not accepted\n");

  UVP_CHECK(received.find("HTTP/1.1 413 Payload Too Large\r\n") != std::string::npos);
  UVP_CHECK(received.find("handler\n") == std::string::npos);
}

UVP_TEST_CASE("http server returns request timeout for incomplete headers") {
  const auto received = perform_http_request_until_close(
    uvp::http::server_options{}.header_timeout(std::chrono::milliseconds{20}),
    [](uvp::http::server&) {},
    "GET /slow HTTP/1.1\r\nHost");

  UVP_CHECK(received.find("HTTP/1.1 408 Request Timeout\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nRequest Timeout\n") != std::string::npos);
}

UVP_TEST_CASE("http server closes slow request bodies using route timeout") {
  const auto received = perform_http_request_until_close(
    uvp::http::server_options{}.body_timeout(std::chrono::seconds{1}),
    [](uvp::http::server& server) {
      server.post(
        "/upload",
        uvp::http::route_options{}.body_timeout(std::chrono::milliseconds{20}),
        uvp::http::body::text{},
        [](uvp::http::request&, uvp::http::response& res, std::string_view) {
          res.text("accepted\n");
        });
    },
    "POST /upload HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "Content-Length: 4\r\n"
    "\r\n");

  UVP_CHECK(received.find("accepted\n") == std::string::npos);
}

UVP_TEST_CASE("http server closes idle keep-alive connections") {
  const auto received = perform_http_request_until_close(
    uvp::http::server_options{}.idle_timeout(std::chrono::milliseconds{20}),
    [](uvp::http::server& server) {
      server.get("/hello", [](uvp::http::request&, uvp::http::response& res) {
        res.text("hello\n");
      });
    },
    "GET /hello HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "\r\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("connection: keep-alive\r\n") != std::string::npos);
  UVP_CHECK(received.find("\r\n\r\nhello\n") != std::string::npos);
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

UVP_TEST_CASE("http server keeps response hook handles stable for deferred responses") {
  std::size_t captured_hook_calls = 0;
  std::size_t late_hook_calls = 0;

  const auto received = perform_http_request(
    [&](uvp::http::server& server) {
      server.on_response([&](const uvp::http::response_info&) {
        ++captured_hook_calls;
      });
      server.get("/later", [&](uvp::http::request&, uvp::http::response& res) {
        auto reply = res.defer();
        for (int i = 0; i < 32; ++i) {
          server.on_response([&](const uvp::http::response_info&) {
            ++late_hook_calls;
          });
        }
        reply.text("stable\n");
      });
    },
    "GET /later HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Connection: close\r\n"
    "\r\n",
    "stable\n");

  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK_EQ(captured_hook_calls, 1U);
  UVP_CHECK_EQ(late_hook_calls, 0U);
}
