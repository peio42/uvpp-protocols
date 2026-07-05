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

[[nodiscard]] uvp::result<http::response> parse_response(std::string_view bytes, std::size_t max_body_bytes) {
  const auto header_end = bytes.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return make_client_error(errc::client_malformed_response, "response headers are incomplete");
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
    if (colon == std::string_view::npos) {
      return make_client_error(errc::client_malformed_response, "response header line is invalid");
    }
    response_headers.add(line.substr(0, colon), trim(line.substr(colon + 1)));
    if (line_end == std::string_view::npos) {
      break;
    }
    line_offset = line_end + 2;
  }

  auto body = std::string{};
  const auto raw_body = bytes.substr(header_end + 4);
  const auto transfer_encoding = lowercase(response_headers.get("transfer-encoding"));
  if (transfer_encoding.find("chunked") != std::string::npos) {
    auto error = std::string{};
    if (!decode_chunked(raw_body, body, error)) {
      return make_client_error(errc::client_malformed_response, std::move(error));
    }
  } else if (const auto length = response_headers.get("content-length"); !length.empty()) {
    auto content_length = 0U;
    if (!parse_uint(length, content_length)) {
      return make_client_error(errc::client_malformed_response, "content-length is invalid");
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
  out.status(status_code);
  for (const auto& [name, value] : response_headers) {
    out.header(name, value);
  }
  out.bytes(std::as_bytes(std::span{body.data(), body.size()}));
  return out;
}

class request_state : public std::enable_shared_from_this<request_state> {
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

  void cancel() noexcept {
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
      auto parsed = parse_response(received_, options_.max_body_bytes);
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
    if (!response_headers_complete_ && received_.find("\r\n\r\n") != std::string::npos) {
      response_headers_complete_ = true;
      start_phase_timeout(timeout_phase::response_body, options_.response_body_timeout);
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

[[nodiscard]] std::shared_ptr<request_state> request_state_from(const std::shared_ptr<void>& state) noexcept {
  return std::static_pointer_cast<request_state>(state);
}

} // namespace

request_operation::request_operation(std::shared_ptr<void> state)
    : state_(std::move(state)) {}

void request_operation::cancel() noexcept {
  if (!state_) {
    return;
  }
  request_state_from(state_)->cancel();
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

} // namespace uvp::http
