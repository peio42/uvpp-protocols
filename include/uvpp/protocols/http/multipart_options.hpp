#pragma once

#include <cstddef>
#include <utility>

namespace uvp::http {

struct multipart_limits {
  std::size_t max_total_bytes = 0;
  std::size_t max_file_bytes = 0;
  std::size_t max_field_bytes = 1024 * 1024;
  std::size_t max_part_header_bytes = 16 * 1024;
  std::size_t max_part_headers = 64;
  std::size_t max_parts = 64;
  std::size_t max_field_name_bytes = 256;
  std::size_t max_filename_bytes = 1024;
};

struct multipart_stream_options {
  multipart_stream_options& limits(multipart_limits value) & noexcept {
    limits_ = value;
    return *this;
  }

  multipart_stream_options&& limits(multipart_limits value) && noexcept {
    limits(std::move(value));
    return std::move(*this);
  }

  multipart_stream_options& max_total_bytes(std::size_t value) & noexcept {
    limits_.max_total_bytes = value;
    return *this;
  }

  multipart_stream_options&& max_total_bytes(std::size_t value) && noexcept {
    max_total_bytes(value);
    return std::move(*this);
  }

  multipart_stream_options& max_file_bytes(std::size_t value) & noexcept {
    limits_.max_file_bytes = value;
    return *this;
  }

  multipart_stream_options&& max_file_bytes(std::size_t value) && noexcept {
    max_file_bytes(value);
    return std::move(*this);
  }

  multipart_stream_options& max_field_bytes(std::size_t value) & noexcept {
    limits_.max_field_bytes = value;
    return *this;
  }

  multipart_stream_options&& max_field_bytes(std::size_t value) && noexcept {
    max_field_bytes(value);
    return std::move(*this);
  }

  [[nodiscard]] const multipart_limits& limits() const noexcept { return limits_; }

private:
  multipart_limits limits_;
};

struct multipart_form_options {
  multipart_limits limits{
    16 * 1024 * 1024,
    0,
    1024 * 1024,
    16 * 1024,
    64,
    64,
    256,
    1024,
  };
  std::size_t max_memory_bytes = 8 * 1024 * 1024;
};

} // namespace uvp::http
