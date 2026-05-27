#pragma once

#include <string_view>

namespace uv::http {

enum class method {
  get,
  head,
  post,
  put,
  delete_,
  connect,
  options,
  trace,
  patch,
  unknown,
};

std::string_view to_string(method value) noexcept;

} // namespace uv::http

