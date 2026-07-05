# HTTP Metrics

## Motivation

`response_info::response_body_size` reports the application response body size.
For streaming responses this counts payload chunks, not HTTP/1.1 chunk framing,
response headers, or other bytes written on the wire. That is the right stable
meaning for existing hooks, but operations code often needs lower-level byte
accounting too.

## Proposed Shape

Keep `response_body_size` unchanged and add a separate metrics surface for
encoded HTTP traffic. Candidate counters:

- request header bytes accepted by the parser
- request header count
- request body payload bytes received
- response header bytes written
- response body payload bytes written
- response body encoded bytes written
- total response bytes written on the underlying stream

The encoded counters should include transfer-coding overhead such as chunked
framing. The payload counters should not.

## Open Questions

- Should these counters live directly on `response_info`, or behind a nested
  `http_metrics` value to keep future expansion tidy?
- Should request metrics be observable before a response exists, for rejected
  requests and parser errors?
- Should low-level byte counters include bytes queued for write, bytes
  successfully handed to the transport, or both?
