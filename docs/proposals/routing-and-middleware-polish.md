# Routing and Middleware Polish Proposal

Status: Draft

## Context

The router already supports route parameters, wildcard tail segments,
method-aware matching, `not_found`, and `on_error`. The roadmap still tracks
Milestone 3 as routing and middleware polish.

## Current State

- Implemented: route parameters, wildcard tails, method-aware `405`, `HEAD`
  fallback, automatic `OPTIONS`, `not_found`, and `on_error`.
- Not implemented: middleware chains, route groups, and hooks attached to route
  groups or subtrees.

## Draft Scope

- Define middleware execution order.
- Decide whether middleware attaches globally, per route group, or both.
- Define how middleware can continue, short-circuit, or produce a response.
- Preserve the existing router trie and method-aware behavior.

## Out Of Scope

- Authentication or authorization framework.
- Dependency injection container.
- Application-level validation framework.

## Source Documents

- [Roadmap](../roadmap.md)
- [HTTP server design](../design/http-server.md)
- [API audit](../audits/api.md)
