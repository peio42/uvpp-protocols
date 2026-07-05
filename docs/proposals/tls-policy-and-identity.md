# TLS Policy And Identity Proposal

Status: Draft, follow-up after initial TLS milestone

## Decision

TLS policy and peer identity features should be grouped into one follow-up
track, separate from the completed stream/listener adapter work.

This track covers:

- public TLS protocol version policy;
- server-side SNI context selection;
- server-side client certificate verification.

These features all affect context selection or certificate verification during
the handshake. They should stay in `uvp::tls` and remain backend-neutral in the
public API.

## Priority

Recommended order:

1. TLS version policy.
2. Server-side client certificate verification.
3. Server-side SNI context selection.

TLS version policy is small, deterministic, and useful immediately for
hardening deployments. Client certificate verification is also a direct context
policy. SNI context selection is more invasive because it selects or switches
server context state during an OpenSSL handshake and affects ALPN policy,
certificate choice, and future certificate reload behavior.

## TLS Version Policy

Current implementation sets a TLS 1.2 minimum internally. That should become
public and backend-neutral.

Suggested API:

```cpp
enum class protocol_version {
  tls_1_2,
  tls_1_3,
};

auto server_context = uvp::tls::server_context{}
  .minimum_version(uvp::tls::protocol_version::tls_1_2);

auto client_context = uvp::tls::client_context{}
  .minimum_version(uvp::tls::protocol_version::tls_1_2);
```

Open questions:

- expose only minimum version first, or both minimum and maximum;
- whether TLS 1.2 remains the minimum default or TLS 1.3 becomes selectable by
  default policy later;
- how to report unsupported versions on a platform OpenSSL build.

Initial recommendation: expose `minimum_version(...)` only. Keep TLS 1.2 as the
default minimum. Do not expose TLS 1.0 or TLS 1.1.

## Client Certificate Verification

Server-side client certificate verification should be explicit. The default
must remain "do not request a client certificate".

Suggested API:

```cpp
enum class client_certificate_mode {
  none,
  request,
  require,
};

auto context = uvp::tls::server_context{}
  .certificate_chain_file("server.crt")
  .private_key_file("server.key")
  .client_certificate_mode(uvp::tls::client_certificate_mode::require)
  .client_ca_file("clients-ca.pem");
```

Expected behavior:

- `none`: do not request client certificates;
- `request`: request a certificate, but allow handshake without one;
- `require`: fail the handshake when the client does not present a valid
  certificate;
- trust can be configured through explicit CA file/directory and possibly
  default verify paths;
- failures surface as `errc::verification_failed` or a more specific TLS error
  if the error vocabulary is extended.

Follow-up API may expose peer certificate metadata. The first slice only needs
to enforce policy and report success/failure.

## Server-Side SNI Context Selection

SNI context selection is useful for multi-tenant HTTPS and protocol gateways.
It should be listener-oriented because the listener owns accepted handshakes and
can keep selected contexts alive until each handshake finishes.

Possible API:

```cpp
uvp::tls::listener_options{}
  .on_server_name([](std::string_view name) {
    if (name == "api.example.com") {
      return uvp::tls::server_name_result::context(api_context);
    }
    return uvp::tls::server_name_result::use_default();
  });
```

The result type should model at least:

- use a selected `server_context`;
- continue with the listener default context;
- reject the handshake.

Questions to settle before implementation:

- should the callback live only on `listener_options`, or also on
  `server_context` for direct `accept(byte_stream, ...)` users;
- whether selected context ALPN policy replaces the default context ALPN policy;
- how to represent rejected unknown names;
- whether a missing SNI name should use the default context or fail when a
  strict option is enabled;
- how certificate reload should update the contexts returned by the callback.

Initial recommendation:

- put the primary callback on `listener_options`;
- selected context fully owns certificate, verification, ALPN, and version
  policy for that handshake;
- default behavior for missing or unknown SNI is to use the listener default
  context;
- provide an explicit reject result for strict virtual-host deployments.

## Tests

TLS version policy:

- context default allows TLS 1.2+;
- `minimum_version(tls_1_3)` rejects a TLS 1.2-only peer if that can be
  configured reliably in tests;
- invalid platform/backend combinations report deterministic errors.

Client certificate verification:

- default server context succeeds without client certificate;
- `require` fails without client certificate;
- `require` succeeds with a client certificate signed by configured CA;
- invalid client certificate fails verification.

SNI context selection:

- callback receives the client SNI name;
- selected context certificate/ALPN policy is used;
- default context is used when callback returns `use_default`;
- reject result fails the handshake;
- selected contexts remain alive until handshakes finish.

## Out Of Scope

- ACME automation.
- Certificate storage.
- Runtime certificate reload API, except ensuring SNI selection can support it
  later.
- OCSP stapling.
- Session resumption.
- Backend-specific OpenSSL handle escape hatches.

## Related Documents

- [TLS support](tls-support.md)
- [TLS listener adapter](tls-listener-adapter.md)
- [HTTP client](http-client.md)
