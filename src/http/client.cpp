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
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uvp::http {

namespace detail {

struct request_operation_state {
  virtual ~request_operation_state() = default;
  virtual void cancel() noexcept = 0;
};

} // namespace detail

namespace {

[[nodiscard]] uvp::error make_client_error(errc code, std::string detail = {}) {
  return uvp::error{make_error_code(code), std::move(detail)};
}

[[nodiscard]] uvp::error wrap_client_error(errc code, const uvp::error& source) {
  return make_client_error(code, source.detail.empty() ? source.code.message() : source.detail);
}

[[nodiscard]] std::string lowercase(std::string_view value) {
  auto result = std::string{value};
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
      return static_cast<char>(ch - 'A' + 'a');
    }
    return static_cast<char>(ch);
  });
  return result;
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

[[nodiscard]] bool parse_uint(std::string_view value, unsigned int& out, int base = 10) {
  const auto* first = value.data();
  const auto* last = first + value.size();
  const auto [ptr, ec] = std::from_chars(first, last, out, base);
  return ec == std::errc{} && ptr == last;
}

[[nodiscard]] bool response_must_not_have_body(http::method request_method, unsigned int status_code) noexcept {
  return request_method == http::method::head ||
         (status_code >= 100 && status_code < 200) ||
         status_code == 204 ||
         status_code == 304;
}

struct parsed_response_head {
  http::response_head head;
  std::size_t body_offset = 0;
};

[[nodiscard]] uvp::result<parsed_response_head> parse_response_head(
  std::string_view bytes,
  std::size_t max_header_bytes) {
  const auto header_end = bytes.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return make_client_error(errc::client_malformed_response, "response headers are incomplete");
  }
  if (header_end + 4 > max_header_bytes) {
    return make_client_error(errc::client_header_limit_exceeded);
  }

  const auto header_block = bytes.substr(0, header_end);
  const auto first_line_end = header_block.find("\r\n");
  const auto status_line = first_line_end == std::string_view::npos ? header_block : header_block.substr(0, first_line_end);
  if (!status_line.starts_with("HTTP/")) {
    return make_client_error(errc::client_malformed_response, "response status line is invalid");
  }

  const auto status_start = status_line.find(' ');
  if (status_start == std::string_view::npos || status_line.size() < status_start + 4) {
    return make_client_error(errc::client_malformed_response, "response status code is missing");
  }

  auto status_code = 0U;
  if (!parse_uint(status_line.substr(status_start + 1, 3), status_code)) {
    return make_client_error(errc::client_malformed_response, "response status code is invalid");
  }

  auto response_headers = http::headers{};
  std::size_t line_offset = first_line_end == std::string_view::npos ? header_block.size() : first_line_end + 2;
  while (line_offset < header_block.size()) {
    const auto line_end = header_block.find("\r\n", line_offset);
    const auto end = line_end == std::string_view::npos ? header_block.size() : line_end;
    const auto line = header_block.substr(line_offset, end - line_offset);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return make_client_error(errc::client_malformed_response, "response header line is invalid");
    }
    response_headers.add(line.substr(0, colon), trim(line.substr(colon + 1)));
    if (line_end == std::string_view::npos) {
      break;
    }
    line_offset = line_end + 2;
  }

  return parsed_response_head{
    http::response_head{
      .status_code = status_code,
      .headers = std::move(response_headers),
    },
    header_end + 4,
  };
}

[[nodiscard]] bool decode_chunked(std::string_view encoded, std::string& out, std::string& error) {
  std::size_t offset = 0;
  while (true) {
    const auto line_end = encoded.find("\r\n", offset);
    if (line_end == std::string_view::npos) {
      error = "chunk size line is incomplete";
      return false;
    }

    auto size_line = encoded.substr(offset, line_end - offset);
    if (const auto semicolon = size_line.find(';'); semicolon != std::string_view::npos) {
      size_line = size_line.substr(0, semicolon);
    }

    auto chunk_size = 0U;
    if (!parse_uint(size_line, chunk_size, 16)) {
      error = "chunk size is invalid";
      return false;
    }

    offset = line_end + 2;
    if (chunk_size == 0) {
      if (encoded.substr(offset, 2) == "\r\n" && offset + 2 == encoded.size()) {
        return true;
      }
      const auto trailer_end = encoded.find("\r\n\r\n", offset);
      if (trailer_end == std::string_view::npos) {
        error = "chunk trailers are incomplete";
        return false;
      }
      if (trailer_end + 4 != encoded.size()) {
        error = "unexpected bytes after chunked body";
        return false;
      }
      return true;
    }

    if (encoded.size() - offset < chunk_size + 2) {
      error = "chunk body is incomplete";
      return false;
    }

    out.append(encoded.substr(offset, chunk_size));
    offset += chunk_size;
    if (encoded.substr(offset, 2) != "\r\n") {
      error = "chunk body is not terminated";
      return false;
    }
    offset += 2;
  }
}

