#include <uvpp/protocols/tls/stream.hpp>

#include "context_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace uvp::tls {

namespace {

constexpr std::size_t tls_io_buffer_size = 16 * 1024;

std::string drain_openssl_errors(std::string_view fallback) {
  std::string detail(fallback);
  bool first = true;

  for (auto code = ERR_get_error(); code != 0; code = ERR_get_error()) {
    char buffer[256]{};
    ERR_error_string_n(code, buffer, sizeof(buffer));
    detail += first ? ": " : "; ";
    detail += buffer;
    first = false;
  }

  return detail;
}

uvp::error make_tls_error(errc code, std::string detail) {
  return uvp::error{make_error_code(code), std::move(detail)};
}

class tls_state;

class tls_stream_model final : public uvp::io::byte_stream::concept_ {
public:
  explicit tls_stream_model(std::shared_ptr<tls_state> state);

  uv::loop& loop() noexcept override;
  void read_start(uvp::io::read_callback on_read) override;
  void read_stop() override;
  void write(std::span<const std::byte> bytes, uvp::io::write_callback on_write) override;
  void close(uvp::io::close_callback on_close) override;
  uvp::io::endpoint local_endpoint() const override;
  uvp::io::endpoint remote_endpoint() const override;
  uv::tcp* tcp() noexcept override;
  uv::pipe* pipe() noexcept override;

private:
  std::shared_ptr<tls_state> state_;
};

class tls_state final : public std::enable_shared_from_this<tls_state> {
public:
  enum class mode {
    server,
    client,
  };

  tls_state(uvp::io::byte_stream lower, SSL* ssl, mode direction, handshake_callback callback)
      : lower_(std::move(lower)),
        ssl_(ssl),
        direction_(direction),
        handshake_callback_(std::move(callback)) {}

  ~tls_state() {
    SSL_free(ssl_);
  }

  void start() {
    if (direction_ == mode::server) {
      SSL_set_accept_state(ssl_);
    } else {
      SSL_set_connect_state(ssl_);
    }

    auto self = shared_from_this();
    lower_.read_start([self](uvp::io::read_result result) {
      self->on_lower_read(std::move(result));
    });

    drive_handshake();
  }

  uv::loop& loop() noexcept {
    return lower_.loop();
  }

  void read_start(uvp::io::read_callback on_read) {
    on_read_ = std::move(on_read);
    deliver_pending_clear();
    if (open_) {
      read_clear_from_ssl();
    }
  }

  void read_stop() {
    on_read_ = {};
  }

  void write(std::span<const std::byte> bytes, uvp::io::write_callback on_write) {
    if (!open_ || closing_) {
      if (on_write) {
        on_write(uvp::io::stream_error{make_error_code(errc::closed)});
      }
      return;
    }

    if (bytes.empty()) {
      if (on_write) {
        on_write({});
      }
      return;
    }

    std::size_t written = 0;
    const auto ok = SSL_write_ex(
      ssl_,
      bytes.data(),
      bytes.size(),
      &written);

    if (ok != 1 || written != bytes.size()) {
      const auto ssl_error = SSL_get_error(ssl_, ok);
      if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        if (on_write) {
          on_write(uvp::io::stream_error{make_error_code(errc::protocol_error)});
        }
        return;
      }

      if (on_write) {
        on_write(uvp::io::stream_error{make_error_code(errc::protocol_error)});
      }
      fail(make_tls_error(errc::protocol_error, drain_openssl_errors("TLS write failed")));
      return;
    }

    drain_wbio([callback = std::move(on_write)](uvp::io::stream_error error) mutable {
      if (callback) {
        callback(std::move(error));
      }
    });
  }

  void close(uvp::io::close_callback on_close) {
    if (closed_) {
      if (on_close) {
        on_close();
      }
      return;
    }

    closing_ = true;
    on_close_ = std::move(on_close);

    if (ssl_) {
      const auto rc = SSL_shutdown(ssl_);
      if (rc < 0) {
        const auto ssl_error = SSL_get_error(ssl_, rc);
        if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
          ERR_clear_error();
        }
      }
      drain_wbio([self = shared_from_this()](uvp::io::stream_error) {
        self->close_lower();
      });
    } else {
      close_lower();
    }
  }

  uvp::io::endpoint local_endpoint() const {
    return lower_.local_endpoint();
  }

  uvp::io::endpoint remote_endpoint() const {
    return lower_.remote_endpoint();
  }

  uv::tcp* tcp() noexcept {
    return nullptr;
  }

  uv::pipe* pipe() noexcept {
    return nullptr;
  }

