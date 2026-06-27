# Static File Helper Proposal

Status: Draft

## Context

The HTTP server design lists a static file helper as future work. No public API
or implementation exists yet.

## Current State

- Implemented: route handlers can manually read files and write responses.
- Not implemented: a first-class static file helper.

## Draft Scope

- Serve files from an explicit directory root.
- Prevent path traversal outside the configured root.
- Decide default behavior for indexes, hidden files, content types, and cache
  headers.
- Integrate with existing response writing and error handling.

## Out Of Scope

- Full asset pipeline.
- Directory listing by default.
- Template rendering.

## Source Documents

- [HTTP server design](../design/http-server.md)
