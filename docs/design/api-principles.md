# API Principles

## Namespace and Naming

Public APIs live under `uvp::<protocol>`, for example `uvp::http::server` and
`uvp::websocket::session`.

Names should use the same C++ style as uvpp:

```cpp
srv.listen("127.0.0.1", 8080);
res.status(204).end();
session.close();
```

Keep protocol terminology when it is already familiar: `request`, `response`,
`header`, `route`, `upgrade`, `frame`, `handshake`, `status`, `method`,
`listen`, and `close`.

## Member Storage and Accessors

Use a trailing underscore for non-static data members of public types and for
private implementation types that own behavior or invariants:

```cpp
class response {
public:
  [[nodiscard]] unsigned int status_code() const noexcept;

private:
  std::shared_ptr<detail::response_state> state_;
};
```

The underscore marks storage, not protocol vocabulary. Public APIs should keep
the canonical protocol name without a prefix or suffix:

```cpp
request.path();
response.status(201);
options.max_body_bytes(1024 * 1024);
```

Use setters/getters at public API boundaries and where a type protects an
invariant. For options, fluent setters keep the canonical name and the storage
field remains private with a trailing underscore. If public reads are useful,
expose a const accessor with the same canonical name rather than a `get_`
prefix:

```cpp
struct server_options {
  server_options& max_header_bytes(std::size_t value) &;
  [[nodiscard]] std::size_t max_header_bytes() const noexcept;

private:
  std::size_t max_header_bytes_ = 16 * 1024;
};
```

Do not add getters and setters merely to route private implementation code
through accessors. Inside a private implementation type, direct field access is
fine when the caller is part of the same invariant boundary.

Plain aggregate-style helper structs may use unsuffixed fields only when they
are simple records with no meaningful behavior, such as parser events,
snapshots, or small return objects. Once a private struct grows lifecycle
methods, callbacks, queues, timers, or protocol state, prefer member suffixes
there as well.

## Construction

Protocol owners should be constructed from an explicit uvpp loop or stream:

```cpp
uv::loop loop;
uvp::http::server srv(loop);
```

Modules must not use a hidden default loop. If a module adapts an existing
stream, that stream dependency should be visible:

```cpp
uvp::some_protocol::session session(stream, options);
```

## Callbacks

Runtime callbacks are the ergonomic default. They may capture state and are
stored by the module owner or operation owner.

```cpp
srv.get("/health", uvp::http::body::none{}, [](uvp::http::request& req, uvp::http::response& res) {
  res.text("ok");
});
```

Static callback variants may be added for hot paths if the underlying module can
avoid storing callable state without making the API awkward.

Callback parameters should be typed protocol objects, not raw libuv types.
Borrowed values must be documented as borrowed. Values that can reasonably be
needed after the callback should either be cheap to copy or have an explicit
copying API.

## Layered Protocol Control

Protocol APIs should default to compliant automatic behavior for mandatory
control flow. A small service should not need to implement protocol maintenance
frames, parser bookkeeping, or required response handshakes just to stay
correct.

Advanced users may opt out through explicit options. Once opted out, they own
the corresponding protocol obligation:

```cpp
auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
  .auto_pong(false));

ws.on_ping([](uvp::websocket::session& ws, std::span<const std::byte> payload) {
  ws.pong(payload);
});
```

Hooks in the automatic mode are observational by default. If a callback both
observes and takes over protocol behavior, the opt-out should be visible at the
call site.

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
explicit functions, but only on the adapter that naturally owns that handle:

```cpp
uv::tcp& tcp = tcp_stream.tcp();
uv::pipe& pipe = pipe_stream.pipe();
```

There should be no implicit conversion from protocol objects to raw libuv
pointers.

## Configuration

Use option structs when configuration grows beyond one or two parameters. Option
types should remain plain value types and should follow uvpp's fluent
value-object style rather than a separate builder object with a final
`.build()` step:

```cpp
auto options = uvp::http::server_options{}
  .max_header_bytes(16 * 1024)
  .max_body_bytes(1024 * 1024)
  .max_pending_write_bytes(1024 * 1024)
  .keep_alive(true);

uvp::http::server srv(loop, options);
```

Option defaults should be safe for small services. Expensive or memory-heavy
features should be opt-in.

Fluent setters should validate values that are immediately invalid, such as a
zero header limit when the protocol cannot operate with one. Cross-field
validation should happen when the options are consumed by a protocol owner:

```cpp
struct server_options {
  server_options& max_header_bytes(std::size_t value) &;
  server_options&& max_header_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_header_bytes() const noexcept;

  server_options& max_pending_write_bytes(std::size_t value) &;
  server_options&& max_pending_write_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_pending_write_bytes() const noexcept;

  server_options& keep_alive(bool value) & noexcept;
  server_options&& keep_alive(bool value) && noexcept;
  [[nodiscard]] bool keep_alive() const noexcept;

private:
  std::size_t max_header_bytes_ = 16 * 1024;
  std::size_t max_body_bytes_ = 1024 * 1024;
  std::size_t max_pending_write_bytes_ = 1024 * 1024;
  bool keep_alive_ = true;
};
```

When storage and a fluent setter represent the exact same value, the setter
keeps the canonical public name and the storage field stays private with a
trailing underscore. If public reads are useful, expose a const accessor with
the same canonical name rather than a `get_` prefix:

```cpp
struct server_options {
  server_options& max_header_bytes(std::size_t value) &;
  server_options&& max_header_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_header_bytes() const noexcept;

private:
  std::size_t max_header_bytes_ = 16 * 1024;
};
```

This avoids maintaining two user-facing write paths for the same concept. It
also makes the fluent call site match the option name used in documentation:

```cpp
uvp::http::server srv(
  loop,
  uvp::http::server_options{}
    .max_body_bytes(10 * 1024 * 1024)
    .max_pending_write_bytes(2 * 1024 * 1024)
    .server_header(false));
```

For simple examples, default construction should stay valid:

```cpp
uvp::http::server srv(loop);
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
