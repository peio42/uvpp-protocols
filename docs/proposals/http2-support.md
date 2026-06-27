# HTTP/2 Support Proposal

Status: Draft

## Context

HTTP/2 is accepted as future work, but it is not a mechanical extension of the
current HTTP/1.1 server. The dependency notes identify `libnghttp2` as the
preferred engine if HTTP/2 remains in this package.

## Current State

- Implemented: HTTP/1.1 server over `llhttp`.
- Not implemented: HTTP/2 framing, stream multiplexing, HPACK, ALPN selection,
  or HTTP/2 public/session model.

## Draft Scope

- Design a version-neutral HTTP layer between transport/session mechanics and
  the public HTTP model.
- Use `libnghttp2` behind a private adapter if the feature stays in scope.
- Preserve the shared HTTP request/response model where practical.
- Design connection and stream ownership before exposing public APIs.
- Ensure HTTP/1 abstractions do not assume one request at a time per
  connection.

## Out Of Scope

- HTTP/3 or QUIC.
- Replacing the HTTP/1 parser.
- Public backend selection.

## Source Documents

- [Roadmap](../roadmap.md)
- [Dependency decisions](../design/dependency-decisions.md)
- [HTTP server design](../design/http-server.md)
- [ADR 0004](../adr/0004-private-protocol-engines-and-no-public-backend-selector.md)
