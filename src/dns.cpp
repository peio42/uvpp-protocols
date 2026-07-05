#include <uvpp/protocols/dns.hpp>

#include <uvpp/uv.hpp>

#include <uv.h>

#include <cassert>
#include <memory>
#include <netdb.h>
#include <string>
#include <utility>
#include <vector>

namespace uvp::dns {

namespace {

class dns_error_category : public std::error_category {
public:
  [[nodiscard]] const char* name() const noexcept override { return "uvp.dns"; }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<errc>(value)) {
    case errc::invalid_query:
      return "DNS query is invalid";
    case errc::unsupported_address_family:
      return "DNS address family is unsupported";
    case errc::name_not_found:
      return "DNS name was not found";
    case errc::temporary_failure:
      return "temporary DNS resolution failure";
    case errc::no_usable_addresses:
      return "DNS returned no usable addresses";
    case errc::cancelled:
      return "DNS resolution was cancelled";
    case errc::system_failure:
      return "DNS resolution failed";
    }
    return "unknown DNS error";
  }
};

[[nodiscard]] uvp::error make_dns_error(errc code, std::string detail = {}) {
  return uvp::error{make_error_code(code), std::move(detail)};
}

[[nodiscard]] errc map_uv_error(int status) noexcept {
  switch (status) {
  case UV_ECANCELED:
    return errc::cancelled;
  case UV_EAI_NONAME:
    return errc::name_not_found;
  case UV_EAI_AGAIN:
    return errc::temporary_failure;
  default:
    return errc::system_failure;
  }
}

[[nodiscard]] int to_native_family(address_family family) noexcept {
  switch (family) {
  case address_family::any:
    return AF_UNSPEC;
  case address_family::ipv4:
    return AF_INET;
  case address_family::ipv6:
    return AF_INET6;
  }
  return AF_UNSPEC;
}

[[nodiscard]] address_family from_native_family(int family) noexcept {
  switch (family) {
  case AF_INET:
    return address_family::ipv4;
  case AF_INET6:
    return address_family::ipv6;
  default:
    return address_family::any;
  }
}

[[nodiscard]] address_list copy_addresses(const uv::getaddrinfo_result& result) {
  auto addresses = std::vector<address>{};
  for (auto entry : result) {
    auto value = entry.to_value();
    if (!value.address.is_v4() && !value.address.is_v6()) {
      continue;
    }

    addresses.emplace_back(
      from_native_family(value.family),
      uvp::io::tcp_endpoint{
        .host = value.address.to_string(),
        .port = static_cast<unsigned int>(value.address.port()),
      });
  }
  return address_list{std::move(addresses)};
}

class resolve_state : public std::enable_shared_from_this<resolve_state> {
public:
  resolve_state(uv::loop& loop, query request, resolve_callback callback)
      : loop_(&loop), request_(std::move(request)), callback_(std::move(callback)) {}

  resolve_state(const resolve_state&) = delete;
  resolve_state& operator=(const resolve_state&) = delete;

  [[nodiscard]] resolve_operation start() {
    if (!callback_) {
      complete(make_dns_error(errc::invalid_query, "missing DNS callback"));
      return resolve_operation{shared_from_this()};
    }
    if (request_.host().empty() || request_.service().empty()) {
      complete(make_dns_error(errc::invalid_query, "DNS host and service are required"));
      return resolve_operation{shared_from_this()};
    }

    auto hints = addrinfo{};
    hints.ai_family = to_native_family(request_.family());
    hints.ai_socktype = SOCK_STREAM;

    auto self = shared_from_this();
    uv::getaddrinfo(
      *loop_,
      native_request_,
      request_.host(),
      request_.service(),
      &hints,
      [self](uv::getaddrinfo_request&, uv::getaddrinfo_result result) mutable {
        self->on_resolved(std::move(result));
      });

    return resolve_operation{std::move(self)};
  }

  void cancel() noexcept {
    if (completed_) {
      return;
    }

    cancelled_ = true;
    (void)native_request_.try_cancel();
    complete(make_dns_error(errc::cancelled));
  }

private:
  void on_resolved(uv::getaddrinfo_result result) {
    if (completed_) {
      return;
    }

    if (cancelled_) {
      complete(make_dns_error(errc::cancelled));
      return;
    }

    if (!result) {
      complete(make_dns_error(map_uv_error(result.raw_status()), result.error_code().message()));
      return;
    }

    auto addresses = copy_addresses(result);
    if (addresses.empty()) {
      complete(make_dns_error(errc::no_usable_addresses));
      return;
    }

    complete(std::move(addresses));
  }

  void complete(uvp::result<address_list> result) {
    if (completed_) {
      return;
    }

    completed_ = true;
    auto callback = std::move(callback_);
    if (callback) {
      callback(std::move(result));
    }
  }

  uv::loop* loop_;
  dns::query request_;
  resolve_callback callback_;
  uv::getaddrinfo_request native_request_;
  bool cancelled_ = false;
  bool completed_ = false;
};

[[nodiscard]] std::shared_ptr<resolve_state> resolve_state_from(const std::shared_ptr<void>& state) noexcept {
  return std::static_pointer_cast<resolve_state>(state);
}

} // namespace

const std::error_category& error_category() noexcept {
  static const dns_error_category instance;
  return instance;
}

std::error_code make_error_code(errc value) noexcept {
  return {static_cast<int>(value), error_category()};
}

query& query::host(std::string value) & {
  host_ = std::move(value);
  return *this;
}

query&& query::host(std::string value) && {
  host(std::move(value));
  return std::move(*this);
}

query& query::service(std::string value) & {
  service_ = std::move(value);
  return *this;
}

query&& query::service(std::string value) && {
  service(std::move(value));
  return std::move(*this);
}

query& query::port(unsigned int value) & {
  service_ = std::to_string(value);
  return *this;
}

query&& query::port(unsigned int value) && {
  port(value);
  return std::move(*this);
}

query& query::family(address_family value) noexcept {
  family_ = value;
  return *this;
}

address::address(address_family family, uvp::io::tcp_endpoint endpoint)
    : family_(family), endpoint_(std::move(endpoint)) {}

address_list::address_list(container_type addresses)
    : addresses_(std::move(addresses)) {}

resolve_operation::resolve_operation(std::shared_ptr<void> state)
    : state_(std::move(state)) {}

void resolve_operation::cancel() noexcept {
  if (!state_) {
    return;
  }
  resolve_state_from(state_)->cancel();
}

resolver::resolver(uv::loop& loop) noexcept
    : loop_(&loop) {}

resolve_operation resolver::resolve(query request, resolve_callback callback) {
  assert(loop_ != nullptr);
  auto state = std::make_shared<resolve_state>(*loop_, std::move(request), std::move(callback));
  return state->start();
}

} // namespace uvp::dns
