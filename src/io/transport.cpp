#include <uvpp/protocols/io.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <uvpp/uv.hpp>

namespace uvp::io {

namespace {

constexpr int default_backlog = 64;

stream_error from_result(uv::result result) {
  return stream_error{result.error_code()};
}

endpoint tcp_local_endpoint(uv::tcp& tcp) {
  const auto addr = tcp.sockname();
  if (!addr.is_v4() && !addr.is_v6()) {
    return {};
  }
  return tcp_endpoint{addr.to_string(), static_cast<unsigned int>(addr.port())};
}

endpoint tcp_remote_endpoint(uv::tcp& tcp) {
  const auto addr = tcp.peername();
  if (!addr.is_v4() && !addr.is_v6()) {
    return {};
  }
  return tcp_endpoint{addr.to_string(), static_cast<unsigned int>(addr.port())};
}

endpoint pipe_local_endpoint(uv::pipe& pipe) {
  return pipe_endpoint{pipe.sockname()};
}

endpoint pipe_remote_endpoint(uv::pipe& pipe) {
  return pipe_endpoint{pipe.peername()};
}

template<class Stream>
class stream_model final : public byte_stream::concept_ {
public:
  stream_model(uv::loop& loop, std::unique_ptr<Stream> stream)
      : loop_(&loop), stream_(std::move(stream)) {}

  uv::loop& loop() noexcept override {
    return *loop_;
  }

  void read_start(read_callback on_read) override {
    on_read_ = std::move(on_read);
    stream_->read_start(
      [this](Stream&, std::size_t suggested_size) {
        read_buffer_.resize(std::max<std::size_t>(suggested_size, 4096));
        return uv::buffer_view{read_buffer_.data(), read_buffer_.size()};
      },
      [this](Stream&, uv::read_result result) {
        if (!on_read_) {
          return;
        }

        if (result.eof()) {
          on_read_(read_result{{}, {}, true});
          return;
        }

        if (!result) {
          on_read_(read_result{{}, stream_error{result.status().error_code()}});
          return;
        }

        on_read_(read_result{result.bytes()});
      });
  }

  void read_stop() override {
    stream_->read_stop();
  }

  void write(std::span<const std::byte> bytes, write_callback on_write) override {
    if (bytes.empty()) {
      if (on_write) {
        on_write({});
      }
      return;
    }

    auto op = std::make_shared<write_operation>(bytes);
    stream_->write(op->request, op->payload, [op, callback = std::move(on_write)](uv::write_request&, uv::result result) mutable {
      if (callback) {
        callback(from_result(result));
      }
    });
  }

  void close(close_callback on_close) override {
    if (closed_) {
      if (on_close) {
        on_close();
      }
      return;
    }

    closed_ = true;
    if (stream_->closing()) {
      if (on_close) {
        on_close();
      }
      return;
    }

    stream_->close([callback = std::move(on_close)](Stream&) mutable {
      if (callback) {
        callback();
      }
    });
  }

  endpoint local_endpoint() const override {
    if constexpr (std::is_same_v<Stream, uv::tcp>) {
      return tcp_local_endpoint(*stream_);
    } else {
      return pipe_local_endpoint(*stream_);
    }
  }

  endpoint remote_endpoint() const override {
    if constexpr (std::is_same_v<Stream, uv::tcp>) {
      return tcp_remote_endpoint(*stream_);
    } else {
      return pipe_remote_endpoint(*stream_);
    }
  }

  uv::tcp* tcp() noexcept override {
    if constexpr (std::is_same_v<Stream, uv::tcp>) {
      return stream_.get();
    } else {
      return nullptr;
    }
  }

  uv::pipe* pipe() noexcept override {
    if constexpr (std::is_same_v<Stream, uv::pipe>) {
      return stream_.get();
    } else {
      return nullptr;
    }
  }

private:
  struct write_operation {
    explicit write_operation(std::span<const std::byte> bytes)
        : payload(bytes.begin(), bytes.end()) {}

    uv::write_request request;
    std::vector<std::byte> payload;
  };

  uv::loop* loop_;
  std::unique_ptr<Stream> stream_;
  read_callback on_read_;
  std::vector<char> read_buffer_;
  bool closed_ = false;
};

template<class Listener, class Client>
class listener_model final : public stream_listener::concept_ {
public:
  listener_model(uv::loop& loop, std::unique_ptr<Listener> listener, endpoint local, unsigned int backlog)
      : loop_(&loop),
        listener_(std::move(listener)),
        local_(std::move(local)),
        backlog_(backlog == 0 ? default_backlog : static_cast<int>(backlog)) {}

  uv::loop& loop() noexcept override {
    return *loop_;
  }

  void listen(accept_callback on_accept) override {
    on_accept_ = std::move(on_accept);
    listener_->listen(backlog_, [this](Listener& server, uv::result status) {
      if (!on_accept_) {
        return;
      }

      if (!status) {
        on_accept_(accept_result{from_result(status)});
        return;
      }

      auto client = std::make_unique<Client>(*loop_);
      try {
        server.accept(*client);
        auto stream = byte_stream{std::make_unique<stream_model<Client>>(*loop_, std::move(client))};
        on_accept_(accept_result{std::move(stream)});
      } catch (const uv::error& error) {
        on_accept_(accept_result{stream_error{error.code()}});
      }
    });
  }

  void close() override {
    if (!listener_->closing()) {
      listener_->close();
    }
  }

