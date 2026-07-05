#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <uvpp/protocols/http/upgrade.hpp>
#include <uvpp/protocols/io/byte_stream.hpp>

namespace uvp::websocket {

class session;

enum class close_code : unsigned short {
  normal = 1000,
  going_away = 1001,
  protocol_error = 1002,
  unsupported_data = 1003,
  invalid_payload = 1007,
  policy_violation = 1008,
  message_too_large = 1009,
  mandatory_extension = 1010,
  internal_error = 1011,
};

struct accept_options {
  accept_options& max_message_bytes(std::size_t value) &;
  accept_options&& max_message_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_message_bytes() const noexcept { return max_message_bytes_; }

  accept_options& max_pending_write_bytes(std::size_t value) &;
  accept_options&& max_pending_write_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_pending_write_bytes() const noexcept { return max_pending_write_bytes_; }

  accept_options& close_timeout(std::chrono::milliseconds value) &;
  accept_options&& close_timeout(std::chrono::milliseconds value) &&;
  [[nodiscard]] std::chrono::milliseconds close_timeout() const noexcept { return close_timeout_; }

  accept_options& subprotocol(std::string_view value) &;
  accept_options&& subprotocol(std::string_view value) &&;
  [[nodiscard]] const std::string& subprotocol() const noexcept { return subprotocol_; }

  accept_options& auto_pong(bool value) & noexcept;
  accept_options&& auto_pong(bool value) && noexcept;
  [[nodiscard]] bool auto_pong() const noexcept { return auto_pong_; }

private:
  std::size_t max_message_bytes_ = 1024 * 1024;
  std::size_t max_pending_write_bytes_ = 1024 * 1024;
  std::chrono::milliseconds close_timeout_ = std::chrono::seconds{5};
  std::string subprotocol_;
  bool auto_pong_ = true;
};

class session {
public:
  using text_callback = std::function<void(session&, std::string_view)>;
  using binary_callback = std::function<void(session&, std::span<const std::byte>)>;
  using control_callback = std::function<void(session&, std::span<const std::byte>)>;
  using close_callback = std::function<void(session&, close_code, std::string_view)>;
  using error_callback = std::function<void(session&, std::error_code)>;

  session() = default;
  ~session();

  session(session&&) noexcept;
  session& operator=(session&&) noexcept;

  session(const session&) = delete;
  session& operator=(const session&) = delete;

  void text(std::string_view message);
  void binary(std::span<const std::byte> message);
  void ping(std::span<const std::byte> payload = {});
  void pong(std::span<const std::byte> payload = {});
  void close(close_code code = close_code::normal, std::string_view reason = {});

  session& on_text(text_callback callback) &;
  session&& on_text(text_callback callback) &&;

  session& on_binary(binary_callback callback) &;
  session&& on_binary(binary_callback callback) &&;

  session& on_ping(control_callback callback) &;
  session&& on_ping(control_callback callback) &&;

  session& on_pong(control_callback callback) &;
  session&& on_pong(control_callback callback) &&;

  session& on_close(close_callback callback) &;
  session&& on_close(close_callback callback) &&;

  session& on_error(error_callback callback) &;
  session&& on_error(error_callback callback) &&;

  [[nodiscard]] uvp::io::endpoint local_endpoint() const;
  [[nodiscard]] uvp::io::endpoint remote_endpoint() const;
  [[nodiscard]] uvp::io::byte_stream into_byte_stream() &&;

  [[nodiscard]] explicit operator bool() const noexcept;

private:
  friend session accept(uvp::http::upgrade_request& req, accept_options options);
  friend session accept_detached(uvp::http::upgrade_request& req, accept_options options);
  friend class websocket_byte_stream;

  struct state;
  explicit session(std::shared_ptr<state> state, bool owns_lifetime = false);

  void release_owned() noexcept;

  std::shared_ptr<state> state_;
  bool owns_lifetime_ = false;
};

[[nodiscard]] session accept(uvp::http::upgrade_request& req, accept_options options = {});
[[nodiscard]] session accept_detached(uvp::http::upgrade_request& req, accept_options options = {});
uvp::io::byte_stream accept_byte_stream(uvp::http::upgrade_request& req, accept_options options = {});

} // namespace uvp::websocket
