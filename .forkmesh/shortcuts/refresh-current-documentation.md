# name: Refresh current product documentation
# description: Find one coherent set of documentation drift, correct it from executable evidence, and leave a review-ready update with regression coverage.
# agent: claude-code
# model: claude-opus-5

Perform one focused documentation-stewardship pass. This is implementation
work: when the evidence supports a correction, update the repository rather
than stopping at a report.

0. Start from [`docs/README.md`](../../docs/README.md), which maps each
   subsystem to its developer home page (under `docs/`) and to the public
   pages under `www/docs/`. Source files hold no comments (`AGENTS.md`), so
   the docs are the only written explanation: keep each home page's sections
   (Code map, Architecture, Protocols and interfaces, Configuration, Build,
   run, test, Operations, Security, Traps, Recent changes, Related docs) and
   cite files, not line numbers.
1. Establish the evidence window from recent commits, merged work, signed
   releases, deployed-surface markers, tests, and the implementation. Read the
   canonical documents for the area before editing. Treat old posts, plans,
   screenshots, and generated copy as historical evidence rather than current
   truth.
2. Choose one coherent set of drift with meaningful user impact: a security or
   privacy boundary, setup path, supported capability, limitation, command,
   route, compatibility promise, or cross-client behavior. Prefer a complete
   correction across every surface that repeats the claim over scattered copy
   edits in unrelated areas.
3. State behavior precisely. Distinguish public from private, current release
   from current main, hosted from self-hosted, automatic from user-configured,
   and passive protection from an active attacker where those distinctions
   matter. Preserve limitations, retention, migration, and compatibility
   details. Never strengthen a claim beyond what implementation and tests
   prove.
4. Update the canonical technical document first, then README, SECURITY,
   generated/public docs, examples, FAQs, blog corrections, and linked copy as
   applicable. Preserve historical records; add a prominent dated
   current-behavior or superseded note instead of silently rewriting what an
   old release did. Keep prose direct, accessible, and free of duplicated
   source-of-truth blocks.
5. Add or extend a regression check when stale wording could recreate a false
   security, privacy, availability, compatibility, or custody claim. Build any
   generated assets through their checked-in source and run focused docs,
   links, content, and rendering tests. Inspect desktop and mobile layouts when
   the changed material is rendered UI.
6. Review the final diff for accidental product-code changes and unrelated
   runtime files. Leave a small review-ready commit on the branch; never open a
   pull request, and never merge or deploy it yourself.

Finish with the evidence window, the drift found, every surface corrected, the
current behavior and limitations now documented, tests run, and the next
highest-risk documentation gap.
