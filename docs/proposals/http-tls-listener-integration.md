# HTTP TLS Listener Integration Proposal

Status: Draft

## Context

HTTP should not depend directly on TLS, but HTTP over TLS should be easy once
the TLS module exists.

The intended composition is:

```cpp
auto tcp = uvp::io::tcp_listener{loop}
  .bind("0.0.0.0", 443);

auto context = uvp::tls::server_context{}
  .certificate_chain_file("server.crt")
  .private_key_file("server.key")
  .alpn({"http/1.1"});

uvp::http::server http(loop);
http.listen(uvp::tls::listener{std::move(tcp), std::move(context)});
```

The HTTP module should observe only the existing `io::stream_listener` shape.
TLS owns handshake mechanics before HTTP receives a clear `byte_stream`.

## Current State

- Implemented: `http::server::listen(io::stream_listener)` and transport
  composition over `byte_stream`.
- Not implemented: TLS listener adapter, HTTP examples for TLS listener
  composition, and any optional HTTP convenience helper for TLS listeners.

## Draft Scope

- Ensure `http::server` accepts a TLS listener through the generic listener
  path.
- Document that TLS handshake errors are listener accept errors, not HTTP
  parser errors.
- Preserve HTTP behavior for already-clear streams; no HTTP request object
  should expose OpenSSL or TLS backend details.
- Consider an optional convenience helper only after the generic path works.
- Keep OpenSSL and TLS types out of the HTTP server core.

## Out Of Scope

- Making TLS a required HTTP dependency.
- HTTPS-specific routing model.
- Certificate management beyond TLS context configuration.
- ALPN-driven HTTP/2 dispatch; that belongs after HTTP/2 server design.

## Source Documents

- [HTTP server design](../design/http-server.md)
- [TLS support proposal](tls-support.md)
- [TLS listener adapter](tls-listener-adapter.md)
- [Protocol composition](../design/protocol-composition.md)
- [ADR 0002](../adr/0002-module-boundaries-and-dependency-direction.md)
