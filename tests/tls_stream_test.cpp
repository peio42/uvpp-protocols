#include "test.hpp"

#include <uvpp/protocols/http.hpp>
#include <uvpp/protocols/io.hpp>
#include <uvpp/protocols/tls.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <uvpp/uv.hpp>

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

struct memory_endpoint {
  uv::loop* loop = nullptr;
  uvp::io::read_callback on_read;
  std::deque<std::vector<std::byte>> pending_reads;
  std::weak_ptr<memory_endpoint> peer;
  bool closed = false;
};

class tcp_client_stream final : public uvp::io::byte_stream::concept_ {
public:
  tcp_client_stream(uv::loop& loop, std::shared_ptr<uv::tcp> tcp)
      : loop_(&loop), tcp_(std::move(tcp)) {}

  uv::loop& loop() noexcept override {
    return *loop_;
  }

  void read_start(uvp::io::read_callback on_read) override {
    on_read_ = std::move(on_read);
    tcp_->read_start(
      [this](uv::tcp&, std::size_t suggested_size) {
        read_buffer_.resize(std::max<std::size_t>(suggested_size, 4096));
        return uv::buffer_view{read_buffer_.data(), read_buffer_.size()};
      },
      [this](uv::tcp&, uv::read_result result) {
        if (!on_read_) {
          return;
        }

        if (result.eof()) {
          on_read_(uvp::io::read_result{{}, {}, true});
          return;
        }

        if (!result) {
          on_read_(uvp::io::read_result{{}, uvp::io::stream_error{result.status().error_code()}});
          return;
        }

        on_read_(uvp::io::read_result{result.bytes()});
      });
  }

  void read_stop() override {
    tcp_->read_stop();
  }

  void write(std::span<const std::byte> bytes, uvp::io::write_callback on_write) override {
    auto op = std::make_shared<write_operation>(bytes);
    tcp_->write(op->request, op->payload, [op, callback = std::move(on_write)](uv::write_request&, uv::result result) mutable {
      if (callback) {
        callback(uvp::io::stream_error{result.error_code()});
      }
    });
  }

  void close(uvp::io::close_callback on_close) override {
    if (closed_) {
      if (on_close) {
        on_close();
      }
      return;
    }

    closed_ = true;
    if (tcp_->closing()) {
      if (on_close) {
        on_close();
      }
      return;
    }

    tcp_->close([callback = std::move(on_close)](uv::tcp&) mutable {
      if (callback) {
        callback();
      }
    });
  }

  uvp::io::endpoint local_endpoint() const override {
    return {};
  }

  uvp::io::endpoint remote_endpoint() const override {
    return {};
  }

  uv::tcp* tcp() noexcept override {
    return tcp_.get();
  }

  uv::pipe* pipe() noexcept override {
    return nullptr;
  }

private:
  struct write_operation {
    explicit write_operation(std::span<const std::byte> bytes)
        : payload(bytes.begin(), bytes.end()) {}

    uv::write_request request;
    std::vector<std::byte> payload;
  };

  uv::loop* loop_;
  std::shared_ptr<uv::tcp> tcp_;
  uvp::io::read_callback on_read_;
  std::vector<char> read_buffer_;
  bool closed_ = false;
};

class memory_stream final : public uvp::io::byte_stream::concept_ {
public:
  explicit memory_stream(std::shared_ptr<memory_endpoint> state)
      : state_(std::move(state)) {}

  uv::loop& loop() noexcept override {
    return *state_->loop;
  }

  void read_start(uvp::io::read_callback on_read) override {
    state_->on_read = std::move(on_read);
    while (state_->on_read && !state_->pending_reads.empty()) {
      auto payload = std::move(state_->pending_reads.front());
      state_->pending_reads.pop_front();
      state_->on_read(uvp::io::read_result{payload});
    }
  }

  void read_stop() override {
    state_->on_read = {};
  }

