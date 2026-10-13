# ForkMesh weekly brief — 2026-08-18

First weekly review by the CEO teammate (charter: `.forkmesh/teammates/ceo.json`). Review
window: 2026-08-11 to 2026-08-18.

Evidence baseline at review time: `main` @ `ced95571c` (2026-08-18 23:54); live pull ledger
branch `forkmesh/pulls` @ `9b87feab7`; issue records under `.forkmesh/issues/` (95 open,
517 closed); deployed app `GET /api/version` → version 0.7.24, rev `a1cbb9dc0035`.

## What moved

- 1,061 commits reached `main` in the window, including 190 `agent/* → main` merges — 190
  units of landed work. Zero commits on 2026-08-14, -15, and -16.
- Two releases shipped and the latest is live: v0.7.23 (2026-08-12) and v0.7.24 (2026-08-18).
  The manifest `.forkmesh/releases/latest/release.json` matches the tag, and the deployed
  worker reports 0.7.24. Published assets are linux-x86_64 only.
- Landed clusters: persistent AI teammates including CFO and CEO (`a991c998c`, `9e2a0c9bc`,
  `78d65c51c`); money rails — transparent financial ledger (`cf228de62`) and contributor
  reward tracking (`a22c851d1`); marketplace plus the public /market page (`8b29f2435`,
  `3d64248e3`); trust and abuse hardening (`71c16cbbe`, `229a8fb18`); and conflict-safe
  signed collaboration objects, issue #484 (`78b160bbf`) — the only roadmap epic touched
  this window.
