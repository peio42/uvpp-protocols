#include <uvpp/protocols/websocket/client.hpp>

#include <uvpp/protocols/detail/operation_deadline.hpp>
#include <uvpp/protocols/detail/operation_lifetime.hpp>
#include <uvpp/protocols/dns.hpp>
#include <uvpp/protocols/io/tcp_connector.hpp>
#include <uvpp/protocols/tls.hpp>
#include <uvpp/protocols/url.hpp>
#include <uvpp/uv.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "detail/handshake.hpp"

namespace uvp::websocket {

namespace {

inline constexpr uvp::detail::operation_phase dns_phase{"dns"};
inline constexpr uvp::detail::operation_phase connect_phase{"connect"};
inline constexpr uvp::detail::operation_phase tls_phase{"tls-handshake"};
inline constexpr uvp::detail::operation_phase handshake_phase{"websocket-handshake"};

class client_error_category_impl final : public std::error_category {
public:
  [[nodiscard]] const char* name() const noexcept override { return "uvp.websocket.client"; }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<client_errc>(value)) {
    case client_errc::invalid_url:
      return "WebSocket client URL is invalid";
    case client_errc::unsupported_scheme:
      return "WebSocket client URL scheme is unsupported";
    case client_errc::invalid_option:
      return "WebSocket client option is invalid";
    case client_errc::dns_failed:
      return "WebSocket client DNS resolution failed";
    case client_errc::connect_failed:
      return "WebSocket client connection failed";
    case client_errc::tls_failed:
      return "WebSocket client TLS handshake failed";
    case client_errc::handshake_failed:
      return "WebSocket client handshake failed";
    case client_errc::cancelled:
      return "WebSocket client connection was cancelled";
    case client_errc::timeout:
      return "WebSocket client connection timed out";
    }
    return "unknown WebSocket client error";
  }
};

[[nodiscard]] uvp::error make_client_error(client_errc code, std::string detail = {}) {
  return {make_error_code(code), std::move(detail)};
}

[[nodiscard]] uvp::error wrap_client_error(client_errc code, const uvp::error& source) {
  return make_client_error(code, source.detail.empty() ? source.code.message() : source.detail);
}

