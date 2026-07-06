#include "test.hpp"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <uvpp/protocols/http.hpp>
#include <uvpp/protocols/tls.hpp>
#include <uvpp/uv.hpp>

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace {

constexpr auto test_certificate = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUeCbcn8RBu5za2QE14pO+pVrZ0jIwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDcwNTExMDc0MFoXDTM2MDcw
MjExMDc0MFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAw6pTRpJrXyGV95CIl18Voh3UZsqyM+EEKK08tJCoG6Q+
etlNKe7Z8gSfXvYdOG9jYQwT9W+eeLQyk8EPgr9gPxxu2Yg4DvVTC5naSu+Cx122
uAlZ46FOecAhpcamuC1Ie0/CwbS7QI65Cra150asIZW9Cg0WaIU19OCD8wMallgM
2BPk0mWSUjFTVt0v2hUGITarWig4NYHj5vWW00yjFOs1LxV8y6bM70judC/g/2Z7
RTuRk1+vyUfoW8Q+soTaYbClws6uK1FBK+gA88RKZbi17vKavWjh3wrNkDHnPhD6
vUo5vnTeK4luDuf72x7cvDV5vpamqodK9UW3j4zxAQIDAQABo1MwUTAdBgNVHQ4E
FgQUM9+pT084oqrPS2tNVSMRuLFTZH4wHwYDVR0jBBgwFoAUM9+pT084oqrPS2tN
VSMRuLFTZH4wDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAMrTm
gRUcFJvFlHiEB3Nxd4BMPcMA+ci02zXhn8SF1oN1udpcNwzU6oJ+xHQH0Af52dUg
m00dRA0DnjsI3PU4f4WQ8Dx1/d+/29xT3AIaDQgO0B2VmKX0BTaJzJVIzV/9iOFn
BLfczzMRL36SL+Obb87jsVMAoXP7KGtp5qTAwctZ+kC/K75D71q3I6gt7OL/2P8C
EJ3VXt1Ni649srglFWs5ravJbIc7gmR9F5jFT6yTCyw+tLtw8WpA1cbEXJrkAXre
Np97aGfgHVw5AdhG9vf4BGO/WvT4ZSdaRaFUYBiF1SJyNzizyor112/UCdaxgFgr
7IVpk0bnemE5dOl/sw==
-----END CERTIFICATE-----
)";

constexpr auto test_private_key = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDDqlNGkmtfIZX3
kIiXXxWiHdRmyrIz4QQorTy0kKgbpD562U0p7tnyBJ9e9h04b2NhDBP1b554tDKT
wQ+Cv2A/HG7ZiDgO9VMLmdpK74LHXba4CVnjoU55wCGlxqa4LUh7T8LBtLtAjrkK
trXnRqwhlb0KDRZohTX04IPzAxqWWAzYE+TSZZJSMVNW3S/aFQYhNqtaKDg1gePm
9ZbTTKMU6zUvFXzLpszvSO50L+D/ZntFO5GTX6/JR+hbxD6yhNphsKXCzq4rUUEr
6ADzxEpluLXu8pq9aOHfCs2QMec+EPq9Sjm+dN4riW4O5/vbHty8NXm+lqaqh0r1
RbePjPEBAgMBAAECggEABgU5H7xEmn49j4r+cO3ni//v96O3/Pmo95lw+ztSONuC
YqRKCAbF5Pj4cGMRPhnLTIKjIJOpJByjS8GOeR7rVrXIwV+8HdW1ku9OdKzO8NR0
2U/MWMEvWXascl3c5mDaUJUBMJWfh1p83hQGH9IgXL4vPV5uuJOUt+6qkLEhQcvw
6uDN/tcxiCodCvTP5n5Zq/aUljbXWYMrzh5HJ3rVrzNTvc9uiTGNqC9qfC0EEs2p
vUjpJrPe3KlVtlv0K3f2XUYOgQht3Rm5100R9MhYZ+D/ulo0POZYWcS3JqBMt01D
/kA4KqBZlpRR/SH9nf2fixVJCZzCyvOTS9GJGCBsKwKBgQD5RiavdNKmbaNOPjsC
N2pkuwiBlHfsGRh/K1kRXdu0KPXp0tRpppM67Z7katRR80fj7rr+iv9E3qpAF2dx
Qa+KqiYPTugXwHQ1jEjYq2em2wbKRs8JB2R+nyWOowL0T5W9PvaCNn7KZ0dm4Gtl
+V4tf6jaMrJkxQ91vIOqVsGLwwKBgQDI8d/k/G5gEo6AW78UBAHjsS1lJg4LMXuy
pSFXuRu6Kx2b9TXLp5KKvWr8UGM1MBSetTCaMO9r7ZMCYPggGMZaKRxLSRAM/Mi9
i1IQf2vJJ57dM/xkkO+54V7wriheERQNwda+FrenGqkrHXpEwu+nFdm7THDel6mx
n/3rv8536wKBgF2uhZ9vMjOmBLfFH3wnw25z9DBu0dsDW3d/nQuv0IAW3MSxnW7P
UYnV/98sXvsliSEaeWBscJ87Z5SKty+TVhuw8njSWNuEUqhFPqNfV6cXraebkPd9
tcD4oq7GiLe0qTvkS9SIEoKS6fy53uMGIuTKk3TdlLnWbYb8ACemTzrtAoGBAI1X
pRcagEDPjLC42BSqJPIVpEqrk+FHsyybfnKH3/r5bOBQgMB5ZFh2mBRWLxIwebCQ
3lj25tHR0EAyGRXql0q/9Aj4oXOhM0ov/09fcV+SoOoTMQtD73ueDPvaZMaV2Lc8
i2I19IRz+l47Y8+OFqg+dGKMiC/qGhC46xCyX/Z1AoGBAIABbh2187I4SoOVbLKG
t/PpNQ7uV5WVnWrGyng7UXnVTTOjAeSdPvX4VQIKL9DPa7V22SnYkPSjelTsl2Kg
U7vDLTZ/3gZPWdGQpwmNkmbHyIJ62cXvHUObmW3bmd4lFtjWXE4s3w2/ewLLgAFg
Jmcgq7zyPeY+rXdqNowARHbF
-----END PRIVATE KEY-----
)";

