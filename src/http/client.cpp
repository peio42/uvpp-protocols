#include <uvpp/protocols/http/client.hpp>

#include <uvpp/protocols/dns.hpp>
#include <uvpp/protocols/http/error.hpp>
#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/io/tcp_connector.hpp>
#include <uvpp/protocols/tls.hpp>
#include <uvpp/protocols/url.hpp>
#include <uvpp/uv.hpp>
#include <uvpp/handles/timer.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "detail/http1_response_parser.hpp"

namespace uvp::http {

namespace detail {

struct request_operation_state {
  virtual ~request_operation_state() = default;
  virtual void cancel() noexcept = 0;
};

class connection_pool : public std::enable_shared_from_this<connection_pool> {
public:
  struct entry {
    std::string origin;
    uvp::io::byte_stream stream;
    std::shared_ptr<uv::timer> timer;
  };

  std::optional<uvp::io::byte_stream> take(const std::string& origin) {
    auto found = idle_.find(origin);
    if (found == idle_.end()) {
      return std::nullopt;
    }

    auto& queue = found->second;
    while (!queue.empty()) {
      auto item = std::move(queue.front());
      queue.pop_front();
      if (item->timer && !item->timer->closing()) {
        try {
          item->timer->stop();
        } catch (...) {
        }
        auto timer = std::move(item->timer);
        timer->close([timer](uv::timer&) {});
      }
      if (item->stream) {
        auto stream = std::move(item->stream);
        stream.ref();
        if (queue.empty()) {
          idle_.erase(found);
        }
        return stream;
      }
    }

    idle_.erase(found);
    return std::nullopt;
  }

  void release(
    uv::loop& loop,
    std::string origin,
    uvp::io::byte_stream stream,
    std::size_t max_idle,
    std::chrono::milliseconds idle_timeout) {
    if (!stream || max_idle == 0) {
      if (stream) {
        auto item = std::make_shared<entry>();
        item->stream = std::move(stream);
        close_entry(item);
      }
      return;
    }

    auto item = std::make_shared<entry>();
    item->origin = std::move(origin);
    item->stream = std::move(stream);
    item->stream.unref();
    if (idle_timeout > std::chrono::milliseconds{0}) {
      item->timer = std::make_shared<uv::timer>(loop);
      auto weak_pool = weak_from_this();
      std::weak_ptr<entry> weak_item = item;
      item->timer->start(idle_timeout, [weak_pool, weak_item](uv::timer&) {
        if (auto pool = weak_pool.lock()) {
          if (auto locked = weak_item.lock()) {
            pool->expire(locked);
          }
        }
      });
    }

    auto& queue = idle_[item->origin];
    queue.push_back(item);
    while (queue.size() > max_idle) {
      auto evicted = std::move(queue.front());
      queue.pop_front();
      close_entry(evicted);
    }
  }

  void close_all() noexcept {
    for (auto& [_, queue] : idle_) {
      for (auto& item : queue) {
        close_entry(item);
      }
    }
    idle_.clear();
  }

private:
  void expire(const std::shared_ptr<entry>& item) {
    auto found = idle_.find(item->origin);
    if (found != idle_.end()) {
      auto& queue = found->second;
      queue.erase(
        std::remove_if(
          queue.begin(),
          queue.end(),
          [&](const std::shared_ptr<entry>& candidate) {
            return candidate == item;
          }),
        queue.end());
      if (queue.empty()) {
        idle_.erase(found);
      }
    }
    close_entry(item);
  }

  void close_entry(const std::shared_ptr<entry>& item) noexcept {
    if (item->timer && !item->timer->closing()) {
      try {
        item->timer->stop();
      } catch (...) {
      }
      auto timer = std::move(item->timer);
      timer->close([timer](uv::timer&) {});
    }
    if (item->stream) {
      item->stream.ref();
      item->stream.close([item] {});
    }
  }

  std::unordered_map<std::string, std::deque<std::shared_ptr<entry>>> idle_;
};

} // namespace detail

namespace {

[[nodiscard]] uvp::error make_client_error(errc code, std::string detail = {}) {
  return uvp::error{make_error_code(code), std::move(detail)};
}

[[nodiscard]] uvp::error wrap_client_error(errc code, const uvp::error& source) {
  return make_client_error(code, source.detail.empty() ? source.code.message() : source.detail);
}

[[nodiscard]] std::string trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return std::string{value};
}

[[nodiscard]] bool response_is_redirect(unsigned int status_code) noexcept {
  return status_code == 301 ||
         status_code == 302 ||
         status_code == 303 ||
         status_code == 307 ||
         status_code == 308;
}

[[nodiscard]] bool method_can_follow_redirect(http::method method) noexcept {
  return method == http::method::get || method == http::method::head;
}

[[nodiscard]] std::string base64_encode(std::string_view input) {
  static constexpr auto alphabet =
    std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

  auto out = std::string{};
  out.reserve(((input.size() + 2) / 3) * 4);

  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const auto b0 = static_cast<unsigned char>(input[offset]);
    const auto b1 = offset + 1 < input.size() ? static_cast<unsigned char>(input[offset + 1]) : 0U;
    const auto b2 = offset + 2 < input.size() ? static_cast<unsigned char>(input[offset + 2]) : 0U;

    out.push_back(alphabet[(b0 >> 2) & 0x3f]);
    out.push_back(alphabet[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0f)]);
    out.push_back(offset + 1 < input.size() ? alphabet[((b1 & 0x0f) << 2) | ((b2 >> 6) & 0x03)] : '=');
    out.push_back(offset + 2 < input.size() ? alphabet[b2 & 0x3f] : '=');
  }

  return out;
}

[[nodiscard]] std::string origin_key(const uvp::url& value) {
  auto key = std::string{value.scheme()};
  key += "://";
  key += value.hostname();
  key += ':';
  auto port = effective_port(value);
  key += port ? std::to_string(port.value()) : std::string{value.port()};
  return key;
}

void append_ascii(std::vector<std::byte>& out, std::string_view value) {
  const auto offset = out.size();
  out.resize(offset + value.size());
  if (!value.empty()) {
    std::memcpy(out.data() + offset, value.data(), value.size());
  }
}

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view value) {
  auto out = std::vector<std::byte>(value.size());
  if (!value.empty()) {
    std::memcpy(out.data(), value.data(), value.size());
  }
  return out;
}