[[nodiscard]] uvp::result<http::response> parse_response(
  http::method request_method,
  std::string_view bytes,
  std::size_t max_header_bytes,
  std::size_t max_body_bytes) {
  auto parsed_head = parse_response_head(bytes, max_header_bytes);
  if (!parsed_head) {
    return parsed_head.error();
  }

  auto body = std::string{};
  const auto& head = parsed_head.value().head;
  const auto raw_body = bytes.substr(parsed_head.value().body_offset);
  const auto transfer_encoding = lowercase(head.headers.get("transfer-encoding"));
  if (response_must_not_have_body(request_method, head.status_code)) {
    body.clear();
  } else if (transfer_encoding.find("chunked") != std::string::npos) {
    auto error = std::string{};
    if (!decode_chunked(raw_body, body, error)) {
      return make_client_error(errc::client_malformed_response, std::move(error));
    }
  } else if (const auto length = head.headers.get("content-length"); !length.empty()) {
    auto content_length = 0U;
    if (!parse_uint(length, content_length)) {
      return make_client_error(errc::client_malformed_response, "content-length is invalid");
    }
    if (content_length > max_body_bytes) {
      return make_client_error(errc::client_body_limit_exceeded);
    }
    if (raw_body.size() < content_length) {
      return make_client_error(errc::client_malformed_response, "response body is incomplete");
    }
    body.assign(raw_body.substr(0, content_length));
  } else {
    body.assign(raw_body);
  }

  if (body.size() > max_body_bytes) {
    return make_client_error(errc::client_body_limit_exceeded);
  }

  auto out = http::response{};
  out.status(head.status_code);
  for (const auto& [name, value] : head.headers) {
    out.header(name, value);
  }
  out.bytes(std::as_bytes(std::span{body.data(), body.size()}));
  return out;
}

class request_state : public detail::request_operation_state, public std::enable_shared_from_this<request_state> {
public:
  request_state(uv::loop& loop, client_options options, http::method method, std::string_view url, client_callback callback)
      : loop_(&loop),
        options_(options),
        method_(method),
        url_input_(url),
        callback_(std::move(callback)),
        resolver_(loop),
        connector_(loop) {}

  request_operation start() {
    auto parsed = uvp::parse_url(url_input_);
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
      auto context = uvp::tls::client_context{}
        .server_name(url_.hostname())
        .alpn({"http/1.1"});
      if (options_.tls_default_verify_paths) {
        context.default_verify_paths();
      }
      if (!options_.tls_ca_file.empty()) {
        context.ca_file(options_.tls_ca_file);
      }
      if (!options_.tls_ca_path.empty()) {
        context.ca_path(options_.tls_ca_path);
      }

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
    request += "\r\nUser-Agent: uvpp-protocols/0\r\nAccept: */*\r\nConnection: close\r\n\r\n";

    write_payload_.resize(request.size());
    std::memcpy(write_payload_.data(), request.data(), request.size());
    auto self = shared_from_this();
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
      auto parsed = parse_response(method_, received_, options_.max_header_bytes, options_.max_body_bytes);
      close_stream();
      complete(std::move(parsed));
      return;
    }

    if (!result) {
      close_stream();
      complete(make_client_error(errc::client_connect_failed, result.error().message()));
      return;
    }

    received_.append(reinterpret_cast<const char*>(result.bytes().data()), result.bytes().size());
    const auto header_end = received_.find("\r\n\r\n");
    if (!response_headers_complete_ && header_end != std::string::npos) {
      if (header_end + 4 > options_.max_header_bytes) {
        close_stream();
        complete(make_client_error(errc::client_header_limit_exceeded));
        return;
      }
      response_headers_complete_ = true;
      start_phase_timeout(timeout_phase::response_body, options_.response_body_timeout);
    }

    if (!response_headers_complete_ && received_.size() > options_.max_header_bytes) {
      close_stream();
      complete(make_client_error(errc::client_header_limit_exceeded));
      return;
    }

