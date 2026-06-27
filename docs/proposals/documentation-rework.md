# Documentation Rework Proposal

## Context

The documentation used to be split mostly between user-facing pages and design
notes. As the project grows, that makes it hard to distinguish stable design,
tentative plans, audits, and historical context.

## Proposed Structure

- `docs/user`: what library users should read.
- `docs/design`: how the project currently works or is intended to work.
- `docs/proposals`: active or tentative plans.
- `docs/adr`: durable decision records for structural choices.
- `docs/audits`: findings, reviews, and correction lists.
- `docs/archive`: obsolete documents kept for history, deferred for now.

## Migration Plan

- Move user guides into `docs/user`.
- Move audit documents into `docs/audits`.
- Keep stable architecture and protocol design notes in `docs/design`.
- Move roadmap-style planning out of `docs/design` and keep a concise decision
  summary in `docs/roadmap.md`.
- Add a short `docs/README.md` that tells agents which directories they can
  safely ignore for most tasks.

## ADR Policy

Do not create an ADR for this migration yet. The structure is a working model,
not a final project decision.
