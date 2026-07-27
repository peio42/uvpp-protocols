# Contributor Documentation

These guides are for developers extending `uvpp-protocols`: adding a protocol
module, composing existing transports, or evolving shared foundations. They
describe implementation patterns and integration contracts rather than the
public API of an individual protocol.

- [Authoring a cancellable protocol operation](authoring-protocol-operations.md):
  structure a multi-phase client operation with cancellation, timeouts,
  transport ownership, and exactly-one completion.