[[nodiscard]] std::vector<std::byte> serialize_chunk(std::span<const std::byte> chunk) {
  auto size_buffer = std::array<char, 2 * sizeof(std::size_t)>{};
  const auto [ptr, ec] = std::to_chars(
    size_buffer.data(),
    size_buffer.data() + size_buffer.size(),
    chunk.size(),
    16);
  auto out = std::vector<std::byte>{};
  if (ec != std::errc{}) {
    return out;
  }
  append_ascii(out, std::string_view{size_buffer.data(), static_cast<std::size_t>(ptr - size_buffer.data())});
  append_ascii(out, "\r\n");
  const auto offset = out.size();
  out.resize(offset + chunk.size());
  if (!chunk.empty()) {
    std::memcpy(out.data() + offset, chunk.data(), chunk.size());
  }
  append_ascii(out, "\r\n");
  return out;
}

[[nodiscard]] detail::http1_response_limits response_limits(const client_options& options) {
  return {
    .max_header_bytes = options.max_header_bytes,
    .max_header_count = options.max_header_count,
    .max_body_bytes = options.max_body_bytes,
  };
}

[[nodiscard]] uvp::error response_parse_error(const detail::http1_response_parse_result& result) {
  switch (result.kind) {
  case detail::http1_response_parse_result::error_kind::header_limit:
    return make_client_error(errc::client_header_limit_exceeded, result.error);
  case detail::http1_response_parse_result::error_kind::body_limit:
    return make_client_error(errc::client_body_limit_exceeded, result.error);
  case detail::http1_response_parse_result::error_kind::malformed:
    return make_client_error(errc::client_malformed_response, result.error);
  }
  return make_client_error(errc::client_malformed_response, result.error);
}

[[nodiscard]] http::response make_buffered_response(
  const http::response_head& head,
  std::string_view body) {
  auto response = http::response{};
  response.status(head.status_code);
  for (const auto& [name, value] : head.headers) {
    response.header(name, value);
  }
  response.bytes(std::as_bytes(std::span{body.data(), body.size()}));
  return response;
}

class request_state : public detail::request_operation_state, public std::enable_shared_from_this<request_state> {
public:
  request_state(
    uv::loop& loop,
    client_options options,
    std::shared_ptr<detail::connection_pool> pool,
    http::method method,
    std::string_view url,
    client_callback callback)
      : loop_(&loop),
        options_(options),
        pool_(std::move(pool)),
        method_(method),
        url_input_(url),
        callback_(std::move(callback)),
        resolver_(loop),
        connector_(loop) {}

  request_operation start() {
    auto parsed = uvp::parse_url(url_input_);
    return start(std::move(parsed));
  }

  request_operation start(uvp::result<uvp::url> parsed) {
    if (!parsed) {
      complete(wrap_client_error(errc::client_invalid_url, parsed.error()));
      return request_operation{shared_from_this()};
    }

    url_ = std::move(parsed).value();
    const auto scheme = uvp::scheme_id(url_);
    if (scheme != uvp::url_scheme::http && scheme != uvp::url_scheme::https) {
      complete(make_client_error(errc::client_unsupported_scheme));
      return request_operation{shared_from_this()};
    }

    auto endpoint = uvp::authority_endpoint(url_);
    if (!endpoint) {
      complete(wrap_client_error(errc::client_invalid_url, endpoint.error()));
      return request_operation{shared_from_this()};
    }

    auto connect_endpoint = select_connect_endpoint(scheme);
    if (!connect_endpoint) {
      complete(connect_endpoint.error());
      return request_operation{shared_from_this()};
    }

    origin_key_ = connection_pool_key();
    if (auto pooled = pool_->take(origin_key_)) {
      stream_ = std::move(*pooled);
      write_buffered_request();
      return request_operation{shared_from_this()};
    }

    auto self = shared_from_this();
    dns_operation_ = resolver_.resolve(
      uvp::dns::query{}
        .host(connect_endpoint.value().host)
        .port(connect_endpoint.value().port)
        .family(uvp::dns::address_family::any),
      [self](uvp::result<uvp::dns::address_list> result) mutable {
        self->on_resolved(std::move(result));
      });
    start_phase_timeout(timeout_phase::dns, options_.dns_timeout);

    return request_operation{std::move(self)};
  }

  void cancel() noexcept override {
    if (completed_) {
      return;
    }

    cancelled_ = true;
    dns_operation_.cancel();
    connect_operation_.cancel();
    tls_operation_.cancel();
    close_stream();
    complete(make_client_error(errc::client_cancelled));
  }

private:
  enum class timeout_phase {
    none,
    dns,
    tls_handshake,
    request_body,
    response_header,
    response_body,
  };

  void start_phase_timeout(timeout_phase phase, std::chrono::milliseconds duration) {
    stop_phase_timeout();
    if (duration <= std::chrono::milliseconds{0}) {
      return;
    }

    timeout_phase_ = phase;
    timeout_timer_ = std::make_shared<uv::timer>(*loop_);
    auto self = shared_from_this();
    timeout_timer_->start(duration, [self, phase](uv::timer&) {
      self->on_timeout(phase);
    });
  }

  void stop_phase_timeout() noexcept {
    timeout_phase_ = timeout_phase::none;
    if (!timeout_timer_) {
      return;
    }

    auto timer = std::move(timeout_timer_);
    if (timer->closing()) {
      return;
    }

    try {
      timer->stop();
    } catch (...) {
    }
    timer->close([timer](uv::timer&) {});
  }

  void on_timeout(timeout_phase phase) {
    if (completed_ || phase != timeout_phase_) {
      return;
    }

    cancelled_ = true;
    timed_out_ = true;
    dns_operation_.cancel();
    connect_operation_.cancel();
    tls_operation_.cancel();
    close_stream();
    complete(make_client_error(errc::client_timeout, timeout_phase_name(phase)));
  }

  [[nodiscard]] static std::string timeout_phase_name(timeout_phase phase) {
    switch (phase) {
    case timeout_phase::dns:
      return "DNS resolution timed out";
    case timeout_phase::tls_handshake:
      return "TLS handshake timed out";
    case timeout_phase::request_body:
      return "request body timed out";
    case timeout_phase::response_header:
      return "response headers timed out";
    case timeout_phase::response_body:
      return "response body timed out";
    case timeout_phase::none:
      break;
    }
    return "request timed out";
  }

