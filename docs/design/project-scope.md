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
- It should not become an application server suite, a proxy product, a broker,
  or a database framework. Larger protocol domains should stay limited to
  reusable client/session building blocks unless the project explicitly chooses
  otherwise later.
- It should not make synchronous-looking APIs that secretly block the event
  loop.

## Candidate Modules

The initial family of modules can grow in this order:

- `uvp::http`: HTTP/1.1 server first, including response helpers such as
  Server-Sent Events, then client primitives.
- `uvp::dns`: asynchronous host/service resolution for outbound protocol
  clients.
- `uvp::websocket`: WebSocket server/client sessions built on HTTP upgrade.
- `uvp::tls`: TLS stream adapter over uvpp streams, with backend-specific
  providers such as OpenSSL or mbedTLS.
- `uvp::smtp`: SMTP client primitives.
- `uvp::multipart`: multipart/form-data parser and streaming upload helpers for
  HTTP.
- `uvp::redis`: RESP client/session protocol for simple service integration.
- `uvp::mqtt`: MQTT client session primitives for event-based messaging.
- database client adapters, if they remain in this package after dependency and
  scope review.

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