std::vector<std::byte> bytes(std::string_view text) {
  auto out = std::vector<std::byte>(text.size());
  std::memcpy(out.data(), text.data(), text.size());
  return out;
}

std::filesystem::path test_temp_directory() {
  static const auto directory = [] {
    const auto base = std::filesystem::temp_directory_path();
    for (auto index = 0; index != 1000; ++index) {
      auto candidate = base / ("uvpp-protocols-http-client-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(index));
      auto error = std::error_code{};
      if (std::filesystem::create_directory(candidate, error)) {
        return candidate;
      }
      if (error && !std::filesystem::exists(candidate)) {
        throw std::filesystem::filesystem_error{"failed to create HTTP client test temp directory", candidate, error};
      }
    }

    throw std::runtime_error{"failed to allocate unique HTTP client test temp directory"};
  }();

  return directory;
}

std::filesystem::path write_test_file(std::string_view name, std::string_view content) {
  auto path = test_temp_directory() / name;
  auto file = std::ofstream(path);
  file << content;
  return path;
}

template<class Assert>
void run_raw_client_response(
  std::string_view wire_response,
  uvp::http::method request_method,
  uvp::http::client_options options,
  Assert assert_result) {
  uv::loop loop;

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp)};
  const auto port = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint()).port;
  std::optional<uvp::io::byte_stream> accepted;
  auto payload = bytes(wire_response);

  listener.listen([&](uvp::io::accept_result result) {
    UVP_REQUIRE(result);
    accepted.emplace(std::move(result).stream());
    accepted->write(payload, [&](uvp::io::stream_error error) {
      UVP_CHECK(!error);
      accepted->close([&] {
        accepted.reset();
      });
    });
  });

  uvp::http::client client(loop, std::move(options));
  auto completed = false;
  auto request = client.fetch(
    request_method,
    "http://127.0.0.1:" + std::to_string(port) + "/raw",
    [&](uvp::result<uvp::http::response> result) {
      completed = true;
      assert_result(std::move(result));
      listener.close();
    });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

template<class Assert>
void run_raw_client_response(std::string_view wire_response, Assert assert_result) {
  run_raw_client_response(
    wire_response,
    uvp::http::method::get,
    uvp::http::client_options{},
    std::move(assert_result));
}