  uvp::result<uvp::url_authority_endpoint> select_connect_endpoint(uvp::url_scheme target_scheme) {
    using_forward_proxy_ = false;
    proxy_key_.clear();
    proxy_url_ = {};

    if (options_.proxy.url.empty()) {
      return uvp::authority_endpoint(url_);
    }

    if (!http::headers::is_valid_value(options_.proxy.authorization)) {
      return make_client_error(errc::client_proxy_failed, "proxy authorization contains invalid header characters");
    }

    if (target_scheme != uvp::url_scheme::http) {
      return make_client_error(
        errc::client_proxy_failed,
        "HTTP CONNECT proxying is not implemented");
    }

    auto parsed_proxy = uvp::parse_url(options_.proxy.url);
    if (!parsed_proxy) {
      return wrap_client_error(errc::client_proxy_failed, parsed_proxy.error());
    }

    proxy_url_ = std::move(parsed_proxy).value();
    if (uvp::scheme_id(proxy_url_) != uvp::url_scheme::http) {
      return make_client_error(errc::client_proxy_failed, "proxy scheme is unsupported");
    }

    auto endpoint = uvp::authority_endpoint(proxy_url_);
    if (!endpoint) {
      return wrap_client_error(errc::client_proxy_failed, endpoint.error());
    }

    using_forward_proxy_ = true;
    proxy_key_ = origin_key(proxy_url_);
    return endpoint.value();
  }

  [[nodiscard]] std::string connection_pool_key() const {
    auto key = origin_key(url_);
    if (using_forward_proxy_) {
      key += " via ";
      key += proxy_key_;
    }
    return key;
  }

  void on_resolved(uvp::result<uvp::dns::address_list> result) {
    if (completed_) {
      return;
    }
    if (cancelled_) {
      if (timed_out_) {
        return;
      }
      complete(make_client_error(errc::client_cancelled));
      return;
    }
    if (!result) {
      complete(wrap_client_error(errc::client_dns_failed, result.error()));
      return;
    }

    stop_phase_timeout();
    auto self = shared_from_this();
    connect_operation_ = connector_.connect(
      result.value(),
      uvp::io::connect_options{.timeout = options_.connect_timeout},
      [self](uvp::result<uvp::io::byte_stream> connect_result) mutable {
        self->on_connected(std::move(connect_result));
      });
  }

  void on_connected(uvp::result<uvp::io::byte_stream> result) {
    if (completed_) {
      return;
    }
    if (cancelled_) {
      if (timed_out_) {
        return;
      }
      complete(make_client_error(errc::client_cancelled));
      return;
    }
    if (!result) {
      if (result.error().code == uvp::io::connect_errc::timeout) {
        complete(make_client_error(errc::client_timeout, "TCP connect timed out"));
        return;
      }
      complete(wrap_client_error(errc::client_connect_failed, result.error()));
      return;
    }

    auto stream = std::move(result).value();
    if (uvp::scheme_id(url_) == uvp::url_scheme::https) {
      start_tls(std::move(stream));
      return;
    }

    stream_ = std::move(stream);
    write_buffered_request();
  }

  void start_tls(uvp::io::byte_stream lower) {
    try {
      auto context_options = uvp::tls::client_context_options{}
        .server_name(url_.hostname())
        .alpn({"http/1.1"})
        .default_verify_paths(options_.tls_default_verify_paths);
      if (!options_.tls_ca_file.empty()) {
        context_options.ca_file(options_.tls_ca_file);
      }
      if (!options_.tls_ca_path.empty()) {
        context_options.ca_path(options_.tls_ca_path);
      }
      auto context = uvp::tls::client_context{std::move(context_options)};

      start_phase_timeout(timeout_phase::tls_handshake, options_.tls_handshake_timeout);
      auto self = shared_from_this();
      tls_operation_ = uvp::tls::connect(
        std::move(lower),
        std::move(context),
        [self](uvp::tls::handshake_result result) mutable {
          self->on_tls_connected(std::move(result));
        });
    } catch (const std::exception& error) {
      complete(make_client_error(errc::client_tls_failed, error.what()));
    }
  }

  void on_tls_connected(uvp::tls::handshake_result result) {
    if (completed_) {
      return;
    }
    if (cancelled_) {
      if (timed_out_) {
        return;
      }
      complete(make_client_error(errc::client_cancelled));
      return;
    }
    if (!result) {
      complete(wrap_client_error(errc::client_tls_failed, result.error()));
      return;
    }

    const auto selected_alpn = result.selected_alpn();
    if (!selected_alpn.empty() && selected_alpn != "http/1.1") {
      std::move(result).stream().close();
      complete(make_client_error(errc::client_tls_failed, "unsupported TLS ALPN protocol"));
      return;
    }

    stream_ = std::move(result).stream();
    write_buffered_request();
  }

  void write_buffered_request() {
    response_parser_.reset(method_);
    response_parser_.limits(response_limits(options_));
    response_head_ = {};
    response_body_.clear();
    response_headers_complete_ = false;
    response_keep_alive_ = false;

    auto request = std::string{};
    request += http::to_string(method_);
    request += ' ';
    request += using_forward_proxy_ ? uvp::absolute_form_target(url_) : uvp::origin_form_target(url_);
    request += " HTTP/1.1\r\nHost: ";
    request += url_.host();
    if (url_.has_port()) {
      request += ':';
      request += url_.port();
    }
    request += "\r\nUser-Agent: uvpp-protocols/0\r\nAccept: */*\r\n";
    if (using_forward_proxy_ && !options_.proxy.authorization.empty()) {
      request += "Proxy-Authorization: ";
      request += options_.proxy.authorization;
      request += "\r\n";
    }
    if (options_.max_idle_connections_per_origin == 0) {
      request += "Connection: close\r\n";
    }
    request += "\r\n";

    write_payload_.resize(request.size());
    std::memcpy(write_payload_.data(), request.data(), request.size());
    auto self = shared_from_this();
    start_phase_timeout(timeout_phase::request_body, options_.request_body_timeout);
    stream_.write(write_payload_, [self](uvp::io::stream_error result) {
      self->on_written(result);
    });
  }

  void on_written(uvp::io::stream_error result) {
    if (completed_) {
      return;
    }
    if (result) {
      complete(make_client_error(errc::client_connect_failed, result.message()));
      return;
    }

    auto self = shared_from_this();
    stream_.read_start(
      [self](uvp::io::read_result result) {
        self->on_read(result);
      });
    start_phase_timeout(timeout_phase::response_header, options_.response_header_timeout);
  }

