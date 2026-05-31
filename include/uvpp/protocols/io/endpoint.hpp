#pragma once

#include <string>
#include <variant>

namespace uvp::io {

struct tcp_endpoint {
  std::string host;
  unsigned int port = 0;
};

struct pipe_endpoint {
  std::string path;
};

using endpoint = std::variant<std::monostate, tcp_endpoint, pipe_endpoint>;

} // namespace uvp::io

