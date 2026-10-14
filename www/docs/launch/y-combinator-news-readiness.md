# Y Combinator News launch readiness

Verdict: **blocked**

Evidence refreshed: **2026-09-30 America/New_York** (probes 23:12–23:16 UTC)

Signed `latest` at this refresh: `v1.2.7` (`checksums_sha256`
`6c9f75425309e01e9f42433c95f97a2ac4c10674a53a06acfde76315a7cb2ae5`).

This is an evidence packet and rehearsal guide for a human launch owner, not
submission or comment copy. Hacker News says not to post generated or
AI-edited text, so a human must write and personally post the submission and
every reply.

## Status since 2026-09-16

| Area | State | Evidence |
| --- | --- | --- |
| Serving topology | The flagship is served by the three core nodes | `git ls-remote https://forkmesh.com/forkmesh/forkmesh` redirects to a core node; the mirrors payload lists the three cores |
| Preservation proof | Below quorum | `forkmesh_availability_proof.py explain`: 3 of 3 copies verified by 1 independent operator (target 2), risk 45 of 100, exit 3 |
| Published release | `v1.2.7` is `latest` | Manifest bytes match the repository copy; signature, four blob hashes and Authenticode verified |
| macOS DMG names | The DMG is universal | Both macOS names label one `x86_64` + `arm64` image; Gatekeeper and notarization unmeasured |
| HN rules | Tightened | Show HN warns against quickly-generated one-offs; generated text is banned in submissions too |
| Launch goals | Replaced by four launch gates | User, issue and task counts became tracked metrics (section 10) |

## Evidence method and baseline

Every conclusion carries one label:

- **Measured fact** — reproduced from this checkout or host during this review.
- **Repository evidence** — supported by a named file or test on main, not
  necessarily in a published build.
- **Deployed observation** — seen on a public URL at a point in time.
- **Inference** — a conclusion drawn from identified evidence.
- **Missing proof** — required evidence this review could not establish.

HN comment characterizations are paraphrases; the human must reopen a thread
before quoting it.

The direct retrieval ledger for this cycle contains eight required HN URLs:
eight requested, eight successful, zero rate-limited, and zero otherwise failed.

