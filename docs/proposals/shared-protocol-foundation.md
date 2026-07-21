# Shared Protocol Foundation Proposal

Status: Proposed for Milestone 7 protocol expansion foundations

## Context

Milestone 6 established the client-side path for URL parsing, DNS resolution,
TCP connection, TLS composition, HTTP/1.1 parsing/writing, streaming, pooling,
timeouts, cancellation, and proxy-aware routing decisions.

The next protocol families will repeat many of the same mechanisms:

- operation lifetime and exactly-once completion;
- cancellation across multi-phase workflows;
- phase timeouts and overall deadlines;
- read and write backpressure;
- byte-oriented framing;
- authentication material and secret redaction;
- URL scheme and endpoint handling;
- diagnostic error categories.

Those pieces should be extracted deliberately as protocols need them. The goal
is not a large generic framework. The goal is a small set of concrete building
blocks that make the second, third, and fourth protocol modules simpler and more
consistent than the first.

## Decision

Milestone 7 should define and begin extracting a reusable protocol substrate
below protocol-specific modules.

The first consumers should be:

- the WebSocket client, because it combines HTTP upgrade, transport handoff,
  masking, framing, backpressure, and cancellation;
- small service clients such as Redis, SMTP, syslog, or statsd, because they
  validate the substrate without requiring a large external dependency stack.

HTTP/2 should remain outside the immediate focus until stream ownership, pool
capacity, and flow control are clearer.

## Goals

- Provide reusable operation patterns for cancellation, timeout, and exactly-once
  completion.
- Provide bounded read/write queue helpers that express backpressure without
  hiding transport ownership.
- Provide small framing helpers for common byte protocols.
- Keep URL and endpoint expansion extensible for new schemes.
- Define credential containers and redaction rules before adding many auth-heavy
  protocols.
- Preserve explicit composition over `uvp::io::byte_stream`.
- Avoid leaking implementation libraries into public protocol APIs.

## Non-Goals

- A universal protocol engine.
- A coroutine runtime or scheduler replacement.
- A complete authentication framework.
- A complete parser-generator layer.
- Hiding protocol-specific state machines behind over-generic abstractions.
- Rewriting already-working HTTP/TLS/DNS code before a concrete consumer needs
  the extraction.

## Building Blocks

### Operation Lifetime

Common client workflows have this shape:

```text
resolve/connect/handshake/write/read
  -> complete exactly once
  -> cancel from any phase
  -> close or recycle transport according to protocol state
```

The foundation should provide reusable conventions or helpers for:

- completion guards;
- cancellation propagation;
- phase naming;
- close-on-failure behavior;
- user callback dispatch policy.

The helper should remain optional. Protocols with unusual lifetimes should be
able to use the conventions without inheriting from a heavy base class.

### Timeouts and Deadlines

Existing HTTP client timeouts are phase-specific. Future modules need the same
shape:

- DNS timeout;
- connect timeout;
- TLS handshake timeout;
- protocol handshake timeout;
- write or upload timeout;
- response/read idle timeout;
- overall request or session deadline.

The foundation should provide a small timer owner that supports:

- replacing the active phase timeout;
- stopping timers safely during completion;
- reporting the phase that timed out;
- composing a global deadline with phase timers.

### Backpressure

Reusable backpressure should cover:

- bounded outbound queues;
- `ready` / `backpressure` / `rejected` write results;
- drain callbacks;
- pause/resume for inbound stream delivery;
- clear rules for cancellation while paused or backpressured.

HTTP upload streaming already has a first version of this shape. Milestone 7
should decide what can be promoted without freezing HTTP-specific names.

### Framing

Many protocol families are byte-stream protocols with reusable framing styles:

- delimiter or line based: SMTP, Redis inline commands, syslog variants;
- length-prefix: many binary protocols;
- varint length-prefix: protobuf-style transports;
- chunked or segmented payloads: HTTP/1.1, WebSocket, application protocols;
- fixed header plus variable payload: MQTT, AMQP, Memcached binary.

The foundation should offer small parser/writer utilities where the mechanics
are shared, but keep protocol validation in protocol modules.

### Authentication Material

Several future modules will need credentials:

- plain username/password;
- bearer tokens;
- API keys;
- OAuth2 tokens;
- SASL mechanisms;
- TLS client certificates.

The first foundation step should be modest:

- credential value containers;
- explicit opt-in copying;
- redaction in diagnostics;
- helper APIs that do not accidentally log secrets.

Protocol-specific mechanisms such as SASL, OAuth2, LDAP bind, or AMQP auth can
then build on those containers.

### URL and Endpoint Expansion

The shared URL module is implemented and archived from Milestone 6. Future work
should extend scheme handling without making every module parse strings by hand.

Needed directions:

- scheme-specific default ports;
- module-owned scheme registration or mapping;
- endpoint normalization;
- service names where useful;
- clear separation between URL identity and transport route identity.

This also feeds outbound connector and proxy route work.

### Errors and Diagnostics

Each protocol should keep its own error category, but shared helpers should make
it easy to preserve source detail:

- transport error;
- TLS error;
- timeout phase;
- parser/framing error;
- authentication failure;
- remote protocol rejection.

Diagnostics should be useful without exposing secrets or backend-specific types
as mandatory public API.

## Milestone 7 Slice

Suggested first slice:

1. Audit HTTP client, WebSocket server, TLS, DNS, and IO for repeated lifetime
   patterns.
2. Define internal helper shapes for completion guards, timeout phase owners,
   and bounded write queues.
3. Use WebSocket client implementation to validate the shape.
4. Draft common framing helper boundaries, but implement only what the first
   consumer needs.
5. Define credential/redaction guidelines before adding auth-heavy protocols.
6. Update protocol proposals to say which shared pieces they expect to reuse.

## Tests

Add tests for shared helpers only when they own real behavior:

- exactly-once completion after cancel/timeout/success races;
- phase timeout replacement and stop behavior;
- bounded queue backpressure and drain;
- pause/resume edge cases;
- secret redaction in diagnostics.

Protocol modules should still own protocol-level tests.

## Source Documents

- [Protocol module portfolio](protocol-module-portfolio.md)
- [WebSocket client](websocket-client.md)
- [Outbound connector and proxy routes](outbound-connector-and-proxy-routes.md)
- [HTTP client flow control and deadlines](http-client-flow-control-and-deadlines.md)
- [Protocol composition](../design/protocol-composition.md)
- [Transport abstractions](../design/transport-abstractions.md)