  void write(std::span<const std::byte> bytes, uvp::io::write_callback on_write) override {
    auto peer = state_->peer.lock();
    if (!peer || peer->closed) {
      if (on_write) {
        on_write(uvp::io::stream_error{std::make_error_code(std::errc::broken_pipe)});
      }
      return;
    }

    auto payload = std::vector<std::byte>(bytes.begin(), bytes.end());
    if (peer->on_read) {
      peer->on_read(uvp::io::read_result{payload});
    } else {
      peer->pending_reads.push_back(std::move(payload));
    }

    if (on_write) {
      on_write({});
    }
  }

  void close(uvp::io::close_callback on_close) override {
    state_->closed = true;
    auto peer = state_->peer.lock();
    if (peer && peer->on_read) {
      peer->on_read(uvp::io::read_result{{}, {}, true});
    }
    if (on_close) {
      on_close();
    }
  }

  uvp::io::endpoint local_endpoint() const override {
    return {};
  }

  uvp::io::endpoint remote_endpoint() const override {
    return {};
  }

  uv::tcp* tcp() noexcept override {
    return nullptr;
  }

  uv::pipe* pipe() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<memory_endpoint> state_;
};

std::pair<uvp::io::byte_stream, uvp::io::byte_stream> memory_pair(uv::loop& loop) {
  auto first = std::make_shared<memory_endpoint>();
  auto second = std::make_shared<memory_endpoint>();
  first->loop = &loop;
  second->loop = &loop;
  first->peer = second;
  second->peer = first;

  return {
    uvp::io::byte_stream{std::make_unique<memory_stream>(std::move(first))},
    uvp::io::byte_stream{std::make_unique<memory_stream>(std::move(second))},
  };
}

std::filesystem::path write_test_file(std::string_view name, std::string_view content) {
  auto path = std::filesystem::temp_directory_path() / name;
  auto file = std::ofstream(path);
  file << content;
  return path;
}

} // namespace

UVP_TEST_CASE("tls stream handshakes over byte streams and exchanges data") {
  uv::loop loop;
  auto [server_lower, client_lower] = memory_pair(loop);

  const auto cert_path = write_test_file("uvpp-protocols-test-cert.pem", test_certificate);
  const auto key_path = write_test_file("uvpp-protocols-test-key.pem", test_private_key);

  auto server_context = uvp::tls::server_context{}
    .certificate_chain_file(cert_path.string())
    .private_key_file(key_path.string())
    .alpn({"http/1.1"});

  auto client_context = uvp::tls::client_context{}
    .insecure_no_verify_peer()
    .alpn({"http/1.1"});

  auto server_done = false;
  auto client_done = false;
  auto server_alpn = std::string{};
  auto client_alpn = std::string{};
  auto server_stream = uvp::io::byte_stream{};
  auto client_stream = uvp::io::byte_stream{};

  uvp::tls::accept(std::move(server_lower), server_context, [&](uvp::tls::handshake_result result) {
    UVP_REQUIRE(result);
    server_alpn = std::string(result.selected_alpn());
    server_stream = std::move(result).stream();
    server_done = true;
  });

  uvp::tls::connect(std::move(client_lower), client_context, [&](uvp::tls::handshake_result result) {
    UVP_REQUIRE(result);
    client_alpn = std::string(result.selected_alpn());
    client_stream = std::move(result).stream();
    client_done = true;
  });

  UVP_REQUIRE(server_done);
  UVP_REQUIRE(client_done);
  UVP_CHECK_EQ(server_alpn, "http/1.1");
  UVP_CHECK_EQ(client_alpn, "http/1.1");

  auto received = std::string{};
  server_stream.read_start([&](uvp::io::read_result result) {
    UVP_REQUIRE(result);
    auto bytes = result.bytes();
    received.assign(
      reinterpret_cast<const char*>(bytes.data()),
      bytes.size());
  });

  auto write_done = false;
  const auto message = std::string{"ping"};
  client_stream.write(
    std::as_bytes(std::span{message.data(), message.size()}),
    [&](uvp::io::stream_error error) {
      UVP_CHECK(!error);
      write_done = true;
    });

  UVP_CHECK(write_done);
  UVP_CHECK_EQ(received, "ping");
}

