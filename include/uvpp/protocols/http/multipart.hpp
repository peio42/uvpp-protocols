#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <uvpp/protocols/http/error.hpp>
#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/multipart_options.hpp>
#include <uvpp/protocols/http/request.hpp>
#include <uvpp/protocols/result.hpp>

namespace uvp::http {

namespace detail {
struct multipart_form_part {
  std::string name;
  std::optional<std::string> filename;
  http::headers headers;
  std::vector<std::byte> body;
};
struct multipart_part_state;
struct multipart_stream_state;
} // namespace detail

class multipart_part_view {
public:
  multipart_part_view() = default;

  [[nodiscard]] std::string_view name() const noexcept;
  [[nodiscard]] std::optional<std::string_view> filename() const noexcept;
  [[nodiscard]] std::string safe_filename() const;
  [[nodiscard]] const http::headers& headers() const noexcept;
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] std::string_view text() const noexcept;
  [[nodiscard]] bool is_file() const noexcept;

private:
  friend class multipart_form;

  explicit multipart_part_view(const detail::multipart_form_part* part) noexcept
      : part_(part) {}

  const detail::multipart_form_part* part_ = nullptr;
};

class multipart_field_view {
public:
  multipart_field_view() = default;

  [[nodiscard]] std::string_view name() const noexcept;
  [[nodiscard]] const http::headers& headers() const noexcept;
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] std::string_view text() const noexcept;

private:
  friend class multipart_form;

  explicit multipart_field_view(const detail::multipart_form_part* part) noexcept
      : part_(part) {}

  const detail::multipart_form_part* part_ = nullptr;
};

class multipart_file_view {
public:
  multipart_file_view() = default;

  [[nodiscard]] std::string_view name() const noexcept;
  [[nodiscard]] std::string_view filename() const noexcept;
  [[nodiscard]] std::string safe_filename() const;
  [[nodiscard]] const http::headers& headers() const noexcept;
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] std::string_view text() const noexcept;

private:
  friend class multipart_form;

  explicit multipart_file_view(const detail::multipart_form_part* part) noexcept
      : part_(part) {}

  const detail::multipart_form_part* part_ = nullptr;
};

class multipart_form {
public:
  multipart_form() = default;

  multipart_form(multipart_form&&) noexcept = default;
  multipart_form& operator=(multipart_form&&) noexcept = default;

  multipart_form(const multipart_form&) = delete;
  multipart_form& operator=(const multipart_form&) = delete;

  [[nodiscard]] std::span<const multipart_part_view> parts() const noexcept;
  [[nodiscard]] std::span<const multipart_field_view> fields(std::string_view name) const noexcept;
  [[nodiscard]] std::span<const multipart_file_view> files(std::string_view name) const noexcept;
  [[nodiscard]] std::optional<multipart_field_view> first_field(std::string_view name) const noexcept;
  [[nodiscard]] uvp::result<multipart_field_view> single_field(std::string_view name) const;
  [[nodiscard]] std::optional<multipart_file_view> first_file(std::string_view name) const noexcept;
  [[nodiscard]] uvp::result<multipart_file_view> single_file(std::string_view name) const;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

private:
  friend uvp::result<multipart_form> parse_multipart_form(
    std::string_view content_type,
    std::span<const std::byte> body,
    const multipart_form_options& options);

  struct view_group {
    std::string name;
    std::size_t offset = 0;
    std::size_t count = 0;
  };

  void rebuild_indexes();

  std::vector<detail::multipart_form_part> parts_;
  std::vector<multipart_part_view> part_views_;
  std::vector<multipart_field_view> field_views_;
  std::vector<multipart_file_view> file_views_;
  std::vector<view_group> field_groups_;
  std::vector<view_group> file_groups_;
};

[[nodiscard]] uvp::result<multipart_form> parse_multipart_form(
  std::string_view content_type,
  std::span<const std::byte> body,
  const multipart_form_options& options = {});

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
  multipart_stream(request_body_stream& body, std::string_view content_type, multipart_stream_options options);
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
