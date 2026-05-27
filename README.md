# uvpp-protocols

Higher-level protocol modules for [`uvpp`](https://github.com/peio42/uvpp).

`uvpp-protocols` is intended to provide reusable protocol building blocks on top
of uvpp's event-based libuv wrapper. The goal is to let applications keep the
explicit libuv/uvpp event-loop model while avoiding repeated implementations of
common protocols such as HTTP, WebSocket, TLS, SMTP, and MQTT.

This repository is currently in the design phase. The first planned module is
an HTTP/1.1 server.

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

Configuration APIs should use plain value structs with fluent builders:

```cpp
uv::http::server srv(
  loop,
  uv::http::server_options::builder()
    .max_header_bytes(32 * 1024)
    .max_body_bytes(10 * 1024 * 1024)
    .idle_timeout(2min)
    .server_header(false)
    .build());
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

## Design Documents

The design notes live in [`docs/design`](docs/design):

- [Project scope](docs/design/project-scope.md)
- [API principles](docs/design/api-principles.md)
- [Module architecture](docs/design/module-architecture.md)
- [Protocol composition](docs/design/protocol-composition.md)
- [HTTP server design](docs/design/http-server.md)
- [Roadmap](docs/design/roadmap.md)

## Status

No public implementation is available yet. The current repository content is a
design baseline for the first implementation pass.

## License

License to be decided.