private:
  struct encrypted_write {
    std::vector<std::byte> payload;
    std::function<void(uvp::io::stream_error)> done;
  };

  void on_lower_read(uvp::io::read_result result) {
    if (closed_ || failed_) {
      return;
    }

    if (result.eof()) {
      if (!open_) {
        fail(make_tls_error(errc::handshake_failed, "TLS handshake ended before completion"));
        return;
      }

      if (on_read_) {
        on_read_(uvp::io::read_result{{}, {}, true});
      }
      return;
    }

    if (!result) {
      fail(uvp::error{result.error().code(), result.error().message()});
      return;
    }

    auto bytes = result.bytes();
    while (!bytes.empty()) {
      const auto written = BIO_write(SSL_get_rbio(ssl_), bytes.data(), static_cast<int>(bytes.size()));
      if (written <= 0) {
        fail(make_tls_error(errc::protocol_error, drain_openssl_errors("failed to feed TLS input")));
        return;
      }

      bytes = bytes.subspan(static_cast<std::size_t>(written));
    }

    if (!open_) {
      drive_handshake();
    }

    if (open_) {
      read_clear_from_ssl();
    }
  }

  void drive_handshake() {
    if (closed_ || failed_ || open_) {
      return;
    }

    const auto rc = SSL_do_handshake(ssl_);
    drain_wbio();

    if (rc == 1) {
      open_ = true;
      complete_handshake();
      read_clear_from_ssl();
      return;
    }

    const auto ssl_error = SSL_get_error(ssl_, rc);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      return;
    }

    const auto verify = SSL_get_verify_result(ssl_);
    if (verify != X509_V_OK) {
      fail(make_tls_error(errc::verification_failed, X509_verify_cert_error_string(verify)));
      return;
    }

    fail(make_tls_error(errc::handshake_failed, drain_openssl_errors("TLS handshake failed")));
  }

  void complete_handshake() {
    auto callback = std::move(handshake_callback_);
    handshake_callback_ = {};
    if (!callback) {
      return;
    }

    callback(handshake_result{
      uvp::io::byte_stream{std::make_unique<tls_stream_model>(shared_from_this())},
      selected_alpn()});
  }

  void read_clear_from_ssl() {
    if (!open_ || closed_ || failed_) {
      return;
    }

    for (;;) {
      clear_buffer_.resize(tls_io_buffer_size);
      std::size_t read = 0;
      const auto ok = SSL_read_ex(ssl_, clear_buffer_.data(), clear_buffer_.size(), &read);
      drain_wbio();

      if (ok == 1) {
        clear_buffer_.resize(read);
        queue_or_deliver_clear(std::move(clear_buffer_));
        clear_buffer_ = {};
        continue;
      }

      const auto ssl_error = SSL_get_error(ssl_, ok);
      if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        return;
      }

      if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        if (on_read_) {
          on_read_(uvp::io::read_result{{}, {}, true});
        }
        return;
      }

      fail(make_tls_error(errc::protocol_error, drain_openssl_errors("TLS read failed")));
      return;
    }
  }

  void queue_or_deliver_clear(std::vector<std::byte> bytes) {
    if (bytes.empty()) {
      return;
    }

    if (!on_read_) {
      pending_clear_.push_back(std::move(bytes));
      return;
    }

    on_read_(uvp::io::read_result{bytes});
  }

  void deliver_pending_clear() {
    while (on_read_ && !pending_clear_.empty()) {
      auto bytes = std::move(pending_clear_.front());
      pending_clear_.pop_front();
      on_read_(uvp::io::read_result{bytes});
    }
  }

  void drain_wbio(std::function<void(uvp::io::stream_error)> done = {}) {
    bool queued = false;

    for (;;) {
      std::vector<std::byte> encrypted(tls_io_buffer_size);
      const auto read = BIO_read(SSL_get_wbio(ssl_), encrypted.data(), static_cast<int>(encrypted.size()));
      if (read <= 0) {
        break;
      }

      encrypted.resize(static_cast<std::size_t>(read));
      encrypted_writes_.push_back(encrypted_write{std::move(encrypted), {}});
      queued = true;
    }

    if (done) {
      if (queued) {
        encrypted_writes_.back().done = std::move(done);
      } else {
        done({});
      }
    }

    flush_encrypted_writes();
  }

  void flush_encrypted_writes() {
    if (encrypted_write_active_ || encrypted_writes_.empty() || closed_) {
      return;
    }

    encrypted_write_active_ = true;
    auto self = shared_from_this();
    auto& item = encrypted_writes_.front();
    lower_.write(item.payload, [self](uvp::io::stream_error error) {
      auto done = std::move(self->encrypted_writes_.front().done);
      self->encrypted_writes_.pop_front();
      self->encrypted_write_active_ = false;

      if (error) {
        if (done) {
          done(error);
        }
        self->fail(uvp::error{error.code(), error.message()});
        return;
      }

      if (done) {
        done({});
      }

      self->flush_encrypted_writes();
    });
  }

  void fail(uvp::error error) {
    if (failed_ || closed_) {
      return;
    }

    failed_ = true;
    auto handshake = std::move(handshake_callback_);
    handshake_callback_ = {};
    if (handshake) {
      handshake(handshake_result{std::move(error)});
    } else if (on_read_) {
      on_read_(uvp::io::read_result{{}, uvp::io::stream_error{error.code}});
    }

    close_lower();
  }

  void close_lower() {
    if (closed_) {
      return;
    }

    closed_ = true;
    auto callback = std::move(on_close_);
    lower_.close([callback = std::move(callback)]() mutable {
      if (callback) {
        callback();
      }
    });
  }

  std::string selected_alpn() const {
    const unsigned char* data = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(ssl_, &data, &size);
    if (!data || size == 0) {
      return {};
    }

    return std::string(reinterpret_cast<const char*>(data), size);
  }

  uvp::io::byte_stream lower_;
  SSL* ssl_ = nullptr;
  mode direction_;
  handshake_callback handshake_callback_;
  uvp::io::read_callback on_read_;
  uvp::io::close_callback on_close_;
  std::deque<std::vector<std::byte>> pending_clear_;
  std::deque<encrypted_write> encrypted_writes_;
  std::vector<std::byte> clear_buffer_;
  bool encrypted_write_active_ = false;
  bool open_ = false;
  bool closing_ = false;
  bool closed_ = false;
  bool failed_ = false;
};