[[nodiscard]] char lower_ascii(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

[[nodiscard]] bool equal_ascii_ci(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (lower_ascii(lhs[index]) != lower_ascii(rhs[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] bool token_list_contains(std::string_view value, std::string_view expected) noexcept {
  while (!value.empty()) {
    const auto comma = value.find(',');
    if (equal_ascii_ci(trim(value.substr(0, comma)), expected)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    value.remove_prefix(comma + 1U);
  }
  return false;
}

[[nodiscard]] std::string base64(std::span<const std::byte> bytes) {
  static constexpr std::string_view alphabet{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
  auto output = std::string{};
  output.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
    const auto b0 = static_cast<unsigned char>(bytes[offset]);
    const auto b1 = offset + 1U < bytes.size() ? static_cast<unsigned char>(bytes[offset + 1U]) : 0U;
    const auto b2 = offset + 2U < bytes.size() ? static_cast<unsigned char>(bytes[offset + 2U]) : 0U;
    output.push_back(alphabet[(b0 >> 2U) & 0x3fU]);
    output.push_back(alphabet[((b0 & 0x03U) << 4U) | ((b1 >> 4U) & 0x0fU)]);
    output.push_back(offset + 1U < bytes.size() ? alphabet[((b1 & 0x0fU) << 2U) | ((b2 >> 6U) & 0x03U)] : '=');
    output.push_back(offset + 2U < bytes.size() ? alphabet[b2 & 0x3fU] : '=');
  }
  return output;
}

[[nodiscard]] std::string fresh_websocket_key() {
  std::array<std::byte, 16> nonce{};
  if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), static_cast<int>(nonce.size())) != 1) {
    throw std::runtime_error("unable to generate WebSocket nonce");
  }
  return base64(nonce);
}

[[nodiscard]] bool reserved_header(std::string_view name) noexcept {
  static constexpr std::array<std::string_view, 7> reserved{
    "host",
    "connection",
    "upgrade",
    "sec-websocket-key",
    "sec-websocket-version",
    "sec-websocket-protocol",
    "sec-websocket-extensions",
  };
  return std::ranges::any_of(reserved, [name](std::string_view candidate) {
    return uvp::http::headers::names_equal(name, candidate);
  });
}

[[nodiscard]] std::string join_subprotocols(const std::vector<std::string>& values) {
  auto result = std::string{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0U) {
      result += ", ";
    }
    result += values[index];
  }
  return result;
}

struct response_head {
  unsigned int status = 0;
  uvp::http::headers headers;
};

[[nodiscard]] bool parse_response_head(
  std::string_view input,
  std::size_t max_header_count,
  response_head& output,
  std::string& error) {
  const auto status_end = input.find("\r\n");
  if (status_end == std::string_view::npos) {
    error = "response status line is malformed";
    return false;
  }
  const auto status_line = input.substr(0, status_end);
  if (!status_line.starts_with("HTTP/1.1 ") && !status_line.starts_with("HTTP/1.0 ")) {
    error = "response is not HTTP/1.x";
    return false;
  }
  if (status_line.size() < 12U || status_line[8] != ' ') {
    error = "response status is malformed";
    return false;
  }
  unsigned int status = 0;
  const auto* first = status_line.data() + 9;
  const auto* last = first + 3;
  const auto [parsed, ec] = std::from_chars(first, last, status);
  if (ec != std::errc{} || parsed != last || (status_line.size() > 12U && status_line[12] != ' ')) {
    error = "response status is malformed";
    return false;
  }
  output.status = status;

  auto remaining = input.substr(status_end + 2U);
  auto count = std::size_t{0};
  while (!remaining.empty()) {
    const auto line_end = remaining.find("\r\n");
    if (line_end == std::string_view::npos) {
      error = "response header is malformed";
      return false;
    }
    const auto line = remaining.substr(0, line_end);
    remaining.remove_prefix(line_end + 2U);
    if (line.empty()) {
      return remaining.empty();
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || count >= max_header_count) {
      error = colon == std::string_view::npos ? "response header is malformed" : "response has too many headers";
      return false;
    }
    const auto name = line.substr(0, colon);
    const auto value = trim(line.substr(colon + 1U));
    if (!uvp::http::headers::is_valid_name(name) || !uvp::http::headers::is_valid_value(value)) {
      error = "response header is invalid";
      return false;
    }
    output.headers.add(name, value);
    ++count;
  }
  error = "response headers are incomplete";
  return false;
}

[[nodiscard]] std::size_t header_count(const uvp::http::headers& headers, std::string_view expected) noexcept {
  return static_cast<std::size_t>(std::count_if(headers.begin(), headers.end(), [expected](const auto& field) {
    return uvp::http::headers::names_equal(field.first, expected);
  }));
}

[[nodiscard]] bool selected_subprotocol_is_valid(
  const uvp::http::headers& headers,
  const std::vector<std::string>& offered) noexcept {
  const auto count = header_count(headers, "sec-websocket-protocol");
  if (count == 0U) {
    return true;
  }
  if (count != 1U) {
    return false;
  }
  const auto selected = headers.get("sec-websocket-protocol");
  return uvp::http::headers::is_valid_name(selected) &&
         std::ranges::find(offered, selected) != offered.end();
}

} // namespace

namespace detail {

struct client_connect_state final : std::enable_shared_from_this<client_connect_state> {
  client_connect_state(uv::loop& loop, client_options options, std::string_view input, connect_callback callback)
      : loop(&loop),
        options(std::move(options)),
        input(input),
        lifetime(std::move(callback)),
        deadlines(loop, [this](uvp::detail::operation_phase phase) { on_timeout(phase); }),
        resolver(loop),
        connector(loop) {
    lifetime.set_finish_action([this]() noexcept {
      deadlines.stop();
      request.clear();
      response.clear();
    });
    lifetime.set_abort_action([this]() noexcept {
      dns.cancel();
      tcp.cancel();
      tls.cancel();
      close_stream();
    });
  }

