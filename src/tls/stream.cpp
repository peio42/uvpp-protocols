#include <uvpp/protocols/tls/stream.hpp>

#include "context_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
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

class tls_state final : public handshake_operation::state, public std::enable_shared_from_this<tls_state> {
public:
  enum class mode {
    server,
    client,
  };

  tls_state(
    uvp::io::byte_stream lower,
    SSL* ssl,
    mode direction,
    std::size_t max_pending_write_bytes,
    std::size_t max_pending_read_bytes,
    handshake_callback callback)
      : lower_(std::move(lower)),
        ssl_(ssl),
        direction_(direction),
        max_pending_write_bytes_(max_pending_write_bytes),
        max_pending_read_bytes_(max_pending_read_bytes),
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

    start_lower_reads();

    drive_handshake();
  }

  uv::loop& loop() noexcept {
    return lower_.loop();
  }

  void read_start(uvp::io::read_callback on_read) {
    on_read_ = std::move(on_read);
    deliver_pending_clear();
    deliver_terminal_read();
    if (terminal_read_ != terminal_read_kind::none) {
      return;
    }
    if (open_ && on_read_) {
      start_lower_reads();
    }
    if (open_ && on_read_) {
      read_clear_from_ssl();
    }
  }

  void read_stop() {
    on_read_ = {};
    if (open_ && !clear_write_waiting_for_read_) {
      stop_lower_reads();
    }
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

    if (bytes.size() > max_pending_write_bytes_ ||
        pending_clear_write_bytes_ > max_pending_write_bytes_ - bytes.size()) {
      if (on_write) {
        on_write(uvp::io::stream_error{make_error_code(errc::write_buffer_limit)});
      }
      return;
    }

    pending_clear_write_bytes_ += bytes.size();
    clear_writes_.push_back(clear_write{
      std::vector<std::byte>(bytes.begin(), bytes.end()),
      0,
      std::move(on_write),
    });
    process_clear_writes();
  }

  void close(uvp::io::close_callback on_close) {
    if (closed_) {
      if (on_close) {
        on_close();
      }
      return;
    }

    if (on_close) {
      on_close_.push_back(std::move(on_close));
    }

    if (closing_) {
      return;
    }

    closing_ = true;
    discard_pending_clear();
    on_read_ = {};
    fail_pending_clear_writes(make_error_code(errc::closed));

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

  void cancel(uvp::error error) override {
    if (!active()) {
      return;
    }

    fail(std::move(error));
  }

  bool active() const noexcept override {
    return !open_ && !closed_ && !failed_;
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

  struct clear_write {
    std::vector<std::byte> payload;
    std::size_t offset = 0;
    uvp::io::write_callback done;
  };

  void start_lower_reads() {
    if (lower_reading_ || closed_ || failed_ || terminal_read_ != terminal_read_kind::none) {
      return;
    }

    lower_reading_ = true;
    auto self = shared_from_this();
    lower_.read_start([self](uvp::io::read_result result) {
      self->on_lower_read(std::move(result));
    });
  }

  void stop_lower_reads() {
    if (!lower_reading_) {
      return;
    }

    lower_reading_ = false;
    lower_.read_stop();
  }

  void on_lower_read(uvp::io::read_result result) {
    if (closed_ || failed_) {
      return;
    }

    if (result.eof()) {
      if (!open_) {
        fail(make_tls_error(errc::handshake_failed, "TLS handshake ended before completion"));
        return;
      }

      if (close_notify_received_ || terminal_read_ != terminal_read_kind::none) {
        set_terminal_read_eof();
      } else {
        fail(make_tls_error(errc::unexpected_eof, "TLS transport ended before close_notify"));
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
      if (clear_write_waiting_for_read_) {
        clear_write_waiting_for_read_ = false;
        process_clear_writes();
      }
      if (!on_read_ && !clear_write_waiting_for_read_) {
        stop_lower_reads();
      }
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
    if (!open_ || closed_ || failed_ || terminal_read_ != terminal_read_kind::none) {
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
        if (failed_ || closed_) {
          return;
        }
        continue;
      }

      const auto ssl_error = SSL_get_error(ssl_, ok);
      if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        return;
      }

      if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        close_notify_received_ = true;
        set_terminal_read_eof();
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
      if (bytes.size() > max_pending_read_bytes_ ||
          pending_clear_bytes_ > max_pending_read_bytes_ - bytes.size()) {
        fail(make_tls_error(errc::read_buffer_limit, "TLS read buffer limit reached"));
        return;
      }

      pending_clear_bytes_ += bytes.size();
      pending_clear_.push_back(std::move(bytes));
      return;
    }

    auto callback = on_read_;
    callback(uvp::io::read_result{bytes});
  }

  void deliver_pending_clear() {
    while (on_read_ && !pending_clear_.empty()) {
      auto bytes = std::move(pending_clear_.front());
      pending_clear_.pop_front();
      pending_clear_bytes_ -= bytes.size();
      auto callback = on_read_;
      callback(uvp::io::read_result{bytes});
    }
  }

  enum class terminal_read_kind {
    none,
    eof,
    error,
  };

  void discard_pending_clear() {
    pending_clear_.clear();
    pending_clear_bytes_ = 0;
  }

  void set_terminal_read_eof() {
    if (terminal_read_ != terminal_read_kind::none) {
      deliver_terminal_read();
      return;
    }

    terminal_read_ = terminal_read_kind::eof;
    if (open_) {
      stop_lower_reads();
    }
    deliver_terminal_read();
  }

  void set_terminal_read_error(std::error_code code) {
    if (terminal_read_ != terminal_read_kind::none) {
      deliver_terminal_read();
      return;
    }

    terminal_read_ = terminal_read_kind::error;
    terminal_read_error_ = std::move(code);
    if (open_) {
      stop_lower_reads();
    }
    deliver_terminal_read();
  }

  void deliver_terminal_read() {
    if (terminal_read_delivered_ || terminal_read_ == terminal_read_kind::none || !on_read_ ||
        !pending_clear_.empty()) {
      return;
    }

    auto callback = std::move(on_read_);
    terminal_read_delivered_ = true;
    if (terminal_read_ == terminal_read_kind::eof) {
      callback(uvp::io::read_result{{}, {}, true});
      return;
    }

    callback(uvp::io::read_result{{}, uvp::io::stream_error{terminal_read_error_}});
  }

  void process_clear_writes() {
    if (!open_ || closing_ || closed_ || failed_ || processing_clear_writes_ ||
        clear_write_waiting_for_read_ || clear_write_waiting_for_write_ ||
        clear_write_in_progress_ || clear_writes_.empty()) {
      return;
    }

    processing_clear_writes_ = true;
    auto finish_processing = [this] {
      processing_clear_writes_ = false;
      if (!clear_write_in_progress_ && !clear_writes_.empty() && !closed_ && !failed_) {
        process_clear_writes();
      }
    };

    auto& current = clear_writes_.front();
    while (current.offset < current.payload.size()) {
      std::size_t written = 0;
      const auto remaining = current.payload.size() - current.offset;
      const auto ok = SSL_write_ex(
        ssl_,
        current.payload.data() + current.offset,
        remaining,
        &written);

      if (ok == 1) {
        if (written == 0) {
          auto callback = std::move(current.done);
          pending_clear_write_bytes_ -= current.payload.size() - current.offset;
          clear_writes_.pop_front();
          if (callback) {
            callback(uvp::io::stream_error{make_error_code(errc::protocol_error)});
          }
          fail(make_tls_error(errc::protocol_error, "TLS write made no progress"));
          finish_processing();
          return;
        }

        current.offset += written;
        pending_clear_write_bytes_ -= written;
        drain_wbio();
        continue;
      }

      const auto ssl_error = SSL_get_error(ssl_, ok);
      if (ssl_error == SSL_ERROR_WANT_WRITE) {
        clear_write_waiting_for_write_ = true;
        drain_wbio([self = shared_from_this()](uvp::io::stream_error error) {
          self->clear_write_waiting_for_write_ = false;
          if (error) {
            self->fail(uvp::error{error.code(), error.message()});
            return;
          }
          self->process_clear_writes();
        });
        finish_processing();
        return;
      }

      if (ssl_error == SSL_ERROR_WANT_READ) {
        clear_write_waiting_for_read_ = true;
        start_lower_reads();
        drain_wbio();
        finish_processing();
        return;
      }

      auto callback = std::move(current.done);
      pending_clear_write_bytes_ -= current.payload.size() - current.offset;
      clear_writes_.pop_front();
      if (callback) {
        callback(uvp::io::stream_error{make_error_code(errc::protocol_error)});
      }
      fail(make_tls_error(errc::protocol_error, drain_openssl_errors("TLS write failed")));
      finish_processing();
      return;
    }

    auto callback = std::move(current.done);
    clear_writes_.pop_front();
    clear_write_in_progress_ = true;
    drain_wbio([self = shared_from_this(), callback = std::move(callback)](uvp::io::stream_error error) mutable {
      if (callback) {
        callback(error);
      }

      self->clear_write_in_progress_ = false;
      if (!error) {
        self->process_clear_writes();
      }
    });
    finish_processing();
  }

  void fail_pending_clear_writes(std::error_code code) {
    while (!clear_writes_.empty()) {
      auto current = std::move(clear_writes_.front());
      clear_writes_.pop_front();
      if (current.offset < current.payload.size()) {
        pending_clear_write_bytes_ -= current.payload.size() - current.offset;
      }
      if (current.done) {
        current.done(uvp::io::stream_error{code});
      }
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

      if (!error && self->failed_) {
        error = uvp::io::stream_error{
          self->terminal_error_code_ ? self->terminal_error_code_ : make_error_code(errc::closed)};
      }

      if (error) {
        if (done) {
          done(error);
        }
        if (!self->failed_) {
          self->fail(uvp::error{error.code(), error.message()});
        }
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
    terminal_error_code_ = error.code;
    fail_pending_clear_writes(error.code);
    auto handshake = std::move(handshake_callback_);
    handshake_callback_ = {};
    if (handshake) {
      handshake(handshake_result{std::move(error)});
    } else {
      set_terminal_read_error(error.code);
    }

    close_lower();
  }

  void close_lower() {
    if (closed_) {
      return;
    }

    closed_ = true;
    auto callbacks = std::move(on_close_);
    auto self = shared_from_this();
    lower_.close([self, callbacks = std::move(callbacks)]() mutable {
      for (auto& callback : callbacks) {
        if (!callback) {
          continue;
        }
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
  std::size_t max_pending_write_bytes_ = 0;
  std::size_t max_pending_read_bytes_ = 0;
  handshake_callback handshake_callback_;
  uvp::io::read_callback on_read_;
  std::vector<uvp::io::close_callback> on_close_;
  std::deque<std::vector<std::byte>> pending_clear_;
  std::deque<clear_write> clear_writes_;
  std::deque<encrypted_write> encrypted_writes_;
  std::vector<std::byte> clear_buffer_;
  std::error_code terminal_read_error_;
  std::error_code terminal_error_code_;
  std::size_t pending_clear_bytes_ = 0;
  std::size_t pending_clear_write_bytes_ = 0;
  terminal_read_kind terminal_read_ = terminal_read_kind::none;
  bool processing_clear_writes_ = false;
  bool clear_write_in_progress_ = false;
  bool clear_write_waiting_for_read_ = false;
  bool clear_write_waiting_for_write_ = false;
  bool lower_reading_ = false;
  bool encrypted_write_active_ = false;
  bool open_ = false;
  bool close_notify_received_ = false;
  bool terminal_read_delivered_ = false;
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

handshake_operation::handshake_operation(std::shared_ptr<state> self)
    : self_(std::move(self)) {}

void handshake_operation::cancel() {
  cancel(uvp::error{make_error_code(errc::cancelled), "TLS handshake cancelled"});
}

void handshake_operation::cancel(uvp::error error) {
  if (self_) {
    self_->cancel(std::move(error));
  }
}

bool handshake_operation::active() const noexcept {
  return self_ && self_->active();
}

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

handshake_operation accept(uvp::io::byte_stream lower, server_context context, handshake_callback callback) {
  auto* ssl = make_ssl(context_access::native(context));
  auto state = std::make_shared<tls_state>(
    std::move(lower),
    ssl,
    tls_state::mode::server,
    context_access::max_pending_write_bytes(context),
    context_access::max_pending_read_bytes(context),
    std::move(callback));
  state->start();
  return handshake_operation{std::move(state)};
}

handshake_operation connect(uvp::io::byte_stream lower, client_context context, handshake_callback callback) {
  auto* ssl = make_ssl(context_access::native(context));
  configure_client_ssl(ssl, context);

  auto state = std::make_shared<tls_state>(
    std::move(lower),
    ssl,
    tls_state::mode::client,
    context_access::max_pending_write_bytes(context),
    context_access::max_pending_read_bytes(context),
    std::move(callback));
  state->start();
  return handshake_operation{std::move(state)};
}

} // namespace uvp::tls