template<class Configure>
void run_raw_streaming_response(std::string_view wire_response, Configure configure_request) {
  uv::loop loop;

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp)};
  const auto port = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint()).port;
  std::optional<uvp::io::byte_stream> accepted;
  auto payload = bytes(wire_response);

  listener.listen([&](uvp::io::accept_result result) {
    UVP_REQUIRE(result);
    accepted.emplace(std::move(result).stream());
    accepted->write(payload, [&](uvp::io::stream_error error) {
      UVP_CHECK(!error);
      accepted->close([&] {
        accepted.reset();
      });
    });
  });

  uvp::http::client client(loop);
  auto request = client.stream_get("http://127.0.0.1:" + std::to_string(port) + "/stream");
  configure_request(request, listener);
  auto operation = request.start();

  UVP_CHECK(operation.valid());
  loop.run();
  loop.close();
}

} // namespace

UVP_TEST_CASE("http client performs a plain get request") {
  uv::loop loop;

  uvp::http::server server(loop);
  server.get("/hello", [](uvp::http::request&, uvp::http::response& res) {
    res.header("x-test", "ok");
    res.text("hello client");
  });

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp)};
  const auto port = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint()).port;
  server.listen(std::move(listener));

  uvp::http::client client(loop);
  auto completed = false;
  auto request = client.get(
    "http://127.0.0.1:" + std::to_string(port) + "/hello",
    [&](uvp::result<uvp::http::response> result) {
      completed = true;
      UVP_REQUIRE(result);
      UVP_CHECK_EQ(result.value().status_code(), 200U);
      UVP_CHECK_EQ(result.value().headers().get("x-test"), "ok");
      UVP_CHECK_EQ(result.value().body(), "hello client");
      server.close();
    });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("http client performs an https get request") {
  uv::loop loop;

  uvp::http::server server(loop);
  server.get("/secure", [](uvp::http::request&, uvp::http::response& res) {
    res.header("x-secure", "yes");
    res.text("hello tls client");
  });

  const auto cert_path = write_test_file("https-client-cert.pem", test_certificate);
  const auto key_path = write_test_file("https-client-key.pem", test_private_key);

  auto context = uvp::tls::server_context{}
    .certificate_chain_file(cert_path.string())
    .private_key_file(key_path.string())
    .alpn({"http/1.1"});

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto secure = uvp::io::stream_listener{
    uvp::tls::listener{
      uvp::io::stream_listener{std::move(tcp)},
      std::move(context),
      uvp::tls::listener_options{}
        .handshake_timeout(std::chrono::seconds{2})
        .max_pending_handshakes(8)}};
  const auto port = std::get<uvp::io::tcp_endpoint>(secure.local_endpoint()).port;
  server.listen(std::move(secure));

  uvp::http::client client(
    loop,
    uvp::http::client_options{
      .connect_timeout = std::chrono::seconds{2},
      .response_header_timeout = std::chrono::seconds{2},
      .response_body_timeout = std::chrono::seconds{2},
      .tls_default_verify_paths = false,
      .tls_ca_file = cert_path.string(),
    });

  auto completed = false;
  auto request = client.get(
    "https://localhost:" + std::to_string(port) + "/secure",
    [&](uvp::result<uvp::http::response> result) {
      completed = true;
      UVP_REQUIRE(result);
      UVP_CHECK_EQ(result.value().status_code(), 200U);
      UVP_CHECK_EQ(result.value().headers().get("x-secure"), "yes");
      UVP_CHECK_EQ(result.value().body(), "hello tls client");
      server.close();
    });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("http client rejects unsupported schemes") {
  uv::loop loop;
  uvp::http::client client(loop);

  auto completed = false;
  auto request = client.get("ftp://example.com/", [&](uvp::result<uvp::http::response> result) {
    completed = true;
    UVP_CHECK(!result);
    UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_unsupported_scheme);
  });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("http client rejects malformed response status line") {
  run_raw_client_response("NOTHTTP\r\nContent-Length: 0\r\n\r\n", [](uvp::result<uvp::http::response> result) {
    UVP_CHECK(!result);
    UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_malformed_response);
  });
}

UVP_TEST_CASE("http client rejects malformed response header line") {
  run_raw_client_response("HTTP/1.1 200 OK\r\nbroken-header\r\n\r\n", [](uvp::result<uvp::http::response> result) {
    UVP_CHECK(!result);
    UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_malformed_response);
  });
}