  void on_read(uvp::io::read_result result) {
    if (completed_) {
      return;
    }

    if (result.eof()) {
      finish_buffered_response();
      return;
    }

    if (!result) {
      close_stream();
      complete(make_client_error(errc::client_connect_failed, result.error().message()));
      return;
    }

    process_buffered_response(std::string_view{
      reinterpret_cast<const char*>(result.bytes().data()), result.bytes().size()});
  }

  bool on_buffered_response_event(const detail::http1_response_event& event) {
    switch (event.event_type()) {
    case detail::http1_response_event::type::headers:
      response_head_ = event.head();
      if (!response_headers_complete_) {
        response_headers_complete_ = true;
        start_phase_timeout(timeout_phase::response_body, options_.response_body_timeout);
      }
      break;
    case detail::http1_response_event::type::body:
      response_body_.append(event.body());
      break;
    case detail::http1_response_event::type::complete:
      response_head_ = event.message().head;
      response_keep_alive_ = event.message().keep_alive;
      break;
    }
    return !completed_;
  }

  void process_buffered_response(std::string_view bytes) {
    const auto parsed = response_parser_.parse(bytes, [this](const detail::http1_response_event& event) {
      return on_buffered_response_event(event);
    });
    handle_buffered_parse_result(parsed, bytes.size());
  }

  void finish_buffered_response() {
    const auto parsed = response_parser_.finish([this](const detail::http1_response_event& event) {
      return on_buffered_response_event(event);
    });
    handle_buffered_parse_result(parsed, 0);
  }

  void handle_buffered_parse_result(
    const detail::http1_response_parse_result& parsed,
    std::size_t input_size) {
    if (completed_) {
      return;
    }
    if (parsed.code == detail::http1_response_parse_result::status::error) {
      close_stream();
      complete(response_parse_error(parsed));
      return;
    }
    if (parsed.code == detail::http1_response_parse_result::status::upgrade) {
      close_stream();
      complete(make_client_error(errc::client_malformed_response, "HTTP protocol upgrades are not supported by the client"));
      return;
    }
    if (parsed.code != detail::http1_response_parse_result::status::complete) {
      return;
    }

    const auto reusable = options_.max_idle_connections_per_origin > 0 &&
                          response_keep_alive_ &&
                          parsed.parsed_bytes == input_size;
    complete_buffered_response(make_buffered_response(response_head_, response_body_), reusable);
  }

  void complete_buffered_response(uvp::result<http::response> result, bool reusable) {
    if (result && should_follow_redirect(result.value())) {
      follow_redirect(result.value(), reusable);
      return;
    }

    if (reusable && result && stream_) {
      try {
        stream_.read_stop();
      } catch (...) {
      }
      pool_->release(
        *loop_,
        origin_key_,
        std::move(stream_),
        options_.max_idle_connections_per_origin,
        options_.idle_connection_timeout);
    } else {
      close_stream();
    }
    complete(std::move(result));
  }

  bool should_follow_redirect(const http::response& response) const noexcept {
    return options_.follow_redirects && response_is_redirect(response.status_code());
  }

  void follow_redirect(const http::response& response, bool reusable) {
    if (!method_can_follow_redirect(method_)) {
      close_or_release_current(reusable);
      complete(make_client_error(
        errc::client_redirect_failed,
        "redirect method is not replayable"));
      return;
    }
    if (redirects_followed_ >= options_.max_redirects) {
      close_or_release_current(reusable);
      complete(make_client_error(errc::client_redirect_failed, "too many redirects"));
      return;
    }

    const auto location = trim(response.headers().get("location"));
    if (location.empty()) {
      close_or_release_current(reusable);
      complete(make_client_error(errc::client_redirect_failed, "redirect location is missing"));
      return;
    }

    auto redirected = uvp::parse_url(location, url_.href());
    if (!redirected) {
      close_or_release_current(reusable);
      complete(wrap_client_error(errc::client_redirect_failed, redirected.error()));
      return;
    }

    const auto scheme = uvp::scheme_id(redirected.value());
    if (scheme != uvp::url_scheme::http && scheme != uvp::url_scheme::https) {
      close_or_release_current(reusable);
      complete(make_client_error(errc::client_redirect_failed, "redirect scheme is unsupported"));
      return;
    }

    response_headers_complete_ = false;
    ++redirects_followed_;

    auto self = shared_from_this();
    close_or_release_current(reusable, [self, redirected = std::move(redirected)]() mutable {
      self->start(std::move(redirected));
    });
  }

  void close_or_release_current(bool reusable) {
    close_or_release_current(reusable, {});
  }

  void close_or_release_current(bool reusable, std::function<void()> after) {
    stop_phase_timeout();
    if (reusable && stream_) {
      try {
        stream_.read_stop();
      } catch (...) {
      }
      pool_->release(
        *loop_,
        origin_key_,
        std::move(stream_),
        options_.max_idle_connections_per_origin,
        options_.idle_connection_timeout);
      if (after) {
        after();
      }
      return;
    }
    if (stream_ && after) {
      stream_.close([callback = std::move(after)]() mutable {
        callback();
      });
      return;
    }
    close_stream();
    if (after) {
      after();
    }
  }

  void close_stream() noexcept {
    if (stream_) {
      stream_.close();
    }
  }

  void complete(uvp::result<http::response> result) {
    if (completed_) {
      return;
    }

    completed_ = true;
    stop_phase_timeout();
    auto callback = std::move(callback_);
    if (callback) {
      callback(std::move(result));
    }
  }

  uv::loop* loop_;
  client_options options_;
  std::shared_ptr<detail::connection_pool> pool_;
  http::method method_;
  std::string url_input_;
  client_callback callback_;
  std::string origin_key_;
  std::string proxy_key_;
  uvp::url url_;
  uvp::url proxy_url_;
  uvp::dns::resolver resolver_;
  uvp::io::tcp_connector connector_;
  uvp::dns::resolve_operation dns_operation_;
  uvp::io::connect_operation connect_operation_;
  uvp::tls::handshake_operation tls_operation_;
  uvp::io::byte_stream stream_;
  std::shared_ptr<uv::timer> timeout_timer_;
  std::vector<std::byte> write_payload_;
  detail::http1_response_parser response_parser_;
  http::response_head response_head_;
  std::string response_body_;
  std::size_t redirects_followed_ = 0;
  timeout_phase timeout_phase_ = timeout_phase::none;
  bool response_headers_complete_ = false;
  bool response_keep_alive_ = false;
  bool using_forward_proxy_ = false;
  bool cancelled_ = false;
  bool timed_out_ = false;
  bool completed_ = false;
};

} // namespace

