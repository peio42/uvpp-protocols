# uvpp-protocols

Higher-level protocol modules for [`uvpp`](https://github.com/peio42/uvpp).

`uvpp-protocols` is intended to provide reusable protocol building blocks on top
of uvpp's event-based libuv wrapper. The goal is to let applications keep the
explicit libuv/uvpp event-loop model while avoiding repeated implementations of
common protocols such as HTTP, WebSocket, TLS, SMTP, and MQTT.

This repository is currently in the design phase. The first planned module is
an HTTP/1.1 server.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The default build compiles the library, a structure test, and the first example.
The example requires `uvpp::uvpp`; CMake first tries `find_package(uvpp CONFIG)`
and then fetches [`uvpp`](https://github.com/peio42/uvpp) when needed. Because
`uvpp::uvpp` links `LibUV::LibUV` and `Threads::Threads`, the example is built
against libuv instead of a local placeholder. Real network listening is reserved
for the HTTP MVP milestone.

## Target HTTP API

```cpp
#include <uvpp/protocols/http.hpp>

uv::loop loop;

uv::http::server srv(loop);

srv.get("/health", [](uv::http::request& req, uv::http::response& res) {
  res.json({{"status", "ok"}});
});

srv.get("/logs", [](uv::http::request& req, uv::http::response& res) {
  res.text(read_recent_logs());
});

srv.post("/config", [](uv::http::request& req, uv::http::response& res) {
  auto cfg = parse_config(req.body());
  apply_config(cfg);
  res.status(204).end();
});

srv.listen("127.0.0.1", 8080);
loop.run();
```

Configuration APIs should follow uvpp's fluent value-object style, without a
separate `.build()` step:

```cpp
uv::http::server srv(
  loop,
  uv::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_body_bytes(10 * 1024 * 1024)
    .idle_timeout(2min)
    .server_header(false));
```

## Planned Modules

- `uv::http`: HTTP/1.1 server first, then client primitives.
- `uv::websocket`: WebSocket server/client sessions built on HTTP upgrade.
- `uv::tls`: TLS stream adapter over uvpp streams.
- `uv::smtp`: SMTP client and minimal server/session primitives.
- `uv::sse`: Server-Sent Events helper on top of HTTP responses.
- `uv::multipart`: multipart/form-data parser and streaming upload helpers.
- `uv::mqtt`: MQTT sessions over TCP, TLS, or WebSocket.
- `uv::proxy`: CONNECT and reverse-proxy helpers.

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

## Dependency Policy

The project uses a mixed build strategy:

- header-only for public vocabulary types and small pure helpers;
- compiled sources for anything that owns state, talks to libuv, or wraps an
  external dependency.

The HTTP/1 parser backend is `llhttp`, integrated behind `src/http/detail` as a
synchronous state machine. When `llhttp` is fetched by CMake, its C sources are
compiled into `uvpp-protocols` rather than installed as a separate package.
HTTP/2 remains in the roadmap, but early milestones do not configure or link
`libnghttp2`. External HTTP dependencies are allowed only under `detail/`. They
never own the socket, the loop, timers, output buffers, or the public model.

Current CMake dependency options:

```sh
cmake -S . -B build -DUVPP_PROTOCOLS_FETCH_UVPP=ON
cmake -S . -B build -DUVPP_PROTOCOLS_FETCH_LLHTTP=ON
cmake -S . -B build -DUVPP_PROTOCOLS_LLHTTP_SOURCE_DIR=/path/to/llhttp
```

`llhttp` is not a replaceable backend. It is the HTTP/1 parser used by the
library. The source-directory option exists for offline or vendored builds.

## Design Documents

The design notes live in [`docs/design`](docs/design):

- [Project scope](docs/design/project-scope.md)
- [API principles](docs/design/api-principles.md)
- [Module architecture](docs/design/module-architecture.md)
- [Dependency decisions](docs/design/dependency-decisions.md)
- [Protocol composition](docs/design/protocol-composition.md)
- [HTTP server design](docs/design/http-server.md)
- [Roadmap](docs/design/roadmap.md)

## Status

Milestone 0 foundation is available: repository structure, CMake packaging,
public HTTP vocabulary, a header-only router skeleton, initial `llhttp`-backed
HTTP/1 parser adapter, uvpp-backed example wiring, dependency backend hooks,
and one structure test.

## License

MIT. See [`LICENSE`](LICENSE).
