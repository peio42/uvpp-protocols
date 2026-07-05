#pragma once

#include <uvpp/protocols/dns.hpp>
#include <uvpp/protocols/io/byte_stream.hpp>
#include <uvpp/protocols/io/endpoint.hpp>
#include <uvpp/protocols/result.hpp>

#include <functional>
#include <memory>
#include <system_error>
#include <type_traits>

namespace uv {
class loop;
}

namespace uvp::io {

enum class connect_errc {
  no_addresses = 1,
  cancelled,
  connect_failed,
};

std::error_code make_error_code(connect_errc value) noexcept;
const std::error_category& connect_category() noexcept;

using connect_callback = std::function<void(uvp::result<byte_stream>)>;

class connect_operation {
public:
  connect_operation() = default;
  explicit connect_operation(std::shared_ptr<void> state);

  void cancel() noexcept;
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
  std::shared_ptr<void> state_;
};

class tcp_connector {
public:
  explicit tcp_connector(uv::loop& loop) noexcept;

  [[nodiscard]] connect_operation connect(tcp_endpoint endpoint, connect_callback callback);
  [[nodiscard]] connect_operation connect(const uvp::dns::address_list& addresses, connect_callback callback);

private:
  uv::loop* loop_;
};

} // namespace uvp::io

namespace std {

template<>
struct is_error_code_enum<uvp::io::connect_errc> : true_type {};

} // namespace std
