# Y Combinator News launch readiness

Verdict: **blocked**

Evidence refreshed: **2026-09-30 America/New_York** (probes 23:12–23:16 UTC)

Previous refresh: 2026-09-16, with release rows re-measured 2026-09-24 and the
homepage re-fetched 2026-09-28

Signed `latest` at this refresh: `v1.2.7` (`checksums_sha256`
`6c9f75425309e01e9f42433c95f97a2ac4c10674a53a06acfde76315a7cb2ae5`). Rows still
naming `v1.2.4` or `v1.2.5` are dated measurements of earlier releases.

Owner: Y Combinator News Agent (`.forkmesh/teammates/y-combinator-news-agent.json`)

This is an evidence packet and rehearsal guide for a human launch owner. It is
not submission or comment copy. Hacker News explicitly says not to post
generated or AI-edited text, so a human must write and personally post the
submission and every reply.

## Status since 2026-09-16

| Area | 2026-09-30 state | Evidence |
| --- | --- | --- |
| Serving topology | **Changed: the flagship is now served by the three ForkMesh Next core nodes** | `git ls-remote https://forkmesh.com/forkmesh/forkmesh` redirects to `n1.next.forkmesh.com`; the mirrors payload lists `fmnext-n1`–`fmnext-n3`, all owned by user `jett`. `mirror35`–`mirror38` no longer appear |
| Preservation proof | **Still below quorum** | `forkmesh_availability_proof.py verify` reproduced the report (exit 0); `explain` says 3 of 3 copies verified by 1 independent operator (target 2), risk 45 of 100 (high), and exits 3 |
| Published release | **`v1.2.7` is `latest`; blobs, signature and Authenticode re-measured** | Deployed manifest bytes equal the repository copy; Ed25519 signature verifies; four blobs match `blob_sha256`; setup executable signed by the pinned certificate with a timestamp of 2026-10-01 20:23:58Z |
| Two macOS DMG names, one digest (#704 remainder) | **Explained: the DMG is universal** | The extracted `ForkMesh.app/Contents/MacOS/ForkMesh` and `QtCore` are Mach-O universal binaries (`x86_64` and `arm64`), so both names correctly label the same file. Gatekeeper, notarization and a first run are still unmeasured |
| Readiness tests | **Were failing on main; fixed on this branch** | `app/tests/test_y_combinator_news_readiness.py` failed two tests on main because the report and clean-machine page still cited `v1.2.4`'s checksum |
| HN rules | **Tightened** | Show HN now warns against “quickly-generated one-offs”; the guidelines now forbid generated text in submissions and automated posting, not only in comments. `showlim` restriction unchanged |
| Open issues on main | **6 open**, all readiness blockers (#702, #703, #707–#710) | #701 and #704 closed by merged fixes. #705 and #706 were closed by `jett` with no stated reason, and their acceptance evidence is still missing (section 10) |
| Users | **235, down 5 from 240** | NodeInfo `usage.users.total` at 23:13Z |
| Open tasks | **81, down from 204; 20 on 2026-10-06** | Authenticated `list_tasks` `status: open` at 23:12Z; on 2026-10-06 at 05:06Z, 20 open of 34 live after 180 deletions (section 10 lists them with owners) |
| Launch goals | **Replaced by four launch gates** | At the human owner's request, 1,000/2,000 users, all issues and all tasks became tracked metrics; section 10 lists the gates |
| Ecosystem directory | **471 published listings, up from 29** | The 406-listing `tools/ecosystem_third_parties.json` had never been imported; the signed import ran at 23:27Z (435). The 36 comparable HN projects in this report followed at 23:35Z from `tools/ecosystem_hn_launches.json` |

## Evidence method and baseline

Every conclusion uses one of these labels:

- **Measured fact** — reproduced from this checkout or this host during this review.
- **Repository evidence** — supported by a named current-main file or test, but
  not necessarily present in a published build.
- **Deployed observation** — observed on a public URL during this review; it is
  a point-in-time observation, not an uptime guarantee.
- **Inference** — a conclusion drawn from identified evidence.
- **Missing proof** — required evidence that this review could not establish.

HN pages were retrieved either through a summarizing fetch tool or as raw HTML
with the comment text extracted by a script. Comment characterizations below
are paraphrases. The human must reopen a thread before relying on any quotation
from it.

The direct retrieval ledger for this cycle contains eight required HN URLs:
eight requested, eight successful, zero rate-limited, and zero otherwise failed.
No fallback route was needed for these eight, so none was attempted or counted.
Point and comment counts were cross-checked against the HN Algolia items API
and the raw item HTML; that cross-check is a second read of a successful URL,
not a fallback.

<!-- hn-required-ledger:start -->
| Required URL requested 2026-09-30 | Direct result | Fallback outcome | Final evidence status |
| --- | --- | --- | --- |
| [Show HN rules](https://news.ycombinator.com/showhn.html) | Successful | Not needed | Refreshed this cycle; wording tightened (see below) |
| [HN guidelines](https://news.ycombinator.com/newsguidelines.html) | Successful | Not needed | Refreshed this cycle; generated-text rule now also covers submissions |
| [20+ coordinating agents](https://news.ycombinator.com/item?id=46990733) | Successful | Not needed | Refreshed this cycle; 53 points, 39 comments |
| [Ticket-to-PR orchestration](https://news.ycombinator.com/item?id=47520220) | Successful | Not needed | Refreshed this cycle; 88 points, 60 comments |
| [MicroVM coding-agent isolation](https://news.ycombinator.com/item?id=47877726) | Successful | Not needed | Refreshed this cycle; 63 points, 3 comments |
| [Desktop worktree orchestration](https://news.ycombinator.com/item?id=47303711) | Successful | Not needed | Refreshed this cycle; 6 points, 12 comments |
| [Autonomous company agents](https://news.ycombinator.com/item?id=47281538) | Successful | Not needed | Refreshed this cycle; 4 points, 2 comments |
| [Local goal-driven development](https://news.ycombinator.com/item?id=47153941) | Successful | Not needed | Refreshed this cycle; 36 points, 22 comments |
<!-- hn-required-ledger:end -->

### Freshness and retrieval ledger — 2026-09-30

Each row satisfies requested = direct success + rate-limited + other failure,
which `app/tests/test_y_combinator_news_readiness.py` checks. “Direct” is the
first retrieval attempt for a URL, whichever tool made it; “fallback” is a
second route tried only after a direct failure. For route probes, a success is
a completed request with the expected product response. An HTTP error, DNS
error, or `not found` counts as another failure. Unexpected content, such as a
redirect, still counts as a success.

<!-- hn-freshness-ledger:start -->
| Evidence class | Requested | Direct success | Direct rate-limited | Direct other failure | Successful fallback | Final freshness statement |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Required HN rules and six baseline threads | 8 | 8 | 0 | 0 | 0 | All eight refreshed |
| HN Show restriction notice (`showlim`) | 1 | 1 | 0 | 0 | 0 | Restriction still in effect; no threshold published |
| HN Algolia searches for newer comparables (posted after 2026-09-16) | 25 | 9 | 0 | 16 | 16 | 16 first attempts got HTTP 400 because the `>` in the date filter was not URL-encoded; all 16 succeeded re-sent with `%3E`. No search on Git mirroring, decentralized Git, or portable signed issues found a relevant post in the window |
| Newer comparable item fetches (Drop, Foremerge, Rickub, Agentgit, Skillsync) | 5 | 5 | 0 | 0 | 0 | Skillsync fetched but not analysed |
| ForkMesh claim surfaces (homepage, `/desktop`, `/docs/product-promise`, `/install.sh`, `/api/status`, `/nodeinfo/2.1`) | 6 | 6 | 0 | 0 | 0 | `/docs/product-promise` answered 308 to its trailing-slash form, which returned 200. Point-in-time text; JavaScript-rendered cards were not observed in a browser |
| Status history (`/api/status/history`) | 1 | 0 | 0 | 1 | 0 | 404: no public status history exists |
| Signed manifest, signature and `SHASUMS256.txt` from the homepage's manifest URL | 3 | 3 | 0 | 0 | 0 | Byte-identical to the repository copies; signature verified |
| Flagship availability proof (`/api/repo/forkmesh/forkmesh/mirrors`) plus CLI `verify` | 1 | 1 | 0 | 0 | 0 | Recomputed locally and matched the payload |
| Git route probes (`ls-remote`, depth-1 clone, `ls-remote` direct to `n1`–`n3`) | 5 | 5 | 0 | 0 | 0 | All passed 23:12–23:16Z; no outage window observed |
| Release blob downloads (`releases/blob/sha256/…`) | 4 | 4 | 0 | 0 | 0 | Linux binary, mirror node, Windows setup, macOS DMG: every SHA-256 and size matched the manifest |
| Agent `list_tasks` read with `status: open` through the operator desktop signed in as `jett` (23:12Z) | 1 | 1 | 0 | 0 | 0 | 81 open ForkMesh tasks, below the 2,000-task read limit, so the list is complete |
| Ecosystem API reads (`/api/ecosystem` before and after the import) | 2 | 2 | 0 | 0 | 0 | 29 published before, 435 after |
| Signed ecosystem imports (`POST /api/ecosystem/import` via `tools/ecosystem_seed.py --node jett`, 23:27Z and 23:35Z) | 2 | 2 | 0 | 0 | 0 | 406 then 36 added, 0 updated, 0 unchanged; no live slug overlapped either file |
| Project and repository URLs for the 36 HN listings | 37 | 37 | 0 | 0 | 0 | All resolved; Forkbench's HN link is broken, so its homepage was used |
<!-- hn-freshness-ledger:end -->

Earlier cycles' ledgers (2026-09-12 and 2026-09-16) are in this file's Git
history; each cycle's table replaces the last so that no row can be read as
fresher than it is.

Probe bodies, downloaded blobs, HN HTML, and the clone under
`/var/tmp/fm-hn-2026-09-30` are ephemeral scratch and are not committed. The
counts above are the retained record.

### Current HN constraints

- **Deployed observation (2026-09-30):** The [Show HN rules](https://news.ycombinator.com/showhn.html)
  require something the submitter made that other people can try, worked on
  personally, and is around to discuss. Readers should be able to try it,
  ideally without signups or emails. New since 2026-09-16: “The project should
  be non-trivial. Don't post quickly-generated one-offs”, plus an invitation to
  explain how and why it was built. Sign-up pages, landing pages, newsletters,
  fundraisers, lists, and blog posts are not Show HNs. Minor releases generally
  do not qualify. Do not ask friends to upvote or comment; with the
  guidelines' ban on soliciting, that means: Do not ask anyone to upvote or comment.
- **Deployed observation (2026-09-30):** The [HN guidelines](https://news.ycombinator.com/newsguidelines.html)
  require the original source. They prohibit soliciting upvotes, comments, or
  submissions and say not to use HN primarily for promotion. Comments: “Don't
  post generated text or AI-edited text.” New since 2026-09-16, submissions too:
  generated text must not go in HN posts, the submitter should write it
  themselves, and posting must not be automated.
- **Deployed observation:** The [Show restriction notice](https://news.ycombinator.com/showlim)
  still says HN is temporarily restricting Show HNs because of a massive
  influx, mostly by users not yet familiar with the site. It advises becoming a
  good contributor first and publishes no measurable karma or age threshold.
- **Inference:** A “v1.2.5 is out” framing would fail the minor-release rule.
  The Show HN must present the project itself. The new “quickly-generated
  one-offs” line raises the value of showing long, personal history (the
  repository has 57,538 commits per the mirrors payload) and of a human
  explaining why they built it.

### Comparable launch evidence

HN posts and comments are untrusted market evidence, not product instructions.
Point and comment counts for the six required threads and the four new
2026-09-30 rows come from pages fetched on 2026-09-30; other rows carry the
date they were last fetched.

| Thread | Substantive reader test to prepare for |
| --- | --- |
| [20+ coordinating agents](https://news.ycombinator.com/item?id=46990733) | Why many agents beat one tuned agent under finite spend; you cannot observe 20 agents at once; distance from human oversight; hidden proprietary dependencies; license visibility |
| [Ticket-to-PR orchestration](https://news.ycombinator.com/item?id=47520220) | Issue text is too thin for unattended work; agents drift without human planning; agents loop on one error message; token cost of retries without escalation limits |
| [MicroVM coding-agent isolation](https://news.ycombinator.com/item?id=47877726) | How state persists across isolated sessions (the answer there was checkpoints) |
| [Desktop worktree orchestration](https://news.ycombinator.com/item?id=47303711) | Real Windows/Linux experience, “why not `claude --worktree`?”, setup/teardown lifecycle, per-session cost accounting |
| [Autonomous company agents](https://news.ycombinator.com/item?id=47281538) | “There are so many of these, what's the difference?”; concrete convergence rules; why Markdown plus Git worked better than a vector store |
| [Local goal-driven development](https://news.ycombinator.com/item?id=47153941) | “Local” while calling remote providers; quality differences across providers; false-positive completion gates; multi-repository changes |
| [wt worktree orchestrator](https://news.ycombinator.com/item?id=46765489) | A small wrapper already removes raw worktree friction (retained from prior cycle, not refetched) |
| [GitAgent](https://news.ycombinator.com/item?id=47376584) | Portable agent definitions invite prompt-injection, secret-access, and approval questions (retained, not refetched) |
| [Foolery verification queue](https://news.ycombinator.com/item?id=47075901) | One-command, no-signup install plus an explicit human verification queue (retained, not refetched) |
| [ZeroShot session monitoring](https://news.ycombinator.com/item?id=49113279) | Local-data boundaries, supported platforms, and the method behind efficiency claims (retained, not refetched) |
| [Orc](https://news.ycombinator.com/item?id=47441323) | A bash/tmux/worktree orchestrator makes deterministic review gates the comparison point (retained, not refetched) |
| [Sandbox Agent SDK](https://news.ycombinator.com/item?id=46795584) | Session portability and transcript ownership (retained, not refetched) |
| [Shikigami](https://news.ycombinator.com/item?id=48966140) | No-account, resumable local sessions (retained search-snapshot signal only) |
| [metaswarm](https://news.ycombinator.com/item?id=46864977) | Unverifiable volume claims earn distrust, not credit (retained, not refetched) |
| [Clawk](https://news.ycombinator.com/item?id=48892859) | Differentiation from Docker/Vagrant/VM tools, credential handling, egress filtering, rollback snapshots (retained, not refetched) |
| [Codelegate](https://news.ycombinator.com/item?id=47357070) | Opened with the crowded-category objection; the author answered with specific workflow constraints, platform support, and a visible GPL license (retained from 2026-09-01, not refetched) |
| [Docker agent sandboxes](https://news.ycombinator.com/item?id=49239751) | Readers asked for measured filesystem-performance numbers instead of broad microVM claims; publish no isolation-performance claim without a reproducible comparison (retained from 2026-09-01, not refetched) |
| [md² feature/worktree coordination](https://news.ycombinator.com/item?id=49258045) | 2 points, 1 comment: worktree coordination alone draws little attention (retained) |
| [MindFlock](https://news.ycombinator.com/item?id=49102636) | 4 points, 5 comments, all about the name (retained) |
| [Trunks](https://news.ycombinator.com/item?id=47933429) | Git repositories backed by your own storage for short-lived agent sandboxes; the closest preservation-adjacent Show HN found (retained) |
| [Leaving GitHub for Forgejo discussion](https://news.ycombinator.com/item?id=48121439) | Moving Git bytes is easy; moving issues, PRs, CI, and releases is hard; social discovery is the real lock-in (retained) |
| [Grov – multiplayer for coding agents](https://news.ycombinator.com/item?id=46711958) | New this cycle: 24 points, 8 comments. One reader listed prior tools that tried shared agent memory and abandoned it; another comment was a moderator-flagged stub. Expect “this was tried and dropped, what is different?” |
| [Pizza Bot – inbox for background agents](https://news.ycombinator.com/item?id=49713894) | New this cycle, posted 2026-09-15: 56 points, 33 comments. Readers asked why a desktop app rather than a self-hosted service that runs with the computer off, how it differs from wiring an agent to a ticket tracker, whether sandboxing exists (the author deferred it until after launch and said so), and why depend on a framework at all. Candid limitation answers were well received |
| [Amika – multiplayer cloud workstations](https://news.ycombinator.com/item?id=49716282) | New this cycle, posted 2026-09-15: 7 points, 5 comments. Platform coverage (Linux-only VMs, no macOS) was the first question |
| [Drop – rootless Linux sandbox with gVisor](https://news.ycombinator.com/item?id=49801329) | New 2026-09-30, posted 2026-09-22: 193 points, 63 comments, well received. Readers asked how it differs from bubblewrap or Docker with `runsc`, why gVisor rather than a microVM and at what performance cost, and exactly where the filesystem boundary sits; the author answered with the documented mount table. Expect the same precision about what ForkMesh host mode does not isolate |
| [Foremerge – intent conflicts between parallel agents](https://news.ycombinator.com/item?id=49789356) | New 2026-09-30, posted 2026-09-21: 45 points, 20 comments. Readers doubted agents can declare in advance what they will change and asked when a conflict should stop an agent; the author called claims advisory with a hard stop only on high-severity signals. Expect “what happens when two teammates touch the same file?” |
| [Rickub – EU-hosted GitHub alternative](https://news.ycombinator.com/item?id=49755312) | New 2026-09-30, posted 2026-09-18: 25 points, 20 comments, hostile. Called “a GitHub alternative, not a Git alternative” and doubted for an AI-generated-looking landing page. One unanswered comment: when leaving GitHub, the bottleneck was the dependencies still living there. The closest recent post to ForkMesh's preservation story, and a warning about marketing-heavy links |
| [Agentgit – Git host for agents, no account](https://news.ycombinator.com/item?id=49761528) | New 2026-09-30, posted 2026-09-18: 10 points, 6 comments. Unanswered questions about use cases and about abuse and spam without accounts. ForkMesh's anonymous clone invites the same abuse question; the tabletop found no Git-path rate limit |

**Inference:** the recent threads reward the same things: name the boundary
you did not build before a reader finds it, answer “why not the simpler thing?”
with a concrete mechanism, and link something runnable rather than a landing
page. Rickub shows a Git-hosting post read as marketing gets a hostile
reception. ForkMesh's availability proof is a concrete mechanism, and it
currently says the flagship repository is below quorum. That stays the honest
headline until a second operator exists.

### Fresh stock-Git matrix run

**Measured fact (2026-09-30, rerun this cycle):** `python3 tools/git_conformance.py validate`
and `render --check` passed for suite `forkmesh-stock-git 1.0.0`, run with
system Git 2.47.3 on Linux x86_64:

| Run | Result | Coverage |
| --- | --- | --- |
| `run --transport http --protocol-version 0` | Passed | 15/15 operations, 39/39 cells |
| `run --transport http --protocol-version 2` | Passed | 15/15 operations, 39/39 cells |

These results validate stock-Git compatibility against the repository's
disposable loopback fixture. They do not establish public relay availability or
failover between independent mirrors.

## 1. Precise technical position

**Repository evidence plus measured route behavior:** ForkMesh is a
Git-compatible routing and collaboration layer. A stable HTTPS clone URL
redirects stock Git to a registered mirror with fresh signed health and matching
refs. Issues, pulls, and patches are stored inside the repository, and every
repository publishes a recomputable availability proof that says how many
independent operators hold a verified copy. The narrow reason to care: clone
availability and project history are designed not to depend on one host, and
the product measures whether that is true rather than asserting it.

**Measured fact (2026-09-30 23:12:30Z):** `git ls-remote https://forkmesh.com/forkmesh/forkmesh`
got a redirect to `n1.next.forkmesh.com` and returned
`a565f79017072d9d853ccf999bfd88408b9dfef0` for `HEAD` and `refs/heads/main`,
matching main in this checkout at that moment. At 23:15Z all three core nodes
answered a direct `ls-remote` with the next main commit, `b3c05d8b7`.
**Measured fact:** the flagship availability proof reports three verified
copies on `fmnext-n1`, `fmnext-n2` and `fmnext-n3` (the three ForkMesh Next core
nodes, each registered to user `jett`), one independent operator, target two,
`proof: partial`, risk 45 of 100 (high), top risk “Below trusted quorum”. The
four `mirror35`–`mirror38` nodes measured on 2026-09-16 are no longer in the
proof. **Inference:** the copies that serve clones are the operator's own
core nodes, so the “independent hosts” clause is a design goal the flagship
repository does not yet demonstrate, and the product's own proof says so.

| Skeptical question | Evidence-based boundary |
| --- | --- |
| Why not GitHub/GitLab? | Those are full forges with proven reliability and integrations. ForkMesh's narrower bet is that clone routing and collaboration records need not belong to one forge. The Forgejo thread shows readers find issue/PR portability the hard part. ForkMesh has no parity claim on reliability, integrations, or support. |
| Why not a raw Git remote or `git clone --mirror`? | Raw Git preserves bytes. ForkMesh adds one URL that routes among health- and ref-checked mirrors, in-repository collaboration records, and a proof that counts independent operators. The tradeoff is relay, identity, health, and operator complexity, and the relay is a routing dependency. |
| Why not Claude/Codex worktrees? | Provider worktrees separate branches. The released v1.2.7 desktop (Linux x86_64, Windows x86_64, and a universal macOS DMG) adds a queue, provider/model routing, transcript/diff/cost views, repository-owned teammate definitions, and a human decision gate each continual cycle. |
| Why another agent orchestrator? | Agent count is not the differentiator, and recent worktree-orchestrator Show HNs drew almost no discussion. Lead with preservation, the availability proof, and portable records; agents are supporting machinery. |

## 2. Five-minute no-signup show path

### Anonymous clone plus recomputed availability proof — measured

Prerequisites: stock Git, Python 3, HTTPS egress, about 300 MB of disk for a
depth-1 checkout. No ForkMesh account or desktop install.

```sh
demo_dir=$(mktemp -d /tmp/forkmesh-show.XXXXXX)
git ls-remote https://forkmesh.com/forkmesh/forkmesh HEAD refs/heads/main
git clone --depth 1 https://forkmesh.com/forkmesh/forkmesh "$demo_dir/forkmesh"
git -C "$demo_dir/forkmesh" rev-parse HEAD
git -C "$demo_dir/forkmesh" fsck --no-progress
ls "$demo_dir/forkmesh/.forkmesh"
curl -fsS https://forkmesh.com/api/repo/forkmesh/forkmesh/mirrors > "$demo_dir/mirrors.json"
python3 "$demo_dir/forkmesh/tools/forkmesh_availability_proof.py" verify "$demo_dir/mirrors.json"
python3 "$demo_dir/forkmesh/tools/forkmesh_availability_proof.py" explain "$demo_dir/mirrors.json"
printf 'Disposable checkout: %s\n' "$demo_dir"
```

- **Measured fact (2026-09-30 23:12:30–23:12:55Z):** `ls-remote` returned
  `a565f79017072d9d853ccf999bfd88408b9dfef0` for `HEAD` and `refs/heads/main`.
  The depth-1 clone took 23 s from this host and produced 250 MB (195 MB on
  2026-09-16; the prerequisite below now says 300 MB). `fsck` reported
  nothing. `LICENSE` is MIT. `.forkmesh/` carries `issues`, `discussions`,
  `projects`, `teammates`, `releases`, `skills`, and more; pulls are served
  from the immutable `forkmesh/pulls` branch.
- **Measured fact (23:13:06Z):** the mirrors payload's `availability` block
  recomputed locally with `verify` and matched the published report (exit 0).
  `explain` printed “Below quorum: Verified, but not by enough independent
  operators. 3 of 3 copies verified by 1 independent operator (target 2).
  Risk 45 of 100 (high).” and exited 3, the documented below-quorum code.
- **Measured fact:** each of the three copies reports `source: remote-clone`,
  `integrity: ok`, a sync about 12 s old, and version `1.2.5`. The registered
  working-copy node named `forkmesh` is listed as the source of truth but is
  no longer a reference for the mirrors (`referenceForMirrors: false`).
  **Inference:** the core nodes pull from the operator's push path, so a reader
  asking “how do the copies get new commits?” should hear that directly.
- **Inference:** this path now lets a visitor check the product's central
  claim against the product's own arithmetic without trusting the dashboard.
  What it shows today is that the claim is not yet met for the flagship
  repository. That is honest and defensible, but it is not a demo of success.
- **Missing proof:** a signed collaboration record that a reader can verify
  offline from a clone. The exporter and verifier now ship (next subsection),
  but no public repository has committed a policy and log yet.
- **Cleanup:** the command prints the one disposable path. Inspect it before
  removing that exact directory; the script does not delete recursively.

### Clone-side signed-record verification — tooling shipped, public fixture pending

`tools/export_collaboration_log.py` (#706) turns a checkout's
`.forkmesh/issues` records into `.forkmesh/collaboration/policy.json` and
`.forkmesh/collaboration/issues.ndjson`. The repository authority's Ed25519 seed
signs one importer `create` event per issue and is the policy's only
maintainer. Import events never claim a legacy author signed them. Each one
binds the SHA-256 of the exact record file and says whether every legacy event
signature in it verified (`valid`), any failed (`invalid`), or some event was
unsigned (`unavailable`). Records the v1 protocol cannot hold, such as a title
over 512 bytes, are listed as `skipped` rather than truncated.

Owner, once, in the disposable public repository (the seed never leaves the
owner's machine and is never committed):

```sh
python3 tools/export_collaboration_log.py keygen ~/.config/forkmesh/<repo>-authority.seed
python3 tools/export_collaboration_log.py export \
  --repository forkmesh.com/<owner>/<repo> \
  --signing-key ~/.config/forkmesh/<repo>-authority.seed --checkout <repo-checkout>
git -C <repo-checkout> add .forkmesh/collaboration
git -C <repo-checkout> commit -m "Export signed collaboration log"
```

Visitor, no account (the verifier comes from the flagship clone in the show
path above; it needs only Python 3):

```sh
git clone --depth 1 https://forkmesh.com/<owner>/<repo> "$demo_dir/fixture"
python3 "$demo_dir/forkmesh/tools/verify_collaboration.py" --require-complete \
  "$demo_dir/fixture/.forkmesh/collaboration/policy.json" \
  "$demo_dir/fixture/.forkmesh/collaboration/issues.ndjson"
echo "exit=$?"
```

Exit 0 means every event's signature verified against the committed policy and
nothing is pending or rejected; 3 means something was rejected or pending (the
`audit` block names it); 2 means unreadable input. `--policy POLICY LOG` is the
equivalent flag form.

- **Repository evidence (local run, 2026-09-16):** the exporter over this
  repository's own 665 issue records, signed with a throwaway key kept in
  `/tmp`, imported 663 issues in 30 s and skipped #409 and #579 (titles over
  512 bytes). Legacy signature status: 575 `valid`, 26 `invalid`, 62
  `unavailable`. `verify_collaboration.py --require-complete` exited 0 in 5 s
  with `accepted: 663`, `rejected: 0`, `pending: 0`. The throwaway key is not
  the flagship's authority, so nothing from that run is committed and it proves
  the pipeline, not authorship.
- **Repository evidence (local rehearsal of the full path, 2026-09-27):** the
  681 issue records copied into a fresh local repository, exported with a
  throwaway `/tmp` key (679 imported in 41 s, #409 and #579 skipped; 583
  `valid`, 26 `invalid`, 70 `unavailable`), committed, pushed to a local bare
  repository, and fetched with `git clone --depth 1 file://…` (shallow, one
  commit). The visitor block's verifier exited 0 in 7 s with `accepted: 679`,
  `rejected: 0`, `pending: 0`; changing one field in the first log line made it
  exit 3. This shows the owner and visitor steps work as written. It is not the
  acceptance evidence, because the repository is local and the key is no
  repository's authority.
- **Measured fact:** of six `invalid` records spot-checked, three open events
  verify under an earlier issue number (#133 and #134 signed as 131, #140 as
  139) and three match no number from 0 to 999. **Inference:** renumbering an
  issue at merge invalidates its original open signature. The export reports
  that honestly instead of hiding it.
- **Repository evidence:** `app/tests/test_export_collaboration_log.py` pins
  the legacy content canonicalization to the Worker's `issue_event_content` and
  checks that the log verifies offline, that tampering exits 3, and that export
  is byte-deterministic.
- **Missing proof:** the disposable public repository, with the policy and log
  committed by its owner's authority key, and a fresh `git clone --depth 1`
  transcript of the visitor block above exiting 0. An agent cannot create a
  public repository or hold its owner's key, so a human owner must take this
  step.

### Distinctive preservation path — blocked

**Deployed observation (2026-09-30):** the homepage no longer has a “Run the
proof” link. `/docs/product-promise/` still exists; its steps need an account
and operator control to stop a `forkmesh-mirror-node`, and only the final clone
is runnable by a visitor.

The launch path stays blocked until a human can do this in under five minutes
from a clean machine:

1. Open a public fixture page showing the stable clone URL, expected commit,
   source mirror, a mirror run by a different operator, signed health freshness,
   and an availability proof reading `quorum`.
2. Clone from the stable URL and record `HEAD`.
3. Watch an operator-run drill remove only the source from eligibility; the
   visitor gets no host authority.
4. Clone the same URL into a second directory and compare `HEAD`.
5. Verify one signed issue or pull event from the clone with a shipped verifier
   and a committed policy, then watch the source recover.

Do not simulate failover with two local bare repositories; that proves Git, not
ForkMesh.

## 3. Release, install, verification, and cleanup

### Published versus current-main

| State | Measured evidence | Launch meaning |
| --- | --- | --- |
| Published `latest` | `v1.2.7`, manifest `created_at` 1790886238000 (2026-10-01). Manifest `tag_commit` and `build_commit` are both `6c906106f00c26a4bfd5e715efb837969c0ece8c` (“release: bump version to 1.2.7”, 2026-10-01) | The only release that may be called published |
| Tag `v1.2.7` | Peels to `6c906106f00c26a4bfd5e715efb837969c0ece8c` (2026-10-01, “release: bump version to 1.2.7”). The build commit is the same; a same-day release | Meets the `tag_commit` definition in `.forkmesh/releases/README.md` (`git merge-base --is-ancestor` passes). |
| Tag `v1.2.5` (previous) | Peels to `e1dd68653fa537bb7e9d7d33c454b5472312dd88` (2026-09-28, “publish v1.2.5 … macOS DMGs and promote to latest”). The build commit is its ancestor, 917 commits earlier | Superseded by `v1.2.7` |
| Stale candidate | `v0.7.95` in `candidate-0.7.95`; tag commit `6a38fef538359aac5a57349d4e66723a04cfbf91`; build commit `ccbd7af4c957799d28c77eedd2c2491328424d03` | Superseded and unpublished; do not cite |
| Current main at review | `590ecb8901` (2026-10-01 Merge 1280); 380 commits after the `v1.2.7` build commit | Newer than v1.2.7; not what the installer delivers |
| Human-gated continual loop | `96c5e6391` | Ancestor of the v1.2.7 build commit |

**Measured fact (2026-10-01):** `release.json`,
`release.json.sig` and `SHASUMS256.txt` fetched from the manifest URL the
homepage uses (`https://app.forkmesh.com/api/repo/forkmesh/forkmesh/raw?path=.forkmesh/releases/latest/…`)
are byte-identical to the repository copies. `release.json` passes
`openssl pkeyutl -verify` with the Ed25519 key embedded in
`www/public/install.sh`, and `sha256(SHASUMS256.txt)` equals the manifest's
`checksums_sha256` value,
`6c9f75425309e01e9f42433c95f97a2ac4c10674a53a06acfde76315a7cb2ae5`.

**Measured fact (2026-10-01):** four blobs downloaded from
`/api/repo/forkmesh/forkmesh/releases/blob/sha256/<hash>` matched their manifest
`blob_sha256` exactly for v1.2.7: the Linux x86_64 binary, the Linux
mirror node, the Windows setup executable (a PE32 Nullsoft installer), and
the macOS universal DMG (an APFS disk image). The AppImage, its zsync file,
and the Android APK were not downloaded. Hashes are listed in the
[clean-machine evidence](clean-machine-evidence.md#v127-release-checks).

### Availability and prerequisites

- **Measured fact:** the signed latest manifest publishes Linux x86_64 (bare
  binary, AppImage plus zsync, mirror node), Windows x86_64 (setup executable),
  macOS as `forkmesh-macos-arm64.dmg` and `forkmesh-macos-x86_64.dmg` (built by
  the Codemagic `macos-desktop` workflow, ad-hoc signed, not notarized), and an
  Android universal APK. There is no Linux arm64 or Windows arm64 asset.
  Build-workflow intent does not mean availability.
- **Measured fact (2026-09-30):** the two macOS names carry one digest
  (`96ecd3f1f76cc7dd47d1f3746dc6817dd4ac52c7e28aecb98672be9981396acb`)
  because the DMG is universal. `7z` lists the image as APFS, and
  `ForkMesh.app/Contents/MacOS/ForkMesh` and
  `Contents/Frameworks/QtCore.framework/Versions/A/QtCore`, extracted from it,
  are each a Mach-O universal binary with `x86_64` and `arm64` slices. Both
  labels are therefore correct; the download list shows two cards for one file.
  **Missing proof:** no Mac has mounted it, run `codesign --verify`, or
  recorded the Gatekeeper dialog.
- **Repository evidence:** `www/docs/install-support-matrix.md` lists the
  Windows package as Authenticode-signed with a self-issued certificate, so
  SmartScreen warns, and lists macOS DMGs as “Preview, not notarized”.
  **Measured fact (2026-10-01, v1.2.7):** `osslsigncode verify` on the
  downloaded setup executable shows a matching SHA-256 message digest, a
  verified timestamp, and a signer certificate whose SHA-256
  equals the release host's pinned Authenticode certificate. Verification fails
  against public roots (“self-signed certificate”) and succeeds when pinned to
  that certificate. See the
  [clean-machine evidence](clean-machine-evidence.md).
- **Repository evidence:** the installer defaults to
  `FORKMESH_NO_SOURCE_FALLBACK=1`, so `curl -fsSL https://forkmesh.com/install.sh | bash`
  on macOS fails rather than building; macOS users install the `.dmg`. Source builds are opt-in
  (`FORKMESH_FROM_SOURCE=1`) and need Qt 6.4+, CMake 3.16+, a C++17 compiler,
  OpenSSL, and Git (`www/docs/qt-client.md`).
- **Deployed observation (2026-10-01):** `https://forkmesh.com/`
  returned HTTP 200. Outside HTML comments, the hero reads
  “Install the desktop app Linux · macOS, Windows & Android downloads”, linking
  to the manifest-driven `#downloads` section, and the JSON-LD says
  `”operatingSystem”: “macOS, Windows, Linux, Android”`. Every one of those
  operating systems has a signed `v1.2.7` asset. No live mesh-activity ticker
  renders, and no “Run the proof” link remains.
- **Deployed observation (routed variants, 2026-09-28):** `https://forkmesh.com/homev2`,
  `/homepage-update`, and `/new-home` each returned HTTP 404 (a 10,297-byte error
  page). Those files are deleted from `www/public`, so the homepage is the single
  install surface a launch links to and no variant can drift from the manifest.
- **Deployed observation:** the live `/desktop` page is manifest-driven. It
  says no implicit source fallback is promised and each package appears only
  when the signed latest manifest includes it. v1.2.7 carries both macOS
  names, so both macOS cards should render; the rendered cards were not
  observed in a browser.
- **Repository evidence:** first launch creates a node identity, connects to the
  default mainnode, and begins syncing ForkMesh. Anonymous clone is separate;
  signed collaboration and mirror control require account/device setup.
- **Missing proof:** no retained clean-machine log covers install, launch,
  update, first-run, provider discovery, gate, pause, and uninstall for v1.2.7
  (or any earlier release) on Linux, Windows, or macOS. The
  [clean-machine evidence](clean-machine-evidence.md) page
  tracks each step, holds the measured release and Authenticode checks, and
  gives the fresh-VM procedure (`tools/clean_machine_transcript.sh run` on
  Linux). The steps cannot run on the deploy box: `uninstall.sh` stops every
  process named `forkmesh`, which includes the production source node.
- **Repository evidence:** MIT license at `LICENSE`, also present in the
  anonymous clone.

### Verification and removal

The installer verifies the Ed25519 manifest signature, binds `SHASUMS256.txt`
to the manifest, and checks the selected blob's SHA-256 before installing. A
fleet install can also pin `build_commit`. This cycle reproduced the signature,
binding, and four blob hashes by hand; it did not run the installer.

The uninstaller (`www/public/uninstall.sh`) removes the binary, launchers,
cache, and installer checkout, and keeps settings, the identity key, mirrored
repositories, and chat history. Only `--purge` erases that data, and it requires
an explicit `DELETE`, `--yes`, or `FORKMESH_ASSUME_YES=1`. Export configuration
first; deleting the identity can make the account unrecoverable. Put this
warning beside any launch-day purge instruction. See the
[installation support matrix](../install-support-matrix.md).

## 4. Reproducible claim ledger

| Candidate claim | Classification | Evidence or missing acceptance proof |
| --- | --- | --- |
| The flagship repository clones anonymously | Measured fact | Passed 2026-09-30 23:12Z through `n1.next.forkmesh.com`; the 2026-09-12 outage was not reproduced and no probe window was run |
| Availability is measured, not asserted | Measured fact | `forkmesh-availability-proof-v1` in the public mirrors payload; CLI `verify` matched on 2026-09-30; fixture 28/28 on 2026-09-16 |
| Independent operators hold the flagship repository | False today, by the product's own proof | 1 operator of target 2 (the three core nodes); `proof: partial`; risk 45, high |
| Status reflects clone availability | Unproven | Contradicted on 2026-09-12; no history endpoint (`/api/status/history` 404); no external probe log since |
| Source-offline failover returns the same commit | Repository evidence only | `www/docs/product-promise.md` plus mirror routing tests; no visitor-runnable drill |
| Collaboration records are portable and signed | Partly measured | Records travel in the clone with per-event signatures; `tools/export_collaboration_log.py` and `tools/verify_collaboration.py` work end to end locally, but no public repository has committed a log (missing proof) |
| Mirror eligibility uses fresh signed health and matching refs | Repository evidence plus deployed observation | `endpointFresh: true`, `integrity: ok` on all three serving copies; `directVerified: false` in the payload, although this review's direct `ls-remote` to each node matched |
| Desktop install works on Windows | Measured blob and signature only (v1.2.7) | Setup executable downloads and matches its hash; Authenticode digest and timestamp verify, signer is the pinned self-issued certificate; no clean-machine install log; SmartScreen dialog text not captured |
| Desktop install works on macOS | Measured blob and architecture only (v1.2.7) | One ad-hoc signed, not notarized universal (`x86_64` + `arm64`) DMG, published under both macOS names, matches its hash; Gatekeeper warns on first launch; no clean-machine install log |
| Human-gated continual teammates are released | Measured fact (ancestry) | `96c5e6391` is an ancestor of the v1.2.7 build commit `6c906106f…`; no released-binary smoke log |
| Stock Git protocol v0/v2 compatibility | Measured fact (fixture, 2026-09-30) | HTTP matrix 15/15 and 39/39 for both versions |
| Worktrees sandbox an agent | False | They separate Git working directories, not OS, filesystem, or network authority |
| KVM isolation protects the host | Repository evidence | Optional Linux Lima/QEMU/KVM path; no published-build clean-machine proof |
| Adoption or performance superiority | Missing proof | Make no claim |
| Decentralized or always available | Unsupported wording | Relay dependency, one operator, and the 2026-09-12 correlated outage |
| Homepage activity ticker events are real | Not claimed on the deployed page | Measured 2026-09-28 and again 2026-09-30: the `hero-cards-track` markup exists only inside HTML comments, so the ticker does not render |

### Deployed claim-surface audit

**Deployed observation (2026-10-01):** the live homepage says “Install the
desktop app Linux · macOS, Windows & Android downloads”, and every one of those
operating systems has a signed `v1.2.7` asset. The illustrative mesh-activity
ticker does not render: its markup survives only inside HTML comments, still
annotated “set dressing, not data”. The homepage no longer links a “Run the
proof” page, and `/docs/product-promise/` still needs an account and operator
control. Its meta keywords still
include “decentralized Git collaboration”, which the one-operator proof does
not support; that is page metadata, not visible copy, but a reader who views
source will see it. `/api/status` reported every system operational,
including `flagship_repository`, `git_hosting`, and `core:n1`–`core:n3`.

**Inference:** the claim-by-claim evidence ledger above is the review input for
the linked surface. A skeptical reader can check the macOS claim against the
signed manifest in a minute, and can check the operator count against the
mirrors payload in another. The launch owner should not repeat topology,
availability, or cross-platform wording in their own words until each has a
dated evidence link.

### Required evidence bundle before launch

- a flagship availability proof reading `quorum` with two independent
  operators, plus successful anonymous clone and same-commit failover logs
  from a disposable public fixture;
- a status history showing Git-path failures as failures (not green);
- signed latest manifest, signature output, checksum output, tag/build
  provenance that agrees, and downloaded binary hashes (blob hashes done);
- clean-machine logs for every platform named on the launch surface;
- a committed policy and NDJSON event log that a visitor can verify with
  `tools/verify_collaboration.py`;
- a known-limitations page and the MIT license;
- no unsupported adoption, speed, cost, security, decentralization, autonomy,
  platform, or availability claims on the linked surface.

## 5. Security and trust FAQ

### What can an agent read, change, execute, and send?

**Repository evidence:** In ordinary host mode the provider CLI runs as the
current OS user in an agent worktree. A worktree is not an operating-system sandbox;
it limits branch collision but does
not prevent reads elsewhere, subprocess execution, network access, or access to
credentials already available to that user. Claude Auto maps to
`--dangerously-skip-permissions`; Codex runs with `danger-full-access` in every
mode (a workspace-write sandbox blocks loopback ports), and Auto asks no approval. Plan/Ask behavior is delegated to the selected provider CLI. Teammate
permission lists are prompt and application workflow gates, not a kernel policy.

The optional agent jail (`desktop/src/AgentJail.h`) gives each session a
private `TMPDIR` and cache directory and a memory cap. It is a resource fence,
not a security sandbox: the agent still sees the filesystem.

Optional Linux KVM mode runs agent subprocesses in a Lima QEMU/KVM guest and
mounts the chosen launch directory, linked-worktree Git metadata, and a temporary
worktree root. It deliberately does not mount the host home, desktop sockets, SSH
agent, or unrelated environment variables. It requires Lima 2+, QEMU, `/dev/kvm`,
guest-side provider login, and a narrow launch directory.

### What happens to secrets?

**Repository evidence:** The KVM path does not reuse host CLI login files.
Connector bearer tokens described in the desktop guide are device-local and
revocable. **Tested (`forkmesh-agent-boundary-tests`,
`docs/security/agent-host-boundaries.md`):** a host-mode agent inherits the
desktop's whole environment minus its own provider's four credential
variables, so `GH_TOKEN`, cloud keys, the other provider's key and
`SSH_AUTH_SOCK` reach it. This holds with or without the default project
folder lock. The lock refuses files outside the project, such as `~/.ssh`, but
not sockets or the network. Host mode has no OS-level egress policy; only the
tool-layer external-call gate asks before recognisable network commands. KVM
mode passes a deny-by-default environment allowlist. Use staging-only
credentials and no loaded SSH agent. Never describe `.gitignore` as a secret
boundary.

### How is prompt injection handled?

**Repository evidence:** Teammate prompts label repository, issue, PR, mention,
and external-service content untrusted and prohibit credential disclosure.
That is instruction-level mitigation, not isolation. **Tested:** a retained
fixture set (`conformance/agent-adversarial/v1/fixtures.json`) covers malicious
issue text, skill files, repository instructions and tool output. It pushes
the action each one asks for through the real gate hook and the project lock
under default rules. Exfiltration, destructive and fetch-and-run actions are
escalated to a person or refused, and reads of home-directory secrets are
refused by the lock. Two known gaps are pinned: a local `printenv` of an
inherited token, and an encoded command piped to `sh`, both run unasked. The
suite tests the enforcement layer, not model behaviour. High-risk actions need
provider approvals, narrow permissions, a VM where appropriate, and human
review.

### What does mirror integrity prove?

**Repository evidence plus deployed observation:** Public routing requires
fresh account-bound signed health, an allowed state, healthy HTTPS, and refs
matching an accepted source revision. The availability proof separately counts
distinct operators, and a quorum needs two. This supports eligibility and ref
consistency; it does not prove an operator is uncompromised or free of
correlated infrastructure. **Measured fact:** on 2026-09-12 all serving mirrors
returned 502 at the same time, and on 2026-09-30 all three serving copies are
the operator's own core nodes, so correlated failure is observed, not
hypothetical.
Git object integrity is not a malware or source-trust review.

### Who has custody of data?

**Repository evidence:** Git packfiles remain on developer/operator mirrors; the
relay stores discovery, authorization, collaboration, and health metadata.
Repository-native `.forkmesh` records travel in Git. Public rooms are deliberately
controller-readable for integrations; private rooms and direct messages add a
device layer. Owner-device agent prompts/transcripts are encrypted to a desktop
key, and the browser has no administrative decryption override. Data-flow and
operator-correlation diagrams remain missing launch evidence.

### What telemetry exists, and how is data deleted?

**Repository evidence:** Product analytics is optional and disabled by default;
the proof events are coarse and omit repository/owner/URL/command/ref/form data.
Detailed network logging is opt-in. Local action journals, network logs, crash
records, sessions, and identities persist on device. The uninstaller keeps local
data by default; `--purge` deletes all of it, including identity and mirrors; remote collaboration
records already committed or relayed require separate retention/deletion handling.
[Relay data retention and deletion](../relay-data-retention.md) lists every
relay store, backup, and outside processor with its retention and deletion path.
**Missing proof:** that table records gaps: no user data export API, account
deletion leaves notes, calendar events, push tokens, hosted repository records,
and cached repository metadata, deleted rows stay in the nightly backups for
about 4 weeks, and the final D1 export has no documented retention or deletion.

### What still requires a human?

The continual design, released since v0.8.3 and present in the v1.2.7 build
commit, stops at a human decision every cycle. Product and repository permissions separately
govern code and external actions. Merge, release, production deploy, payout,
destructive cleanup, and public posting must remain explicit human-controlled
operations. ForkMesh has no authorization to write or post HN
submissions/comments, vote, or solicit engagement.

## 6. Agent-system FAQ

| Question | Current answer and limitation |
| --- | --- |
| What persists across cycles? | Repository-owned teammate definitions and learned notes live under `.forkmesh/teammates`; sessions/transcripts and runtime metadata persist in device application data; work is kept on a branch/worktree. A continual teammate resumes one conversation after the human answer. Retention/export details need one canonical table. |
| What stops a runaway loop? | Current main starts no next continual cycle until a human answers the decision card. Automatic work requires opt-in, runs only while the desktop is open, and **Stop work** pauses the loop. Released (build commit contains `96c5e6391`); no released-binary smoke log exists. |
| Is spend hard-capped? | No. The controls are a per-session cost estimate, a running-agent limit (default 5), a human gate per continual cycle and at most two relaunches. ForkMesh has no hard dollar ceiling in any provider or mode; only the provider account's own limits stop spend (`docs/security/agent-host-boundaries.md`). That blocks unattended-cost claims. |
| How many run concurrently? | The user selects the max-running-agent limit; excess work queues and lowering the limit parks newer runs first. The actual safe number depends on machine and provider quotas. |
| How are context and tokens handled? | Codex token-usage events and Claude stream events populate context/total-token/cost UI. Context/output settings are visible. Estimates are not invoices, and provider admin keys may be required for organization spend. |
| What is observable? | Live transcript, status, diff/branch, queue, token/context/cost summary, and local action journals are available. There is no proof that one view reconstructs every filesystem/network/tool action. |
| What retries? | A CLI that exits without a result is relaunched at most twice after the first attempt; then the session fails and preserves branch work. Credit-exhaustion messages can move the same session/branch through configured backup models. Transient service retry policies vary by provider. |
| What if branches conflict? | Worktrees prevent simultaneous writes to one directory, not logical overlap. PR mergeability/review must resolve conflicting branches; no “zero conflicts” claim is justified. |
| Why human gates? | They expose the next material decision before another cycle gains momentum, preserving scope and authority. They reduce but do not eliminate review or runtime risk. |
| How do I pause? | Choose **Stop work**; it stops the live agent and pauses the continual loop. Starting the mode again resumes it. Released; this still needs released-binary smoke evidence. |

## 7. Capability comparison without slogans

| Capability | ForkMesh boundary | GitHub/GitLab | Raw Git remote/mirror | Provider worktree | Agent orchestrator |
| --- | --- | --- | --- | --- | --- |
| Git byte storage | Operator mirrors; relay routes; today one operator | Forge-managed storage | Operator-selected remote(s) | Uses existing repository | Usually uses existing repository |
| Stable failover route | Designed and repository-tested; public drill missing; serving fleet failed together 2026-09-12 | Provider-managed HA | User scripts/DNS | No | Sometimes, if remote execution is included |
| Availability measurement | Public recomputable proof counting independent operators (measured) | Status page | None | No | No |
| Portable collaboration records | Signed repository-native records; exporter and offline verifier in the repository; no public repository has committed a log | Mostly service database/API | Manual conventions | No | Tool-specific task/session state |
| Branch isolation | Git worktrees | Branches/workspaces | Git branches/worktrees | Native worktree feature | Commonly worktrees/containers/VMs |
| OS isolation | Optional Linux KVM path; host mode is not isolated | Hosted runners vary | None inherent | None inherent | Varies widely |
| Human decision gate | Continual mode released in v1.2.7 (Linux x86_64, Windows x86_64, universal macOS); binary smoke missing | Reviews/rules | Manual | Provider approvals | Varies |
| Provider portability | Codex/Claude routes plus configured fallbacks | Product-specific agents vary | Not applicable | Provider-specific | Varies |
| Operational burden | Relay plus mirror health, identity, and recovery | Mostly outsourced | User-operated | Local session lifecycle | Scheduler/state/compute stack |

ForkMesh should be evaluated as preservation-aware collaboration infrastructure,
not as a claim that adjacent tools are wrong. A raw mirror remains simpler when
discovery, health routing, collaboration portability, and teammate workflow are
not needed.

## 8. Demo and launch operations

### Human-authored explanation outline

The human writes from these facts in their own words; this is a checklist,
not copy:

1. the personal problem and why keeping a repository alive across operators
   mattered;
2. the architecture boundary in one sentence: mirrors hold Git bytes, the
   relay routes and holds metadata;
3. the exact runnable clone and proof-recompute exercise, and the expected
   commit;
4. what is in the signed `latest` release versus current main;
5. the limitations that matter most: one mirror operator today, the relay
   dependency, worktrees are not an OS sandbox, and the platform coverage in
   the signed manifest;
6. the specific technical questions where firsthand feedback would help.

### Five-minute operator demo script

1. Show the fixture page. Name the source, the other-operator mirror, the
   expected commit, the stable URL, and the availability proof reading
   `quorum`.
2. In a clean terminal, shallow-clone and show `HEAD`. Leave Git's
   `warning: redirecting to …` line visible and explain it.
3. Recompute the availability proof from the public payload with the shipped
   CLI and show the operator count.
4. Verify one signed collaboration event from the clone with the shipped
   verifier and committed policy.
5. Remove only the source from eligibility using the prepared operator control.
   Show health and the proof changing while the other mirror stays eligible.
6. Clone the same URL again, compare commits, restore the source, and show
   recovery. If any step fails, stop and use the incident plan; do not narrate
   around the failure.

### Screenshot/video shot list

- stable repository URL, expected commit, and two mirrors run by different operators;
- availability proof at `quorum`, signed health freshness, matching refs, and eligibility before the drill;
- first anonymous clone, the redirect line, and exact `HEAD`;
- CLI `verify` output matching the dashboard;
- offline verification of one signed event;
- source ineligible while the second mirror stays eligible and the proof still reads `quorum`;
- second clone through the unchanged URL and commit comparison;
- source recovery, final healthy state, and a status page that recorded the drill;
- limitations card: relay dependency, one operator, worktree-not-sandbox, macOS as an ad-hoc signed, not notarized universal `.dmg`.

Every frame needs a timestamp and commit. Blur credentials and private host
data. Do not crop out failures or splice separate runs into one.

### Architecture diagram checklist

- clients and stock Git, including the 307 redirect to a mirror;
- stable app/clone route and the relay control-plane boundary;
- mirror operators, with the note that today's fleet is one operator;
- which component holds Git bytes versus metadata;
- signed health, ref, integrity, and operator-quorum eligibility flow;
- public/private collaboration storage and encryption boundaries;
- agent host worktree versus optional KVM boundary;
- human approval points and recovery path;
- telemetry/log destinations and deletion ownership;
- failure states for relay, all mirrors, one mirror, and monitoring.

### Likely questions and evidence locations

| Question prompt for the human | Evidence location |
| --- | --- |
| Why does `git clone` say “redirecting to n1.next.forkmesh.com”? | Section 1 measured route behavior; relay `x-forkmesh-served-by` header |
| Who runs the mirrors, and are they really independent? | Today: one operator, three core nodes, and the public proof says `partial`. Answer candidly; `www/docs/availability-proof.md` |
| Your own dashboard says the flagship repo is high risk? | Yes. Section 2 measured proof; the fix is a second operator, not a wording change |
| What survives if the original forge or account disappears? | `www/docs/product-promise.md`; mirror serving tests; no visitor-runnable drill yet |
| The registered source node is offline; how do mirrors get commits? | Section 2 `sourceOfTruth` observation; operator push path, not the registered node |
| Can I verify an issue or PR signature myself? | `tools/verify_collaboration.py` over a log from `tools/export_collaboration_log.py`; no public repository has committed one yet (section 2, missing proof) |
| Why isn't `git clone --mirror` enough? | Comparison matrix; signed health, ref routing, and operator-quorum evidence |
| How does this help move issues and PRs off a forge? | `.forkmesh/issues`, the `forkmesh/pulls` branch in the clone; Forgejo-thread objections |
| Your status page was green while clone failed? | 2026-09-12 outage measurement; no history endpoint; incident plan |
| Does it work on macOS or Windows? | Signed `latest/release.json`: Windows x86_64 yes (self-issued Authenticode, SmartScreen warns); macOS yes as one universal `.dmg` for Apple silicon and Intel (ad-hoc signed, not notarized, Gatekeeper warns); no clean-machine install log on either |
| Why trust the release? | Ed25519 verification, checksum binding, four matching `v1.2.7` blob hashes, Authenticode digest and pinned signer; `tag_commit` is the release-cut commit (`.forkmesh/releases/README.md`); clean-machine log still missing |
| Can an agent steal secrets or phone home? | Security FAQ; KVM guide; candid host-mode limitation |
| What prevents infinite spend or loops? | Human gate per continual cycle; concurrency limit; no universal hard dollar cap |
| What happens during conflicts or crashes? | Worktree/PR evidence, bounded relaunch, preserved branch |
| How do I delete it? | `www/public/uninstall.sh` plus the export and identity-loss warning |

### Incident plan

| Incident | Immediate action | Evidence and recovery |
| --- | --- | --- |
| Clone fails on every mirror (as measured 2026-09-12) | Say clone is down in the thread's own words; stop the demo; do not retry in a loop | Run `git ls-remote https://n1.next.forkmesh.com/forkmesh/forkmesh.git` (and `n2`, `n3`) to see which node fails; check whether a deploy or restart is running; record onset and recovery times |
| Status green while Git path fails | Do not cite the status page; use independent probes with timestamps | Reliability fixes status probes separately; never edit history to look green |
| Install/checksum/signature failure | Stop recommending the installer; capture OS, manifest hash, asset hash, exact error | Keep source build clearly separate; release engineering corrects after human approval |
| macOS reader tries the installer | Point to the `.dmg` (one universal build under two names); say it is not notarized and Gatekeeper will warn | Link the signed manifest; do not suggest the source build as equivalent |
| Windows SmartScreen warning | Say the certificate is self-issued and the warning is expected | Link the support matrix row; do not tell readers to disable SmartScreen |
| Security report | Do not ask for public details; direct the reporter to `security@forkmesh.com` | Acknowledge privately, preserve evidence, scope affected commits/releases, coordinate disclosure |
| Heavy traffic or abuse | Use the relay limits named in the tabletop record; protect control and drill endpoints first. No Git-path or public-API rate limit exists yet | Watch errors, latency, and capacity; disable the drill before weakening auth or integrity checks |
| Wrong or unsupported claim found live | Correct the factual surface; tell the thread's human owner | Link manifest, test, or limitation; never argue from roadmap intent |

**Repository evidence:** the [2026-09-16 incident tabletop](incident-tabletop-2026-09-16.md)
walked this matrix as a desk exercise, starting with the 2026-09-12 outage. It
names `jett` as incident lead, lists the exact rate-limit and reset controls,
and records nine findings. The biggest: no rate limit on anonymous Git or API
reads, no public incident record, and one person holding every mirror control.
**Missing proof:** the lead has not accepted the role, and no exercise with
people present has been held.

## 9. HN compliance gate

- **Repository evidence:** ForkMesh is non-trivial with a long implementation
  history. **Missing proof:** the submitter must personally attest they built it
  and can discuss it firsthand.
- **Deployed observation:** Show HNs are currently restricted
  ([showlim](https://news.ycombinator.com/showlim)). The human must confirm
  their account can post a Show HN immediately before launch. No threshold is
  published, so do not create or age an account for this.
- The submitted link must be the runnable original project or source, not a
  waitlist, fundraiser, pricing page, generated launch article, marketing-only
  page, or a version-bump announcement.
- The core demo must need no signup, email, payment, private credential, or
  infrastructure authority. Anonymous clone and the recomputed availability
  proof pass that gate; the distinctive failover proof does not yet.
- Nobody may be asked to submit, upvote, comment, or coordinate engagement.
- A human author must write and personally post every submission and comment,
  stay available, and answer from verified evidence in their own words. This
  document supplies facts and question prompts only.

## 10. Verdict and ordered blockers

Verdict: **blocked**. “Dry-run ready” requires a complete private rehearsal
with the released artifact and a visitor-runnable preservation fixture whose
proof reads `quorum`. “Ready” also requires all four launch gates below.

### Launch gates

Changed 2026-09-30 by the human launch owner: user-count, all-issues and
all-tasks targets no longer block the post. They were replaced with four gates
a launch can actually meet, and the old targets became tracked metrics.

<!-- launch-gates:start -->
| Gate | Acceptance evidence | State 2026-09-30 | Owner |
| --- | --- | --- | --- |
| Second independent operator | Live mirrors payload for `forkmesh/forkmesh`: `availability.proof == "quorum"` and `forkmesh_availability_proof.py explain` exits 0, with the second copy run by another account on infrastructure the current operator does not control | Not met: 3 copies, 1 operator (`jett`), exit 3 at 23:13Z | `mdkaifansari04` (Kaif) as operator; `jett` to confirm; #702 |
| Launch blockers closed | Every P0 row in the table below closed with its acceptance evidence | Not met: #702, the public collaboration-log fixture, and Show HN eligibility are open | Row owners |
| Try-it path verified | Dated show-path transcript from a clean environment plus one fresh Linux install of the signed `latest`, both in launch week | Not met: show path measured 2026-09-30 from the deploy box, not a clean environment; no fresh-machine install log | Quality; human launch owner |
| Human staffed and eligible | The poster confirms Show HN eligibility and authorship and names 24-hour thread and incident cover | Not met: `showlim` restriction in effect; no one named | Human launch owner |
<!-- launch-gates:end -->

### Second operator: what `mdkaifansari04` has to do

**Repository evidence (code read 2026-09-30, not yet run against the live
stack):** the proof counts operators by `ownerUser`, then `owner`, then node
name (`app/src/availability_proof.py`, `_operator_key`), excluding the
source's operator. Linked to Kaif's account, one verified full copy gives
`{jett, mdkaifansari04}`, which meets the target of 2. There is no admin
approval step for a mirror. The path that reaches `verified` today is the
headless mirror node installed by `install.sh` in direct-HTTPS mode; the
desktop “Mirror” button copies the repository locally but registers no
endpoint, so it never counts. Runbook: `www/docs/operations/mirror-node.md`
and `www/docs/operations/direct-https-mirror-gateway.md`.

1. **A machine Kaif controls:** a Linux x86_64 or arm64 VPS with systemd and
   root, paid for and administered by Kaif, not by `jett`. The repository is
   about 4.6 GB; plan for 15 GB or more free (estimate; preflight only
   hard-fails under 512 MiB).
2. **A public hostname:** a DNS-only A/AAAA record straight at the VPS (no
   proxy), and ports 80 and 443 open. The installer sets up Caddy and TLS; no
   Cloudflare token is needed.
3. **Install as root:**
   `FORKMESH_NODE_NAME=<name> FORKMESH_PUBLIC_HOSTNAME=<host> bash install.sh`.
   The name must match `^[a-z][a-z0-9-]{0,62}$`, not be an organization name,
   and not reuse a retired name (`mirror2`…`mirror38` in
   `STATUS_RETIRED_MIRRORS`). The installer verifies the signed release,
   mirrors `forkmesh/forkmesh`, registers the signed HTTPS endpoint, and
   prints a `FORKMESH LINK CODE`.
4. **Link the node to `mdkaifansari04` within 30 minutes** (`LINK_CODE_TTL_MS`),
   from Kaif's signed-in desktop, or pre-mint a code with
   `POST /api/accounts/link-install` and pass it as `FORKMESH_LINK_CODE`. An
   unlinked node falls back to its node name as the operator, which the proof
   would count but which #702 does not accept.
5. **Wait for activation:** the router challenges `/health` with a nonce and
   needs a signed proof whose refs digest matches the cores'. Receipts renew
   every 4 minutes and count for 10.
6. **Check:** press the Mirrors-tab check (or
   `POST /api/repo/forkmesh/forkmesh/mirrors/<name>/reachability`), then
   confirm the row shows `ownerUser: mdkaifansari04` and run
   `tools/forkmesh_availability_proof.py explain` over the live payload; exit
   0 meets the gate.

**Unconfirmed risks:** a row is `behind` when its all-refs state differs from
the freshest core's, so the node must carry every ref the cores carry (agent
and PR branches too), not just `main`. The website claim flow probably does not
work for headless nodes, so use the link code. Git clones on Next still
redirect only to the cores, so Kaif's copy adds preservation, not clone
serving. A reader may also ask whether a co-maintainer is “independent”; the
honest answer is a second person on separate infrastructure and a separate
account, not an unrelated organization.

### Tracked metrics

Re-measured every cycle; none of these gates the launch.

<!-- launch-goal-targets:start -->
| Target | Measure | Baseline 2026-09-30 | Gap | Evidence class |
| --- | --- | --- | --- | --- |
| Users | Public NodeInfo `usage.users.total` at `https://forkmesh.com/nodeinfo/2.1` (`SELECT COUNT(*) FROM users`) | 235 at 23:13Z; 240 on 2026-09-16 and 2026-09-12 (down 5 in two weeks) | Not a gate; 1,000 and 2,000 are post-launch goals | Deployed observation |
| Open issues | Files under `.forkmesh/issues/open/` on `main` | 6 open at `b3c05d8b7` (23:15Z): #702, #703, #707, #708, #709, #710, every one a readiness blocker this teammate filed; 677 closed | Not a gate; only P0 issues gate | Repository evidence |
| Open tasks | ForkMesh organization tasks in `/api/tasks` with no `completedAt` and `status` not `done` | 20 open of 34 live on 2026-10-06 at 05:06Z (authenticated `list_tasks` with `status: all` as `jett`; owners below); 81 at 23:12Z on 2026-09-30; 204 on 2026-09-27 | Not a gate | Deployed observation |
| Ecosystem listings | Published projects at `https://forkmesh.com/api/ecosystem` | 471 after two signed imports: `tools/ecosystem_third_parties.json` (406 added, 23:27Z) and `tools/ecosystem_hn_launches.json` (36 added, 23:35Z); 29 before | Third-party listings, never evidence of users or adoption | Deployed observation |
<!-- launch-goal-targets:end -->

- **Repository evidence:** NodeInfo counts every `users` row (`app/src/fediverse_routes.py`), including accounts
  that never verified an email. It is not deduplicated or activity-based. The
  site badge and World HUD use a narrower public-roster count
  (`_public_member_count` in `app/src/account_users.py`), so the two numbers
  can differ. Quote which one you mean.
- **Repository evidence:** `/api/tasks` lists rows from the table
  `organization_tasks` for the organization's `org_bi`
  (`world/src/world_office_tasks.py`, `_list`), and `_project_task` reports a
  row as `done` exactly when `completed_at > 0`.
- **Inference:** the ecosystem directory lists 435 third-party projects, most
  of them dependencies ForkMesh uses. A reader who sees “435 projects” next to
  235 users could read it as adoption. 36 of the listings are the comparable
  Show HN projects in this report, so the directory is also a map of the
  crowded category a reader will ask about. The human should describe it as a
  directory of other people's projects, or not mention the count.
- **Measured fact:** #705 (clean-machine logs) and #706 (visitor-verifiable
  collaboration log) were closed by `jett` with a status event and no comment
  or reason. Neither acceptance evidence exists: no clean-machine log is
  committed and no public repository carries `.forkmesh/collaboration`. Under
  the target's rule those closures are neither resolved nor declined with a
  reason, so both blockers stay in the table below without an open issue. The
  human owner should either reopen them or record why they were declined.
- **Deployed observation (2026-10-06, 05:06Z):** an authenticated
  `/api/tasks` read signed by the desktop identity key as `jett`
  (`canManage: true`) returned 34 live tasks: 20 open, 14 done. The drop from
  81 is deletion, not completion: `jett` deleted 180 tasks between
  2026-09-30 23:19Z and 2026-10-01 07:32Z, and they are still restorable from
  `status: deleted`. The `[Done] …` task with `completedAt` 0 noted on
  2026-09-30 is among them. Open tasks by owner (task id prefix, priority):
  - **`mdkaifansari04` (9):** Apple App Group for the app (`e7be02da`, 5) and
    for the watch (`de40c645`, 72); fix the App Review rejection of 1.4.0 (18)
    and resubmit (`7ff1ef91`, 10); accept the new Apple terms and ship the
    latest build (`27d9de7e`, 10); test the Flutter-to-native upgrade on an
    iPhone (`f9bee889`, 14); App Store Connect metadata for 1.4.0
    (`33583ed2`, 15); App Store Connect cleanup (`c922a444`, 40); deploy the
    latest app to the App Store (`a253a87d`, 50); one personal errand
    (`22c5d21b`, 50).
  - **`jett` (8):** post-cutover checks, blocked on the user (`954bfdee`, 6);
    three security key-handling and credential tasks, blocked on the user
    (`c4ed49fc`, `4f0afa12`, 15; `57f7ca07`, 16); sales pipeline setup
    (`fcfd0c19`, 20); MacBook Pro 2017 upgrade, paused (`656e0480`, 48); a CRM
    access check (`056196f3`, 55); Apple Watch UDID, paused (`8bb1e789`, 75).
  - **Unassigned (3):** milestone 1,000 users (`9210c565`, 1); verify direct
    email from the cores (`3170b004`, 7); Android parity device test
    (`bd33e3d0`, 20).

| Priority | Blocker and user impact | Owner | Exact acceptance evidence | Smallest next action |
| --- | --- | --- | --- | --- |
| P0 (#702) | The flagship repository is below trusted quorum: three verified copies, all on the operator's own core nodes. A reader who runs the show path sees the product's own proof say “high risk”, which undercuts the reason to care | Mirror operations / human owner | `availability.proof == "quorum"` in the public mirrors payload with `diversity.independentOperators >= 2`; CLI `explain` exits 0; dated in this report | Bring up one mirror node under a different ForkMesh account on infrastructure the current operator does not control, and let it verify |
| P0 (#706 closed without a reason) | No visitor-runnable failover proof and no public collaboration log to verify. The product-promise page still needs an account and operator control | Mirror/product engineering; human owner for the key | Disposable public fixture with a second-operator mirror; same `HEAD` before and after source removal; a committed policy plus NDJSON log passing `tools/verify_collaboration.py --require-complete` from a plain clone; no signup | The owner runs `tools/export_collaboration_log.py export` in one disposable public repository, commits `.forkmesh/collaboration`, and records the fresh-clone verifier transcript |
| P0 | Show HN is currently restricted and submitter eligibility is unknown; nobody is named to cover the thread | Human launch owner | Human confirms account eligibility and personal authorship immediately before posting, and names 24-hour thread and incident cover | Recheck `showlim` in the week of the planned launch |
| P1 (#705 closed without a reason) | No clean-machine install, first-run, gate, pause, and uninstall log for any release on Linux, Windows, or macOS. `v1.2.7` blobs, signature, Authenticode and macOS architecture are measured ([evidence page](clean-machine-evidence.md#v127-release-checks)) | Quality | Retained logs with manifest hash, blob hash, first-run, decision gate, Stop work, uninstall, and the SmartScreen and Gatekeeper dialog text | Run `tools/clean_machine_transcript.sh run` on one fresh Linux VM and keep the transcript |
| P1 (#707) | Correlated outage invisible to status (2026-09-12) has no fix evidence; there is no public status history (`/api/status/history` 404 on 2026-09-30) and no external Git-path probe log | Reliability | A 24-hour external `ls-remote` plus shallow-clone probe log with every failure matched by a down status sample; a public history or incident record | Run the probe loop from a host outside the fleet and compare with status samples |
| P1 (#708) | Proof now exists (`forkmesh-agent-boundary-tests`, `docs/security/agent-host-boundaries.md`). What it shows: host-mode agents inherit every non-provider secret in the desktop environment; host mode has no OS egress policy; two injection paths run unasked; there is no hard dollar cap. The Drop thread shows readers expect an exact filesystem boundary | Security / desktop agents | Provider-by-mode table with a check per row, adversarial fixture suite, secret/egress matrix and spend statement (done); an environment allowlist for host launches would close the inherited-secret row | Decide whether host launches get an environment allowlist like the KVM bridge; quote the known gaps, not "isolation", on the launch surface |
| P1 (#709) | Status, data custody, deletion, and heavy-traffic response are rehearsed only on paper: no Git-path rate limit, no public incident record, and an incident lead who has not accepted | Reliability / operations | Retention table (done); desk tabletop with named lead and controls (done); lead accepts; Git-path rate limit exists; tabletop repeated with people present | Incident lead accepts the role and names a second responder |
| P1 (#703), resolved by definition | Release provenance: `tag_commit` is the release-cut commit, an ancestor of the published tag. For `v1.2.7` tag and build commit are the same (same-day release) | Release engineering | Done: `.forkmesh/releases/README.md` states the definition; v0.8.3, v0.8.10, v1.2.4, v1.2.5 and v1.2.7 pass `git merge-base --is-ancestor` | Close #703 on main once this branch merges |
| P2 | Page metadata says “decentralized Git collaboration” while the proof shows one operator | Marketing | Homepage `<meta name="keywords">` drops “decentralized” or the proof reads `quorum` | Remove the one keyword |
| P1 | No skeptical-reader usability cohort | Quality / product | Four of five participants finish uncoached, restate collaborate/mirror/preserve, and find license, limits, security, and uninstall | Test the show path with one non-maintainer first |

The highest-leverage next step is the first P0: a second independent operator
for the flagship repository. It is the one change that turns the product's own
proof from “high risk” to `quorum`, it needs no code, and every demo, screenshot,
and hard question in this packet gets easier once it exists. The public
collaboration-log fixture and the status probe log follow it.
