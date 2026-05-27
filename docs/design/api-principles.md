# API Principles

## Namespace and Naming

Public APIs live under `uv::<protocol>`, for example `uv::http::server` and
`uv::websocket::session`.

Names should use the same C++ style as uvpp:

```cpp
srv.listen("127.0.0.1", 8080);
res.status(204).end();
session.close();
```

Keep protocol terminology when it is already familiar: `request`, `response`,
`header`, `route`, `upgrade`, `frame`, `handshake`, `status`, `method`,
`listen`, and `close`.

## Construction

Protocol owners should be constructed from an explicit uvpp loop or stream:

```cpp
uv::loop loop;
uv::http::server srv(loop);
```

Modules must not use a hidden default loop. If a module adapts an existing
stream, that stream dependency should be visible:

```cpp
uv::tls::stream tls(client_tcp, context);
uv::websocket::session ws(http_request, http_response);
```

## Callbacks

Runtime callbacks are the ergonomic default. They may capture state and are
stored by the module owner or operation owner.

```cpp
srv.get("/health", [](uv::http::request& req, uv::http::response& res) {
  res.text("ok");
});
```

Static callback variants may be added for hot paths if the underlying module can
avoid storing callable state without making the API awkward.

Callback parameters should be typed protocol objects, not raw libuv types.
Borrowed values must be documented as borrowed. Values that can reasonably be
needed after the callback should either be cheap to copy or have an explicit
copying API.

## Errors

Immediate setup failures should throw `uv::error` or protocol-specific errors
that carry an underlying `std::error_code`.

Asynchronous failures should be delivered through result objects, close events,
or response/session state rather than thrown from libuv callbacks.

HTTP route handlers may throw application exceptions only if the server has a
documented exception policy. The default policy should convert uncaught handler
exceptions into `500 Internal Server Error`, close the response if needed, and
keep the event loop alive.

## Ownership

Protocol modules may own higher-level state when ownership is part of the
abstraction:

- accepted connection sessions;
- parser state;
- route tables;
- request body buffers up to configured limits;
- queued write operations;
- timers used for timeouts.

Ownership must not be surprising. APIs that retain data should either copy it
or document that the caller must keep it alive. APIs that start asynchronous
work should make the operation owner clear.

Low-level escape hatches may expose uvpp handles or native objects through
explicit functions:

```cpp
uv::tcp& server_tcp = srv.tcp();
uv::tcp& session_tcp = req.connection().tcp();
```

There should be no implicit conversion from protocol objects to raw libuv
pointers.

## Configuration

Use option structs when configuration grows beyond one or two parameters. Option
types should remain plain value types, but each option type should also provide
a fluent builder for user-facing construction:

```cpp
auto options = uv::http::server_options::builder()
  .max_header_bytes(16 * 1024)
  .max_body_bytes(1024 * 1024)
  .body_timeout(30s)
  .keep_alive(true)
  .build();

uv::http::server srv(loop, options);
```

Option defaults should be safe for small services. Expensive or memory-heavy
features should be opt-in.

Builder setters should validate values that are immediately invalid, such as a
zero header limit when the protocol cannot operate with one. Cross-field
validation may happen in `build()`:

```cpp
struct server_options {
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_body_bytes = 1024 * 1024;
  std::chrono::milliseconds body_timeout = std::chrono::seconds{30};
  bool keep_alive = true;

  class builder_type;

  static builder_type builder();
};
```

The public fields make tests, logging, and advanced integration straightforward.
The builder gives application code a stable, readable configuration style:

```cpp
uv::http::server srv(
  loop,
  uv::http::server_options::builder()
    .max_body_bytes(10 * 1024 * 1024)
    .idle_timeout(2min)
    .server_header(false)
    .build());
```

For simple examples, default construction should stay valid:

```cpp
uv::http::server srv(loop);
```

## Dependency Policy

Each protocol module should keep third-party dependencies local to that module.
For example, an HTTP parser dependency should not leak into the public API.

When a protocol has tricky parsing or security-sensitive state machines, prefer
a mature backend over a custom parser. The wrapper should provide the C++ API,
event-loop integration, ownership, and uvpp interop.

The project uses a mixed build strategy:

- header-only for public vocabulary types and small pure helpers;
- compiled sources for anything that owns state, talks to libuv, or wraps an
  external dependency.

For HTTP specifically, external dependencies are allowed only under `detail/`,
as synchronous state machines. They never own the socket, the loop, timers,
output buffers, or the public model.
