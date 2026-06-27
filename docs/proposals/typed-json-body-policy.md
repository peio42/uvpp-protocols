# Typed JSON Body Policy Proposal

Status: Draft

## Context

HTTP body policies leave room for an explicit `body::json<T>` request policy.
Responses already expose `response::json()` and `uvp::json` is available as the
project JSON type.

## Current State

- Implemented: `body::bytes`, `body::text`, `body::stream`, `response::json`,
  and the `uvp::json` alias.
- Not implemented: `body::json<T>` request parsing and typed conversion.

## Draft Scope

- Parse JSON request bodies through `uvp::json`.
- Convert typed bodies through `from_json` customization.
- Define parse error and validation error response behavior.
- Keep typed JSON policies explicit at route declaration time.

## Out Of Scope

- General schema validation framework.
- Content negotiation.
- Non-JSON body binding.

## Source Documents

- [HTTP server design](../design/http-server.md)
- [ADR 0005](../adr/0005-explicit-http-body-policies.md)