    if (received_.size() > options_.max_header_bytes + options_.max_body_bytes + 4096) {
      close_stream();
      complete(make_client_error(errc::client_body_limit_exceeded));
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
  http::method method_;
  std::string url_input_;
  client_callback callback_;
  uvp::url url_;
  uvp::dns::resolver resolver_;
  uvp::io::tcp_connector connector_;
  uvp::dns::resolve_operation dns_operation_;
  uvp::io::connect_operation connect_operation_;
  uvp::tls::handshake_operation tls_operation_;
  uvp::io::byte_stream stream_;
  std::shared_ptr<uv::timer> timeout_timer_;
  std::vector<std::byte> write_payload_;
  std::string received_;
  timeout_phase timeout_phase_ = timeout_phase::none;
  bool response_headers_complete_ = false;
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
  streaming_request_state(uv::loop& loop, client_options options, http::method method, std::string_view url)
      : loop_(&loop),
        options_(std::move(options)),
        method_(method),
        url_input_(url),
        resolver_(loop),
        connector_(loop) {}

  void on_response_headers(response_headers_callback callback) {
    on_headers_ = std::move(callback);
  }

  void on_data(response_data_callback callback) {
    on_data_ = std::move(callback);
  }

  void on_complete(response_complete_callback callback) {
    on_complete_ = std::move(callback);
  }

  request_operation start() {
    auto parsed = uvp::parse_url(url_input_);
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
    response_header,
    response_body,
  };

  enum class body_mode {
    unknown,
    none,
    content_length,
    chunked,
    eof,
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
      auto context = uvp::tls::client_context{}
        .server_name(url_.hostname())
        .alpn({"http/1.1"});
      if (options_.tls_default_verify_paths) {
        context.default_verify_paths();
      }
      if (!options_.tls_ca_file.empty()) {
        context.ca_file(options_.tls_ca_file);
      }
      if (!options_.tls_ca_path.empty()) {
        context.ca_path(options_.tls_ca_path);
      }

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
    request += "\r\nUser-Agent: uvpp-protocols/0\r\nAccept: */*\r\nConnection: close\r\n\r\n";

    write_payload_.resize(request.size());
    std::memcpy(write_payload_.data(), request.data(), request.size());
    auto self = shared_from_this();
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
      on_eof();
      return;
    }

    if (!result) {
      close_stream();
      complete(make_client_error(errc::client_connect_failed, result.error().message()));
      return;
    }

    buffer_.append(reinterpret_cast<const char*>(result.bytes().data()), result.bytes().size());
    process_buffer();
  }

  void process_buffer() {
    if (completed_) {
      return;
    }

    if (!headers_seen_) {
      const auto header_end = buffer_.find("\r\n\r\n");
      if (header_end == std::string::npos) {
        if (buffer_.size() > options_.max_header_bytes) {
          close_stream();
          complete(make_client_error(errc::client_header_limit_exceeded));
        }
        return;
      }

      auto parsed = parse_response_head(buffer_, options_.max_header_bytes);
      if (!parsed) {
        close_stream();
        complete(parsed.error());
        return;
      }

      response_head_ = std::move(parsed.value().head);
      buffer_.erase(0, parsed.value().body_offset);
      headers_seen_ = true;
      if (on_headers_) {
        on_headers_(response_head_);
      }

      if (response_must_not_have_body(method_, response_head_.status_code)) {
        body_mode_ = body_mode::none;
        close_stream();
        complete_success();
        return;
      }

      const auto transfer_encoding = lowercase(response_head_.headers.get("transfer-encoding"));
      if (transfer_encoding.find("chunked") != std::string::npos) {
        body_mode_ = body_mode::chunked;
      } else if (const auto length = response_head_.headers.get("content-length"); !length.empty()) {
        auto parsed_length = 0U;
        if (!parse_uint(length, parsed_length)) {
          close_stream();
          complete(make_client_error(errc::client_malformed_response, "content-length is invalid"));
          return;
        }
        if (parsed_length > options_.max_body_bytes) {
          close_stream();
          complete(make_client_error(errc::client_body_limit_exceeded));
          return;
        }
        body_mode_ = body_mode::content_length;
        remaining_content_length_ = parsed_length;
      } else {
        body_mode_ = body_mode::eof;
      }

      start_phase_timeout(timeout_phase::response_body, options_.response_body_timeout);
    }

    switch (body_mode_) {
    case body_mode::none:
      return;
    case body_mode::content_length:
      process_content_length_body();
      return;
    case body_mode::chunked:
      process_chunked_body();
      return;
    case body_mode::eof:
      emit_buffer_as_body();
      return;
    case body_mode::unknown:
      return;
    }
  }

  void process_content_length_body() {
    while (!completed_ && remaining_content_length_ > 0 && !buffer_.empty()) {
      const auto size = std::min<std::size_t>(remaining_content_length_, buffer_.size());
      emit_body(buffer_.data(), size);
      buffer_.erase(0, size);
      remaining_content_length_ -= size;
    }

    if (!completed_ && remaining_content_length_ == 0) {
      close_stream();
      complete_success();
    }
  }