  connect_operation start() {
    deadlines.arm_deadline(options.overall_timeout);
    if (!validate_options()) {
      return connect_operation{shared_from_this()};
    }
    auto parsed = uvp::parse_url(input);
    if (!parsed) {
      fail(wrap_client_error(client_errc::invalid_url, parsed.error()));
      return connect_operation{shared_from_this()};
    }
    url = std::move(parsed).value();
    const auto scheme = uvp::scheme_id(url);
    if (scheme != uvp::url_scheme::ws && scheme != uvp::url_scheme::wss) {
      fail(make_client_error(client_errc::unsupported_scheme));
      return connect_operation{shared_from_this()};
    }
    if (!url.has_authority() || url.has_credentials()) {
      fail(make_client_error(client_errc::invalid_url, "WebSocket URL must have authority and no userinfo"));
      return connect_operation{shared_from_this()};
    }
    auto endpoint = uvp::authority_endpoint(url);
    if (!endpoint) {
      fail(wrap_client_error(client_errc::invalid_url, endpoint.error()));
      return connect_operation{shared_from_this()};
    }
    endpoint_ = std::move(endpoint).value();
    start_phase(dns_phase, options.dns_timeout);
    auto self = shared_from_this();
    dns = resolver.resolve(
      uvp::dns::query{}.host(endpoint_.host).port(endpoint_.port).family(uvp::dns::address_family::any),
      [self](uvp::result<uvp::dns::address_list> result) { self->on_resolved(std::move(result)); });
    return connect_operation{std::move(self)};
  }

  void cancel() noexcept {
    (void)lifetime.cancel(make_client_error(client_errc::cancelled));
  }

  [[nodiscard]] bool active() const noexcept { return lifetime.active(); }

private:
  bool validate_options() {
    if (options.close_timeout <= std::chrono::milliseconds{0}) {
      fail(make_client_error(client_errc::invalid_option, "close_timeout must be greater than zero"));
      return false;
    }
    if (options.max_header_bytes == 0U || options.max_header_count == 0U) {
      fail(make_client_error(client_errc::invalid_option, "response header limits must be greater than zero"));
      return false;
    }
    for (std::size_t index = 0; index < options.subprotocols.size(); ++index) {
      const auto& value = options.subprotocols[index];
      if (!uvp::http::headers::is_valid_name(value) ||
          std::ranges::find(options.subprotocols.begin(), options.subprotocols.begin() + static_cast<std::ptrdiff_t>(index), value) !=
            options.subprotocols.begin() + static_cast<std::ptrdiff_t>(index)) {
        fail(make_client_error(client_errc::invalid_option, "WebSocket subprotocol offer is invalid or duplicated"));
        return false;
      }
    }
    if (options.origin && !uvp::http::headers::is_valid_value(*options.origin)) {
      fail(make_client_error(client_errc::invalid_option, "Origin header value is invalid"));
      return false;
    }
    for (const auto& [name, value] : options.headers) {
      if (!uvp::http::headers::is_valid_name(name) || !uvp::http::headers::is_valid_value(value) || reserved_header(name)) {
        fail(make_client_error(client_errc::invalid_option, "WebSocket request header is invalid or reserved"));
        return false;
      }
    }
    return true;
  }

  void start_phase(uvp::detail::operation_phase phase, std::chrono::milliseconds timeout) {
    lifetime.enter_phase(phase);
    deadlines.arm_phase(phase, timeout);
  }

  void on_timeout(uvp::detail::operation_phase phase) {
    if (!lifetime.active()) {
      return;
    }
    std::string detail = "WebSocket connection timed out";
    if (phase == dns_phase) detail = "DNS resolution timed out";
    else if (phase == connect_phase) detail = "TCP connect timed out";
    else if (phase == tls_phase) detail = "TLS handshake timed out";
    else if (phase == handshake_phase) detail = "WebSocket handshake timed out";
    else if (phase == uvp::detail::overall_deadline_phase) detail = "overall connection deadline exceeded";
    (void)lifetime.abort(make_client_error(client_errc::timeout, std::move(detail)));
  }

  void on_resolved(uvp::result<uvp::dns::address_list> result) {
    if (!lifetime.active()) return;
    if (!result) {
      fail(wrap_client_error(client_errc::dns_failed, result.error()));
      return;
    }
    start_phase(connect_phase, options.connect_timeout);
    auto self = shared_from_this();
    tcp = connector.connect(
      result.value(), uvp::io::connect_options{.timeout = options.connect_timeout},
      [self](uvp::result<uvp::io::byte_stream> connected) { self->on_connected(std::move(connected)); });
  }