namespace detail {

class streaming_request_state final
    : public request_operation_state,
      public std::enable_shared_from_this<streaming_request_state> {
public:
  streaming_request_state(
    uv::loop& loop,
    client_options options,
    std::shared_ptr<detail::connection_pool> pool,
    http::method method,
    std::string_view url)
      : loop_(&loop),
        options_(std::move(options)),
        pool_(std::move(pool)),
        method_(method),
        url_input_(url),
        resolver_(loop),
        connector_(loop) {}

  void header(std::string_view name, std::string_view value) {
    if (!started_) {
      request_headers_.set(name, value);
    }
  }

  void content_length(std::size_t bytes) {
    if (started_) {
      return;
    }
    upload_mode_ = upload_mode::content_length;
    content_length_ = bytes;
    request_headers_.set("content-length", std::to_string(bytes));
  }

  void chunked() {
    if (started_) {
      return;
    }
    enable_chunked_upload();
  }

  void on_response_headers(response_headers_callback callback) {
    on_headers_ = std::move(callback);
  }

  void on_data(response_data_callback callback) {
    on_data_ = std::move(callback);
  }

  void on_complete(response_complete_callback callback) {
    on_complete_ = std::move(callback);
  }

  void on_drain(request_body_drain_callback callback) {
    on_drain_ = std::move(callback);
  }

  stream_write_result write(std::string payload) {
    if (payload.empty()) {
      return stream_write_result::ready();
    }
    if (completed_ || upload_ended_) {
      return stream_write_result::rejected(std::make_error_code(std::errc::not_connected));
    }
    if (upload_backpressured_) {
      return stream_write_result::rejected(std::make_error_code(std::errc::operation_would_block));
    }
    if (headers_written_ && upload_mode_ == upload_mode::none) {
      return stream_write_result::rejected(std::make_error_code(std::errc::invalid_argument));
    }
    if (upload_mode_ == upload_mode::none) {
      if (headers_started_) {
        return stream_write_result::rejected(std::make_error_code(std::errc::invalid_argument));
      }
      enable_chunked_upload();
    }

    auto wire = std::vector<std::byte>{};
    if (upload_mode_ == upload_mode::content_length) {
      if (!content_length_ || accepted_upload_body_bytes_ + payload.size() > *content_length_) {
        return stream_write_result::rejected(std::make_error_code(std::errc::invalid_argument));
      }
      accepted_upload_body_bytes_ += payload.size();
      wire = to_bytes(payload);
    } else if (upload_mode_ == upload_mode::chunked) {
      accepted_upload_body_bytes_ += payload.size();
      wire = serialize_chunk(std::as_bytes(std::span{payload.data(), payload.size()}));
    } else {
      return stream_write_result::rejected(std::make_error_code(std::errc::invalid_argument));
    }

    pending_upload_wire_bytes_ += wire.size();
    upload_writes_.push_back(pending_upload_write{std::move(wire)});
    flush_upload_writes();

    if (options_.max_pending_request_body_bytes > 0 &&
        pending_upload_wire_bytes_ >= options_.max_pending_request_body_bytes) {
      upload_backpressured_ = true;
      return stream_write_result::backpressure();
    }
    return stream_write_result::ready();
  }

  stream_write_result write(std::span<const std::byte> payload) {
    auto copy = std::string{};
    copy.resize(payload.size());
    if (!payload.empty()) {
      std::memcpy(copy.data(), payload.data(), payload.size());
    }
    return write(std::move(copy));
  }

  void end() {
    if (completed_ || upload_ended_) {
      return;
    }

    if (upload_mode_ == upload_mode::none) {
      upload_ended_ = true;
      if (headers_written_) {
        begin_response_read();
      }
      return;
    }

    if (upload_mode_ == upload_mode::content_length) {
      if (!content_length_ || accepted_upload_body_bytes_ != *content_length_) {
        fail_request_body("content-length request body size mismatch");
        return;
      }
    } else if (upload_mode_ == upload_mode::chunked) {
      pending_upload_wire_bytes_ += 5;
      upload_writes_.push_back(pending_upload_write{to_bytes("0\r\n\r\n")});
    }

    upload_ended_ = true;
    flush_upload_writes();
    if (headers_written_ && upload_writes_.empty() && !upload_writing_) {
      begin_response_read();
    }
  }

  request_body_writer start() {
    started_ = true;
    auto parsed = uvp::parse_url(url_input_);
    if (!parsed) {
      complete(wrap_client_error(errc::client_invalid_url, parsed.error()));
      return request_body_writer{shared_from_this()};
    }

    url_ = std::move(parsed).value();
    const auto scheme = uvp::scheme_id(url_);
    if (scheme != uvp::url_scheme::http && scheme != uvp::url_scheme::https) {
      complete(make_client_error(errc::client_unsupported_scheme));
      return request_body_writer{shared_from_this()};
    }

    if (!options_.proxy.url.empty()) {
      complete(make_client_error(
        errc::client_proxy_failed,
        "HTTP proxying is not implemented for streaming requests"));
      return request_body_writer{shared_from_this()};
    }

    auto endpoint = uvp::authority_endpoint(url_);
    if (!endpoint) {
      complete(wrap_client_error(errc::client_invalid_url, endpoint.error()));
      return request_body_writer{shared_from_this()};
    }

    origin_key_ = origin_key(url_);
    if (auto pooled = pool_->take(origin_key_)) {
      stream_ = std::move(*pooled);
      write_request();
      return request_body_writer{shared_from_this()};
    }

    auto self = shared_from_this();
    dns_operation_ = resolver_.resolve(
      uvp::dns::query{}
        .host(endpoint.value().host)
        .port(endpoint.value().port)
        .family(uvp::dns::address_family::any),
      [self](uvp::result<uvp::dns::address_list> result) mutable {
        self->on_resolved(std::move(result));
      });
    start_phase_timeout(timeout_phase::dns, options_.dns_timeout);

    return request_body_writer{std::move(self)};
  }

  void cancel() noexcept override {
    if (completed_) {
      return;
    }

    cancelled_ = true;
    dns_operation_.cancel();
    connect_operation_.cancel();
    tls_operation_.cancel();
    close_stream();
    complete(make_client_error(errc::client_cancelled));
  }

private:
  enum class timeout_phase {
    none,
    dns,
    tls_handshake,
    request_body,
    response_header,
    response_body,
  };

  enum class upload_mode {
    none,
    content_length,
    chunked,
  };

  struct pending_upload_write {
    std::vector<std::byte> payload;
  };

  void enable_chunked_upload() {
    upload_mode_ = upload_mode::chunked;
    content_length_.reset();
    request_headers_.set("transfer-encoding", "chunked");
  }

  void start_phase_timeout(timeout_phase phase, std::chrono::milliseconds duration) {
    stop_phase_timeout();
    if (duration <= std::chrono::milliseconds{0}) {
      return;
    }

    timeout_phase_ = phase;
    timeout_timer_ = std::make_shared<uv::timer>(*loop_);
    auto self = shared_from_this();
    timeout_timer_->start(duration, [self, phase](uv::timer&) {
      self->on_timeout(phase);
    });
  }

  void stop_phase_timeout() noexcept {
    timeout_phase_ = timeout_phase::none;
    if (!timeout_timer_) {
      return;
    }

    auto timer = std::move(timeout_timer_);
    if (timer->closing()) {
      return;
    }

    try {
      timer->stop();
    } catch (...) {
    }
    timer->close([timer](uv::timer&) {});
  }

  void on_timeout(timeout_phase phase) {
    if (completed_ || phase != timeout_phase_) {
      return;
    }

    cancelled_ = true;
    timed_out_ = true;
    dns_operation_.cancel();
    connect_operation_.cancel();
    tls_operation_.cancel();
    close_stream();
    complete(make_client_error(errc::client_timeout, timeout_phase_name(phase)));
  }

  [[nodiscard]] static std::string timeout_phase_name(timeout_phase phase) {
    switch (phase) {
    case timeout_phase::dns:
      return "DNS resolution timed out";
    case timeout_phase::tls_handshake:
      return "TLS handshake timed out";
    case timeout_phase::request_body:
      return "request body timed out";
    case timeout_phase::response_header:
      return "response headers timed out";
    case timeout_phase::response_body:
      return "response body timed out";
    case timeout_phase::none:
      break;
    }
    return "request timed out";
  }

  void on_resolved(uvp::result<uvp::dns::address_list> result) {
    if (completed_) {
      return;
    }
    if (cancelled_) {
      if (timed_out_) {
        return;
      }
      complete(make_client_error(errc::client_cancelled));
      return;
    }
    if (!result) {
      complete(wrap_client_error(errc::client_dns_failed, result.error()));
      return;
    }

    stop_phase_timeout();
    auto self = shared_from_this();
    connect_operation_ = connector_.connect(
      result.value(),
      uvp::io::connect_options{.timeout = options_.connect_timeout},
      [self](uvp::result<uvp::io::byte_stream> connect_result) mutable {
        self->on_connected(std::move(connect_result));
      });
  }

  void on_connected(uvp::result<uvp::io::byte_stream> result) {
    if (completed_) {
      return;
    }
    if (cancelled_) {
      if (timed_out_) {
        return;
      }
      complete(make_client_error(errc::client_cancelled));
      return;
    }
    if (!result) {
      if (result.error().code == uvp::io::connect_errc::timeout) {
        complete(make_client_error(errc::client_timeout, "TCP connect timed out"));
        return;
      }
      complete(wrap_client_error(errc::client_connect_failed, result.error()));
      return;
    }

    auto stream = std::move(result).value();
    if (uvp::scheme_id(url_) == uvp::url_scheme::https) {
      start_tls(std::move(stream));
      return;
    }

    stream_ = std::move(stream);
    write_request();
  }

  void start_tls(uvp::io::byte_stream lower) {
    try {
      auto context_options = uvp::tls::client_context_options{}
        .server_name(url_.hostname())
        .alpn({"http/1.1"})
        .default_verify_paths(options_.tls_default_verify_paths);
      if (!options_.tls_ca_file.empty()) {
        context_options.ca_file(options_.tls_ca_file);
      }
      if (!options_.tls_ca_path.empty()) {
        context_options.ca_path(options_.tls_ca_path);
      }
      auto context = uvp::tls::client_context{std::move(context_options)};

      start_phase_timeout(timeout_phase::tls_handshake, options_.tls_handshake_timeout);
      auto self = shared_from_this();
      tls_operation_ = uvp::tls::connect(
        std::move(lower),
        std::move(context),
        [self](uvp::tls::handshake_result result) mutable {
          self->on_tls_connected(std::move(result));
        });
    } catch (const std::exception& error) {
      complete(make_client_error(errc::client_tls_failed, error.what()));
    }
  }

  void on_tls_connected(uvp::tls::handshake_result result) {
    if (completed_) {
      return;
    }
    if (cancelled_) {
      if (timed_out_) {
        return;
      }
      complete(make_client_error(errc::client_cancelled));
      return;
    }
    if (!result) {
      complete(wrap_client_error(errc::client_tls_failed, result.error()));
      return;
    }

    const auto selected_alpn = result.selected_alpn();
    if (!selected_alpn.empty() && selected_alpn != "http/1.1") {
      std::move(result).stream().close();
      complete(make_client_error(errc::client_tls_failed, "unsupported TLS ALPN protocol"));
      return;
    }

    stream_ = std::move(result).stream();
    write_request();
  }

  void write_request() {
    auto request = std::string{};
    request += http::to_string(method_);
    request += ' ';
    request += uvp::origin_form_target(url_);
    request += " HTTP/1.1\r\nHost: ";
    request += url_.host();
    if (url_.has_port()) {
      request += ':';
      request += url_.port();
    }
    request += "\r\n";
    if (!request_headers_.contains("user-agent")) {
      request += "User-Agent: uvpp-protocols/0\r\n";
    }
    if (!request_headers_.contains("accept")) {
      request += "Accept: */*\r\n";
    }
    if (!request_headers_.contains("connection")) {
      if (options_.max_idle_connections_per_origin == 0) {
        request += "Connection: close\r\n";
      }
    }
    for (const auto& [name, value] : request_headers_) {
      if (http::headers::names_equal(name, "host")) {
        continue;
      }
      request += name;
      request += ": ";
      request += value;
      request += "\r\n";
    }
    request += "\r\n";

    headers_started_ = true;
    if (upload_mode_ == upload_mode::content_length && content_length_ == 0) {
      upload_ended_ = true;
    }
    write_payload_.resize(request.size());
    std::memcpy(write_payload_.data(), request.data(), request.size());
    auto self = shared_from_this();
    start_phase_timeout(timeout_phase::request_body, options_.request_body_timeout);
    stream_.write(write_payload_, [self](uvp::io::stream_error result) {
      self->on_request_headers_written(result);
    });
  }

  void on_request_headers_written(uvp::io::stream_error result) {
    if (completed_) {
      return;
    }
    if (result) {
      complete(make_client_error(errc::client_request_body_failed, result.message()));
      return;
    }

    headers_written_ = true;
    if (upload_mode_ == upload_mode::none || upload_ended_) {
      if (upload_writes_.empty() && !upload_writing_) {
        begin_response_read();
        return;
      }
    }
    flush_upload_writes();
  }

  void flush_upload_writes() {
    if (completed_ || !headers_written_ || upload_writing_ || upload_writes_.empty()) {
      return;
    }

    upload_writing_ = true;
    auto self = shared_from_this();
    stream_.write(upload_writes_.front().payload, [self](uvp::io::stream_error result) {
      self->on_upload_written(result);
    });
  }

  void on_upload_written(uvp::io::stream_error result) {
    if (completed_) {
      return;
    }
    upload_writing_ = false;
    if (result) {
      fail_request_body(result.message());
      return;
    }

    if (!upload_writes_.empty()) {
      pending_upload_wire_bytes_ -= std::min(pending_upload_wire_bytes_, upload_writes_.front().payload.size());
      upload_writes_.pop_front();
    }
    notify_upload_drain_if_needed();

    if (!upload_writes_.empty()) {
      flush_upload_writes();
      return;
    }
    if (upload_ended_) {
      begin_response_read();
    }
  }

  void notify_upload_drain_if_needed() {
    if (!upload_backpressured_) {
      return;
    }
    const auto low_watermark = options_.max_pending_request_body_bytes / 2;
    if (pending_upload_wire_bytes_ > low_watermark) {
      return;
    }

    upload_backpressured_ = false;
    if (on_drain_) {
      on_drain_();
    }
  }

  void begin_response_read() {
    if (completed_ || reading_response_) {
      return;
    }

    reading_response_ = true;
    response_parser_.reset(method_);
    response_parser_.limits(response_limits(options_));
    response_head_ = {};
    response_keep_alive_ = false;
    auto self = shared_from_this();
    stream_.read_start(
      [self](uvp::io::read_result result) {
        self->on_read(result);
      });
    start_phase_timeout(timeout_phase::response_header, options_.response_header_timeout);
  }

  void fail_request_body(std::string detail) {
    close_stream();
    complete(make_client_error(errc::client_request_body_failed, std::move(detail)));
  }

  void on_read(uvp::io::read_result result) {
    if (completed_) {
      return;
    }

    if (result.eof()) {
      on_eof();
      return;
    }

    if (!result) {
      close_stream();
      complete(make_client_error(errc::client_connect_failed, result.error().message()));
      return;
    }

    process_response(std::string_view{
      reinterpret_cast<const char*>(result.bytes().data()), result.bytes().size()});
  }

  bool on_response_event(const detail::http1_response_event& event) {
    switch (event.event_type()) {
    case detail::http1_response_event::type::headers:
      response_head_ = event.head();
      if (on_headers_) {
        on_headers_(response_head_);
      }
      if (!completed_) {
        start_phase_timeout(timeout_phase::response_body, options_.response_body_timeout);
      }
      break;
    case detail::http1_response_event::type::body:
      if (on_data_) {
        const auto body = event.body();
        on_data_(std::as_bytes(std::span{body.data(), body.size()}));
      }
      break;
    case detail::http1_response_event::type::complete:
      response_head_ = event.message().head;
      response_keep_alive_ = event.message().keep_alive;
      break;
    }
    return !completed_;
  }

  void process_response(std::string_view bytes) {
    const auto parsed = response_parser_.parse(bytes, [this](const detail::http1_response_event& event) {
      return on_response_event(event);
    });
    handle_response_parse_result(parsed, bytes.size());
  }

  void on_eof() {
    const auto parsed = response_parser_.finish([this](const detail::http1_response_event& event) {
      return on_response_event(event);
    });
    handle_response_parse_result(parsed, 0);
  }

  void handle_response_parse_result(
    const detail::http1_response_parse_result& parsed,
    std::size_t input_size) {
    if (completed_) {
      return;
    }
    if (parsed.code == detail::http1_response_parse_result::status::error) {
      close_stream();
      complete(response_parse_error(parsed));
      return;
    }
    if (parsed.code == detail::http1_response_parse_result::status::upgrade) {
      close_stream();
      complete(make_client_error(errc::client_malformed_response, "HTTP protocol upgrades are not supported by the client"));
      return;
    }
    if (parsed.code != detail::http1_response_parse_result::status::complete) {
      return;
    }

    const auto reusable = options_.max_idle_connections_per_origin > 0 &&
                          response_keep_alive_ &&
                          parsed.parsed_bytes == input_size;
    finish_response_success(reusable);
  }

  void close_stream() noexcept {
    if (stream_) {
      stream_.close();
    }
  }

  void complete_success() {
    complete(uvp::result<void>{});
  }

  void finish_response_success(bool reusable) {
    if (reusable && stream_) {
      try {
        stream_.read_stop();
      } catch (...) {
      }
      pool_->release(
        *loop_,
        origin_key_,
        std::move(stream_),
        options_.max_idle_connections_per_origin,
        options_.idle_connection_timeout);
    } else {
      close_stream();
    }
    complete_success();
  }

  void complete(uvp::result<void> result) {
    if (completed_) {
      return;
    }

    completed_ = true;
    stop_phase_timeout();
    auto callback = std::move(on_complete_);
    if (callback) {
      callback(std::move(result));
    }
  }

  uv::loop* loop_;
  client_options options_;
  std::shared_ptr<detail::connection_pool> pool_;
  http::method method_;
  std::string url_input_;
  std::string origin_key_;
  uvp::url url_;
  uvp::dns::resolver resolver_;
  uvp::io::tcp_connector connector_;
  uvp::dns::resolve_operation dns_operation_;
  uvp::io::connect_operation connect_operation_;
  uvp::tls::handshake_operation tls_operation_;
  uvp::io::byte_stream stream_;
  std::shared_ptr<uv::timer> timeout_timer_;
  response_headers_callback on_headers_;
  response_data_callback on_data_;
  response_complete_callback on_complete_;
  request_body_drain_callback on_drain_;
  http::headers request_headers_;
  http::response_head response_head_;
  detail::http1_response_parser response_parser_;
  std::vector<std::byte> write_payload_;
  std::deque<pending_upload_write> upload_writes_;
  std::optional<std::size_t> content_length_;
  std::size_t accepted_upload_body_bytes_ = 0;
  std::size_t pending_upload_wire_bytes_ = 0;
  timeout_phase timeout_phase_ = timeout_phase::none;
  upload_mode upload_mode_ = upload_mode::none;
  bool started_ = false;
  bool headers_started_ = false;
  bool headers_written_ = false;
  bool upload_writing_ = false;
  bool upload_ended_ = false;
  bool upload_backpressured_ = false;
  bool reading_response_ = false;
  bool response_keep_alive_ = false;
  bool cancelled_ = false;
  bool timed_out_ = false;
  bool completed_ = false;
};

} // namespace detail

