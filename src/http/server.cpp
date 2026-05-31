#include <uvpp/protocols/http/server.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/request.hpp>
#include <uvpp/protocols/http/response.hpp>
#include <uvpp/protocols/io/tcp_listener.hpp>

#include "detail/http1_state_machine.hpp"

namespace uvp::http {

namespace {

std::pair<std::string, std::string> split_path_query(std::string_view target) {
  const auto query_offset = target.find('?');
  if (query_offset == std::string_view::npos) {
    return {std::string(target), {}};
  }

  return {
    std::string(target.substr(0, query_offset)),
    std::string(target.substr(query_offset + 1)),
  };
}

std::vector<std::byte> body_bytes(std::string_view body) {
  std::vector<std::byte> bytes(body.size());
  if (!body.empty()) {
    std::memcpy(bytes.data(), body.data(), body.size());
  }
  return bytes;
}

std::string reason_phrase_for(unsigned int status_code) {
  switch (status_code) {
  case 200:
    return std::string(reason_phrase(status::ok));
  case 201:
    return std::string(reason_phrase(status::created));
  case 204:
    return std::string(reason_phrase(status::no_content));
  case 400:
    return std::string(reason_phrase(status::bad_request));
  case 404:
    return std::string(reason_phrase(status::not_found));
  case 405:
    return std::string(reason_phrase(status::method_not_allowed));
  case 413:
    return std::string(reason_phrase(status::payload_too_large));
  case 500:
    return std::string(reason_phrase(status::internal_server_error));
  case 501:
    return std::string(reason_phrase(status::not_implemented));
  default:
    return {};
  }
}

std::string serialize_response(const response& response, bool keep_alive, const server_options& options) {
  const auto status_code = response.status_code();
  auto body = std::string(response.body());
  if (status_code == static_cast<unsigned int>(status::no_content)) {
    body.clear();
  }

  std::ostringstream out;
  out << "HTTP/1.1 " << status_code << ' ' << reason_phrase_for(status_code) << "\r\n";

  bool has_content_length = false;
  bool has_connection = false;
  bool has_server = false;

  for (const auto& [name, value] : response.headers()) {
    if (headers{}.set(name, value).contains("content-length")) {
      has_content_length = true;
    }
    if (headers{}.set(name, value).contains("connection")) {
      has_connection = true;
    }
    if (headers{}.set(name, value).contains("server")) {
      has_server = true;
    }
    out << name << ": " << value << "\r\n";
  }

  if (!has_content_length) {
    out << "content-length: " << body.size() << "\r\n";
  }
  if (!has_connection) {
    out << "connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
  }
  if (options.server_header_ && !has_server) {
    out << "server: uvpp-protocols\r\n";
  }

  out << "\r\n";
  out << body;
  return out.str();
}

std::span<const std::byte> as_bytes(std::string& value) noexcept {
  return std::as_bytes(std::span{value.data(), value.size()});
}

} // namespace

struct server::impl {
  explicit impl(server& owner) : owner(owner) {}

  struct pending_write {
    std::string payload;
    bool close_after = false;
  };

  class session : public std::enable_shared_from_this<session> {
  public:
    session(impl& owner, uvp::io::byte_stream stream)
        : owner_(owner), stream_(std::move(stream)) {}

    void start() {
      auto self = weak_from_this();
      stream_.read_start([self](uvp::io::read_result result) {
        if (auto session = self.lock()) {
          session->on_read(std::move(result));
        }
      });
    }

    void close() {
      if (closed_) {
        return;
      }
      closed_ = true;
      try {
        stream_.read_stop();
      } catch (...) {
      }
      auto self = weak_from_this();
      stream_.close([self] {
        if (auto session = self.lock()) {
          session->owner_.remove_session(session.get());
        }
      });
    }

  private:
    void on_read(uvp::io::read_result result) {
      if (closed_) {
        return;
      }

      if (result.eof()) {
        close();
        return;
      }

      if (!result) {
        close();
        return;
      }

      const auto bytes = result.bytes();
      auto parse_result = parser_.parse(std::string_view{
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size(),
      });

      if (parse_result.code == detail::http1_parse_result::status::error) {
        send_error(status::bad_request, false);
        return;
      }

      const auto& messages = parser_.completed_messages();
      while (handled_messages_ < messages.size()) {
        handle_message(messages[handled_messages_++]);
      }
    }

