# Database Client Adapters Proposal

Status: Draft

## Context

Dependency decisions list PostgreSQL and MariaDB/MySQL client adapters as
candidates, but also leave open whether database client protocols belong in
`uvpp-protocols` or a separate companion package.

## Current State

- Implemented: no database client adapters.
- Not implemented: libpq integration, MariaDB Connector/C integration,
  nonblocking query lifecycle, result streaming, reconnect policy, or package
  boundary decision.

## Draft Scope

- Decide whether database client adapters belong in this repository.
- Evaluate nonblocking integration and socket ownership for candidate
  libraries.
- Define query/result lifetimes in the same callback-oriented style as other
  modules.

## Out Of Scope

- ORM.
- Migration framework.
- Database server protocol implementation.
- Connection pool as a first milestone requirement.

## Source Documents

- [Dependency decisions](../design/dependency-decisions.md)
- [Project scope](../design/project-scope.md)
