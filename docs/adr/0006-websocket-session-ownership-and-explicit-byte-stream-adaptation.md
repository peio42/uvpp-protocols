# ADR 0006: WebSocket Session Ownership and Explicit Byte-Stream Adaptation

Status: Accepted

Date: 2026-06-27

## Context

WebSocket starts as an HTTP upgrade, but after the upgrade it is not just a raw
byte stream. It has message boundaries, text and binary frame types,
fragmentation, masking rules, ping/pong, close codes, close reasons,
subprotocol negotiation, and a protocol-specific send queue.

Some protocols may still want to run over WebSocket as an ordered binary byte
transport. That mapping is useful, but it discards WebSocket-specific semantics
and should not be the default representation.

## Decision

`uvp::websocket::accept()` returns a `[[nodiscard]]` move-only
`uvp::websocket::session`. The session is the WebSocket protocol owner and
keeps frame parsing, message assembly, callbacks, close state, limits, and
queued writes together.

Callback-only endpoints that intentionally do not keep an owning handle must
call `uvp::websocket::accept_detached()` explicitly.

Byte-stream-oriented protocols must opt into an explicit adaptation path, such
as `std::move(session).into_byte_stream()` or the convenience helper
`accept_byte_stream()`. The byte-stream adapter maps outbound writes to binary
WebSocket frames and delivers inbound binary messages as ordered bytes.

## Consequences

- Applications that need WebSocket semantics receive a WebSocket session, not a
  lossy byte-stream abstraction.
- Fire-and-forget lifetime is visible at the call site through
  `accept_detached()`.
- Protocols such as MQTT can still run over WebSocket through an explicit
  binary byte-stream adapter.
- Text frames, close metadata, ping/pong visibility, and subprotocol details
  remain available only to code that keeps the WebSocket session abstraction.

## References

- [WebSocket design](../design/websocket.md)
- [Protocol composition](../design/protocol-composition.md)
- [User WebSocket documentation](../user/websocket.md)
