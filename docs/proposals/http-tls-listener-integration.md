# HTTP TLS Listener Integration Proposal

Status: Draft

## Context

HTTP should not depend directly on TLS, but HTTP over TLS should be easy once
the TLS module exists.

## Current State

- Implemented: `http::server::listen(io::stream_listener)` and transport
  composition over `byte_stream`.
- Not implemented: TLS listener adapter and any HTTP convenience helper for TLS
  listeners.

## Draft Scope

- Ensure `http::server` accepts a TLS listener through the generic listener
  path.
- Consider an optional convenience helper only after the generic path works.
- Keep OpenSSL and TLS types out of the HTTP server core.

## Out Of Scope

- Making TLS a required HTTP dependency.
- HTTPS-specific routing model.
- Certificate management beyond TLS context configuration.

## Source Documents

- [HTTP server design](../design/http-server.md)
- [TLS support proposal](tls-support.md)
- [Protocol composition](../design/protocol-composition.md)
- [ADR 0002](../adr/0002-module-boundaries-and-dependency-direction.md)