proxy_options& proxy_options::basic_auth(std::string_view username, std::string_view password) & {
  auto credentials = std::string{username};
  credentials += ':';
  credentials += password;
  authorization = "Basic ";
  authorization += base64_encode(credentials);
  return *this;
}

proxy_options&& proxy_options::basic_auth(std::string_view username, std::string_view password) && {
  basic_auth(username, password);
  return std::move(*this);
}

request_operation::request_operation(std::shared_ptr<detail::request_operation_state> state)
    : state_(std::move(state)) {}

void request_operation::cancel() noexcept {
  if (!state_) {
    return;
  }
  state_->cancel();
}

request_body_writer::request_body_writer(std::shared_ptr<detail::streaming_request_state> state)
    : state_(std::move(state)) {}

request_body_writer& request_body_writer::on_drain(request_body_drain_callback callback) & {
  if (state_) {
    state_->on_drain(std::move(callback));
  }
  return *this;
}

request_body_writer&& request_body_writer::on_drain(request_body_drain_callback callback) && {
  on_drain(std::move(callback));
  return std::move(*this);
}

stream_write_result request_body_writer::write(const char* chunk) {
  return write(std::string_view{chunk});
}

stream_write_result request_body_writer::write(std::string_view chunk) {
  return write(std::string{chunk});
}