tls_stream_model::tls_stream_model(std::shared_ptr<tls_state> state)
    : state_(std::move(state)) {}

uv::loop& tls_stream_model::loop() noexcept {
  return state_->loop();
}

void tls_stream_model::read_start(uvp::io::read_callback on_read) {
  state_->read_start(std::move(on_read));
}

void tls_stream_model::read_stop() {
  state_->read_stop();
}

void tls_stream_model::write(std::span<const std::byte> bytes, uvp::io::write_callback on_write) {
  state_->write(bytes, std::move(on_write));
}

void tls_stream_model::close(uvp::io::close_callback on_close) {
  state_->close(std::move(on_close));
}

uvp::io::endpoint tls_stream_model::local_endpoint() const {
  return state_->local_endpoint();
}

uvp::io::endpoint tls_stream_model::remote_endpoint() const {
  return state_->remote_endpoint();
}

uv::tcp* tls_stream_model::tcp() noexcept {
  return state_->tcp();
}

uv::pipe* tls_stream_model::pipe() noexcept {
  return state_->pipe();
}

SSL* make_ssl(SSL_CTX* context) {
  auto* ssl = SSL_new(context);
  if (!ssl) {
    throw std::runtime_error(drain_openssl_errors("failed to create TLS session"));
  }

  auto* rbio = BIO_new(BIO_s_mem());
  auto* wbio = BIO_new(BIO_s_mem());
  if (!rbio || !wbio) {
    BIO_free(rbio);
    BIO_free(wbio);
    SSL_free(ssl);
    throw std::runtime_error(drain_openssl_errors("failed to create TLS BIOs"));
  }

  BIO_set_mem_eof_return(rbio, -1);
  BIO_set_mem_eof_return(wbio, -1);
  SSL_set_bio(ssl, rbio, wbio);
  return ssl;
}

void configure_client_ssl(SSL* ssl, const client_context& context) {
  const auto& alpn = context_access::alpn(context);
  if (!alpn.empty() && SSL_set_alpn_protos(ssl, alpn.data(), static_cast<unsigned int>(alpn.size())) != 0) {
    throw std::runtime_error(drain_openssl_errors("failed to configure TLS ALPN"));
  }

  const auto& name = context_access::server_name(context);
  if (!name.empty()) {
    if (SSL_set_tlsext_host_name(ssl, name.c_str()) != 1) {
      throw std::runtime_error(drain_openssl_errors("failed to configure TLS SNI"));
    }

    if (context_access::verify_peer(context)) {
      auto* params = SSL_get0_param(ssl);
      X509_VERIFY_PARAM_set_hostflags(params, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
      if (X509_VERIFY_PARAM_set1_host(params, name.c_str(), name.size()) != 1) {
        throw std::runtime_error(drain_openssl_errors("failed to configure TLS hostname verification"));
      }
    }
  }
}

} // namespace

handshake_result::handshake_result(uvp::io::byte_stream stream, std::string selected_alpn)
    : stream_(std::move(stream)),
      selected_alpn_(std::move(selected_alpn)),
      ok_(true) {}

handshake_result::handshake_result(uvp::error error)
    : error_(std::move(error)), ok_(false) {}

bool handshake_result::ok() const noexcept {
  return ok_;
}

uvp::io::byte_stream& handshake_result::stream() & {
  return stream_;
}

uvp::io::byte_stream&& handshake_result::stream() && {
  return std::move(stream_);
}

std::string_view handshake_result::selected_alpn() const noexcept {
  return selected_alpn_;
}

const uvp::error& handshake_result::error() const& {
  return error_;
}

void accept(uvp::io::byte_stream lower, server_context context, handshake_callback callback) {
  auto* ssl = make_ssl(context_access::native(context));
  auto state = std::make_shared<tls_state>(
    std::move(lower),
    ssl,
    tls_state::mode::server,
    std::move(callback));
  state->start();
}

void connect(uvp::io::byte_stream lower, client_context context, handshake_callback callback) {
  auto* ssl = make_ssl(context_access::native(context));
  configure_client_ssl(ssl, context);

  auto state = std::make_shared<tls_state>(
    std::move(lower),
    ssl,
    tls_state::mode::client,
    std::move(callback));
  state->start();
}

} // namespace uvp::tls
