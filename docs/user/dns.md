# DNS

`uvp::dns::resolver` resolves host and service pairs asynchronously on a
`uv::loop`. It wraps libuv `getaddrinfo` and returns copied address candidates
that can later feed outbound TCP connection helpers.

```cpp
#include <uvpp/protocols/dns.hpp>

uv::loop loop;
uvp::dns::resolver resolver(loop);

auto op = resolver.resolve(
  uvp::dns::query{}
    .host("api.example.com")
    .service("https")
    .family(uvp::dns::address_family::any),
  [](uvp::result<uvp::dns::address_list> result) {
    if (!result) {
      auto error = result.error().code;
      return;
    }

    for (auto const& address : result.value()) {
      auto host = address.host();
      auto port = address.port();
    }
  });

loop.run();
```

Operations are cancellable:

```cpp
op.cancel();
```

Cancellation completes the operation with `uvp::dns::errc::cancelled` exactly
once from the user's point of view. The resolver does not open sockets and does
not implement connection racing; it only returns ordered address candidates.
