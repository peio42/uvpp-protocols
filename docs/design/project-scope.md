# Project Scope

## Purpose

`uvpp-protocols` provides reusable protocol implementations for applications
that already want uvpp's explicit event-loop model but do not want to rebuild
HTTP, WebSocket, TLS, SMTP, or similar protocol machinery in every project.

The library is not a replacement for uvpp. uvpp remains the low-level C++20
wrapper over libuv handles, requests, buffers, callbacks, errors, and filesystem
utilities. `uvpp-protocols` is a higher-level layer that composes those building
blocks into application protocols.

## Goals

- Keep the event loop explicit: users construct protocol objects from
  `uv::loop`.
- Use idiomatic C++20 surface types: `std::string_view`, `std::span`,
  `std::chrono`, typed enums, and small value wrappers.
- Preserve uvpp's ownership honesty: asynchronous lifetime must be visible in
  the type, the function name, or the module documentation.
- Provide practical protocol defaults without hiding advanced control.
- Allow modules to be consumed independently: an application should be able to
  depend on HTTP without also linking SMTP, TLS, or WebSocket support.
- Prefer proven protocol engines where hand-written parsing would be risky.
  `uvpp-protocols` should spend its complexity budget on integration,
  ownership, and API shape rather than reimplementing mature parsers.

## Non-Goals

- It is not a monolithic web framework.
- It is not an opinionated application container, dependency injection system,
  ORM, template engine, or logging framework.
- It should not hide the libuv loop behind global state.
- It should not replace dedicated mature implementations for large domains such
  as full HTTP/2, QUIC, or complete email server suites unless the project
  explicitly chooses those investments later.
- It should not make synchronous-looking APIs that secretly block the event
  loop.

## Candidate Modules

The initial family of modules can grow in this order:

- `uv::http`: HTTP/1.1 server first, then client primitives.
- `uv::websocket`: WebSocket server/client sessions built on HTTP upgrade.
- `uv::tls`: TLS stream adapter over uvpp streams, with backend-specific
  providers such as OpenSSL or mbedTLS.
- `uv::smtp`: SMTP client and minimal server/session primitives.
- `uv::sse`: Server-Sent Events response helper on top of HTTP.
- `uv::multipart`: multipart/form-data parser and streaming upload helpers for
  HTTP.
- `uv::redis`: RESP client/session protocol for simple service integration.
- `uv::mqtt`: MQTT client/server session primitives for event-based messaging.
- `uv::proxy`: CONNECT and simple reverse-proxy building blocks.

Some modules should be thin protocol helpers, while others may be full session
owners. The deciding factor is whether the module must coordinate parsing,
state machines, timers, and writes over time.

## Package Shape

The preferred include layout is:

```cpp
#include <uvpp/protocols/http.hpp>
#include <uvpp/protocols/websocket.hpp>
#include <uvpp/protocols/tls.hpp>
```

Module internals may live under protocol-specific subdirectories:

```text
include/uvpp/protocols/
  http.hpp
  http/
    server.hpp
    request.hpp
    response.hpp
    router.hpp
    headers.hpp
    detail/
  websocket.hpp
  tls.hpp
```

Each public module should expose one aggregate header for common use and
focused headers for users who want smaller includes.

