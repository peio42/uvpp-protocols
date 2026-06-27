# Route Groups and Hooks Proposal

Status: Draft

## Context

The HTTP router uses a segment trie and already supports route parameters,
wildcards, method-aware matching, `not_found`, and `on_error`. The design notes
reserve future route ergonomics for route groups and hooks attached to groups or
subtrees.

## Current State

- Implemented: route parameters, wildcard tails, method-aware matching,
  `not_found`, and `on_error`.
- Not implemented: route groups, shared prefixes, group-level hooks, and
  subtree-level hooks.

## Draft Scope

- Define route group construction and prefix behavior.
- Decide how group hooks relate to global middleware.
- Preserve route matching priority and validation rules.
- Keep route registration readable for small services.

## Out Of Scope

- Full application framework.
- Dependency injection.
- Authorization policy language.

## Source Documents

- [HTTP server design](../design/http-server.md)
- [Routing and middleware polish](routing-and-middleware-polish.md)
