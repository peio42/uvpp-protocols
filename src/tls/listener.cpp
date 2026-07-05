#include <uvpp/protocols/tls/listener.hpp>

#include <algorithm>
#include <chrono>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include <uvpp/handles/timer.hpp>
#include <uvpp/protocols/tls/error.hpp>
#include <uvpp/protocols/tls/stream.hpp>

namespace uvp::tls {

namespace {

struct pending_handshake {
  handshake_operation operation;
  std::shared_ptr<uv::timer> timer;
  bool timer_closed = false;
};

void close_timer(std::shared_ptr<pending_handshake> const& pending) {
  if (!pending->timer || pending->timer_closed) {
    return;
  }

  pending->timer_closed = true;
  auto timer = std::move(pending->timer);
  if (!timer->closing()) {
    timer->stop();
    timer->close([timer](uv::timer&) {});
  }
}

class tls_listener_model final : public uvp::io::stream_listener::concept_ {
public:
  tls_listener_model(
    uvp::io::stream_listener lower,
    server_context context,
    listener_options options)
      : lower_(std::move(lower)),
        context_(std::move(context)),
        options_(options) {}

  uv::loop& loop() noexcept override {
    return lower_.loop();
  }

  void listen(uvp::io::accept_callback on_accept) override {
    on_accept_ = std::move(on_accept);
    lower_.listen([this](uvp::io::accept_result result) {
      if (closing_ || !on_accept_) {
        return;
      }

      if (!result) {
        on_accept_(uvp::io::accept_result{result.error()});
        return;
      }

      if (pending_.size() >= options_.max_pending_handshakes()) {
        auto stream = std::make_shared<uvp::io::byte_stream>(std::move(result).stream());
        stream->close([stream] {});
        on_accept_(uvp::io::accept_result{
          uvp::io::stream_error{make_error_code(errc::pending_handshake_limit)}});
        return;
      }

      auto pending = std::make_shared<pending_handshake>();
      pending_.push_back(pending);
      start_timeout(pending);

      pending->operation = accept(
        std::move(result).stream(),
        context_,
        [this, pending](handshake_result handshake) mutable {
          finish_handshake(std::move(pending), std::move(handshake));
        });
    });
  }

  void close() override {
    if (closing_) {
      return;
    }

    closing_ = true;
    lower_.close();

    auto pending = std::vector<std::shared_ptr<pending_handshake>>{
      pending_.begin(),
      pending_.end(),
    };
    pending_.clear();

    for (auto const& item : pending) {
      close_timer(item);
      item->operation.cancel(
        uvp::error{make_error_code(errc::cancelled), "TLS listener closed"});
    }
  }

  uvp::io::endpoint local_endpoint() const override {
    return lower_.local_endpoint();
  }

private:
  void start_timeout(std::shared_ptr<pending_handshake> const& pending) {
    const auto timeout = options_.handshake_timeout();
    if (timeout <= std::chrono::milliseconds{0}) {
      return;
    }

    pending->timer = std::make_shared<uv::timer>(loop());
    pending->timer->start(timeout, [pending](uv::timer&) {
      close_timer(pending);
      pending->operation.cancel(
        uvp::error{make_error_code(errc::timeout), "TLS handshake timed out"});
    });
  }

  void finish_handshake(
    std::shared_ptr<pending_handshake> pending,
    handshake_result handshake) {
    close_timer(pending);
    pending_.remove(pending);

    if (closing_ || !on_accept_) {
      return;
    }

    if (!handshake) {
      on_accept_(uvp::io::accept_result{
        uvp::io::stream_error{handshake.error().code}});
      return;
    }

    on_accept_(uvp::io::accept_result{std::move(handshake).stream()});
  }

  uvp::io::stream_listener lower_;
  server_context context_;
  listener_options options_;
  std::list<std::shared_ptr<pending_handshake>> pending_;
  uvp::io::accept_callback on_accept_;
  bool closing_ = false;
};

} // namespace

listener_options& listener_options::handshake_timeout(std::chrono::milliseconds value) noexcept {
  handshake_timeout_ = value;
  return *this;
}

listener_options& listener_options::max_pending_handshakes(std::size_t value) noexcept {
  max_pending_handshakes_ = value;
  return *this;
}

std::chrono::milliseconds listener_options::handshake_timeout() const noexcept {
  return handshake_timeout_;
}

std::size_t listener_options::max_pending_handshakes() const noexcept {
  return max_pending_handshakes_;
}

struct listener::impl {
  impl(uvp::io::stream_listener lower, server_context context, listener_options options)
      : lower(std::move(lower)),
        context(std::move(context)),
        options(options) {}

  uvp::io::stream_listener lower;
  server_context context;
  listener_options options;
};

listener::listener(uvp::io::stream_listener lower, server_context context, listener_options options)
    : impl_(std::make_unique<impl>(std::move(lower), std::move(context), options)) {}

listener::~listener() = default;

listener::listener(listener&&) noexcept = default;

listener& listener::operator=(listener&&) noexcept = default;

listener::operator uvp::io::stream_listener() && {
  auto model = std::make_unique<tls_listener_model>(
    std::move(impl_->lower),
    std::move(impl_->context),
    impl_->options);
  return uvp::io::stream_listener{std::move(model)};
}

} // namespace uvp::tls
