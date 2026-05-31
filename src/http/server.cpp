#include <uvpp/protocols/http/server.hpp>

#include <stdexcept>
#include <string>

namespace uvp::http {

server::server(uv::loop& loop)
    : server(loop, server_options{}) {}

server::server(uv::loop& loop, server_options options)
    : loop_(&loop), options_(options) {
  options_.validate();
}

void server::listen(std::string_view host, unsigned int port) {
  (void)host;
  (void)port;
  throw std::logic_error("uvp::http::server::listen is reserved for the HTTP MVP implementation");
}

void server::close() noexcept {
  listening_ = false;
}

} // namespace uvp::http
