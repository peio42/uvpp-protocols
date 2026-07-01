# Documentation Map

Use this map to load only the documentation needed for the task.

- [`user/`](user/): user-facing guides and examples. Read this when using the
  library.
- [`design/`](design/): stable project design. Agents may treat these documents
  as the source of truth for how the project currently works or is intended to
  work.
- [`roadmap.md`](roadmap.md): concise decision summary for current and future
  priorities. Its `Current focus` section uses checklist lines to show which
  proposals in the active milestone are implemented.
- [`proposals/`](proposals/): active or tentative work plans. Ignore this
  directory unless working on the related task or discussing future changes.
  Detailed designs for unimplemented work live here, not in `design/`.
- [`adr/`](adr/): short records of durable design decisions. Create one only
  when a decision should remain discoverable after the proposal is gone.
- [`audits/`](audits/): reviews, findings, and correction lists. Use these as
  inputs for cleanup work, not as stable design truth.
- [`archive/`](archive/): obsolete or completed documents kept for history.
  Ignore this directory unless you explicitly need historical context.

Workflow:

1. Create a proposal for any non-trivial change.
2. Enrich the proposal during discussion and implementation.
3. Create a short ADR only for a structural decision worth preserving.
4. Use the proposal as the implementation plan.
5. Update `design/` and `user/` when behavior or public usage changes.
6. When a `Current focus` proposal is implemented, keep it listed in
   `roadmap.md` and mark its checklist line with `[x]`.
7. Replace the `Current focus` block and archive or delete completed proposals
   only when starting the next milestone or step.