stream_write_result request_body_writer::write(std::span<const std::byte> chunk) {
  if (!state_) {
    return stream_write_result::rejected(std::make_error_code(std::errc::not_connected));
  }
  return state_->write(chunk);
}

stream_write_result request_body_writer::write(std::string chunk) {
  if (!state_) {
    return stream_write_result::rejected(std::make_error_code(std::errc::not_connected));
  }
  return state_->write(std::move(chunk));
}

void request_body_writer::end() {
  if (state_) {
    state_->end();
  }
}

void request_body_writer::cancel() noexcept {
  if (state_) {
    state_->cancel();
  }
}

streaming_request::streaming_request(std::shared_ptr<detail::streaming_request_state> state)
    : state_(std::move(state)) {}

streaming_request& streaming_request::header(std::string_view name, std::string_view value) & {
  if (state_) {
    state_->header(name, value);
  }
  return *this;
}

streaming_request&& streaming_request::header(std::string_view name, std::string_view value) && {
  header(name, value);
  return std::move(*this);
}

streaming_request& streaming_request::content_length(std::size_t bytes) & {
  if (state_) {
    state_->content_length(bytes);
  }
  return *this;
}

streaming_request&& streaming_request::content_length(std::size_t bytes) && {
  content_length(bytes);
  return std::move(*this);
}

