# HTTP/3 and QUIC Support Proposal

Status: Draft

## Context

HTTP/3 is documented as possible future work, but it requires a deeper design
pass than HTTP/1.1 evolution. It would need `nghttp3` for HTTP/3 mapping and a
separate QUIC transport such as `ngtcp2`.

## Current State

- Implemented: no UDP, QUIC, or HTTP/3 support.
- Not implemented: QUIC ownership model, HTTP/3 transport integration, QPACK,
  or public HTTP/3 APIs.

## Draft Scope

- Design QUIC transport ownership before any HTTP/3 API is added.
- Decide how HTTP/3 would share or bypass any version-neutral HTTP layer.
- Evaluate `ngtcp2` plus `nghttp3` as private engines.
- Decide whether HTTP/3 belongs in this package or a separate companion module.

## Out Of Scope

- HTTP/2 support.
- General-purpose QUIC framework.
- Early public API commitments.

## Source Documents

- [Dependency decisions](../design/dependency-decisions.md)
- [Protocol composition](../design/protocol-composition.md)
- [ADR 0004](../adr/0004-private-protocol-engines-and-no-public-backend-selector.md)
