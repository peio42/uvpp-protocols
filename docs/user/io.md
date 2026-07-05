# IO

`uvp::io` provides the shared transport abstractions used by HTTP, TLS,
WebSocket, and client-side protocol foundations.

The core type is `uvp::io::byte_stream`: an asynchronous readable/writable byte
stream with endpoint metadata and explicit close. TCP, Unix sockets, TLS, and
future protocol layers can all adapt to this boundary.

## TCP Connector

`uvp::io::tcp_connector` opens outbound TCP connections and returns a connected
`byte_stream`.

```cpp
#include <uvpp/protocols/io.hpp>

uv::loop loop;
uvp::io::tcp_connector connector(loop);

auto op = connector.connect(
  uvp::io::tcp_endpoint{.host = "127.0.0.1", .port = 8080},
  uvp::io::connect_options{.timeout = std::chrono::seconds{3}},
  [](uvp::result<uvp::io::byte_stream> result) {
    if (!result) {
      auto error = result.error().code;
      return;
    }

    auto stream = std::move(result.value());
    auto remote = stream.remote_endpoint();
  });

loop.run();
```

Operations are cancellable:

```cpp
op.cancel();
```

Cancellation completes the operation with `uvp::io::connect_errc::cancelled`
unless the connection has already completed.

If `connect_options::timeout` is greater than zero and no candidate connects in
time, the operation completes with `uvp::io::connect_errc::timeout`.

## DNS Composition

The connector can consume a `uvp::dns::address_list` directly:

```cpp
#include <uvpp/protocols/dns.hpp>
#include <uvpp/protocols/io.hpp>

uvp::dns::resolver resolver(loop);
uvp::io::tcp_connector connector(loop);

auto resolve = resolver.resolve(
  uvp::dns::query{}.host("api.example.com").service("http"),
  [&](uvp::result<uvp::dns::address_list> addresses) {
    if (!addresses) {
      return;
    }

    connector.connect(
      addresses.value(),
      [](uvp::result<uvp::io::byte_stream> connected) {
        if (!connected) {
          return;
        }

        auto stream = std::move(connected.value());
      });
  });
```

The first connector implementation tries address candidates sequentially. Happy
Eyeballs remains a future hardening point.