streaming_request& streaming_request::chunked() & {
  if (state_) {
    state_->chunked();
  }
  return *this;
}

streaming_request&& streaming_request::chunked() && {
  chunked();
  return std::move(*this);
}

streaming_request& streaming_request::on_response_headers(response_headers_callback callback) & {
  if (state_) {
    state_->on_response_headers(std::move(callback));
  }
  return *this;
}

streaming_request&& streaming_request::on_response_headers(response_headers_callback callback) && {
  on_response_headers(std::move(callback));
  return std::move(*this);
}

streaming_request& streaming_request::on_data(response_data_callback callback) & {
  if (state_) {
    state_->on_data(std::move(callback));
  }
  return *this;
}

streaming_request&& streaming_request::on_data(response_data_callback callback) && {
  on_data(std::move(callback));
  return std::move(*this);
}

streaming_request& streaming_request::on_complete(response_complete_callback callback) & {
  if (state_) {
    state_->on_complete(std::move(callback));
  }
  return *this;
}

streaming_request&& streaming_request::on_complete(response_complete_callback callback) && {
  on_complete(std::move(callback));
  return std::move(*this);
}

request_body_writer streaming_request::start() {
  if (!state_) {
    return {};
  }
  return state_->start();
}

client::client(uv::loop& loop)
    : client(loop, client_options{}) {}

client::client(uv::loop& loop, client_options options)
    : loop_(&loop), options_(options), pool_(std::make_shared<detail::connection_pool>()) {}

request_operation client::get(std::string_view url, client_callback callback) {
  return fetch(http::method::get, url, std::move(callback));
}

request_operation client::fetch(http::method method, std::string_view url, client_callback callback) {
  auto state = std::make_shared<request_state>(*loop_, options_, pool_, method, url, std::move(callback));
  return state->start();
}

streaming_request client::request(http::method method, std::string_view url) {
  return streaming_request{
    std::make_shared<detail::streaming_request_state>(*loop_, options_, pool_, method, url)};
}

streaming_request client::stream(http::method method, std::string_view url) {
  return request(method, url);
}

streaming_request client::stream_get(std::string_view url) {
  return stream(http::method::get, url);
}

void client::close_idle_connections() noexcept {
  if (pool_) {
    pool_->close_all();
  }
}

} // namespace uvp::http
