# Roadmap

## Milestone 0: Repository Foundation

- Create the public include layout under `include/uvpp/protocols`.
- Add CMake and Makefile conventions consistent with uvpp where practical.
- Add a minimal example and test scaffold.
- Use a mixed build strategy: header-only public vocabulary and small pure
  helpers; compiled code for stateful protocols, libuv integration, and external
  dependencies.
- Use `llhttp` as the HTTP/1 state machine backend under `detail/`.
- Do not expose an HTTP/1 backend selector unless a second parser becomes a real
  supported maintenance target.
- Keep HTTP/2 as a future roadmap item without configuring or linking
  `libnghttp2` in early milestones.
- Keep external HTTP dependencies as synchronous state machines only; they do
  not own sockets, loops, timers, output buffers, or the public model.
- Make examples build against `uvpp::uvpp`, fetching uvpp when it is not already
  installed, so they link through uvpp to libuv.

## Milestone 1: HTTP/1.1 Server MVP

- Shared `uvp::io::byte_stream` and `uvp::io::stream_listener`
  contracts.
- TCP and Unix socket listener adapters.
- General `uvp::url` wrapper backed by `ada-url`, at least enough for HTTP
  request targets and client-facing URL parsing.
- `uvp::http::server` with `get`, `post`, generic `route`, and `listen`.
- Exact path routing.
- Request method, target, path, query, headers, and buffered body.
- Response status, headers, `text`, `bytes`, `json`, and `end`.
- Safe default limits for headers, bodies, pending writes, and timeouts.
- Keep-alive support for sequential requests on one connection.
- Default error responses for parser errors, oversized payloads, missing routes,
  and uncaught handler exceptions.

## Milestone 2: HTTP Ergonomics

- Route parameters and wildcard segments.
- Custom `not_found` and error handlers.
- Deferred responses for application-level async work.
- Streaming/chunked responses.
- Request body streaming.
- Access to connection metadata and underlying `uvp::io::byte_stream`.
- Examples for admin endpoints, local JSON API, and log streaming.

## Milestone 3: WebSocket

- HTTP upgrade hook.
- WebSocket handshake.
- Text, binary, ping, pong, and close frames.
- Per-session callbacks.
- Configurable message size and ping/pong timeout.
- Backpressure-aware send queue.

## Milestone 4: TLS

- TLS context and stream adapter.
- Server-side TLS listener integration for HTTP.
- Client-side TLS connector for future HTTP/SMTP clients.
- Backend boundary that keeps OpenSSL, mbedTLS, or another provider out of the
  generic public API as much as possible.

## Milestone 5+: Additional Protocols

Good follow-up modules:

- `uvp::smtp`: SMTP client and minimal server sessions.
- `uvp::sse`: Server-Sent Events helper on top of HTTP responses; see
  [Server-Sent Events design](sse.md).
- `uvp::multipart`: streaming and bounded form-data parser for HTTP uploads;
  see [Multipart design](multipart.md).
- `uvp::redis`: RESP client for simple service integrations.
- `uvp::mqtt`: MQTT sessions for event-driven messaging.
- `uvp::proxy`: CONNECT and reverse-proxy helpers.

Each new module should start with a design note before implementation. The note
should identify parser/backend dependencies, ownership model, timeouts,
backpressure strategy, and how the module composes with existing modules.

## Future HTTP Refinements

- Charset handling for `uvp::http::body::text`, including optional validation
  and transcoding policies once the desired scope is clear.
