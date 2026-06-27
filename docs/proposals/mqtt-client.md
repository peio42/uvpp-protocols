# MQTT Client Proposal

Status: Draft

## Context

The roadmap lists MQTT client as later work. The composition design uses MQTT
as an example of a byte-oriented protocol that can run over TCP, TLS, or
WebSocket.

## Current State

- Implemented: WebSocket byte-stream adaptation.
- Not implemented: MQTT packet parser, client session, connect options,
  keep-alive, subscriptions, QoS state, or dependency strategy.

## Draft Scope

- Define an MQTT client session over `byte_stream`.
- Support TCP/TLS/WebSocket transports through explicit composition.
- Decide whether to implement MQTT packets directly or use a library.
- Keep broker/server behavior out of this package.

## Out Of Scope

- MQTT broker/server.
- Full persistence for QoS state in the first client milestone.
- High-level application event bus.

## Source Documents

- [Roadmap](../roadmap.md)
- [Protocol composition](../design/protocol-composition.md)
- [Dependency decisions](../design/dependency-decisions.md)
