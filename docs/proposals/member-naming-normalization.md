# Member Naming Normalization

Status: Draft

## Context

The project currently mixes two conventions for internal state members:

- public-facing types generally use private storage with a trailing underscore,
  such as `request::path_`, `server_options::max_body_bytes_`, and
  `server::options_`;
- some private implementation states use unsuffixed fields, such as
  `websocket::session::state::closed`, `writes`, and `read_buffer`;
- other private implementation states already use suffixed fields, such as
  `server::impl::session::closed_`, `writes_`, and `parser_`.

The stable rule is now documented in
[API principles](../design/api-principles.md): storage fields in public types
and behavior-owning private implementation types should use a trailing
underscore. Plain aggregate-style records may keep unsuffixed fields.

## Goals

- Make member naming predictable across HTTP, WebSocket, and IO internals.
- Preserve public API behavior and signatures.
- Avoid adding getters/setters to private implementation code unless they
  protect an invariant or simplify ownership.
- Keep the migration mechanical and easy to review.

## Non-Goals

- No broad redesign of HTTP or WebSocket session state.
- No extraction of write queues or other abstractions as part of this cleanup.
- No public API renames beyond already documented storage/accessor rules.
- No formatting churn in unrelated code.

## Proposed Rule

Use trailing underscores for non-static data members when the type:

- is public and exposes behavior through methods;
- owns resources, callbacks, queues, timers, protocol parser state, or lifecycle
  flags;
- has methods that read/write several fields as one invariant boundary.

Unsuffixed fields remain acceptable for simple value records, for example:

- parser events and snapshots;
- POD-like return objects;
- short-lived local structs with no behavior beyond construction.

Do not introduce getters/setters simply to access private fields from methods
of the same implementation type. Direct access is clearer within the same
invariant boundary.

## Candidate Scope

Normalize these areas first:

- `src/websocket/session.cpp`: `session::state` fields such as `options`,
  `stream`, `read_buffer`, `writes`, `pending_write_bytes`, `writing`, `closed`,
  `close_sent`, `fragmented_opcode`, `fragmented_message`, `byte_mode`,
  `byte_read`, and `keep_alive`.
- `src/http/response.cpp`: `detail::response_state` fields such as
  `status_code`, `headers`, `body`, `ended`, `deferred`, `streaming`,
  `headers_committed`, `cancelled`, and callbacks.
- `src/io/transport.cpp`: private model fields that already mostly use suffixes
  should be reviewed for consistency only.

Leave these as records unless a separate change makes them behavior-owning:

- `http::router::match_result`;
- `http::router::fallback_result`;
- `http::request_snapshot`;
- `http::response_info`;
- `detail::http1_event`;
- `detail::http1_message`.

## Migration Plan

1. Normalize `websocket::session::state` in one patch.
   This is the noisiest area because most field accesses are unsuffixed.
   Keep the patch mechanical: rename fields only, with no behavior changes.

2. Normalize `detail::response_state` in one patch.
   This state has fewer call sites but sits behind `response`,
   `deferred_response`, and `streaming_response`, so it deserves a focused
   review.

3. Review IO transport models and HTTP internals for remaining behavior-owning
   unsuffixed members.
   Rename only where the rule clearly applies.

4. Update this proposal with completed scope and archive it when all planned
   renames are done.

## Verification

Each mechanical rename patch should run:

```sh
make test
```

Run `make test-all` after the final cleanup patch or if a rename touches shared
public headers.

## Audit Link

This proposal addresses the code-quality audit finding:

`multiple | Convention d'underscore member incohérente`
