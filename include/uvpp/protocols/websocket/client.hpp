#pragma once

#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/result.hpp>
#include <uvpp/protocols/websocket/session.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace uv {
class loop;
}

namespace uvp::websocket {

enum class client_errc {
  invalid_url = 1,
  unsupported_scheme,
  invalid_option,
  dns_failed,
  connect_failed,
  tls_failed,
  handshake_failed,
  cancelled,
  timeout,
};

std::error_code make_error_code(client_errc value) noexcept;
const std::error_category& client_category() noexcept;

struct client_options {
  std::size_t max_message_bytes = 1024 * 1024;
  std::size_t max_pending_write_bytes = 1024 * 1024;
  std::chrono::milliseconds close_timeout = std::chrono::seconds{5};
  bool auto_pong = true;

  std::vector<std::string> subprotocols;
  std::optional<std::string> origin;
  uvp::http::headers headers;

  std::chrono::milliseconds overall_timeout{0};
  std::chrono::milliseconds dns_timeout{0};
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds tls_handshake_timeout{0};
  std::chrono::milliseconds websocket_handshake_timeout{0};

  bool tls_default_verify_paths = true;
  std::string tls_ca_file;
  std::string tls_ca_path;
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_header_count = 128;
};

using connect_callback = std::function<void(uvp::result<session>)>;

namespace detail {
struct client_connect_state;
}

class connect_operation {
public:
  connect_operation() = default;
  explicit connect_operation(std::shared_ptr<detail::client_connect_state> state);

  void cancel() noexcept;
  [[nodiscard]] bool active() const noexcept;

private:
  std::shared_ptr<detail::client_connect_state> state_;
};

class client {
public:
  explicit client(uv::loop& loop);
  client(uv::loop& loop, client_options options);

  [[nodiscard]] connect_operation connect(std::string_view url, connect_callback callback);
  [[nodiscard]] connect_operation connect(std::string_view url, client_options options, connect_callback callback);

private:
  uv::loop* loop_;
  client_options options_;
};

} // namespace uvp::websocket

namespace std {

template<>
struct is_error_code_enum<uvp::websocket::client_errc> : true_type {};

} // namespace std
