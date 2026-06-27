# HTTP Client Proposal

Status: Draft

## Context

The project scope includes HTTP client primitives after the server milestones.
Dependency notes list libcurl as a constrained candidate for a high-level client
adapter.

## Current State

- Implemented: HTTP/1.1 server and shared HTTP vocabulary.
- Not implemented: HTTP client API, connection pooling, request sending,
  response streaming, redirects, or client TLS integration.

## Draft Scope

- Decide whether the first client is a direct uvpp-protocols client or a
  libcurl-based adapter.
- Keep server and shared HTTP vocabulary independent from libcurl.
- Follow the same explicit transport composition model as server-side protocols.

## Out Of Scope

- Browser-like fetch API.
- Full crawler behavior.
- HTTP/2 client support in the first client milestone.

## Source Documents

- [Protocol composition](../design/protocol-composition.md)
- [Dependency decisions](../design/dependency-decisions.md)
- [Project scope](../design/project-scope.md)
