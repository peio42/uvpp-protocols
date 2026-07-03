#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <uvpp/protocols/http/error.hpp>
#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/request.hpp>
#include <uvpp/protocols/result.hpp>

namespace uvp::http {

namespace detail {
struct multipart_part_state;
struct multipart_stream_state;
} // namespace detail

class multipart_part_stream {
public:
  multipart_part_stream() = default;

  multipart_part_stream& on_data(std::function<void(std::span<const std::byte>)> callback);
  multipart_part_stream& on_end(std::function<void()> callback);
  multipart_part_stream& on_error(std::function<void(uvp::error)> callback);

  void pause();
  void resume();

private:
  friend class multipart_part;
  friend class multipart_stream;

  explicit multipart_part_stream(std::shared_ptr<detail::multipart_part_state> state) noexcept;

  void emit_data(std::span<const std::byte> chunk);
  void emit_end();
  void emit_error(uvp::error error);

  std::shared_ptr<detail::multipart_part_state> state_;
};

class multipart_part {
public:
  multipart_part() = default;

  [[nodiscard]] std::string_view name() const noexcept;
  [[nodiscard]] std::optional<std::string_view> filename() const noexcept;
  [[nodiscard]] std::string safe_filename() const;
  [[nodiscard]] const http::headers& headers() const noexcept;

  multipart_part_stream& stream();
  void text(std::size_t max_bytes, std::function<void(uvp::result<std::string>)> callback);
  void discard();

  void pause();
  void resume();

private:
  friend class multipart_stream;
  friend struct detail::multipart_stream_state;

  explicit multipart_part(std::shared_ptr<detail::multipart_part_state> state) noexcept;

  [[nodiscard]] bool consumed() const noexcept;
  void emit_data(std::span<const std::byte> chunk);
  void emit_end();
  void emit_error(uvp::error error);

  std::shared_ptr<detail::multipart_part_state> state_;
  multipart_part_stream stream_;
};

class multipart_stream {
public:
  multipart_stream();
  multipart_stream(request_body_stream& body, std::string_view content_type);
  ~multipart_stream();

  multipart_stream(multipart_stream&&) noexcept = default;
  multipart_stream& operator=(multipart_stream&&) noexcept = default;

  multipart_stream(const multipart_stream&) = delete;
  multipart_stream& operator=(const multipart_stream&) = delete;

  multipart_stream& on_part(std::function<void(multipart_part&)> callback);
  multipart_stream& on_end(std::function<void()> callback);
  multipart_stream& on_error(std::function<void(uvp::error)> callback);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const uvp::error& error() const noexcept;
  [[nodiscard]] bool has_error_handler() const noexcept;

private:
  std::shared_ptr<detail::multipart_stream_state> state_;
};

} // namespace uvp::http
