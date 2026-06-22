# uvpp-protocols Design Notes

`uvpp-protocols` is a companion project for
[`uvpp`](https://github.com/peio42/uvpp). It provides higher-level protocol
modules built on top of uvpp's event-based libuv wrapper, so applications can
use libuv without rebuilding common application protocols from scratch.

The first target module is an HTTP server API:

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

## Documents

- [Project scope](project-scope.md): goals, non-goals, and candidate protocol
  modules.
- [API principles](api-principles.md): common public API rules across modules.
- [Module architecture](module-architecture.md): how protocol modules should sit
  on top of uvpp handles, streams, buffers, callbacks, and errors.
- [Dependency decisions](dependency-decisions.md): accepted and candidate
  low-level libraries.
- [Protocol composition](protocol-composition.md): how nested protocols should
  be presented and connected.
- [Transport abstractions](transport-abstractions.md): stream listeners and
  byte streams for TCP, Unix sockets, TLS, and future protocols.
- [HTTP server design](http-server.md): first implementation target.
- [Server-Sent Events design](sse.md): response streaming helper for
  `text/event-stream` endpoints.
- [Multipart design](multipart.md): streaming and bounded form-data upload
  policies for HTTP.
- [WebSocket design](websocket.md): HTTP upgrade integration, session
  ownership, and protocols over WebSocket.
- [TLS design](tls.md): reusable TLS stream adaptation over `uvp::io`,
  OpenSSL backend boundaries, STARTTLS, ALPN, SNI, and shutdown.
- [Roadmap](roadmap.md): suggested implementation order and milestones.

## Design Posture

`uvpp-protocols` should feel like a C++20 protocol library that happens to run
on libuv through uvpp. It should keep the event loop visible, respect uvpp's
explicit lifetime model, and avoid hiding asynchronous work behind misleading
RAII.

The protocol modules may own higher-level operation state where that is the
point of the abstraction. For example, `uvp::http::server` may own accepted
connection sessions, parser state, route tables, and queued response writes.
That is different from uvpp's low-level stream API, where users intentionally
provide request objects and buffers. The higher-level ownership must still be
documented and observable through clear API names.
