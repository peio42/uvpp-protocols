# HTTP Redirect Policy Extensions Proposal

Status: Proposed after Milestone 6 proxy support

## Context

Milestone 6 implements a conservative redirect policy for the one-shot HTTP
client:

- redirects are opt-in with `client_options::follow_redirects`;
- only `GET` and `HEAD` are followed automatically;
- `301`, `302`, `303`, `307`, and `308` are recognized;
- absolute and relative `Location` values are supported;
- redirects re-run URL validation, DNS, TCP, TLS, and connection-pool selection;
- unsupported schemes, invalid `Location` values, and redirect loops fail with
  `client_redirect_failed`.

That is enough for client-side foundations. The remaining redirect behavior is
not required to validate the milestone and should be handled after proxy
support, because proxy selection, proxy identity, CONNECT tunneling, and
credential stripping all affect redirect security decisions.

## Decision

Do not expand redirect behavior further in Milestone 6.

Finish the proxy design/minimal implementation first, then revisit redirect
policy as a focused HTTP client polish proposal. This keeps the current client
usable without freezing browser-like behavior too early.

## Goals

- Define explicit redirect policies beyond `GET` and `HEAD`.
- Prevent credential leaks when scheme, host, port, or proxy identity changes.
- Support replay only when the request body is known to be replayable.
- Keep streaming uploads non-replayable unless the user provides an explicit
  replay source.
- Preserve connection-pool correctness across redirected origins and proxies.
- Allow applications to observe redirect history and final URL metadata.

## Non-Goals

- Browser-complete fetch semantics.
- Cookie jar, HSTS, cache, CSP, CORS, or referrer policy.
- Automatic replay of arbitrary streaming bodies.
- Public libcurl compatibility behavior.

## Proposed API Direction

Replace the current boolean-only redirect knob with a policy value while keeping
the existing simple option as a convenience:

```cpp
uvp::http::client_options options{
  .follow_redirects = true,
  .max_redirects = 5,
};
```

Later extension:

```cpp
uvp::http::redirect_policy policy;
policy
  .enabled(true)
  .max_redirects(10)
  .allow_post_to_get(true)
  .strip_credentials_on_origin_change(true)
  .strip_credentials_on_scheme_downgrade(true);

uvp::http::client_options options{
  .redirects = policy,
};
```

Exact naming can change during implementation. The important shape is that
redirect behavior is explicit and testable rather than hidden behind a
browser-compatible default.

## Method Policy

Suggested defaults:

- `GET` and `HEAD`: replay automatically for supported redirect statuses.
- `303`: may convert replayable non-`GET` methods to `GET` only if the policy
  explicitly allows browser-compatible behavior.
- `301` and `302`: do not convert `POST` to `GET` by default.
- `307` and `308`: preserve method and body only when the request body is
  replayable.
- streaming request bodies: fail unless the user supplies a replayable body
  source.

This keeps the native client conservative while allowing applications to opt
into looser compatibility behavior.

## Sensitive Headers

When the redirect target changes origin or proxy identity, strip sensitive
headers by default:

- `Authorization`;
- `Cookie`;
- `Proxy-Authorization`;
- future authentication headers owned by helper APIs.

Scheme downgrade from `https` to `http` should be stricter than a same-origin
path redirect. A future policy can decide whether to reject downgrades entirely
or allow them after stripping credentials.

## Proxy Interaction

This proposal should be implemented after proxy support because redirects need
to account for:

- clear HTTP proxy absolute-form requests;
- HTTPS proxy `CONNECT`;
- per-proxy authentication;
- `NO_PROXY` / bypass decisions if adopted;
- connection-pool keys that include proxy identity;
- redirect targets that move into or out of proxy scope.

The redirect policy should compare the effective transport route, not only the
URL origin, before deciding whether credentials and pooled connections are safe
to reuse.

## Replayable Bodies

The one-shot buffered API may later support replayable bodies:

- string/bytes body stored by the request object;
- file-backed body with explicit reopen policy;
- user-provided replay callback that creates a fresh body writer.

Streaming uploads remain non-replayable unless explicitly wrapped in one of
those replayable sources.

## Response Metadata

Expose redirect metadata without forcing every call site to inspect internal
state:

- final URL;
- redirect count;
- ordered redirect chain;
- intermediate status codes and locations;
- indication that sensitive headers were stripped.

This metadata may live on `http::response`, a `client_result` wrapper, or a
future request handle inspection API. The choice should avoid breaking the
existing `result<http::response>` shape until a broader client result model is
justified.

## Tests

Add coverage for:

- `303` conversion when explicitly enabled;
- `307` / `308` preserving methods with replayable bodies;
- failure for non-replayable streaming upload redirects;
- stripping `Authorization`, `Cookie`, and `Proxy-Authorization` on origin
  change;
- preserving safe headers on same-origin redirects;
- HTTPS-to-HTTP downgrade policy;
- redirects across proxy and no-proxy boundaries;
- redirect history / final URL metadata.

## Source Documents

- [HTTP client](http-client.md)
- [Shared URL module](shared-url-module.md)
- [Dependency decisions](../design/dependency-decisions.md)
