# ADR 0002: Module Boundaries and Dependency Direction

Status: Accepted

Date: 2026-06-27

## Context

`uvpp-protocols` is expected to grow beyond HTTP into WebSocket, TLS, SMTP,
MQTT, database adapters, and other protocols. If each module freely depends on
the others, the library becomes difficult to package, test, and understand.

Some protocols naturally build on another protocol. WebSocket uses the HTTP
upgrade path. TLS wraps byte streams and can sit below HTTP, SMTP, or MQTT.
Those relationships should be explicit without turning lower-level modules into
owners of higher-level protocol behavior.

## Decision

Each protocol module owns its own public types, options, errors, implementation
details, tests, and examples. Cross-module dependencies must be explicit and
oriented toward the layer that actually needs the lower-level capability.

The preferred dependency direction is:

```text
uvp::tls        -> uvpp
uvp::http       -> uvpp
uvp::websocket  -> uvp::http upgrade API, uvpp
uvp::mqtt       -> shared byte stream API
```

HTTP must not depend on TLS or WebSocket. TLS must not depend on HTTP.
WebSocket depends on the generic HTTP upgrade API, not on private HTTP server
internals.

Convenience headers may compose modules, but protocol logic and ownership stay
in the canonical modules.

## Consequences

- Applications can consume one module without linking unrelated protocol
  support whenever packaging is split enough to allow it.
- HTTP exposes generic extension points such as upgrade hooks instead of
  hard-coded WebSocket support.
- TLS can support HTTP, SMTP, MQTT, STARTTLS, and custom protocols through the
  same transport boundary.
- Convenience APIs must delegate to the same composition paths as the explicit
  APIs.

## References

- [Module architecture](../design/module-architecture.md)
- [Protocol composition](../design/protocol-composition.md)
- [WebSocket design](../design/websocket.md)
- [TLS support proposal](../proposals/tls-support.md)