UVP_TEST_CASE("http client enforces response header limit") {
  run_raw_client_response(
    "HTTP/1.1 200 OK\r\nX-Large: abcdefghijklmnopqrstuvwxyz\r\n\r\n",
    uvp::http::method::get,
    uvp::http::client_options{.max_header_bytes = 24},
    [](uvp::result<uvp::http::response> result) {
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_header_limit_exceeded);
    });
}

UVP_TEST_CASE("http client enforces response body limit") {
  run_raw_client_response(
    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nabcdef",
    uvp::http::method::get,
    uvp::http::client_options{.max_body_bytes = 5},
    [](uvp::result<uvp::http::response> result) {
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_body_limit_exceeded);
    });
}

UVP_TEST_CASE("http client accepts eof-delimited response bodies") {
  run_raw_client_response("HTTP/1.1 200 OK\r\n\r\nhello eof", [](uvp::result<uvp::http::response> result) {
    UVP_REQUIRE(result);
    UVP_CHECK_EQ(result.value().status_code(), 200U);
    UVP_CHECK_EQ(result.value().body(), "hello eof");
  });
}

UVP_TEST_CASE("http client decodes chunked responses with extensions and trailers") {
  run_raw_client_response(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
    "5;kind=text\r\nhello\r\n"
    "6\r\n world\r\n"
    "0\r\nX-Trailer: yes\r\n\r\n",
    [](uvp::result<uvp::http::response> result) {
      UVP_REQUIRE(result);
      UVP_CHECK_EQ(result.value().body(), "hello world");
    });
}

UVP_TEST_CASE("http client rejects invalid chunk sizes") {
  run_raw_client_response(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
    "zz\r\nhello\r\n0\r\n\r\n",
    [](uvp::result<uvp::http::response> result) {
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_malformed_response);
    });
}

UVP_TEST_CASE("http client rejects unterminated chunk bodies") {
  run_raw_client_response(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
    "5\r\nhello0\r\n\r\n",
    [](uvp::result<uvp::http::response> result) {
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_malformed_response);
    });
}

UVP_TEST_CASE("http client treats head responses as bodyless") {
  run_raw_client_response(
    "HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nignored body",
    uvp::http::method::head,
    uvp::http::client_options{},
    [](uvp::result<uvp::http::response> result) {
      UVP_REQUIRE(result);
      UVP_CHECK_EQ(result.value().status_code(), 200U);
      UVP_CHECK_EQ(result.value().body(), "");
    });
}

UVP_TEST_CASE("http client treats 204 and 304 responses as bodyless") {
  run_raw_client_response(
    "HTTP/1.1 204 No Content\r\nContent-Length: 12\r\n\r\nignored body",
    [](uvp::result<uvp::http::response> result) {
      UVP_REQUIRE(result);
      UVP_CHECK_EQ(result.value().status_code(), 204U);
      UVP_CHECK_EQ(result.value().body(), "");
    });

  run_raw_client_response(
    "HTTP/1.1 304 Not Modified\r\nContent-Length: 12\r\n\r\nignored body",
    [](uvp::result<uvp::http::response> result) {
      UVP_REQUIRE(result);
      UVP_CHECK_EQ(result.value().status_code(), 304U);
      UVP_CHECK_EQ(result.value().body(), "");
    });
}

UVP_TEST_CASE("http client streams content-length response bodies") {
  auto headers_seen = false;
  auto completed = false;
  auto body = std::string{};

  run_raw_streaming_response(
    "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nX-Mode: stream\r\n\r\nhello stream",
    [&](uvp::http::streaming_request& request, uvp::io::stream_listener& listener) {
      request
        .on_response_headers([&](const uvp::http::response_head& head) {
          headers_seen = true;
          UVP_CHECK_EQ(head.status_code, 200U);
          UVP_CHECK_EQ(head.headers.get("x-mode"), "stream");
        })
        .on_data([&](std::span<const std::byte> chunk) {
          body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        })
        .on_complete([&](uvp::result<void> done) {
          completed = true;
          UVP_REQUIRE(done);
          listener.close();
        });
    });

  UVP_CHECK(headers_seen);
  UVP_CHECK(completed);
  UVP_CHECK_EQ(body, "hello stream");
}

