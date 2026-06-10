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
  message_too_large = 1009,
  internal_error = 1011,
};

struct accept_options {
  std::size_t max_message_bytes_ = 1024 * 1024;
  std::size_t max_pending_write_bytes_ = 1024 * 1024;
  std::string subprotocol_;

  std::function<void(session&, std::string_view)> on_text_;
  std::function<void(session&, std::span<const std::byte>)> on_binary_;
  std::function<void(session&, std::span<const std::byte>)> on_ping_;
  std::function<void(session&, std::span<const std::byte>)> on_pong_;
  std::function<void(session&, close_code, std::string_view)> on_close_;
  std::function<void(session&, std::error_code)> on_error_;

  accept_options& max_message_bytes(std::size_t value) &;
  accept_options&& max_message_bytes(std::size_t value) &&;
  accept_options& max_pending_write_bytes(std::size_t value) &;
  accept_options&& max_pending_write_bytes(std::size_t value) &&;
  accept_options& subprotocol(std::string_view value) &;
  accept_options&& subprotocol(std::string_view value) &&;

  accept_options& on_text(std::function<void(session&, std::string_view)> callback) &;
  accept_options&& on_text(std::function<void(session&, std::string_view)> callback) &&;
  accept_options& on_binary(std::function<void(session&, std::span<const std::byte>)> callback) &;
  accept_options&& on_binary(std::function<void(session&, std::span<const std::byte>)> callback) &&;
  accept_options& on_ping(std::function<void(session&, std::span<const std::byte>)> callback) &;
  accept_options&& on_ping(std::function<void(session&, std::span<const std::byte>)> callback) &&;
  accept_options& on_pong(std::function<void(session&, std::span<const std::byte>)> callback) &;
  accept_options&& on_pong(std::function<void(session&, std::span<const std::byte>)> callback) &&;
  accept_options& on_close(std::function<void(session&, close_code, std::string_view)> callback) &;
  accept_options&& on_close(std::function<void(session&, close_code, std::string_view)> callback) &&;
  accept_options& on_error(std::function<void(session&, std::error_code)> callback) &;
  accept_options&& on_error(std::function<void(session&, std::error_code)> callback) &&;
};

class session {
public:
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

  [[nodiscard]] uvp::io::endpoint local_endpoint() const;
  [[nodiscard]] uvp::io::endpoint remote_endpoint() const;
  [[nodiscard]] uvp::io::byte_stream into_byte_stream() &&;

  [[nodiscard]] explicit operator bool() const noexcept;

private:
  friend session accept(uvp::http::upgrade_request& req, accept_options options);
  friend class websocket_byte_stream;

  struct state;
  explicit session(std::shared_ptr<state> state);

  std::shared_ptr<state> state_;
};

session accept(uvp::http::upgrade_request& req, accept_options options = {});
uvp::io::byte_stream accept_byte_stream(uvp::http::upgrade_request& req, accept_options options = {});

} // namespace uvp::websocket