  endpoint local_endpoint() const override {
    return local_;
  }

private:
  uv::loop* loop_;
  std::unique_ptr<Listener> listener_;
  endpoint local_;
  int backlog_;
  accept_callback on_accept_;
};

} // namespace

read_result::read_result(std::span<const std::byte> bytes, stream_error error, bool eof)
    : bytes_(bytes), error_(std::move(error)), eof_(eof) {}

byte_stream::byte_stream(std::unique_ptr<concept_> self)
    : self_(std::move(self)) {}

byte_stream::~byte_stream() = default;

byte_stream::byte_stream(byte_stream&&) noexcept = default;

byte_stream& byte_stream::operator=(byte_stream&&) noexcept = default;

uv::loop& byte_stream::loop() noexcept {
  return self_->loop();
}

void byte_stream::read_start(read_callback on_read) {
  self_->read_start(std::move(on_read));
}

void byte_stream::read_stop() {
  self_->read_stop();
}

void byte_stream::write(std::span<const std::byte> bytes, write_callback on_write) {
  self_->write(bytes, std::move(on_write));
}

void byte_stream::close(close_callback on_close) {
  self_->close(std::move(on_close));
}

endpoint byte_stream::local_endpoint() const {
  return self_->local_endpoint();
}

endpoint byte_stream::remote_endpoint() const {
  return self_->remote_endpoint();
}

byte_stream::operator bool() const noexcept {
  return static_cast<bool>(self_);
}

uv::tcp* byte_stream::tcp() noexcept {
  return self_ ? self_->tcp() : nullptr;
}

uv::pipe* byte_stream::pipe() noexcept {
  return self_ ? self_->pipe() : nullptr;
}

accept_result::accept_result(byte_stream stream)
    : stream_(std::move(stream)), ok_(true) {}

accept_result::accept_result(stream_error error)
    : error_(std::move(error)), ok_(false) {}

bool accept_result::ok() const noexcept {
  return ok_;
}

byte_stream& accept_result::stream() & {
  return stream_;
}

byte_stream&& accept_result::stream() && {
  return std::move(stream_);
}

const stream_error& accept_result::error() const noexcept {
  return error_;
}

stream_listener::stream_listener(std::unique_ptr<concept_> self)
    : self_(std::move(self)) {}

stream_listener::~stream_listener() = default;

stream_listener::stream_listener(stream_listener&&) noexcept = default;

stream_listener& stream_listener::operator=(stream_listener&&) noexcept = default;

uv::loop& stream_listener::loop() noexcept {
  return self_->loop();
}

void stream_listener::listen(accept_callback on_accept) {
  self_->listen(std::move(on_accept));
}

void stream_listener::close() {
  if (self_) {
    self_->close();
  }
}

endpoint stream_listener::local_endpoint() const {
  return self_ ? self_->local_endpoint() : endpoint{};
}

stream_listener::operator bool() const noexcept {
  return static_cast<bool>(self_);
}

struct tcp_listener::impl {
  explicit impl(uv::loop& loop) : loop(&loop), tcp(std::make_unique<uv::tcp>(loop)) {}

  uv::loop* loop;
  std::unique_ptr<uv::tcp> tcp;
  endpoint local;
  unsigned int backlog = default_backlog;
};

tcp_listener::tcp_listener(uv::loop& loop)
    : impl_(std::make_unique<impl>(loop)) {}

tcp_listener::~tcp_listener() = default;

tcp_listener::tcp_listener(tcp_listener&&) noexcept = default;

tcp_listener& tcp_listener::operator=(tcp_listener&&) noexcept = default;

tcp_listener& tcp_listener::bind(std::string_view host, unsigned int port) {
  const auto has_ipv6_separator = host.find(':') != std::string_view::npos;
  if (has_ipv6_separator) {
    impl_->tcp->bind(uv::ipv6{host, static_cast<int>(port)});
  } else {
    impl_->tcp->bind(uv::ipv4{host, static_cast<int>(port)});
  }
  impl_->local = tcp_local_endpoint(*impl_->tcp);
  return *this;
}

tcp_listener& tcp_listener::backlog(unsigned int value) noexcept {
  impl_->backlog = value;
  return *this;
}

tcp_listener::operator stream_listener() && {
  auto model = std::make_unique<listener_model<uv::tcp, uv::tcp>>(
    *impl_->loop,
    std::move(impl_->tcp),
    std::move(impl_->local),
    impl_->backlog);
  return stream_listener{std::move(model)};
}

struct pipe_listener::impl {
  explicit impl(uv::loop& loop) : loop(&loop), pipe(std::make_unique<uv::pipe>(loop)) {}

  uv::loop* loop;
  std::unique_ptr<uv::pipe> pipe;
  endpoint local;
  unsigned int backlog = default_backlog;
};

pipe_listener::pipe_listener(uv::loop& loop)
    : impl_(std::make_unique<impl>(loop)) {}

pipe_listener::~pipe_listener() = default;

pipe_listener::pipe_listener(pipe_listener&&) noexcept = default;

pipe_listener& pipe_listener::operator=(pipe_listener&&) noexcept = default;

pipe_listener& pipe_listener::bind(std::string_view path) {
  impl_->pipe->bind(path);
  impl_->local = pipe_endpoint{std::string(path)};
  return *this;
}

pipe_listener& pipe_listener::backlog(unsigned int value) noexcept {
  impl_->backlog = value;
  return *this;
}

pipe_listener::operator stream_listener() && {
  auto model = std::make_unique<listener_model<uv::pipe, uv::pipe>>(
    *impl_->loop,
    std::move(impl_->pipe),
    std::move(impl_->local),
    impl_->backlog);
  return stream_listener{std::move(model)};
}

} // namespace uvp::io
