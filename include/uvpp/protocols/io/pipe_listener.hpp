#pragma once

#include <memory>
#include <string_view>

#include <uvpp/protocols/io/stream_listener.hpp>

namespace uv {
class loop;
} // namespace uv

namespace uvp::io {

class pipe_listener {
public:
  explicit pipe_listener(uv::loop& loop);
  ~pipe_listener();

  pipe_listener(pipe_listener&&) noexcept;
  pipe_listener& operator=(pipe_listener&&) noexcept;

  pipe_listener(const pipe_listener&) = delete;
  pipe_listener& operator=(const pipe_listener&) = delete;

  pipe_listener& bind(std::string_view path);
  pipe_listener& backlog(unsigned int value) noexcept;

  operator stream_listener() &&;

private:
  struct impl;

  std::unique_ptr<impl> impl_;
};

} // namespace uvp::io