  void on_connected(uvp::result<uvp::io::byte_stream> result) {
    if (!lifetime.active()) return;
    if (!result) {
      if (result.error().code == uvp::io::connect_errc::timeout) {
        fail(make_client_error(client_errc::timeout, "TCP connect timed out"));
      } else {
        fail(wrap_client_error(client_errc::connect_failed, result.error()));
      }
      return;
    }
    auto lower = std::move(result).value();
    if (uvp::scheme_id(url) == uvp::url_scheme::wss) {
      start_tls(std::move(lower));
    } else {
      stream = std::move(lower);
      write_upgrade_request();
    }
  }

  void start_tls(uvp::io::byte_stream lower) {
    try {
      auto context_options = uvp::tls::client_context_options{}
        .server_name(url.hostname())
        .default_verify_paths(options.tls_default_verify_paths)
        .alpn({"http/1.1"});
      if (!options.tls_ca_file.empty()) context_options.ca_file(options.tls_ca_file);
      if (!options.tls_ca_path.empty()) context_options.ca_path(options.tls_ca_path);
      start_phase(tls_phase, options.tls_handshake_timeout);
      auto self = shared_from_this();
      tls = uvp::tls::connect(
        std::move(lower), uvp::tls::client_context{std::move(context_options)},
        [self](uvp::tls::handshake_result result) { self->on_tls_connected(std::move(result)); });
    } catch (const std::exception& error) {
      fail(make_client_error(client_errc::tls_failed, error.what()));
    }
  }

  void on_tls_connected(uvp::tls::handshake_result result) {
    if (!lifetime.active()) return;
    if (!result) {
      fail(wrap_client_error(client_errc::tls_failed, result.error()));
      return;
    }
    if (!result.selected_alpn().empty() && result.selected_alpn() != "http/1.1") {
      auto lower = std::move(result).stream();
      lower.close();
      fail(make_client_error(client_errc::tls_failed, "unsupported TLS ALPN protocol"));
      return;
    }
    stream = std::move(result).stream();
    write_upgrade_request();
  }

  void write_upgrade_request() {
    try {
      key = fresh_websocket_key();
    } catch (const std::exception& error) {
      fail(make_client_error(client_errc::handshake_failed, error.what()));
      return;
    }
    auto request_text = std::string{};
    request_text += "GET ";
    request_text += uvp::origin_form_target(url);
    request_text += " HTTP/1.1\r\nHost: ";
    request_text += url.host();
    if (url.has_port()) {
      request_text += ':';
      request_text += url.port();
    }
    request_text += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ";
    request_text += key;
    request_text += "\r\n";
    if (!options.subprotocols.empty()) {
      request_text += "Sec-WebSocket-Protocol: ";
      request_text += join_subprotocols(options.subprotocols);
      request_text += "\r\n";
    }
    if (options.origin) {
      request_text += "Origin: ";
      request_text += *options.origin;
      request_text += "\r\n";
    }
    for (const auto& [name, value] : options.headers) {
      request_text += name;
      request_text += ": ";
      request_text += value;
      request_text += "\r\n";
    }
    request_text += "\r\n";
    request.assign(reinterpret_cast<const std::byte*>(request_text.data()),
                   reinterpret_cast<const std::byte*>(request_text.data() + request_text.size()));
    start_phase(handshake_phase, options.websocket_handshake_timeout);
    auto self = shared_from_this();
    stream.write(request, [self](uvp::io::stream_error error) { self->on_upgrade_written(error); });
  }

  void on_upgrade_written(uvp::io::stream_error error) {
    if (!lifetime.active()) return;
    if (error) {
      fail(make_client_error(client_errc::connect_failed, error.message()));
      return;
    }
    auto self = shared_from_this();
    stream.read_start([self](uvp::io::read_result result) { self->on_upgrade_read(result); });
  }

