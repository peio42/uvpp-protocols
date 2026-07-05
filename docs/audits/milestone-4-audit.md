# Milestone 4 Audit — Code Quality, API Consistency & Defects

- **Scope:** Full library audit performed against `origin/milestone-4` at commit `98792d7`
  (typed JSON bodies, multipart handling, Server-Sent Events, static file helper — all four
  "Current focus" items in `docs/roadmap.md` are implemented on this commit).
- **Method:** Read-only static review (no build toolchain available in the audit environment).
  A dedicated detached worktree was used to inspect the true `milestone-4` tip, since the
  working branch for this review was a few commits behind it.
- **Out of scope:** Runtime/perf profiling, fuzzing, compiler-warning analysis (no toolchain).

## 1. Follow-up on the previous JSON/multipart audit

An earlier, narrower audit of the typed-JSON-body and multipart policies found four issues.
They were re-verified against the current `milestone-4` tip:

| # | Original issue | Status |
|---|---|---|
| 1 | `multipart_stream` with no `on_error()` handler + `res.defer()` already called could leave the connection hanging with no response | **Fixed** — `router.hpp` now guards with `!multipart.has_error_handler() && !res.ended() && !res.streaming()` and sends `500 Internal Server Error`; covered by a dedicated integration test |
| 2 | `multipart_part::text()` / `discard()` silently no-op if a consumption mode was already chosen | **Fixed** — now reports `multipart part consumer already selected` through the `on_error` handler; covered by test |
| 3 | `wrap_json_handler` only caught `uvp::json::exception`, not arbitrary exceptions thrown by a custom `from_json` | **Fixed, and better than originally documented** — it now catches any `std::exception` and maps it to `422 Unprocessable Content`. **Documentation was not updated**: `docs/proposals/typed-json-body-policy.md` still states only `uvp::json::exception` is caught |
| 4 | Part-header parsing duplicated between the sync (`multipart_form`) and streaming (`multipart_stream`) paths | **Fixed** — both paths now share a single `parse_part_header_block()` |

New, smaller issues found while re-checking this area:

- **Medium** — The `on_error` guard in `router.hpp` (~line 1029) checks `!res.ended() && !res.streaming()` but not `!res.deferred()` explicitly. It happens to work today because `defer()` doesn't set those flags, but the check doesn't literally match the intent described in `multipart-handling.md`. Recommend adding an explicit `!res.deferred()` term, or clarifying the doc.
- **Low** — On a size-limit failure in text-mode part consumption, a chunk may already be partially buffered before `fail()` propagates; the `on_error` callback has no way to know how much data was buffered. Documentation-only fix is sufficient.
- **Low** — Nothing in the API stops an application from stashing a `multipart_part` reference and trying to consume it asynchronously outside the `on_part` callback; the parser expects synchronous consumption and will fail with `multipart part was not consumed` if it isn't. Worth a one-line doc/comment clarification.
- **Test gap** — No test explicitly exercises a custom `from_json` throwing a non-`uvp::json::exception` type, even though the code path supports it.

## 2. HTTP core (router / server / request / response)

Overall: consistent fluent-builder style, correct `&`/`&&` overload pairs, naming follows
`docs/design/api-principles.md` (trailing underscore for storage, canonical names for accessors).

- **High** — `route_options::body_timeout_` defaults to `0`, but the fluent setter rejects `0`
  as an invalid value. This means the *default* state is not reproducible through the public
  setter, which is a real inconsistency for API polish. Recommend giving the default a
  well-defined non-zero value, or explicitly documenting `0` as "inherit server default" and
  allowing the setter to accept it.
- **High** — `deferred_response::status()` / `header()` silently do nothing if the underlying
  response is no longer active (`lock_active()` returns null). There is no way for calling code
  to distinguish "connection already closed" from "operation applied". Consider returning a
  boolean/status or exposing `active()` as the required check before each call, with the
  contract documented at the call site.
- **Medium** — Reported `stream_body_bytes` (used by response hooks / metrics) counts only
  payload bytes, not the chunked-encoding framing overhead (hex size + CRLFs) when
  `transfer-encoding: chunked` is used. Diagnostics/metrics based on this figure will
  under-report actual bytes on the wire.
- **Medium** — `headers::set()` and `route_params::set()` both use linear scans; fine for the
  typical size of these collections, but undocumented — a handler receiving a very large
  header count would incur O(n²) insertion cost. Worth a note in the docs or an upper bound.
- **Low** — `hex_value()` is duplicated verbatim between `src/http/request.cpp` and
  `src/http/route_path.cpp`. Should be a shared helper.
- **Low** — `std::string_view` returned from `request::path()/query()/header()` is only valid
  for the lifetime of the handler call; this is stated in `docs/user/http-server.md` but not
  reinforced with a comment at the declaration site in `request.hpp`.

> Note: an initial pass flagged a possible integer-overflow/backpressure-accounting issue in
> `pending_write_bytes_`. On closer inspection the decrement is already guarded with
> `pending_write_bytes_ -= std::min(pending_write_bytes_, ...)`, so this is **not** an actual
> defect and has been excluded from the findings above.

## 3. Server-Sent Events (`response.hpp` / `response.cpp`)

The SSE API (`sse_stream`, `sse_options`, `sse_event`) closely follows `docs/proposals/sse-support.md`
and composes cleanly on top of `streaming_response`.