  void process_chunked_body() {
    while (!completed_) {
      const auto line_end = buffer_.find("\r\n");
      if (line_end == std::string::npos) {
        return;
      }

      auto size_line = std::string_view{buffer_.data(), line_end};
      if (const auto semicolon = size_line.find(';'); semicolon != std::string_view::npos) {
        size_line = size_line.substr(0, semicolon);
      }

      auto chunk_size = 0U;
      if (!parse_uint(size_line, chunk_size, 16)) {
        close_stream();
        complete(make_client_error(errc::client_malformed_response, "chunk size is invalid"));
        return;
      }

      const auto data_offset = line_end + 2;
      if (chunk_size == 0) {
        if (buffer_.substr(data_offset, 2) == "\r\n") {
          buffer_.erase(0, data_offset + 2);
          close_stream();
          complete_success();
          return;
        }
        const auto trailer_end = buffer_.find("\r\n\r\n", data_offset);
        if (trailer_end == std::string::npos) {
          return;
        }
        buffer_.erase(0, trailer_end + 4);
        close_stream();
        complete_success();
        return;
      }

      if (buffer_.size() - data_offset < chunk_size + 2) {
        return;
      }

      if (delivered_body_bytes_ + chunk_size > options_.max_body_bytes) {
        close_stream();
        complete(make_client_error(errc::client_body_limit_exceeded));
        return;
      }

      if (buffer_.substr(data_offset + chunk_size, 2) != "\r\n") {
        close_stream();
        complete(make_client_error(errc::client_malformed_response, "chunk body is not terminated"));
        return;
      }

      emit_body(buffer_.data() + data_offset, chunk_size);
      buffer_.erase(0, data_offset + chunk_size + 2);
    }
  }

  void emit_buffer_as_body() {
    if (buffer_.empty()) {
      return;
    }
    if (delivered_body_bytes_ + buffer_.size() > options_.max_body_bytes) {
      close_stream();
      complete(make_client_error(errc::client_body_limit_exceeded));
      return;
    }
    emit_body(buffer_.data(), buffer_.size());
    buffer_.clear();
  }

  void emit_body(const char* data, std::size_t size) {
    if (size == 0) {
      return;
    }
    delivered_body_bytes_ += size;
    if (on_data_) {
      on_data_(std::as_bytes(std::span{data, size}));
    }
  }

  void on_eof() {
    if (!headers_seen_) {
      close_stream();
      complete(make_client_error(errc::client_malformed_response, "response headers are incomplete"));
      return;
    }

    switch (body_mode_) {
    case body_mode::content_length:
      if (remaining_content_length_ != 0) {
        close_stream();
        complete(make_client_error(errc::client_malformed_response, "response body is incomplete"));
        return;
      }
      break;
    case body_mode::chunked:
      close_stream();
      complete(make_client_error(errc::client_malformed_response, "chunked body is incomplete"));
      return;
    case body_mode::eof:
      emit_buffer_as_body();
      if (completed_) {
        return;
      }
      break;
    case body_mode::none:
    case body_mode::unknown:
      break;
    }

    close_stream();
    complete_success();
  }

  void close_stream() noexcept {
    if (stream_) {
      stream_.close();
    }
  }

  void complete_success() {
    complete(uvp::result<void>{});
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
  http::method method_;
  std::string url_input_;
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
  http::response_head response_head_;
  std::vector<std::byte> write_payload_;
  std::string buffer_;
  std::size_t remaining_content_length_ = 0;
  std::size_t delivered_body_bytes_ = 0;
  timeout_phase timeout_phase_ = timeout_phase::none;
  body_mode body_mode_ = body_mode::unknown;
  bool headers_seen_ = false;
  bool cancelled_ = false;
  bool timed_out_ = false;
  bool completed_ = false;
};

} // namespace detail

request_operation::request_operation(std::shared_ptr<detail::request_operation_state> state)
    : state_(std::move(state)) {}

void request_operation::cancel() noexcept {
  if (!state_) {
    return;
  }
  state_->cancel();
}

streaming_request::streaming_request(std::shared_ptr<detail::streaming_request_state> state)
    : state_(std::move(state)) {}

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

request_operation streaming_request::start() {
  if (!state_) {
    return {};
  }
  return state_->start();
}

client::client(uv::loop& loop)
    : client(loop, client_options{}) {}

client::client(uv::loop& loop, client_options options)
    : loop_(&loop), options_(options) {}

request_operation client::get(std::string_view url, client_callback callback) {
  return fetch(http::method::get, url, std::move(callback));
}

request_operation client::fetch(http::method method, std::string_view url, client_callback callback) {
  auto state = std::make_shared<request_state>(*loop_, options_, method, url, std::move(callback));
  return state->start();
}

streaming_request client::stream(http::method method, std::string_view url) {
  return streaming_request{
    std::make_shared<detail::streaming_request_state>(*loop_, options_, method, url)};
}

streaming_request client::stream_get(std::string_view url) {
  return stream(http::method::get, url);
}

} // namespace uvp::http
