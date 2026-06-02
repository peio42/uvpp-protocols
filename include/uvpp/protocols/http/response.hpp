#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/status.hpp>

namespace uvp::http {

namespace detail {
struct response_state;
} // namespace detail

class deferred_response;

class response {
public:
  response() = default;

  response& status(unsigned int code);
  response& status(http::status value);
  response& header(std::string_view name, std::string_view value);
  response& type(std::string_view content_type);

  void text(std::string_view body);
  void json(std::string_view serialized_json);
  void json(std::initializer_list<std::pair<std::string_view, std::string_view>> object);
  void bytes(std::span<const std::byte> body);
  void end();
  [[nodiscard]] deferred_response defer();

  [[nodiscard]] unsigned int status_code() const noexcept;
  [[nodiscard]] const http::headers& headers() const noexcept;
  [[nodiscard]] std::string_view body() const noexcept;
  [[nodiscard]] bool ended() const noexcept;
  [[nodiscard]] bool deferred() const noexcept;

private:
  friend class deferred_response;
  friend class server;

  explicit response(std::shared_ptr<detail::response_state> state);

  void on_complete(std::function<void()> callback);
  void reset();
  void cancel() noexcept;

  [[nodiscard]] detail::response_state& state();
  [[nodiscard]] const detail::response_state& state() const;

  std::shared_ptr<detail::response_state> state_;
};

class deferred_response {
public:
  deferred_response() = default;
  ~deferred_response() = default;

  deferred_response(deferred_response&&) noexcept = default;
  deferred_response& operator=(deferred_response&&) noexcept = default;

  deferred_response(const deferred_response&) = delete;
  deferred_response& operator=(const deferred_response&) = delete;

  [[nodiscard]] bool active() const noexcept;

  deferred_response& on_cancel(std::function<void()> callback);

  deferred_response& status(unsigned int code);
  deferred_response& status(http::status value);
  deferred_response& header(std::string_view name, std::string_view value);
  deferred_response& type(std::string_view content_type);

  void text(std::string_view body);
  void json(std::string_view serialized_json);
  void json(std::initializer_list<std::pair<std::string_view, std::string_view>> object);
  void bytes(std::span<const std::byte> body);
  void end();

private:
  friend class response;

  explicit deferred_response(std::weak_ptr<detail::response_state> state) noexcept;

  [[nodiscard]] std::shared_ptr<detail::response_state> lock_active() const noexcept;

  std::weak_ptr<detail::response_state> state_;
};

} // namespace uvp::http
