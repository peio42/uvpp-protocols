# Shared URL Module Proposal

Status: Draft

## Context

Dependency decisions accept `ada-url` for a shared URL module. Several future
features need URL parsing, including HTTP client helpers and configuration
strings.

## Current State

- Implemented: request target, path, and query parsing inside HTTP request
  handling.
- Not implemented: public `uvp::url`, `ada-url` integration, URL-based
  endpoint helpers, and protocol-specific URL policy.

## Draft Scope

- Add a shared public URL wrapper where WHATWG URL semantics are appropriate.
- Keep endpoint APIs typed where URLs are ambiguous, especially Unix sockets.
- Decide where stricter RFC-specific parsing is required.

## Out Of Scope

- Replacing typed endpoint overloads.
- URL routing DSL.
- Protocol-specific validation for every module.

## Source Documents

- [Dependency decisions](../design/dependency-decisions.md)
- [Transport abstractions](../design/transport-abstractions.md)
