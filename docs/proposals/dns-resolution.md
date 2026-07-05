# DNS Resolution Proposal

Status: Draft, not implemented

## Context

Client-side protocols need asynchronous name resolution before they can connect.
HTTP client work should not hide DNS behind HTTP-specific code because the same
capability will be useful for SMTP, Redis, MQTT, database adapters, and any
future outbound transport helper.

DNS resolution should therefore live as a small reusable foundation near the IO
transport layer.

## Current State

- Implemented: typed IO endpoints and byte-stream/listener abstractions.
- Not implemented: public resolver API, async `getaddrinfo` wrapper, resolver
  cancellation policy, Happy Eyeballs, DNS timeout handling, or reusable
  outbound connect helper.

## Goals

- Resolve host and service/port asynchronously on a `uv::loop`.
- Keep the public API independent from HTTP.
- Return typed address candidates suitable for TCP connect attempts.
- Compose with request cancellation and timeouts.
- Preserve enough DNS error information for diagnostics.
- Leave room for custom resolvers, cache policy, and Happy Eyeballs.

## Public Shape

Initial API sketch:

```cpp
uvp::dns::resolver resolver(loop);

auto op = resolver.resolve(
  uvp::dns::query{}
    .host("api.example.com")
    .service("https")
    .family(uvp::dns::address_family::any),
  [](uvp::result<uvp::dns::address_list> result) {
    if (!result) {
      return;
    }

    for (auto const& address : *result) {
      // Try TCP connect candidates.
    }
  });

op.cancel();
```

The exact type names can change during implementation, but the important shape
is:

- resolver is loop-bound;
- each resolve operation has an owning handle;
- cancellation is explicit;
- callback receives either an address list or a typed error.

## Query Options

Suggested initial options:

- host name;
- service name or numeric port;
- address family: IPv4, IPv6, or any;
- socket type, defaulting to stream/TCP;
- numeric-host fast path;
- optional timeout;
- optional flags that map to safe `getaddrinfo` behavior.

The API should avoid exposing raw `addrinfo*` ownership. Raw platform data can
stay in private implementation files.

## Results

`address_list` should contain stable value objects, not borrowed system
pointers.

Each address candidate should expose:

- address family;
- socket address bytes through a safe endpoint wrapper;
- numeric host and port formatting for diagnostics;
- original query metadata when useful.

Ordering matters. The resolver should either preserve platform ordering or
document any reordering policy. Happy Eyeballs can later add a connection
strategy above the raw result list rather than changing what DNS returns.

## Implementation Direction

The first implementation can wrap libuv `uv_getaddrinfo`.

Responsibilities:

- allocate and free `uv_getaddrinfo_t` and returned `addrinfo` safely;
- copy results into uvpp-protocols value types before invoking user callbacks;
- map libuv and system resolver failures to `uvp::dns` errors;
- support cancellation using libuv request cancellation where available;
- ensure the completion callback fires exactly once unless the documented
  operation lifetime says otherwise.

DNS should not own TCP sockets. It only produces candidates. A separate outbound
connect helper can consume the candidates and attempt connections.

## Outbound Connect Helper

HTTP client work will likely need a reusable connector:

```text
DNS address list
  -> one or more TCP connect attempts
    -> uvp::io::byte_stream
```

This helper may live under `uvp::io` rather than `uvp::dns` because it owns TCP
handles and connection timing. It should provide:

- connect timeout;
- cancellation;
- sequential address attempts at first;
- Happy Eyeballs later;
- endpoint metadata;
- typed failure when all candidates fail.

Keeping this separate lets other protocols reuse it without depending on HTTP.

## Timeouts

DNS operations need a timeout policy that composes with higher-level request
deadlines.

The HTTP client should be able to set both:

- a DNS phase timeout;
- an overall request deadline that also cancels DNS if it expires first.

If both can expire, completion should report the first observed cause exactly
once.

## Cancellation

Cancellation must be safe in every phase:

- before the libuv request is submitted;
- while the system resolver is pending;
- after libuv completion but before user callback dispatch;
- during resolver or client shutdown.

If libuv cannot cancel an in-flight system resolver immediately on a platform,
the uvpp operation should still suppress user-visible success and report or
record cancellation according to a documented exactly-once completion policy.

## Errors

DNS should have its own category so HTTP can distinguish resolution failures
from connect, TLS, and protocol failures.

Suggested error cases:

- invalid query;
- unsupported address family;
- name not found;
- temporary resolver failure;
- no usable addresses;
- timeout;
- cancelled;
- resolver closed;
- system/libuv failure.

Higher-level clients may wrap these errors, but they should preserve the DNS
category or source code for diagnostics.

## Happy Eyeballs

Happy Eyeballs is important for polished client behavior, but it does not need
to be in the first resolver slice.

Design direction:

- DNS returns address candidates;
- connector applies IPv6/IPv4 racing policy;
- connection pool and HTTP client observe only successful `byte_stream` or a
  typed connect failure;
- policy remains configurable for deterministic tests.

## Out Of Scope For First Implementation

- Full DNS message parsing.
- Custom UDP/TCP DNS transport.
- DNS-over-HTTPS or DNS-over-TLS.
- Cache invalidation policy.
- `/etc/hosts` reimplementation beyond what system `getaddrinfo` provides.
- SRV, TXT, MX, or protocol-specific record lookup.

## Source Documents

- [HTTP client](http-client.md)
- [Transport abstractions](../design/transport-abstractions.md)
- [Protocol composition](../design/protocol-composition.md)