- **High** — `sse_stream::retry(std::chrono::milliseconds)` correctly rejects non-positive
  durations, but does not guard against an extreme value (`count()` close to
  `int64` max) before formatting it with `std::to_string`. Low real-world likelihood, but cheap
  to guard.
- **Medium** — No validation that `sse_event::data` is valid UTF-8; likely acceptable since
  the wire format tolerates arbitrary bytes per line, but worth a one-line doc note either way.
- **Test gap** — No explicit test for an empty `data` field (should produce `data:\n\n`), for
  data ending in a lone `\r`, or for data starting with a newline. (Traced the implementation:
  `append_split_sse_lines` correctly falls through to append an empty `data:` line when the
  input is empty, so behavior is correct — but it deserves a regression test since the logic is
  easy to break silently.)

## 4. Static file helper (`static_files.hpp` / `static_files.cpp`)

New in this milestone. Security-conscious by design: explicit `hidden_file_policy` and
`symlink_policy`, path canonicalization, configurable ETag/Last-Modified/Cache-Control/nosniff.

- **High** — Potential TOCTOU race in symlink handling: `resolve_existing_target` checks
  symlinks component-by-component before the final `std::filesystem::canonical()` call. Between
  the per-component check and the final resolution, a symlink could theoretically be swapped by
  a concurrent filesystem writer, allowing an escape from `root`. Recommend re-validating that
  the final canonical path is still contained under the canonical root right before opening the
  file (belt-and-suspenders), even though the proposal explicitly disclaims being a full sandbox.
- **High** — Permission-denied errors are distinguished from "not found" for the primary target,
  but the same care is not applied consistently when resolving an index file — a permission
  error there can surface as `404` instead of `500`.
- **Medium** — Weak ETag is derived from size + mtime only; acceptable per the proposal, but
  worth documenting the known staleness window on filesystems with coarse mtime resolution or
  rapid file replacement.
- **Low** — `pattern_segments()` uses `offset <= pattern.size()` where `<` would be the more
  natural loop bound; currently masked by a downstream `end > offset` check, so not an active
  bug, but worth tightening for clarity.
- **Test gap** — No test for `symlink_policy::follow_within_root` with an out-of-root symlink,
  and no test for a permission-denied file (500) as distinct from a missing file (404).

## 5. WebSocket (`session.hpp` / `session.cpp` / `detail/handshake.cpp`)

Frame parsing correctly implements masking, extended length encoding, and control-frame
constraints per RFC 6455.

- **High** — No timeout is enforced after the server echoes a close frame it received from the
  client: if the client never actually closes the TCP connection afterward, the session/socket
  lingers indefinitely. Recommend a short close-handshake timer, consistent with the "Timers"
  guidance in `docs/design/module-architecture.md`.
- **High** — Error handling is inconsistent between read and write failures: a read error does
  not close the underlying transport (`fail(code, false)`), while a write error always does.
  This asymmetry can leave a socket half-open with no further progress possible.
- **Medium** — HTTP header lookups for the `Upgrade` / `Connection` handshake headers are
  case-sensitive, while HTTP header *names* are case-insensitive per RFC 7230. A client sending
  `Upgrade:` (correct casing) rather than `upgrade:` could be rejected. Worth confirming header
  storage/lookup normalizes case consistently across the whole HTTP layer, not just here.
- **Medium** — Close codes received from the peer are cast directly from the raw 16-bit value
  with no validation against the RFC 6455 allowed ranges (1000–1011, 3000–4999).
- **Low** — `accept_detached()` is not marked `[[nodiscard]]`, unlike `accept()`, despite the
  design docs recommending the distinction.
- **Test gap** — The WebSocket test suite (`tests/websocket_test.cpp`) only has 3 cases and
  does not cover: unmasked client frames, reserved bits set, oversized/fragmented control
  frames, or invalid close codes. Given this module handles untrusted network input, test
  coverage should be expanded before relying on it in adversarial conditions.

> Note: an initial pass flagged the reserved-bits (RSV1–3) check as accepting a masked frame
> with reserved bits set. On closer reading, the guard is
> `if ((first & 0x70U) != 0U || !masked) { ... }` — this is a **logical OR**, so a masked frame
> with any reserved bit set still trips the first branch and is correctly rejected. This is
> **not** a defect; it has been excluded from the findings above.

## 6. Overall assessment

The library shows real API discipline: the fluent-builder conventions, ownership rules, and
naming guidance in `docs/design/api-principles.md` are consistently followed across HTTP,
SSE, static files, and WebSocket. Security-sensitive surfaces (multipart limits, static file
path resolution) show clear intentional design rather than afterthoughts.

Recommended priorities before production use, roughly in order:

1. WebSocket close-handshake timeout and read/write error-handling symmetry.
2. Case-insensitive header lookup for the WebSocket upgrade handshake.
3. Static file symlink TOCTOU hardening and consistent permission-error mapping for index files.
4. `route_options::body_timeout` default/setter inconsistency.
5. Expand WebSocket protocol-compliance tests (malformed/adversarial frames).
6. Update `typed-json-body-policy.md` to reflect the broader exception catching already
   implemented.

No blocking defects were found; the items above are hardening and consistency improvements
rather than correctness failures in the common/happy-path usage the test suite already covers.