    void handle_message(const detail::http1_message& message) {
      const auto [path, query] = split_path_query(message.target);
      request req{
        message.method,
        message.target,
        path,
        query,
        message.headers,
        body_bytes(message.body),
        route_params{},
        connection{&stream_},
      };

      response res;
      const auto* handler = owner_.owner.router_.find(req.method(), req.path());
      const auto keep_alive = owner_.owner.options_.keep_alive_ && message.keep_alive;

      if (!handler) {
        res.status(status::not_found).text("not found\n");
        enqueue(serialize_response(res, keep_alive, owner_.owner.options_), !keep_alive);
        return;
      }

      try {
        (*handler)(req, res);
        if (!res.ended()) {
          res.end();
        }
      } catch (...) {
        res = response{};
        res.status(status::internal_server_error).text("internal server error\n");
      }

      enqueue(serialize_response(res, keep_alive, owner_.owner.options_), !keep_alive);
    }

    void send_error(status status_code, bool keep_alive) {
      response res;
      res.status(status_code).text(std::string(reason_phrase(status_code)) + "\n");
      enqueue(serialize_response(res, keep_alive, owner_.owner.options_), !keep_alive);
    }

    void enqueue(std::string payload, bool close_after) {
      writes_.push_back(pending_write{std::move(payload), close_after});
      flush_next();
    }

    void flush_next() {
      if (closed_ || writing_ || writes_.empty()) {
        return;
      }

      writing_ = true;
      auto self = weak_from_this();
      stream_.write(as_bytes(writes_.front().payload), [self](uvp::io::stream_error error) {
        if (auto session = self.lock()) {
          session->on_write(std::move(error));
        }
      });
    }

    void on_write(uvp::io::stream_error error) {
      writing_ = false;
      if (closed_) {
        return;
      }

      if (error) {
        close();
        return;
      }

      const auto close_after = !writes_.empty() && writes_.front().close_after;
      if (!writes_.empty()) {
        writes_.pop_front();
      }

      if (close_after) {
        close();
        return;
      }

      flush_next();
    }

    impl& owner_;
    uvp::io::byte_stream stream_;
    detail::http1_state_machine parser_;
    std::size_t handled_messages_ = 0;
    std::deque<pending_write> writes_;
    bool writing_ = false;
    bool closed_ = false;
  };

  void add_listener(uvp::io::stream_listener listener) {
    listeners_.push_back(std::move(listener));
    auto& stored = listeners_.back();
    stored.listen([this](uvp::io::accept_result result) {
      if (!result) {
        return;
      }

      auto accepted = std::make_shared<session>(*this, std::move(result).stream());
      sessions_.push_back(accepted);
      accepted->start();
    });
  }

  void close() {
    for (auto& listener : listeners_) {
      listener.close();
    }
    for (auto& session : sessions_) {
      session->close();
    }
  }

  void remove_session(session* session) {
    std::erase_if(sessions_, [session](const std::shared_ptr<impl::session>& candidate) {
      return candidate.get() == session;
    });
  }

  server& owner;
  std::vector<uvp::io::stream_listener> listeners_;
  std::vector<std::shared_ptr<session>> sessions_;
};

server::server(uv::loop& loop)
    : server(loop, server_options{}) {}

server::server(uv::loop& loop, server_options options)
    : loop_(&loop), options_(options), impl_(std::make_unique<impl>(*this)) {
  options_.validate();
}

server::~server() = default;

server::server(server&&) noexcept = default;

server& server::operator=(server&&) noexcept = default;

void server::listen(std::string_view host, unsigned int port) {
  auto listener = uvp::io::tcp_listener{*loop_};
  listener.bind(host, port);
  listen(std::move(listener));
}

void server::listen(uvp::io::stream_listener listener) {
  impl_->add_listener(std::move(listener));
}

void server::close() noexcept {
  if (impl_) {
    impl_->close();
  }
}

} // namespace uvp::http
