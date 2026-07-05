# TLS

`uvp::tls` adapts existing `uvp::io::byte_stream` and
`uvp::io::stream_listener` values. TLS is a transport layer, not an HTTP-only
feature.

```text
TCP -> TLS -> HTTP
TCP -> TLS -> WebSocket
TCP -> TLS -> custom byte-stream protocol
```

Include the TLS module with:

```cpp
#include <uvpp/protocols/tls.hpp>
```

## Server Context

Servers need a certificate chain and private key:

```cpp
auto context = uvp::tls::server_context{}
  .certificate_chain_file("server.crt")
  .private_key_file("server.key")
  .alpn({"http/1.1"});
```

Use `require_alpn()` when a connection must negotiate one of the configured
protocols:

```cpp
context.require_alpn();
```

Without `require_alpn()`, a handshake can complete with no selected ALPN.

## TLS Listener

`uvp::tls::listener` wraps an existing stream listener and emits clear
`uvp::io::byte_stream` values only after the server-side TLS handshake
succeeds:

```cpp
auto tcp = uvp::io::tcp_listener{loop}
  .bind("0.0.0.0", 8443);

auto secure = uvp::tls::listener{
  uvp::io::stream_listener{std::move(tcp)},
  std::move(context),
  uvp::tls::listener_options{}
    .handshake_timeout(std::chrono::seconds{10})
    .max_pending_handshakes(1024)};
```

Any protocol that accepts `uvp::io::stream_listener` can consume it:

```cpp
uvp::http::server http(loop);
http.listen(std::move(secure));
```

Handshake timeout and pending-handshake-limit failures are reported through the
listener accept error path. Closing the TLS listener cancels pending handshakes
but does not take ownership back from streams that have already been emitted.

## Stream Handshake

Lower-level code can adapt a single stream directly:

```cpp
uvp::tls::accept(
  std::move(lower),
  context,
  [](uvp::tls::handshake_result result) {
    if (!result) {
      return;
    }

    auto clear = std::move(result).stream();
    auto selected = std::string{result.selected_alpn()};
  });
```

Clients use `connect()`:

```cpp
auto client_context = uvp::tls::client_context{}
  .server_name("api.example.com")
  .default_verify_paths()
  .alpn({"http/1.1"});

uvp::tls::connect(
  std::move(lower),
  client_context,
  [](uvp::tls::handshake_result result) {
    if (!result) {
      return;
    }

    auto clear = std::move(result).stream();
  });
```

The returned `handshake_operation` can cancel an in-progress handshake. After
the handshake succeeds or fails, `active()` returns `false` and `cancel()` is a
no-op.

## Verification

Client contexts verify peers by default. Configure trust with system defaults
or explicit CA locations:

```cpp
auto client_context = uvp::tls::client_context{}
  .server_name("localhost")
  .ca_file("server-ca.pem");
```

`server_name(...)` enables SNI and hostname verification. Use
`default_verify_paths()` for the platform trust store.

For local tests or intentionally insecure development connections, verification
can be disabled explicitly:

```cpp
auto client_context = uvp::tls::client_context{}
  .insecure_no_verify_peer();
```

The method name is deliberately noisy. Do not use it for production
connections.

## Backpressure

TLS has bounded cleartext buffering on both sides of the adapter:

```cpp
auto server_context = uvp::tls::server_context{}
  .certificate_chain_file("server.crt")
  .private_key_file("server.key")
  .max_pending_write_bytes(1024 * 1024)
  .max_pending_read_bytes(1024 * 1024);
```

The same limits are available on `client_context`. Exceeding them reports TLS
errors instead of growing memory without bound.

## Close And EOF

TLS close is explicit. `close()` sends `close_notify` when possible, flushes
encrypted shutdown bytes, and then closes the lower stream.

Incoming `close_notify` is reported to the clear stream as EOF. A transport EOF
without TLS `close_notify` is reported as `uvp::tls::errc::unexpected_eof`,
because it may indicate a truncated TLS stream.

Read EOF and read errors are terminal and delivered at most once.

## Errors

TLS errors use the `uvp::tls` error category:

```cpp
if (!result) {
  auto code = result.error().code;
}
```

Common codes include `handshake_failed`, `verification_failed`, `timeout`,
`pending_handshake_limit`, `write_buffer_limit`, `read_buffer_limit`, and
`unexpected_eof`.