<!-- hn-required-ledger:start -->
| Required URL requested 2026-09-30 | Direct result | Fallback outcome | Final evidence status |
| --- | --- | --- | --- |
| [Show HN rules](https://news.ycombinator.com/showhn.html) | Successful | Not needed | Refreshed this cycle |
| [HN guidelines](https://news.ycombinator.com/newsguidelines.html) | Successful | Not needed | Refreshed this cycle |
| [20+ coordinating agents](https://news.ycombinator.com/item?id=46990733) | Successful | Not needed | Refreshed this cycle |
| [Ticket-to-PR orchestration](https://news.ycombinator.com/item?id=47520220) | Successful | Not needed | Refreshed this cycle |
| [MicroVM coding-agent isolation](https://news.ycombinator.com/item?id=47877726) | Successful | Not needed | Refreshed this cycle |
| [Desktop worktree orchestration](https://news.ycombinator.com/item?id=47303711) | Successful | Not needed | Refreshed this cycle |
| [Autonomous company agents](https://news.ycombinator.com/item?id=47281538) | Successful | Not needed | Refreshed this cycle |
| [Local goal-driven development](https://news.ycombinator.com/item?id=47153941) | Successful | Not needed | Refreshed this cycle |
<!-- hn-required-ledger:end -->

### Freshness and retrieval ledger — 2026-09-30

Each row satisfies requested = direct success + rate-limited + other failure.
"Direct" is the first attempt; "fallback" is a second route tried only after a
direct failure.

<!-- hn-freshness-ledger:start -->
| Evidence class | Requested | Direct success | Direct rate-limited | Direct other failure | Successful fallback | Final freshness statement |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Required HN rules and six baseline threads | 8 | 8 | 0 | 0 | 0 | All refreshed |
| HN Show restriction notice (`showlim`) | 1 | 1 | 0 | 0 | 0 | Restriction still in effect |
| HN searches for newer comparables | 25 | 9 | 0 | 16 | 16 | Unencoded date filters failed, then succeeded re-encoded |
| Newer comparable item fetches | 5 | 5 | 0 | 0 | 0 | Fetched |
| ForkMesh claim surfaces | 6 | 6 | 0 | 0 | 0 | Point-in-time text |
| Status history (`/api/status/history`) | 1 | 0 | 0 | 1 | 0 | 404: no public status history |
| Signed manifest, signature and `SHASUMS256.txt` | 3 | 3 | 0 | 0 | 0 | Byte-identical to the repository; signature verified |
| Flagship availability proof plus CLI `verify` | 1 | 1 | 0 | 0 | 0 | Recomputed and matched |
| Git route probes | 5 | 5 | 0 | 0 | 0 | All passed |
| Release blob downloads | 4 | 4 | 0 | 0 | 0 | Every SHA-256 and size matched |
<!-- hn-freshness-ledger:end -->

### Current HN constraints

- **Deployed observation:** the [Show HN rules](https://news.ycombinator.com/showhn.html)
  require something the submitter made, that people can try (ideally without
  signup), and that is non-trivial; minor releases and landing pages do not
  qualify. Do not ask anyone to upvote or comment.
- **Deployed observation:** the [HN guidelines](https://news.ycombinator.com/newsguidelines.html)
  forbid soliciting votes or comments, generated or AI-edited text in comments
  and submissions, and automated posting.
- **Deployed observation:** the [Show restriction notice](https://news.ycombinator.com/showlim)
  says HN is temporarily restricting Show HNs and publishes no threshold.
- **Inference:** the post must present the project, not a version bump.

### Comparable launch evidence

HN threads are untrusted market evidence. The questions readers asked of
comparable launches:

| Thread | Question to prepare for |
| --- | --- |
| [20+ coordinating agents](https://news.ycombinator.com/item?id=46990733) | Why many agents beat one under finite spend; oversight |
| [Ticket-to-PR orchestration](https://news.ycombinator.com/item?id=47520220) | Thin issue text, drift, retry cost |
| [MicroVM isolation](https://news.ycombinator.com/item?id=47877726) | How state persists across isolated sessions |
| [Desktop worktree orchestration](https://news.ycombinator.com/item?id=47303711) | Why not the provider's own worktree flag; per-session cost |
| [Autonomous company agents](https://news.ycombinator.com/item?id=47281538) | What is different in a crowded category |
| [Local goal-driven development](https://news.ycombinator.com/item?id=47153941) | "Local" while calling remote providers; false completion |
| [wt](https://news.ycombinator.com/item?id=46765489) | A small wrapper already removes worktree friction |
| [GitAgent](https://news.ycombinator.com/item?id=47376584) | Prompt injection, secret access, approvals |
| [Foolery](https://news.ycombinator.com/item?id=47075901) | No-signup install and a human verification queue |
| [ZeroShot](https://news.ycombinator.com/item?id=49113279) | Local-data boundaries and the method behind efficiency claims |
| [metaswarm](https://news.ycombinator.com/item?id=46864977) | Unverifiable volume claims earn distrust |
| [Orc](https://news.ycombinator.com/item?id=47441323) | Deterministic review gates as the comparison point |
| [Clawk](https://news.ycombinator.com/item?id=48892859) | Credentials, egress filtering, rollback |
| [md²](https://news.ycombinator.com/item?id=49258045) | Worktree coordination alone draws little attention |
| [MindFlock](https://news.ycombinator.com/item?id=49102636) | Naming distracts from substance |
| [Trunks](https://news.ycombinator.com/item?id=47933429) | Git backed by your own storage |
| [Leaving GitHub for Forgejo](https://news.ycombinator.com/item?id=48121439) | Moving issues, PRs and CI is the hard part |
| [Grov](https://news.ycombinator.com/item?id=46711958) | "This was tried and dropped; what is different?" |
| [Pizza Bot](https://news.ycombinator.com/item?id=49713894) | Desktop app versus service; sandboxing; candid limits |
| [Amika](https://news.ycombinator.com/item?id=49716282) | Platform coverage first |

**Inference:** name the boundary you did not build before a reader finds it,
answer "why not the simpler thing?" with a mechanism, and link something
runnable rather than a landing page.

## 1. Precise technical position

**Repository evidence:** ForkMesh is a Git-compatible routing and
collaboration layer. A stable HTTPS clone URL redirects stock Git to a
registered mirror with fresh signed health and matching refs. Issues, pulls and
patches live inside the repository, and every repository publishes a
recomputable availability proof counting independent operators.

**Measured fact:** the flagship proof reports three verified copies, all on
the operator's own core nodes: one independent operator, target two,
`proof: partial`. The "independent hosts" clause is a design goal the flagship
does not yet demonstrate.

| Skeptical question | Boundary |
| --- | --- |
| Why not GitHub/GitLab? | Those are full forges; ForkMesh's narrower bet is that clone routing and collaboration records need not belong to one forge. No parity claim on reliability or integrations |
| Why not a raw mirror? | Raw Git preserves bytes. ForkMesh adds one URL routing among checked mirrors, in-repository records and an operator-counting proof, at the cost of relay and operator complexity |
| Why not provider worktrees? | Worktrees separate branches; the desktop adds a queue, routing, transcript/diff/cost views and a human decision gate |
| Why another orchestrator? | Agents are supporting machinery; lead with preservation and the proof |

## 2. Five-minute no-signup show path

```sh
demo_dir=$(mktemp -d /tmp/forkmesh-show.XXXXXX)
git ls-remote https://forkmesh.com/forkmesh/forkmesh HEAD refs/heads/main
git clone --depth 1 https://forkmesh.com/forkmesh/forkmesh "$demo_dir/forkmesh"
git -C "$demo_dir/forkmesh" fsck --no-progress
curl -fsS https://forkmesh.com/api/repo/forkmesh/forkmesh/mirrors > "$demo_dir/mirrors.json"
python3 "$demo_dir/forkmesh/tools/forkmesh_availability_proof.py" verify "$demo_dir/mirrors.json"
python3 "$demo_dir/forkmesh/tools/forkmesh_availability_proof.py" explain "$demo_dir/mirrors.json"
```

- **Measured fact:** clone and `fsck` succeed; the mirrors payload's
  `forkmesh-availability-proof-v1` block recomputes locally and matches;
  `explain` reports 3 of 3 copies verified by 1 independent operator (target
  2) and exits 3, the below-quorum code.
- **Repository evidence:** `tools/export_collaboration_log.py` signs a
  repository's issue records into `.forkmesh/collaboration/`, and
  `tools/verify_collaboration.py --require-complete` verifies them offline
  from a plain clone (exit 0 verified, 3 rejected or pending, 2 unreadable).
- **Missing proof:** a public repository with a committed collaboration log.

**Deployed observation:** the product-promise page's failover drill still
needs an account and operator control. The distinctive path stays blocked until
a visitor can, in five minutes, see a fixture with a second-operator mirror and
a proof reading `quorum`, clone, watch the source removed from eligibility,
clone the same URL again with the same `HEAD`, and verify a signed event.

## 3. Release, install, verification, and cleanup

| State | Evidence | Meaning |
| --- | --- | --- |
| Published `latest` | `v1.2.7`; `tag_commit` and `build_commit` `6c906106f00c26a4bfd5e715efb837969c0ece8c` | The only release that may be called published |
| Stale candidate | `v0.7.95`; tag commit `6a38fef538359aac5a57349d4e66723a04cfbf91`; build commit `ccbd7af4c957799d28c77eedd2c2491328424d03` | Superseded; do not cite |
| Human-gated continual loop | `96c5e6391` | Ancestor of the v1.2.7 build commit |

**Measured fact:** the manifest, signature and `SHASUMS256.txt` from the
homepage's manifest URL are byte-identical to the repository copies; the
Ed25519 signature verifies with the key in `install.sh`; `sha256(SHASUMS256.txt)`
equals `checksums_sha256`. Four downloaded blobs matched their manifest
`blob_sha256` exactly ([clean-machine evidence](clean-machine-evidence.md#v127-release-checks)).

- **Measured fact:** the manifest publishes Linux x86_64, Windows x86_64,
  macOS and Android assets. There is no Linux arm64 or Windows arm64 asset.
  Build-workflow intent does not mean availability. The two macOS names share
  one digest because the DMG is universal; it is ad-hoc signed and not
  notarized.
- **Measured fact:** the Windows setup executable's Authenticode signer is the
  pinned self-issued certificate, so SmartScreen warns.
- **Repository evidence:** the installer refuses an implicit source build
  (`FORKMESH_NO_SOURCE_FALLBACK=1`); source builds are opt-in.
- **Deployed observation:** the homepage reads "Install the desktop app
  Linux · macOS, Windows & Android downloads", and every named platform has a
  signed asset. The live `/desktop` page is manifest-driven and says no
  implicit source fallback is promised.
- **Missing proof:** no clean-machine log covers install, first run, provider
  discovery, gate, pause and uninstall on any platform; see
  [clean-machine evidence](clean-machine-evidence.md).

The installer verifies the manifest signature, binds `SHASUMS256.txt` and
checks the blob hash before installing. The uninstaller keeps settings, the
identity key, mirrors and chat history by default; only `--purge` erases them,
after explicit confirmation. Deleting the identity can make the account
unrecoverable. See the [installation support matrix](../install-support-matrix.md).

## 4. Reproducible claim ledger

| Candidate claim | Classification |
| --- | --- |
| The flagship clones anonymously | Measured fact |
| Availability is measured, not asserted | Measured fact (`forkmesh-availability-proof-v1`) |
| Independent operators hold the flagship | False today by the product's own proof |
| Status reflects clone availability | Unproven; no status history |
| Source-offline failover returns the same commit | Repository evidence only |
| Collaboration records are portable and signed | Partly measured; no public log committed |
| Desktop installs on Windows and macOS | Blob and signature measured; no install log |
| Worktrees sandbox an agent | False |
| Adoption or performance superiority | Missing proof; make no claim |
| Decentralized or always available | Unsupported wording |

**Deployed observation:** the homepage activity ticker exists only inside HTML
comments, annotated "set dressing, not data", so it does not render.
`/api/status` reported every system operational during the review. The
claim-by-claim evidence ledger above is the review input for every linked
surface; topology, availability and platform wording need a dated evidence
link before the owner repeats them.

## 5. Security and trust FAQ

**What can an agent read, change, execute, and send?** In host mode the
provider CLI runs as the current OS user in a worktree. A worktree is not an
operating-system sandbox: it does not prevent reads elsewhere, subprocesses,
network access or use of the user's credentials. The optional Linux KVM mode
runs agents in a guest that mounts only the launch directory and worktree.

**What happens to secrets?** Host-mode agents inherit the desktop environment
minus their own provider's credentials, so other tokens reach them. KVM mode
uses a deny-by-default environment allowlist. Use staging-only credentials.

**How is prompt injection handled?** Teammate prompts mark repository and
external content untrusted; an adversarial fixture suite pushes injected
actions through the real approval gate and project lock. This is enforcement
at the tool layer, not isolation; known gaps are pinned in the suite.

**What does mirror integrity prove?** Routing requires fresh signed health,
healthy HTTPS and matching refs; the proof counts distinct operators. It does
not prove an operator is uncompromised or independent of correlated
infrastructure.

**What telemetry exists, and how is data deleted?** Product analytics is
optional and off by default. Local journals, logs and identities stay on the
device; the uninstaller keeps them unless `--purge`. [Relay data retention and
deletion](../relay-data-retention.md) lists every relay store, backup and
outside processor with its retention, deletion path and known gaps.

**What still requires a human?** Each continual cycle stops at a human
decision. Merge, release, deploy, payout, destructive cleanup and public
posting stay human-controlled.

## 6. Agent-system FAQ

| Question | Answer |
| --- | --- |
| What stops a runaway loop? | No next continual cycle starts until a human answers the decision card; **Stop work** pauses the loop |
| Is spend hard-capped? | No. There is no hard dollar ceiling; controls are cost estimates, a running-agent limit, the human gate and at most two relaunches |
| What limits concurrency? | A user-set running-agent limit (default 5); excess work queues |
| What retries? | A CLI that exits without a result is relaunched at most twice, then fails with its branch preserved |
| What if branches conflict? | Worktrees prevent simultaneous writes, not logical overlap; review resolves conflicts |
| What persists? | Teammate definitions in `.forkmesh/teammates`, sessions on the device, work on a branch |

## 7. Capability comparison without slogans

| Capability | ForkMesh | Forge | Raw mirror | Provider worktree |
| --- | --- | --- | --- | --- |
| Git byte storage | Operator mirrors; relay routes | Forge-managed | Operator remotes | Existing repository |
| Failover route | Designed and tested; no public drill | Provider HA | Scripts or DNS | No |
| Availability measurement | Public recomputable proof | Status page | None | No |
| Portable collaboration records | Signed, in-repository | Service database | Conventions | No |
| OS isolation | Optional Linux KVM | Runners vary | None | None |
| Human decision gate | Each continual cycle | Reviews | Manual | Provider approvals |

A raw mirror remains simpler when routing, portability and teammate workflow
are not needed.

## 8. Demo and launch operations

Demo script: show the fixture (stable URL, expected commit, two operators,
proof at `quorum`); shallow-clone and show `HEAD` with the redirect line
visible; recompute the proof; verify one signed event; remove the source from
eligibility; clone again and compare; restore. If a step fails, stop and use
the incident plan. Every frame needs a timestamp and commit; never splice runs.

### Incident plan

| Incident | Immediate action |
| --- | --- |
| Clone fails on every mirror | Say clone is down; stop the demo; probe each core node directly; record onset and recovery |
| Status green while Git fails | Use timestamped independent probes |
| Install, checksum or signature failure | Stop recommending the installer; capture hashes and the exact error |
| macOS or SmartScreen questions | State the signing status; link the manifest and support matrix |
| Security report | Private channel only |
| Heavy traffic or abuse | Use the relay limits; protect control endpoints |
| Wrong claim found live | Correct the surface and link evidence |

The [incident tabletop](incident-tabletop-2026-09-16.md) walked this plan,
listed the exact controls, and recorded the gaps: no rate limit on anonymous
Git reads, no public incident record, and one person holding every control.

## 9. HN compliance gate

- The submitter must personally attest they built the project and can discuss
  it, and confirm their account can post a Show HN.
- The link must be the runnable project, not a waitlist or announcement.
- The core demo needs no signup, payment or credential.
- Nobody may be asked to submit, upvote, comment, or coordinate engagement.
- A human writes and posts every submission and comment.

## 10. Verdict and ordered blockers

Verdict: **blocked**. "Dry-run ready" needs a full rehearsal with the released
artifact and a visitor-runnable fixture whose proof reads `quorum`. "Ready"
also requires all four launch gates below.

<!-- launch-gates:start -->
| Gate | Acceptance evidence | State 2026-09-30 | Owner |
| --- | --- | --- | --- |
| Second independent operator | `availability.proof == "quorum"` for `forkmesh/forkmesh` and `explain` exits 0, with the second copy on infrastructure and an account the current operator does not control | Not met: 3 copies, 1 operator | Mirror operations |
| Launch blockers closed | Every P0 row below closed with its evidence | Not met | Row owners |
| Try-it path verified | Dated show-path transcript from a clean environment plus one fresh Linux install, in launch week | Not met | Quality; launch owner |
| Human staffed and eligible | Poster confirms eligibility and authorship and names 24-hour thread and incident cover | Not met | Launch owner |
<!-- launch-gates:end -->

A second operator runs the headless mirror node (`install.sh` in direct-HTTPS
mode, see [mirror node](../operations/mirror-node.md)) under their own account,
links it within the link-code window, and carries every ref the cores carry.

### Tracked metrics

Re-measured every cycle; none gates the launch.

<!-- launch-goal-targets:start -->
| Target | Measure | Baseline 2026-09-30 | Gap | Evidence class |
| --- | --- | --- | --- | --- |
| Users | NodeInfo `usage.users.total` at `/nodeinfo/2.1` | 235 | Not a gate | Deployed observation |
| Open issues | Files under `.forkmesh/issues/open/` on `main` | 6, all readiness blockers | Only P0 issues gate | Repository evidence |
| Open tasks | Organization tasks in `/api/tasks` not `done` | 81 | Not a gate | Deployed observation |
| Ecosystem listings | Published projects at `/api/ecosystem` | 471 | Third-party listings, never evidence of users or adoption | Deployed observation |
<!-- launch-goal-targets:end -->

NodeInfo counts every account row, including unverified ones; quote which
count you mean.

| Priority | Blocker | Acceptance evidence |
| --- | --- | --- |
| P0 | Flagship below trusted quorum | Public proof reads `quorum` with two independent operators |
| P0 | No visitor-runnable failover proof or public collaboration log | Disposable public fixture; same `HEAD` before and after source removal; committed log passing `verify_collaboration.py --require-complete` |
| P0 | Show HN eligibility unknown; no thread cover | Human confirms eligibility and names cover |
| P1 | No clean-machine install log | Retained transcripts with hashes, first run, gate, Stop work, uninstall and dialog text |
| P1 | No status history or external Git-path probe log | 24-hour external probe log matched by status samples |
| P1 | Host-mode agents inherit secrets; no egress policy; no hard dollar cap | Environment allowlist for host launches, or quote the known gaps |
| P1 | Incident response rehearsed only on paper | Git-path rate limit; tabletop repeated with two responders |
| P2 | Page metadata says "decentralized" while the proof shows one operator | Remove the keyword or reach `quorum` |

The highest-leverage step is the first P0: a second independent operator turns
the product's own proof from "high risk" to `quorum`.
