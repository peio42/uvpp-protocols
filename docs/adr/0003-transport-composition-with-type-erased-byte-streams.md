# ADR 0003: Transport Composition with Type-Erased Byte Streams

Status: Accepted

Date: 2026-06-27

## Context

The project needs protocols to compose over TCP, Unix sockets, TLS, and future
transport adapters without making every protocol directly bind to TCP.

uvpp wraps libuv handles with CRTP and inline raw handle storage. Types such as
`uv::tcp` and `uv::pipe` are distinct final types, and the callback model relies
on layout constraints that do not fit a shared virtual base class.

## Decision

Protocols compose over small transport abstractions rather than inheriting from
each other or from uvpp stream types.

The shared transport boundary is:

- `uvp::io::stream_listener`, which accepts incoming byte streams;
- `uvp::io::byte_stream`, which reads, writes, closes, and exposes endpoint
  metadata for one established byte stream.

These public transport shapes use internal type erasure. Concrete adapters such
as TCP listeners, pipe listeners, TLS listeners, and accepted streams convert
into the shared transport shapes.

## Consequences

- HTTP can listen on TCP, Unix sockets, or TLS without changing its parser or
  session model.
- TLS can be implemented as both a stream adapter and listener adapter.
- Future transports should follow the same composition model instead of adding
  protocol inheritance.
- Type erasure adds a small abstraction cost, but it keeps module boundaries and
  ownership much clearer than forcing all protocols through TCP-specific APIs.

## References

- [Transport abstractions](../design/transport-abstractions.md)
- [Protocol composition](../design/protocol-composition.md)
- [Module architecture](../design/module-architecture.md)
