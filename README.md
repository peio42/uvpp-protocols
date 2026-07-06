# uvpp-protocols

Higher-level protocol modules for [`uvpp`](https://github.com/peio42/uvpp).

`uvpp-protocols` is intended to provide reusable protocol building blocks on top
of uvpp's event-based libuv wrapper. The goal is to let applications keep the
explicit libuv/uvpp event-loop model while avoiding repeated implementations of
common protocols such as HTTP, WebSocket, TLS, SMTP, and MQTT.

This repository is in its early implementation milestones. The first modules
are an HTTP/1.1 server built on uvpp, libuv, `llhttp`, and `nlohmann/json`,
server-side WebSocket sessions on top of HTTP upgrade routes, and TLS stream
and listener adapters backed by OpenSSL.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The default top-level build compiles the library, the focused test executable,
and the runnable HTTP and WebSocket examples. The examples require `uvpp::uvpp`
and `nlohmann_json::nlohmann_json`; CMake first tries `find_package(...)` and
then fetches missing dependencies when enabled. Because `uvpp::uvpp` links
`LibUV::LibUV` and `Threads::Threads`, the examples are built against libuv.

Run the HTTP example:

```sh
./build/uvpp_protocols_http_server_example
curl -i http://127.0.0.1:8080/health
```

Milestone 2 examples:

```sh
./build/uvpp_protocols_admin_endpoints_example
curl -i http://127.0.0.1:8081/admin/health
curl -i -X POST http://127.0.0.1:8081/admin/maintenance/on

./build/uvpp_protocols_local_json_api_example
curl -i http://127.0.0.1:8082/v1/items
curl -i -X POST http://127.0.0.1:8082/v1/items -d '{"title":"ship examples"}'

./build/uvpp_protocols_log_streaming_example
curl -N http://127.0.0.1:8083/logs/stream
```

The log streaming example uses native HTTP/1.1 chunked responses and
newline-delimited JSON. It demonstrates `response::stream()`,
`streaming_response::write()`, `on_drain`, and async error handling.

WebSocket example:

```sh
./build/uvpp_protocols_websocket_echo_example
```

It listens on `ws://127.0.0.1:8084/echo` and echoes text and binary messages.

## Target HTTP API

```cpp
#include <uvpp/protocols/http.hpp>

uv::loop loop;

uvp::http::server srv(loop);

srv.get("/health", uvp::http::body::none{}, [](uvp::http::request& req, uvp::http::response& res) {
  res.json(uvp::json{{"status", "ok"}});
});

srv.get("/logs", uvp::http::body::none{}, [](uvp::http::request& req, uvp::http::response& res) {
  res.text(read_recent_logs());
});

srv.post("/config", uvp::http::body::text{}, [](uvp::http::request& req, uvp::http::response& res, std::string_view body) {
  auto cfg = parse_config(body);
  apply_config(cfg);
  res.status(204).end();
});

srv.listen("127.0.0.1", 8080);
loop.run();
```

Configuration APIs should follow uvpp's fluent value-object style, without a
separate `.build()` step:

```cpp
uvp::http::server srv(
  loop,
  uvp::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_body_bytes(10 * 1024 * 1024)
    .max_pending_write_bytes(2 * 1024 * 1024)
    .server_header(false));
```

## Modules

Available:

- `uvp::dns`: asynchronous host/service resolution over libuv `getaddrinfo`,
  with typed address candidates and cancellable operations.
- `uvp::http`: HTTP/1.1 server, multipart request bodies, Server-Sent Events
  response helpers, and an initial HTTP/1.1 client for `http://` and
  `https://` URLs with one-shot buffered requests, request/response streaming,
  opt-in keep-alive pooling, and phase timeouts are available.
- `uvp::io`: byte-stream/listener transport abstractions and reusable outbound
  TCP connection helpers with connect timeouts and byte-stream handle liveness
  controls.
- `uvp::url`: shared parsed URL values and helpers for client-side protocol
  foundations, including default ports, origin keys, and HTTP request targets.
- `uvp::tls`: TLS stream and listener adapters over uvpp byte streams, with
  client/server contexts, ALPN, client SNI, peer verification, backpressure,
  close-notify handling, listener handshake limits/timeouts, and HTTP listener
  composition.
- `uvp::websocket`: server-side WebSocket sessions are available; client
  sessions are planned.

Planned:

- `uvp::smtp`: SMTP client primitives.
- `uvp::mqtt`: MQTT client sessions over TCP, TLS, or WebSocket.

## Protocol Composition

The project should support explicit protocol stacks:

```text
TCP -> HTTP
TCP -> TLS -> HTTP
TCP -> TLS -> HTTP -> WebSocket
TCP -> TLS -> HTTP -> WebSocket -> MQTT
```

Each protocol layer should own its own state and compose with the layer below it
through explicit transport APIs. Convenience helpers may exist, but they should
not force unrelated dependencies between modules.

The current transport layer exposes `uvp::io::stream_listener`,
`uvp::io::byte_stream`, and `uvp::io::tcp_connector`, with TCP, Unix socket, and
TLS listener adapters. `byte_stream` forwards uvpp/libuv `ref()`, `unref()`, and
`has_ref()` so pooled or idle transports can manage loop liveness without
exposing concrete handles. The HTTP server owns listeners and accepted sessions
through these abstractions, while the HTTP client uses the connector side for
outbound TCP. HTTP over Unix sockets, HTTP over TLS, and future protocol
composition do not require a TCP-only public model.

## Dependency Policy

The project uses a mixed build strategy:

- header-only for public vocabulary types and small pure helpers;
- compiled sources for anything that owns state, talks to libuv, or wraps an
  external dependency.

The HTTP/1 parser backend is `llhttp`, integrated behind `src/http/detail` as a
synchronous state machine. When `llhttp` is fetched by CMake, its C sources are
compiled into `uvpp-protocols` rather than installed as a separate package.
HTTP/2 remains a future design topic, but early milestones do not configure or
link `libnghttp2`. External HTTP dependencies are allowed only under `detail/`.
They never own the socket, the loop, timers, output buffers, or the public
model.

TLS currently uses OpenSSL SSL/Crypto behind the `uvp::tls` module. Public
headers do not expose OpenSSL types, and OpenSSL does not own sockets, loops,
timers, read buffers, or write buffers; the uvpp reactor and byte-stream model
remain the transport boundary.

Current CMake dependency options:

```sh
cmake -S . -B build -DUVPP_PROTOCOLS_FETCH_UVPP=ON
cmake -S . -B build -DUVPP_PROTOCOLS_FETCH_LLHTTP=ON
cmake -S . -B build -DUVPP_PROTOCOLS_LLHTTP_SOURCE_DIR=/path/to/llhttp
```

`llhttp` is not a replaceable backend. It is the HTTP/1 parser used by the
library. The source-directory option exists for offline or vendored builds.
OpenSSL is discovered with `find_package(OpenSSL REQUIRED COMPONENTS SSL
Crypto)`.

## Documentation

Documentation starts in [`docs/README.md`](docs/README.md).

User documentation:

- [DNS](docs/user/dns.md)
- [HTTP client](docs/user/http-client.md)
- [HTTP server](docs/user/http-server.md)
- [IO](docs/user/io.md)
- [URL](docs/user/url.md)
- [TLS](docs/user/tls.md)
- [WebSocket](docs/user/websocket.md)

The design notes live in [`docs/design`](docs/design):

- [Project scope](docs/design/project-scope.md)
- [API principles](docs/design/api-principles.md)
- [Module architecture](docs/design/module-architecture.md)
- [Dependency decisions](docs/design/dependency-decisions.md)
- [Protocol composition](docs/design/protocol-composition.md)
- [Transport abstractions](docs/design/transport-abstractions.md)
- [HTTP server design](docs/design/http-server.md)
- [WebSocket design](docs/design/websocket.md)

Planning and review material:

- [Roadmap](docs/roadmap.md)
- [Proposals](docs/proposals/README.md)
- [TLS policy and identity proposal](docs/proposals/tls-policy-and-identity.md)
- [TLS graceful shutdown proposal](docs/proposals/tls-graceful-shutdown.md)
- [Archived code quality audit](docs/archive/code-quality-audit.md)
- [Archived API audit](docs/archive/api-audit.md)

## Status

The current implementation includes HTTP route ergonomics, chunked responses,
request body policies, request body streaming, the generic HTTP upgrade hook,
server-side WebSocket handshake/framing, WebSocket sessions, byte-stream
adaptation, TLS stream and listener adapters, HTTP over TLS through listener
composition, HTTPS one-shot client requests, typed JSON request bodies,
multipart request streaming, runnable examples, and focused tests. Remaining TLS
hardening topics are tracked in the
TLS policy/identity and graceful shutdown proposals.

## License

MIT. See [`LICENSE`](LICENSE).
