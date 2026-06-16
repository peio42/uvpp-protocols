# Module Architecture

## Layering

`uvpp-protocols` sits above uvpp:

```text
Application
  -> uvpp-protocols module, such as uvp::http or uvp::websocket
    -> uvpp C++20 handles, requests, buffers, errors, timers
      -> libuv
```

The lower layer owns OS event integration. The protocol layer owns protocol
state machines and application-facing convenience.

## Module Boundaries

Each module should define:

- public protocol objects;
- option structs;
- typed enums and result objects;
- module-specific errors;
- private parser/backend adapters under `detail/`;
- tests and examples that do not require unrelated modules.

Cross-module dependencies should be explicit. For example, `uvp::websocket`
depends on `uvp::http` upgrade support, while `uvp::http` should not depend on
WebSocket.

Protocol nesting should be modeled as composition over explicit transports, not
as cross-module inheritance. A higher-level protocol receives a stream-like
transport from the layer below it. For details and examples, see
[Protocol composition](protocol-composition.md).

## Session Ownership

Connection-oriented modules generally need a session object. The session owns
state that must survive callbacks:

- protocol parser;
- partially received message data;
- write queue;
- timeout timers;
- close state;
- per-connection user callbacks.

For server modules, the server should own accepted sessions by default. A
session remains alive until it is closed, timed out, or rejected during setup.
Advanced APIs may allow applications to take ownership later, but the default
should be safe and compact.

## Buffers and Backpressure

Modules should apply backpressure before memory grows without bound.

HTTP, WebSocket, SMTP, and similar stream protocols should have configurable
limits for:

- header bytes;
- body or message bytes;
- pending write bytes;
- request/message count per connection;
- idle and active request timeouts.

Response writes should be queued per session and flushed through uvpp stream
writes. A module may own write request objects internally because that is part
of the high-level abstraction. The queue must retain payload bytes until the
write completion callback runs.

## Timers

Protocol timeouts should use `uv::timer` owned by the server or session:

- idle connection timeout;
- request header timeout;
- request body timeout;
- graceful close timeout;
- ping/pong timeout for WebSocket.

Timers must be stopped and closed as part of session shutdown. Because uvpp
handle close is asynchronous, the session owner must keep timer wrappers alive
until close callbacks complete.

## Raw Interop

Expose raw or lower-level access only through named functions:

```cpp
auto remote = req.connection().remote_endpoint();
upgrade_req.accept(response, [](uvp::io::byte_stream stream) {
  uv::tcp* tcp = stream.tcp();
});
```

Any borrowed view returned by a protocol object must have callback-scoped or
object-scoped lifetime documented at the call site.

## Testing Strategy

Each module should have three layers of tests:

- parser/backend adapter tests with deterministic byte sequences;
- session tests using local uvpp TCP streams or in-memory test adapters if
  introduced;
- public API examples compiled as part of the test suite.

Network tests should bind to loopback and ephemeral ports where possible.
Timeout-heavy behavior should use short, controlled durations and avoid wall
clock assumptions beyond what libuv needs.
