# WebSocket Client Proposal

Status: Proposed for Milestone 7 protocol expansion foundations

## Context

Server-side WebSocket support is implemented. The broader project scope and
composition design leave room for WebSocket client sessions.

## Current State

- Implemented: server-side WebSocket handshake, framing, sessions,
  `accept_detached`, and byte-stream adaptation.
- Not implemented: WebSocket client handshake, client options, URL connection
  helpers, client-side masking writes, and client connection lifecycle.

## Draft Scope

- Reuse the existing WebSocket session and framing model where practical.
- Add client connection and handshake APIs.
- Preserve explicit transport ownership and URL-to-transport expansion.
- Use the client implementation to validate shared cancellation, timeout,
  framing, and backpressure primitives from
  [Shared protocol foundation](shared-protocol-foundation.md).

## Out Of Scope

- Browser API compatibility.
- Automatic reconnect.
- High-level subscription framework.

## Source Documents

- [WebSocket design](../design/websocket.md)
- [Protocol composition](../design/protocol-composition.md)
- [Shared protocol foundation](shared-protocol-foundation.md)
- [User WebSocket documentation](../user/websocket.md)