UVP_TEST_CASE("http client streams chunked response bodies") {
  auto completed = false;
  auto chunks = 0;
  auto body = std::string{};

  run_raw_streaming_response(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
    "5\r\nhello\r\n"
    "1\r\n \r\n"
    "6\r\nstream\r\n"
    "0\r\n\r\n",
    [&](uvp::http::streaming_request& request, uvp::io::stream_listener& listener) {
      request
        .on_data([&](std::span<const std::byte> chunk) {
          ++chunks;
          body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        })
        .on_complete([&](uvp::result<void> done) {
          completed = true;
          UVP_REQUIRE(done);
          listener.close();
        });
    });

  UVP_CHECK(completed);
  UVP_CHECK_EQ(chunks, 3);
  UVP_CHECK_EQ(body, "hello stream");
}

UVP_TEST_CASE("http client streams eof-delimited response bodies") {
  auto completed = false;
  auto body = std::string{};

  run_raw_streaming_response(
    "HTTP/1.1 200 OK\r\n\r\neof stream",
    [&](uvp::http::streaming_request& request, uvp::io::stream_listener& listener) {
      request
        .on_data([&](std::span<const std::byte> chunk) {
          body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        })
        .on_complete([&](uvp::result<void> done) {
          completed = true;
          UVP_REQUIRE(done);
          listener.close();
        });
    });

  UVP_CHECK(completed);
  UVP_CHECK_EQ(body, "eof stream");
}

UVP_TEST_CASE("http client reports streaming malformed responses on complete") {
  auto completed = false;
  auto saw_data = false;

  run_raw_streaming_response(
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
    "nope\r\n",
    [&](uvp::http::streaming_request& request, uvp::io::stream_listener& listener) {
      request
        .on_data([&](std::span<const std::byte>) {
          saw_data = true;
        })
        .on_complete([&](uvp::result<void> done) {
          completed = true;
          UVP_CHECK(!done);
          UVP_CHECK_EQ(done.error().code, uvp::http::errc::client_malformed_response);
          listener.close();
        });
    });

  UVP_CHECK(completed);
  UVP_CHECK(!saw_data);
}

UVP_TEST_CASE("http client times out waiting for response headers") {
  uv::loop loop;

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp)};
  const auto port = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint()).port;
  std::optional<uvp::io::byte_stream> accepted;

  listener.listen([&](uvp::io::accept_result result) {
    UVP_REQUIRE(result);
    accepted.emplace(std::move(result).stream());
  });

  uvp::http::client client(
    loop,
    uvp::http::client_options{
      .response_header_timeout = std::chrono::milliseconds{10},
    });

  auto completed = false;
  auto request = client.get(
    "http://127.0.0.1:" + std::to_string(port) + "/slow",
    [&](uvp::result<uvp::http::response> result) {
      completed = true;
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_timeout);
      if (accepted) {
        accepted->close([&] {
          accepted.reset();
          listener.close();
        });
      } else {
        listener.close();
      }
    });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}

UVP_TEST_CASE("http client times out waiting for response body") {
  uv::loop loop;

  auto tcp = uvp::io::tcp_listener{loop};
  tcp.bind("127.0.0.1", 0);
  auto listener = uvp::io::stream_listener{std::move(tcp)};
  const auto port = std::get<uvp::io::tcp_endpoint>(listener.local_endpoint()).port;
  std::optional<uvp::io::byte_stream> accepted;
  auto header_bytes = bytes("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n");

  listener.listen([&](uvp::io::accept_result result) {
    UVP_REQUIRE(result);
    accepted.emplace(std::move(result).stream());
    accepted->write(header_bytes, [](uvp::io::stream_error error) {
      UVP_CHECK(!error);
    });
  });

  uvp::http::client client(
    loop,
    uvp::http::client_options{
      .response_body_timeout = std::chrono::milliseconds{10},
    });

  auto completed = false;
  auto request = client.get(
    "http://127.0.0.1:" + std::to_string(port) + "/slow-body",
    [&](uvp::result<uvp::http::response> result) {
      completed = true;
      UVP_CHECK(!result);
      UVP_CHECK_EQ(result.error().code, uvp::http::errc::client_timeout);
      if (accepted) {
        accepted->close([&] {
          accepted.reset();
          listener.close();
        });
      } else {
        listener.close();
      }
    });

  UVP_CHECK(request.valid());
  loop.run();
  loop.close();

  UVP_CHECK(completed);
}