UVP_TEST_CASE("tls listener adapts generic stream listener") {
  uv::loop loop;

  const auto cert_path = write_test_file("uvpp-protocols-test-cert.pem", test_certificate);
  const auto key_path = write_test_file("uvpp-protocols-test-key.pem", test_private_key);

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
        .handshake_timeout(std::chrono::milliseconds{100})
        .max_pending_handshakes(8)}};

  UVP_CHECK(static_cast<bool>(secure));
  UVP_CHECK(std::holds_alternative<uvp::io::tcp_endpoint>(secure.local_endpoint()));
}

UVP_TEST_CASE("http server serves requests over tls listener composition") {
  uv::loop loop;
  uvp::http::server server(loop);

  server.get("/secure", [](uvp::http::request&, uvp::http::response& res) {
    res.type("text/plain").text("tls ok");
  });

  const auto cert_path = write_test_file("uvpp-protocols-test-cert.pem", test_certificate);
  const auto key_path = write_test_file("uvpp-protocols-test-key.pem", test_private_key);

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

  auto client = std::make_shared<uv::tcp>(loop);
  uv::connect_request connect_request;
  uv::timer timeout(loop);

  auto tls_done = false;
  auto write_done = false;
  auto response_seen = false;
  auto client_closed = false;
  auto timed_out = false;
  auto received = std::string{};
  auto clear = uvp::io::byte_stream{};

  timeout.start(std::chrono::seconds{3}, [&](uv::timer& timer) {
    timed_out = true;
    server.close();
    if (!client_closed && !client->closing()) {
      client->close([&](uv::tcp&) {
        client_closed = true;
      });
    }
    timer.close();
  });

  client->connect(
    connect_request,
    uv::ipv4{"127.0.0.1", static_cast<int>(port)},
    [&](uv::connect_request&, uv::result status) {
      UVP_REQUIRE(status);

      auto lower = uvp::io::byte_stream{std::make_unique<tcp_client_stream>(loop, client)};
      auto client_context = uvp::tls::client_context{}
        .insecure_no_verify_peer()
        .alpn({"http/1.1"});

      uvp::tls::connect(std::move(lower), client_context, [&](uvp::tls::handshake_result handshake) {
        UVP_REQUIRE(handshake);
        UVP_CHECK_EQ(std::string(handshake.selected_alpn()), "http/1.1");
        clear = std::move(handshake).stream();
        tls_done = true;

        clear.read_start([&](uvp::io::read_result read) {
          if (read.eof()) {
            return;
          }
          UVP_REQUIRE(read);

          const auto bytes = read.bytes();
          received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
          if (received.find("\r\n\r\ntls ok") != std::string::npos) {
            response_seen = true;
            clear.read_stop();
            timeout.close();
            clear.close([&] {
              client_closed = true;
              server.close();
            });
          }
        });

        const auto request = std::string{
          "GET /secure HTTP/1.1\r\n"
          "Host: localhost\r\n"
          "Connection: close\r\n"
          "\r\n"};
        clear.write(
          std::as_bytes(std::span{request.data(), request.size()}),
          [&](uvp::io::stream_error error) {
            UVP_CHECK(!error);
            write_done = true;
          });
      });
    });

  loop.run();

  UVP_CHECK(!timed_out);
  UVP_CHECK(tls_done);
  UVP_CHECK(write_done);
  UVP_CHECK(response_seen);
  UVP_CHECK(client_closed);
  UVP_CHECK(received.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-type: text/plain; charset=utf-8\r\n") != std::string::npos);
  UVP_CHECK(received.find("content-length: 6\r\n") != std::string::npos);

  loop.close();
}