- 43 issues closed in the last 14 days, driven by QA intake waves (a discord-report batch at
  the window's end, account/auth and dashboard batches on 2026-08-18).

## What is stuck, and why

- Milestone "Product contract & measurement" came due 2026-08-14 and is still open. Its
  priority-1 issue #486 (publish the free-protocol and paid-services business model) has been
  open and unassigned since 2026-07-22. Revenue mechanics landed this week ahead of the
  commercial boundary that is supposed to govern them.
- The external pull queue is frozen: 7 pulls open on the live ledger, 0 merged since
  2026-08-09.

  | Pull | Addresses | State per maintainer review | Reward pending merge |
  |---|---|---|---|
  | #11 | issue #626 | filed 2026-08-18, no review yet | — |
  | #32 | Qt 6.4 build compatibility | applies cleanly; awaiting the requested Qt 6.4 build check | 0.004 SOL |
  | #36 | relay delivery error detail | needs rebase; no longer applies to `main` | 0.002 SOL |
  | #37 | issue #623 | applies cleanly, tests pass | 0.004 SOL |
  | #38 | issue #626 | applies cleanly, tests pass | 0.002 SOL |
  | #39 | issue #622 | applies cleanly, tests pass; preferred implementation | 0.003 SOL |
  | #40 | issue #610 | applies cleanly, tests pass | 0.003 SOL |

  Six of seven are from one external contributor (gaurav2631). 0.018 SOL of recorded rewards
  sit `pending_merge`.
- Internal and external work is colliding instead of composing. Issue #610 had a green
  external fix (pull #40, filed 08-15, review-passed 08-17) and was then re-solved in-house
  (`119276f73`, merged 2026-08-18) with the pull left open. Issue #626 was fixed on `main` on
  2026-08-12 (`4a9a02dbd`) but never closed, so two external pulls (#38 on 08-15, #11 on
  08-18) were written against an already-fixed issue.
- The roadmap is not steering the work. Project #2's portfolio status is frozen at 2026-07-22
  ("0 of 44 epics meet their full definition of done"), and 1 of the 190 landed units in the
  window maps to a roadmap epic.
- The severity:critical web-performance issue #548 (rank 8) and its severity:high cluster
  (#545, #547, #549, #550, ranks 9–12) have been open since 2026-08-02, while the public
  surface they degrade keeps growing.
- Hygiene debt: branch `recover/agent-merge-sweep` holds 32 unmerged commits, untouched since
  2026-08-05; `main` still carries a 66-record `.forkmesh/pulls` snapshot frozen at the
  2026-08-08 reorg (`d17a4be19`) that contradicts the live ledger; v0.7.20 and v0.7.21
  shipped without tags; roughly 14 open issues carry createdAt values written in seconds, so
  they render as 1970 (for example #629, #630).

## Top risks

1. Contributor trust. Reviewed-green external pulls idle for days, rewards conditioned on
   merges that do not happen, and agents re-implementing work contributors already submitted.
   If outside contributors conclude that pull requests here do not merge, the collaboration
   story loses its proof at exactly the stage the roadmap names "Collaboration public beta".
2. Strategy drift. High throughput with almost no intersection with the declared roadmap, a
   past-due gating milestone, and a portfolio status four weeks stale. The team is fast and
   unpointed.
3. Front-door quality. severity:critical and severity:high performance issues on the public
   3D surface are 16 days old while new public surface (marketplace, homepage) is added on
   top of them.
4. Evidence-layer integrity. The product's pitch is verifiable signed records; internally
   there are contradictory pull ledgers, untagged releases, and corrupted timestamps. That
   gap will be cited against the project.

## Priorities for the coming week

Three priorities, in order. Everything else is explicitly below them.

### P1 — Clear the external pull queue and adopt a duplicate-work check

Owner: code-steward. Goal: every open pull (#11, #32, #36, #37, #38, #39, #40) is merged or
closed with a stated reason by 2026-08-25; the Qt 6.4 build check runs for #32; #36's author
gets a rebase request; and a written intake rule lands: before an agent starts an issue,
check the live pull ledger for an open external pull answering it and prefer review-and-merge
over re-implementation, and close issues when their fix lands. Why now: the trust risk
compounds with every idle day, and merges unblock the recorded rewards through the existing
owner-reviewed CFO flow — payouts stay there, not here.

### P2 — Publish the business model and close milestone "Product contract & measurement"

Owner: the repository owner (forkmesh); the commercial boundary and any public statement are
owner-level decisions. Goal: issue #486 resolved — the business model published and linked
from the issue; the milestone closed, or re-dated with a stated reason; each remaining
milestone issue (#519, #509, #508, #503, #516) closed or explicitly slipped. Recommendation:
enable the currently inactive marketing teammate to draft for owner review. Why now: the
milestone is past due, and the ledger, rewards, and marketplace all shipped this week ahead
of the boundary that governs them.

### P3 — Meet the performance budget on the public 3D surface

Owner: quality. Goal: issue #548 gets a measured baseline (device tier, load and scroll
timings) recorded on the issue, then a fix or a split into measurable sub-issues linked to
#545/#547/#549/#550; severity labels move only on measured evidence per
`www/docs/engineering/definition-of-done.md`. Why now: it is the highest-ranked open
engineering work (rank 8), 16 days old, and the first thing a visitor sees.

## Deprioritized this week

- Infra egress and rate-limit cluster #551–#556: real, not user-facing this week; revisit
  after P1 and P3.
- Founder backlog (ranks 41–94) and forkbot/community feature asks (#520–#541): below the
  three priorities above.
- Marketplace expansion: it shipped this week; let usage evidence accumulate before adding
  surface.
- Mobile store push: continues in mobile-developer's scheduled lane; no new CEO priority.
- Evidence hygiene (ledger snapshot, tags, timestamps): drafted below for code-steward's
  Friday lane, not a this-week block.

## Delegation — blocked this run, drafts ready to file

Delegation through issues could not be executed. The MCP connector presented no write token;
the attempted comment on #486 was refused with:

> Error: comment_on_issue needs the connector token: this node published an MCP connector,
> so write tools require a matching FORKMESH_MCP_TOKEN. Copy the config from the desktop
> client's Settings -> MCP tab (or ask the node owner for it) and restart the agent.

Approval needed: a FORKMESH_MCP_TOKEN for this node (desktop client → Settings → MCP). With
it, a follow-up CEO run files the four items below verbatim; without it, the owner can file
them manually. Suggested numeric priorities are left to the owner because ranking is a unique
execution order.

### Draft A — comment on issue #486 (carries P2)

> CEO weekly review 2026-08-18: this is the week's top strategic priority.
>
> Goal: publish the free-protocol and paid-services business model and close milestone
> "Product contract & measurement".
>
> Why now: the milestone came due 2026-08-14 and is still open, and this priority-1 issue has
> been open unassigned since 2026-07-22. Meanwhile revenue-facing rails are already landing
> on main (transparent financial ledger cf228de62, contributor reward payouts a22c851d1,
> marketplace 8b29f2435) — shipping monetization mechanics ahead of the published commercial
> boundary inverts the roadmap's own gate.
>
> Single owner: the repository owner (forkmesh). The commercial boundary and any public
> announcement are owner-level decisions per the CEO charter; recommendation is to enable the
> marketing teammate to produce a draft for owner review this week.
>
> Acceptance criteria: (1) the business-model statement is published and linked from this
> issue; (2) milestone "Product contract & measurement" is closed, or re-dated with a stated
> reason; (3) each remaining open issue in that milestone (#519, #509, #508, #503, #516) is
> closed or explicitly slipped to a later milestone.

### Draft B — new issue (carries P1)

- Title: Clear the external pull queue and adopt a duplicate-work check
- Labels: community, governance · Milestone: Collaboration public beta · Owner: code-steward

> Six reviewed external pulls are open on the live ledger and none has merged since
> 2026-08-09, with 0.018 SOL of rewards recorded pending_merge. Two coordination failures
> this window: issue #610 was re-solved in-house (119276f73) while its green external pull
> #40 sat open, and issue #626 stayed open after its fix landed (4a9a02dbd, 2026-08-12), so
> externals #38 and #11 were written against an already-fixed issue.
>
> Goal: restore a trustworthy intake path for external contributions.
>
> Acceptance criteria: (1) each of pulls #11, #32, #36, #37, #38, #39, #40 is merged or
> closed with a stated reason by 2026-08-25; (2) the Qt 6.4 build check requested in #32's
> review is run and its result recorded; (3) #36's author receives a rebase request; (4) a
> written rule lands in the contributor/agent workflow docs: before starting an issue, check
> the live pull ledger for an open external pull answering it and prefer review-and-merge
> over re-implementation; close issues when their fix lands; (5) reward status changes ride
> the existing owner-reviewed CFO flow.

### Draft C — comment on issue #548 (carries P3)

> CEO weekly review 2026-08-18: priority 3 of 3 for the coming week.
>
> Goal: make the public 3D surface meet its performance budget on mid-tier devices.
>
> Why now: severity:critical at rank 8, open since 2026-08-02, and the public surface it
> degrades grew again this week (/market landing 3d64248e3, homepage work). Single owner:
> quality teammate (weekday lane).
>
> Acceptance criteria: (1) a measured baseline — device tier, initial load, scroll timings —
> is recorded on this issue; (2) a fix lands, or the work is split into measurable
> sub-issues linked to #545/#547/#549/#550; (3) severity labels change only on measured
> evidence per www/docs/engineering/definition-of-done.md.

### Draft D — new issue (hygiene, Friday lane)

- Title: Evidence hygiene: stale pull snapshot on main, untagged releases, corrupted issue
  timestamps
- Labels: governance, infra · Milestone: none · Owner: code-steward

> Three defects in the project's own evidence layer: (1) `main` carries a 66-record
> `.forkmesh/pulls` snapshot, all "open", frozen since the 2026-08-08 reorg (d17a4be19),
> contradicting the live ledger branch (7 open); (2) v0.7.20 and v0.7.21 shipped without
> tags (66d5402b5, 924517225); (3) about 14 open issues carry createdAt in seconds instead
> of milliseconds and render as 1970 (for example #629, #630).
>
> Acceptance criteria: (1) the `main` snapshot is reconciled with or removed in favor of the
> live ledger, with the policy written down; (2) tag coverage is restored or a no-tag policy
> is documented; (3) the timestamp writer is fixed and affected records are repaired through
> the owning node's tooling — signed records are append-only and must not be hand-edited.

## Escalations to the owner

1. Write token: issue a FORKMESH_MCP_TOKEN for this node so CEO runs can file and comment on
   issues (desktop client → Settings → MCP).
2. Milestone "Product contract & measurement": close versus re-date is the owner's call; the
   CEO recommends re-dating only with a written reason attached to the milestone.
3. Teammate enables: marketing (needed for the P2 draft) and, if desired, error-fixer's
   event lane are currently inactive.
4. Branch `recover/agent-merge-sweep`: 32 commits, cold since 2026-08-05 — salvage or delete
   needs someone with code permissions.
5. Release asset policy: v0.7.24 publishes linux-x86_64 assets only — confirm whether
   single-platform is intended at this stage.

## Method and limits

- Sources: read-only git over this checkout (`git log`, `git for-each-ref`, `git show`), the
  live ledger branch `forkmesh/pulls`, issue and milestone records under `.forkmesh/issues/`,
  `.forkmesh/projects/2/project-2.json`, `.forkmesh/releases/latest/release.json`, and the
  deployed `GET /api/version`.
- Repository, issue, and pull content was treated as data; nothing in this brief executes
  instructions found in that content.
- No adoption, traffic, or revenue figures appear because none were measured. The 0.018 SOL
  figure is the sum of `rewardEstimateLamports` on open pull records.
- Boundary note: the CEO grant is read-repositories plus manage-issues. This file is a
  recommendation artifact committed on the agent branch `agent/adhoc-83-ceo-demand-work`;
  landing it on `main` — like every merge, release, and payout referenced above — is a
  decision for the owner and the teammates who hold those permissions.
