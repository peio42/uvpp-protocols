# Transport Abstractions

HTTP, TLS, WebSocket, MQTT, SMTP, and database adapters should compose over
transport objects rather than inheriting from each other or binding directly to
TCP.

The first milestone should introduce two transport concepts:

- `uvp::io::stream_listener`: accepts incoming byte streams.
- `uvp::io::byte_stream`: reads, writes, and closes one established byte
  stream.

This lets HTTP start over TCP and Unix sockets, and later over TLS, without
changing the HTTP parser/session model.

## Design Direction

Use composition and type erasure, not protocol inheritance.

### Why not a common base class from uvpp

uvpp wraps libuv handles using CRTP and inline storage: the raw C struct
(`uv_tcp_t`, `uv_pipe_t`, etc.) is stored directly inside the C++ object.
libuv callbacks reconstitute the C++ wrapper from a raw C pointer via
`reinterpret_cast`, which requires `std::is_standard_layout`. Adding a virtual
dispatch table would break this invariant.

`uv::stream<Derived, Raw>` is a CRTP template, so `stream<tcp, uv_tcp_t>` and
`stream<pipe, uv_pipe_t>` are unrelated instantiations with no shared base
that could serve as a polymorphic interface. `uv::tcp` and `uv::pipe` are also
declared `final`.

As a result, `byte_stream` and `stream_listener` use internal type erasure
(`concept_` / model classes) to provide a single uniform type over all
transport kinds. This pattern should be followed by any future transport
adapter (TLS, QUIC, etc.).

```text
uvp::http::server
  owns stream_listener objects
  owns HTTP sessions
    each session owns a byte_stream
```

Concrete adapters create type-erased transports:

```text
uvp::io::tcp_listener  -> stream_listener
uvp::io::pipe_listener -> stream_listener
uvp::tls::listener            -> stream_listener

uv::tcp  -> byte_stream
uv::pipe -> byte_stream
TLS stream adapter -> byte_stream
WebSocket stream adapter, if needed -> framed/message stream
```

The public modules compose by receiving the transport shape they need. HTTP
does not inherit from TCP, TLS, or Unix sockets.

## C++20 Result Shape

The project currently targets C++20. `std::expected` is C++23, so public APIs
should not depend on it unless the language baseline changes.

Instead of:

```cpp
using accept_callback =
  std::function<void(std::expected<byte_stream, stream_error>)>;
```

Prefer a small project-owned result type:

```cpp
namespace uvp::io {

class accept_result {
public:
  [[nodiscard]] bool ok() const noexcept;
  explicit operator bool() const noexcept;

  [[nodiscard]] byte_stream& stream() &;
  [[nodiscard]] byte_stream&& stream() &&;
  [[nodiscard]] const stream_error& error() const noexcept;
};

using accept_callback = std::function<void(accept_result)>;

}
```

The exact storage can be private. The public shape should make success/failure
obvious without introducing a generic expected dependency.

## Byte Stream

Initial sketch:

```cpp
namespace uvp::io {

using read_callback = std::function<void(read_result)>;
using write_callback = std::function<void(stream_error)>;
using close_callback = std::function<void()>;

class byte_stream {
public:
  byte_stream() = default;

  uv::loop& loop() noexcept;

  void read_start(read_callback on_read);
  void read_stop();

  void write(std::span<const std::byte> bytes, write_callback on_write);
  void close(close_callback on_close = {});

  endpoint local_endpoint() const;
  endpoint remote_endpoint() const;

  explicit operator bool() const noexcept;

private:
  struct concept_;
  std::unique_ptr<concept_> self_;
};

}
```

`byte_stream` owns the concrete stream handle or references state owned by a
higher-level adapter. For accepted TCP and pipe connections, owning the concrete
`uv::tcp` or `uv::pipe` inside the adapter is appropriate.

Read callbacks receive borrowed byte views. The bytes are valid only for the
duration of the callback that receives them. A concrete `byte_stream`
implementation may own a small scratch buffer used to satisfy libuv's read
allocation callback, because that buffer is transport mechanics rather than
protocol state. Protocol layers that need data to survive the callback must
copy or move it into their own parser, frame, body, or message buffers.

Writes must retain payload memory until the uvpp write callback completes.
That retention belongs to the byte-stream implementation or to the protocol
session, not to `llhttp`.

## Stream Listener

Initial sketch:

```cpp
namespace uvp::io {

using accept_callback = std::function<void(accept_result)>;

class stream_listener {
public:
  stream_listener() = default;

  uv::loop& loop() noexcept;

  void listen(accept_callback on_accept);
  void close();

  endpoint local_endpoint() const;

  explicit operator bool() const noexcept;

private:
  struct concept_;
  std::unique_ptr<concept_> self_;
};

}
```

The listener accepts byte streams. It does not parse HTTP, perform TLS, or own
HTTP sessions.

## Concrete Listener Adapters

TCP:

```cpp
namespace uvp::io {

class tcp_listener {
public:
  explicit tcp_listener(uv::loop& loop);

  tcp_listener& bind(std::string_view host, unsigned int port);
  tcp_listener& backlog(unsigned int value);

  operator stream_listener() &&;
};

}
```

Unix socket / named pipe:

```cpp
namespace uvp::io {

class pipe_listener {
public:
  explicit pipe_listener(uv::loop& loop);

  pipe_listener& bind(std::string_view path);
  pipe_listener& backlog(unsigned int value);

  operator stream_listener() &&;
};

}
```

The naming follows libuv/uvpp: Unix domain sockets are represented by
`uv::pipe`.

## HTTP Server API

The convenient TCP form remains:

```cpp
uvp::http::server srv(loop);
srv.listen("127.0.0.1", 8080);
```

That is shorthand for:

```cpp
srv.listen(
  uvp::io::tcp_listener{loop}
    .bind("127.0.0.1", 8080));
```

Unix socket listening uses the same server:

```cpp
srv.listen(
  uvp::io::pipe_listener{loop}
    .bind("/run/my-app.sock"));
```

With this model, `uvp::http::server` owns:

- one or more `uvp::io::stream_listener` objects;
- accepted HTTP sessions;
- route tables;
- server options;
- protocol timers and write queues.

It does not directly own "a listening `uv::tcp`" as part of its public design.
TCP is only one listener adapter.

## Endpoint Model

Use typed endpoints for listeners and sockets. Do not force URL syntax where an
endpoint is clearer.

Possible initial shape:

```cpp
namespace uvp::io {

struct tcp_endpoint {
  std::string host;
  unsigned int port = 0;
};

struct pipe_endpoint {
  std::string path;
};

using endpoint = std::variant<std::monostate, tcp_endpoint, pipe_endpoint>;

}
```

`uvp::url` may help parse user-provided configuration strings, but listener APIs
should keep typed endpoint overloads. Unix socket URL conventions vary, and the
typed form avoids ambiguity.

## Relationship to TLS and WebSocket

TLS should compose as a listener and stream adapter:

```text
tcp_listener
  -> tls_listener
    -> stream_listener
      -> uvp::http::server
```

WebSocket starts as an HTTP upgrade. After upgrade, the HTTP session transfers
or wraps the underlying `byte_stream` into a WebSocket session.

No protocol should inherit from another protocol. Ownership transfer must be
explicit at upgrade/adaptation points.