  void on_upgrade_read(uvp::io::read_result result) {
    if (!lifetime.active()) return;
    if (result.eof()) {
      fail(make_client_error(client_errc::handshake_failed, "connection closed during WebSocket handshake"));
      return;
    }
    if (!result) {
      fail(make_client_error(client_errc::connect_failed, result.error().message()));
      return;
    }
    if (result.bytes().size() > options.max_header_bytes || response.size() > options.max_header_bytes - result.bytes().size()) {
      fail(make_client_error(client_errc::handshake_failed, "response headers are too large"));
      return;
    }
    response.append(reinterpret_cast<const char*>(result.bytes().data()), result.bytes().size());
    const auto end = response.find("\r\n\r\n");
    if (end == std::string::npos) return;
    const auto header_size = end + 4U;
    response_head head;
    std::string parse_error;
    if (!parse_response_head(std::string_view{response}.substr(0U, header_size), options.max_header_count, head, parse_error) ||
        !validate_upgrade_response(head, parse_error)) {
      fail(make_client_error(client_errc::handshake_failed, std::move(parse_error)));
      return;
    }
    try { stream.read_stop(); } catch (...) {}
    auto extra = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(response.data() + header_size), response.size() - header_size};
    auto session_options = accept_options{}
      .max_message_bytes(options.max_message_bytes)
      .max_pending_write_bytes(options.max_pending_write_bytes)
      .close_timeout(options.close_timeout)
      .auto_pong(options.auto_pong);
    auto pending = websocket::detail::make_client_session(std::move(stream), std::move(session_options), extra);
    auto start_session = std::move(pending.start);
    if (lifetime.complete(uvp::result<session>{std::move(pending.value)}) && start_session) {
      start_session();
    }
  }

  bool validate_upgrade_response(const response_head& head, std::string& error) const {
    if (head.status != 101U) {
      error = "server did not return 101 Switching Protocols";
      return false;
    }
    if (header_count(head.headers, "upgrade") != 1U || !equal_ascii_ci(trim(head.headers.get("upgrade")), "websocket")) {
      error = "response Upgrade header is invalid";
      return false;
    }
    if (header_count(head.headers, "connection") != 1U || !token_list_contains(head.headers.get("connection"), "upgrade")) {
      error = "response Connection header is invalid";
      return false;
    }
    if (header_count(head.headers, "sec-websocket-accept") != 1U ||
        head.headers.get("sec-websocket-accept") != detail::websocket_accept_value(key)) {
      error = "response Sec-WebSocket-Accept header is invalid";
      return false;
    }
    if (!selected_subprotocol_is_valid(head.headers, options.subprotocols)) {
      error = "response Sec-WebSocket-Protocol header is invalid";
      return false;
    }
    if (header_count(head.headers, "sec-websocket-extensions") != 0U) {
      error = "WebSocket extensions are unsupported";
      return false;
    }
    return true;
  }

  void close_stream() noexcept {
    if (stream) stream.close();
  }

  void fail(uvp::error error) {
    (void)lifetime.abort(uvp::result<session>{std::move(error)});
  }

  uv::loop* loop;
  client_options options;
  std::string input;
  uvp::detail::operation_lifetime<uvp::result<session>> lifetime;
  uvp::detail::operation_deadline deadlines;
  uvp::url url;
  uvp::url_authority_endpoint endpoint_;
  uvp::dns::resolver resolver;
  uvp::io::tcp_connector connector;
  uvp::dns::resolve_operation dns;
  uvp::io::connect_operation tcp;
  uvp::tls::handshake_operation tls;
  uvp::io::byte_stream stream;
  std::vector<std::byte> request;
  std::string response;
  std::string key;
};

} // namespace detail

const std::error_category& client_category() noexcept {
  static const client_error_category_impl instance;
  return instance;
}

std::error_code make_error_code(client_errc value) noexcept {
  return {static_cast<int>(value), client_category()};
}

connect_operation::connect_operation(std::shared_ptr<detail::client_connect_state> state)
    : state_(std::move(state)) {}

void connect_operation::cancel() noexcept {
  if (state_) state_->cancel();
}

bool connect_operation::active() const noexcept {
  return state_ && state_->active();
}

client::client(uv::loop& loop)
    : client(loop, {}) {}

client::client(uv::loop& loop, client_options options)
    : loop_(&loop), options_(std::move(options)) {}

connect_operation client::connect(std::string_view url, connect_callback callback) {
  return connect(url, options_, std::move(callback));
}

connect_operation client::connect(std::string_view url, client_options options, connect_callback callback) {
  auto state = std::make_shared<detail::client_connect_state>(*loop_, std::move(options), url, std::move(callback));
  return state->start();
}

} // namespace uvp::websocket
