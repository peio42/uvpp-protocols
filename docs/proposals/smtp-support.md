# SMTP Client Support Proposal

Status: Draft

## Context

SMTP client support is listed as a candidate module and as a protocol that
should compose with TLS and STARTTLS.

## Current State

- Implemented: no SMTP module.
- Not implemented: SMTP client session, parser, command state machine,
  STARTTLS handoff, or dependency strategy.

## Draft Scope

- Decide whether to implement a small SMTP client state machine or adopt a
  library.
- Define client session scope.
- Use explicit `byte_stream` transport composition.
- Preserve room for STARTTLS handoff.

## Out Of Scope

- SMTP server support.
- Complete mail server suite.
- Queueing, storage, spam filtering, or delivery policy.
- MIME message processing beyond protocol needs.

## Source Documents

- [Project scope](../design/project-scope.md)
- [TLS support proposal](tls-support.md)
- [Dependency decisions](../design/dependency-decisions.md)
