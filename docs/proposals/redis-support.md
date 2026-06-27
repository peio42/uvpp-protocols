# Redis Support Proposal

Status: Draft

## Context

Redis is listed as a future protocol module for simple service integration.

## Current State

- Implemented: no Redis module.
- Not implemented: RESP parser, client/session API, connection lifecycle,
  command/result model, pipelining, or TLS composition.

## Draft Scope

- Define a RESP client/session API.
- Decide command result ownership and callback model.
- Compose over `byte_stream` so TCP and TLS can share the same session logic.

## Out Of Scope

- Full Redis cluster client.
- Persistence or embedded Redis behavior.
- ORM-like data mapping.

## Source Documents

- [Project scope](../design/project-scope.md)
- [Protocol composition](../design/protocol-composition.md)
