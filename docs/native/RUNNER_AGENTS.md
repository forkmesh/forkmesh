# Runner agents on the phone

Added 2026-09-30 (branch `feature/mobile-runner-agents-parity`), asked from the
phone: "revamp the experience with [runner agents] from mobile to be more like
how we interact with the desktop agents … so we can work off of branches and
merge and cleanup just like we do with desktop".

A runner agent is a cloud session (`/api/repo/<owner>/<repo>/agent-sessions`)
that a runner (`fmnext-r1`, a personal runner) or a mirror runs. It works on
its own branch on the cores (`refs/heads/agent/<...>`, [RUNNERS.md](../RUNNERS.md)).
A desktop agent is a session on one of the account's desktops, reached over
local control or the relay ([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)). This page
says what the phones could do with each before this change, what the App now
serves for runner sessions, and how the phones use it.

## Gap audit (before, 2026-09-30)

"Unified screen" is the All agents run screen: iOS `AgentRunScreen`, Android
`UnifiedAgentScreen`. "Launched screen" is `LaunchedAgentSessionScreen`, which
the Agents tab opened for most cloud rows.

| Capability | Desktop agent (iOS / Android) | Runner agent, iOS | Runner agent, Android |
| --- | --- | --- | --- |
| Row: status | yes | yes | yes (merged state lost in `AgentLineChips.facts(row)`) |
| Row: branch | Agents tab | Agents tab only | chip; launched rows also in the detail line |
| Row: files, +/− | iOS Agents tab (`diffStats`); Android never filled | never (not decoded) | never |
| Row: machine name | desktop name | "Node" badge, or "Desktop" for a `runner-pool` target (`AgentLaunchTarget.isMirror` was false for it) | "Node" badge, `cloud · <targetNode>` |
| Transcript | streamed (`agents.follow`), rich blocks | history flattened to `who: text`, re-read on refresh; plain text on the launched screen | polled every 3 s, flattened; plain text on the launched screen |
| Diff | `agentops.diff`: per-file +/−, committed and uncommitted patches, `truncated` | `BranchDiffScreen` over `/compare`, once `branchName` was known | same |
| Merge & clean up | `agentops.prepare` + `agentops.merge` (iOS: Face ID) | none | none |
| Delete | `agentops.prepare` + `agentops.delete` | "Drop" (`DELETE`) on the launched screen only | "Delete" on the launched screen only |
| Stop | `agents.stop` | unified screen and swipe; not on the launched screen | `{action:"stop"}` |
| Continue | `agents.continue` | throws `unsupported` | `canContinue=false` |
| Steer composer | `agents.prompt` (+ images) | `POST …/<sid> {prompt}` once `localAgentId > 0` | same |
| Open PR | relay-session screen | launched screen's task chain only | launched screen only |
| Start: repository, model | yes (model only in quick launch) | yes | yes |
| Start: base branch | no | no | no |

Server side, before:

- The session JSON had `agentInfo.branchName`/`baseBranch` once a node
  reported, but no top-level branch, no runner name, no diff numbers and no
  merge state.
- No App route merged or removed a branch on the cores. The branch-ops queue
  ([app/branch-ops.md](../app/branch-ops.md)) is drained only by the owner's
  desktop, and a core refuses branch deletes from pushes
  ([CORE_PUSH.md](../CORE_PUSH.md)). The PR merge (`merge-pull`) needs a pull
  request, and a runner session never gets one (`runnerBackend.openPull`
  returns 0; nothing in the App opens it).
- A start request could not name a base branch or a branch.
- A follow-up (`kind: "steer"`) was offered only to the runner that held the
  session's last lease, and that runner refused it unless the session was in
  its own `state.json`. A restarted or different runner left it queued.
- The web `/<owner>/<repo>/agents` page had no stop, merge or cleanup for
  cloud sessions.

## What the App serves for a runner session

All routes are the existing owner route
`/api/repo/<owner>/<repo>/agent-sessions/<sid>` (and the organization route
`/api/orgs/<org>/repos/<repo>/agent-bots/<sid>`), with the same auth: the
account session (`Authorization: Bearer`, or the cookie) and, for POST and
DELETE, an allowed `Origin`. Code: `app/src/agent_branches.py`, imported
only inside the handlers that need it and listed in `LAZY_MODULES`
(`world/tools/build_worker_footprint.py`), so it adds nothing to startup
source ([app/worker-footprint.md](../app/worker-footprint.md)).

### Session JSON (list, single session, account list)

New top-level fields on every session:

| Field | Meaning |
| --- | --- |
| `branch` | the session's branch on the cores, e.g. `agent/12-fix-login`; `""` until known |
| `baseBranch` | the branch it was cut from and merges into |
| `runner` | the runner's name when a runner runs it (`fmnext-r1`), else `""` |
| `diffStats` | `{files, added, removed, ahead, behind, conflicted, known}` from the last diff read or the runner's report; `known: false` means not read yet |
| `merge` | `{state, base, mergeOid, mergedAt, branchDeleted, cleanupPending, conflicts, message}`; `state` is `""`, `merged`, `conflict` or `failed` |
| `canMerge` | a branch exists, the run is idle, and it is not merged yet |

### Diff: `GET …/agent-sessions/<sid>/diff?page=0&pageSize=25`

The same shape the phones already parse as `AgentDiff` for a desktop diff,
plus paging and the merge preview:

```json
{"ok": true, "id": "<sid>", "branch": "agent/12-fix-login", "base": "main",
 "baseOid": "…", "headOid": "…", "mergeBaseOid": "…",
 "commits": 3, "ahead": 3, "behind": 1, "clean": true, "conflicts": [],
 "merged": false,
 "files": [{"path": "a.txt", "added": 4, "removed": 1, "binary": false, "status": "modified"}],
 "added": 4, "removed": 1,
 "page": 0, "pageSize": 25, "pageCount": 1,
 "committed": "diff --git a/a.txt b/a.txt\n…",
 "uncommitted": "", "untracked": [], "hasWorktree": false, "truncated": false}
```

- `files` lists every changed file (for the header and stats); `committed`
  holds the patches of this page's files only. `truncated` is true while more
  pages follow.
- `pageSize` is 1 to 100 (default 25).
- Errors: `409 no_branch` (the run has not pushed a branch yet), `404
  branch_not_found`, `413 diff_too_large`, `503 mirror_unavailable`.
- A read also stores `diffStats` on the session, so the list rows show files
  and +/− afterwards.
- The diff, merge and clean up read the repository under the name the cores
  serve it as. An owner session is addressed by its node
  (`/api/repo/fmnext-n1/forkmesh/…`), but a repository linked to an
  organization is served, and pushed by the runner, as `<org>/<repo>`
  (`forkmesh/forkmesh`); the gateway has nothing under the node's name. The
  App looks the organization up in `org_repos` the way the runner lease does
  (`_gateway_owner`, `app/src/runner_leases.py`). Before 2026-09-30 it read
  the node's name, so **Changes** on the phone answered `503
  mirror_unavailable` ("No core answered for this repository") while the
  cores were up.
- The merge and clean up also pick their core under that name
  (2026-10-03). `_mirror_context` builds the routing context for the node
  (`fmnext-n1/forkmesh`, where the catalog keeps the pins) and sets
  `routeOwner` to the organization, like the git proxy does for a clone of
  `/<org>/<repo>.git`. The signed repository proof and the masked
  `merge-agent-branch` / `delete-agent-branch` URL
  (`_https_mirror_merge_proxy`) then name `forkmesh/forkmesh`. Before this
  every core answered `available: false` for `owner=fmnext-n1`, so
  `_pull_mutation_node` found none and the merge answered `503
  merge_node_unavailable` ("No core can merge this branch right now"),
  although all three cores advertised `merge-agent-branch` for
  `forkmesh/forkmesh`.
- A session a mirror ran merges on that mirror first (2026-10-04). Asked from
  the web: "This is already on a mirror so it should be able to merge on
  itself." A mirror pushes the run's branch into its own repository
  (`agentJobRunner.publish`, `server/agents.go`), so it holds the branch
  before any peer fetches it. `session_home` (`app/src/agent_branches.py`)
  names that mirror (`runnerNode`, else the session's `target_node`) and a
  new merge passes it as `sticky` to `_https_mirror_candidates`, so
  `_pull_mutation_node` tries it before the other cores. A runner session has
  no home (a runner holds no repository) and picks its core as before. When
  the mirror is not a current candidate, the merge goes to another core.

### Merge & clean up: `POST …/<sid>` `{"action": "merge", "cleanup": true, "requestId": "<id>"}`

A real `git merge --no-ff` of the session branch into its base on the
repository's core, through the owner merge executor (`merge-agent-branch`,
[server/merge-executor.md](../server/merge-executor.md#agent-branches)). With
`cleanup`, the branch is removed in the same ref transaction after its tip is
saved as `refs/forkmesh/trash/agent-branch/<branch>/<request token>`, and
then on the other cores.

- `requestId` is optional (12 to 80 of `[A-Za-z0-9_-]`); the App makes one when
  it is missing and returns it.
- The App keeps the attempt in the session record (`merge.state:
  "merging"`, the exact request and the core it went to). For 10 minutes
  (`PENDING_MERGE_MS`) every merge POST resumes that attempt, whatever
  `requestId` it carries, and answers with the original one: a second tap
  or a client that gave up polling finishes the same merge instead of
  starting another. After that a new `requestId` starts a new attempt; the
  exact-OID pins make a second merge of the same work fail as `stale_base`
  rather than merge twice. `canMerge` stays true while `merging`, so the
  button is there to finish it.
- Past those 10 minutes a POST with a new `requestId` still asks the core the
  attempt went to first, as long as that core is still a candidate: the
  executor's job record outlives the gateway's memory, so a merge that landed
  while nobody polled is finished instead of answered `branch_not_found`
  (cleanup had removed the branch) or merged again. Only when that core is
  gone does a new attempt start (2026-10-03).
- Like the pull-request merge, this works for public repositories the cores
  serve (`_https_mirror_public_context`); a private repository answers `404
  not_found` here and keeps using the desktop.
- The merge preview's numbers (`commits`, `files`, `insertions`,
  `deletions`) are read from `compare` of the two pinned tips when the
  attempt starts.
- `202 {"ok": true, "status": "processing", "requestId": …}`: the core is
  still working. Repeat the same body (same `requestId`) about once a second.
- `200 {"ok": true, "status": "merged", "requestId", "branch", "base",
  "mergeOid", "branchDeleted", "cleanupPending": ["fmnext-n2"], "stats":
  {"base", "commit", "commits", "files", "insertions", "deletions"},
  "message": "Merged agent/12-fix-login into main and cleaned up."}`.
  `stats` has the desktop's `MergeStats` shape without `velocity`.
- `409 {"ok": false, "error": "merge_conflict", "conflicts": ["a.txt"],
  "message"}`: nothing changed. The session's `merge.state` becomes
  `conflict`.
- `409 stale_base` / `stale_head`: a branch moved between the read and the
  merge. Retry with a new `requestId`.
- `409 session_running` (stop it or wait), `409 no_branch`, `409
  already_merged` (clean up instead), `403 forbidden` (only an owner who may
  merge the repository), `503 merge_node_unavailable` (retry later).

### A run that pushed nothing (2026-10-02)

Asked from the phone, with a screenshot of the Changes screen answering
"agent/1-have-an-option-on-mobile is not on ForkMesh, so there is nothing to
merge": "make the merge process work for the runner; we should have the same
experience on mobile with desktop branches as with runner branches".

A runner pushes its branch only when the run committed something
(`runnerBackend.publish`, [../RUNNERS.md](../RUNNERS.md) "When a run fails"),
but the session names the branch from the start, so **Changes** and **Merge &
clean up** were offered for a branch the cores never got and both answered
`404 branch_not_found`. A desktop agent with no changes shows "No changes
against the base branch." and its Merge & clean up succeeds and removes it.

`published_nothing` (`app/src/agent_branches.py`) now decides, when the
branch is missing from the cores, that the run has nothing to merge if the run
is idle, its `lastError` has no `publish branch:` failure, and either no node
ever reported on it (stopped while queued) or the runner reported
`diffStats` with 0 files and 0 commits ahead. Then:

- the diff answers `200` with no files, so the phones show the desktop's
  "No changes against the base branch.";
- the merge answers `200 {"status": "merged", "branchDeleted": true,
  "mergeOid": "", "stats": {… 0 …}, "message": "agent/x had no changes, so
  nothing was merged into main. Cleaned up."}` without asking a core, and
  stores `merge.state: merged` with `branchDeleted`, so the phones show the
  same Landed screen as for a desktop agent and the row reads merged.

A missing branch whose run reported changes, or whose push failed, still
answers `404 branch_not_found`; the message now says the runner could not push
it (with the error), or, while the run is active, that its changes show once
the runner pushes them. The phones need no update: they already read these
answers.

### Clean up: `POST …/<sid>` `{"action": "cleanup", "requestId": "<id>"}`

Removes a branch that is already merged (its tip is contained in the base on
that core), with the same trash ref first, on every core that holds it.
`200 {"ok": true, "status": "cleaned", "branchDeleted": true,
"cleanupPending": [...]}`, `202 processing`, `409 not_merged`. A core that
has not caught up with the merge yet stays in `cleanupPending`; the next
cleanup (or merge poll) retries it.

### Continue and steer: `POST …/<sid>` `{"prompt": "…"}`

Unchanged request. For a runner session the follow-up now carries the
session's branch and base, any eligible runner may take it when the one that
ran the session is gone, and a runner that no longer has the session resumes
it on the same branch (a fresh conversation, told which branch and task it is
continuing). The phones' **Continue** sends `Continue where you left off.`

**Follow-ups never reached a runner (fixed 2026-10-06).** Asked from the
desktop, with a runner run whose answers "A", "A" and "continue lets go with
A" all sat unanswered: "allow for continuing an agent on the web runner". The
start job of a runner session names the account as its owner
(`organization` = `runnerOwner`, the actor) and, for a personal runner, pins
it (`runner`). The steer job named the URL owner instead (`fmnext-n1` on the
owner route), so `_owner_allowed` turned it away on every runner (they list
the account, not the core), and a personal runner never took it either, since
it takes a pool-target job only when pinned. The POST answered 202 and the job
stayed `queued` for good. Now:

- the start stores `runnerOwner` on the session, and a follow-up on a
  `runner_pool` session carries it as `organization`
  (`agent_branches.runner_job_owner`; older sessions fall back to
  `createdBy`). A mirror session's follow-up keeps the URL owner.
- `_lease_jobs` (`app/src/runner_leases.py`) counts the runner that holds a
  session's latest lease as pinned for that session's steer, so a personal
  runner takes the follow-ups of a run it started. Other personal runners
  still do not.

Steers queued before the fix keep the wrong owner and stay queued; send the
follow-up again. Stop still does not reach a runner: `_job_ref` gives a
`stop` job no ref, so no runner leases it, and the session keeps its status
with `stopRequestedAt` set.

### Stop and delete

`POST {"action": "stop"}` and `DELETE`. `DELETE` alone never removes the
branch. Since 2026-10-01 the phones' Delete first sends
`POST {"action": "cleanup", "discard": true}`, which deletes the branch merged
or not, then the `DELETE` ([../server/merge-executor.md](../server/merge-executor.md),
"Discarding a run's branch"). **Merge & clean up** (or **Clean up** after a
merge) still removes only merged work.

### Start: `POST …/agent-sessions`

Two new optional fields:

- `baseBranch`: the branch to cut from and later merge into (must exist on the
  cores; default: the repository's default branch).
- `branchName`: a short slug (`fix-login`); the App makes it
  `agent/<n>-<slug>` with `<n>` one more than the highest number among the
  repository's sessions, like the desktop's `NNN-slug` worktrees. Without it
  the branch stays `agent/<session>` (runner) or `agent/web-<job>-<slug>`
  (mirror).

## Phones

Both apps now give a runner row the desktop row's shape and a run screen with
the desktop's actions and wording:

- Row: status glyph, title, branch, files and `+added −removed` (from
  `diffStats`), merged state, and a small **Runner · fmnext-r1** badge (the
  `runner` field) instead of the desktop's name.
- Run screen (unified screen for every runner row): the rich transcript,
  **Changes** (the diff above, paged, in the desktop diff view, with its own
  **Merge & clean up** in the top bar), **Merge &
  clean up** (one tap, then the Landed numbers), **Clean up** after a merge
  that left the branch, **Continue** and the steer composer, **Stop**,
  **Delete**.
- Start: repository, base branch (branches read from the repository) and
  model, then the App's `branchName` from the title.

Code, iOS: `FMApi/Services/ApiService+RunnerAgents.swift`,
`FMApi/Models/RunnerAgentOps.swift`, `FMControl/AgentControl.swift`
(`UnifiedAgentRow.fromCloud`), `ForkMesh/Features/Agents/AllAgents/`.

iOS details:

- `AgentLaunchSession` reads `branch`, `runner`, `diffStats`, `merge` and
  `canMerge` (falling back to `agentInfo`); `AgentLaunchTarget.isRunner` makes a
  `runner-pool` target a cloud target labelled "Runner" (it used to count as
  the source desktop).
- `UnifiedAgentRow.fromCloud` fills files, `+/−`, ahead, behind and merged from
  `diffStats` and `merge`, names the runner as the row's machine, and offers
  Continue for an idle run a node has claimed. `badgeLabel` is
  "Runner · fmnext-r1". The transcript (`UnifiedAgentRow.cloudTranscript`) keeps
  the agent's markdown and writes each prompt as a `> ` line, so tables, task
  cards and "You" bubbles render as they do for a desktop run.
- Every cloud run opens the All agents run screen, the one a desktop agent
  opens. `AgentLines.merge` turns a queued session that has a runner or a
  branch into that run's line (a CPU glyph with the runner's name and the
  stats); any other cloud or runner session keeps its queued line, and
  tapping it opens `AgentRunScreen` too, on the control center's row or one
  built by `AgentLines.runRow` (`UnifiedAgentRow.fromCloud` with the line's
  repository). A session queued for a desktop opens it too since 2026-10-06
  ([Every run opens the desktop agent's page](#every-run-opens-the-desktop-agents-page-2026-10-06)).
- `AgentRunScreen` gives a runner run the desktop agent's top bar
  (`RunnerAgentScreens.swift`, models in
  `FMAgents/Remote/RunnerAgentOpsModels.swift`): the green "View branch diff"
  square (`RunnerDiffScreen`), then icon buttons for Merge & clean up (opens
  the diff with `mergeOnOpen`, disabled until `offersMerge`), Fix conflicts
  with agent (only while the branch conflicts), Clean up (merged but the
  branch is still on a core) and Delete. There is no actions menu. Merge and
  Clean up start on the tap, with no confirm step or Face ID (Clean up lost
  both on 2026-09-30, matching merge): the top-bar button in the diff
  animates while the merge runs, then the screen shows the Landed numbers
  with **Go back to agent list**, which counts down five seconds and then
  goes back on its own (2026-10-02); an error, pending notice or conflict (Fix
  with agent) shows above the diff. Clean up opens `RunnerMergeSheet`, which
  shows progress, then the cleanup line.
- `RunnerDiffScreen` looks like the desktop's `AgentDiffScreen`: the
  `branch +/−` title, the file stream, and a note above it only when
  there is something to say (conflicts with the base, **Load more files**
  while pages remain, an error). The old facts line (files, commits ahead,
  page n of m) is gone; the title and the file list carry it.
- The start form (`AgentStartCard`) and the composer's "Where it runs" sheet
  offer a base branch when the repository has more than one; the launch sends
  `baseBranch` (only when it is not the default) and `branchName`, a slug of
  the title (`RunnerAgentText.branchSlug`).
Android: `core/.../agents/RunnerAgentOps.kt`, `AgentQueueApi.kt`,
`control/AgentControl.kt`, `app/.../feature/agents/AllAgentsScreen.kt`.

Android specifics:

- `RunnerAgentOps` (core) wraps the three calls. A merge or cleanup sends one
  `requestId` and re-POSTs the same body once a second while the App answers
  202 (up to 20 times); a `stale_base`/`stale_head` answer is retried once
  with a new `requestId`. Error codes become the desktop's sentences
  (`RunnerAgentOps.describe`). `RunnerDiffModel` and `RunnerMergeModel` are
  the screen state; `RunnerBranchName.slug` makes the `branchName` (4 words,
  30 characters, like the desktop's `titleSlug`).
- `UnifiedAgentRow.fromCloud` gives a runner session the new
  `AgentTransportBadge.Runner` (`badgeLabel` "Runner · fmnext-r1"), its PR
  number, merged state, files and `+/−` (`AgentLineChips` shows the runner's
  name as the host word with a `memory_outlined` glyph, and the churn chip), and `canContinue` once the run is idle and a
  node has started it. The cloud transcript keeps its structure for the rich
  renderer (`cloudTranscript`): your turns become `> ` lines, the agent's
  markdown stays as it is, a failure ends in a `!!` line.
- In a repository's agent list, `launchRunsInCloud` counts any session with a
  `runner` as a cloud run, like iOS's `runsInCloud`. Before, a runner that was
  not one of the repository's launch targets fell through to Desktop, so the
  row sat under the Desktop tab with a desktop glyph. Runner rows there and on
  All agents use the `memory_outlined` icon.
- `UnifiedAgentScreen` shows **Merge & clean up**, **Clean up** (merged but the
  branch is still somewhere) and **Delete** under the status line while the
  run is idle; the branch row opens `RunnerDiffScreen` (the desktop diff view,
  with **Load more** while pages remain, and **Merge & clean up** in its top
  bar when the run screen offers merge; the screen closes after the merge).
  The dialogs are in
  `RunnerAgentViews.kt`: both start on the tap (Clean up no longer asks first,
  since 2026-09-30); the merge dialog turns into "Merged!" with the desktop's
  `MergeStatsBlock` (a runner merge carries no `velocity`, so only the LANDED
  card shows), the cleanup one into "Cleaned up". Closing the merge result
  returns to the agent list. The Agents tab opens every runner or cloud
  session there (since 2026-10-02; before, only runner rows and cloud rows
  with a branch) instead of `LaunchedAgentSessionScreen`.
- The start card has a **Base branch** picker (the repository's branches,
  default branch first) in node mode; the quick-launch composer sends only the
  `branchName`.

## Merge counts as deploy approval (iOS, 2026-10-08)

A runner merge from the phone (`{"action": "merge", ...}`) and a pull merge carry `deployApproval`: `{publicKey, ts, signature}` signed with the phone's node identity over the lines in `app/src/deploy_approvals.py` (`approval_message`), so the cores deploy a merge the owner approved even while the desktop is offline. The phone's key is on Identity and recovery as "Deploy approval key"; add it to `approver_keys` under `[self_deploy]` in `deploy/fleet.toml`. Without a key the merge is sent unsigned. Details: `native/ios/docs/FMApi.md`. The cores build and activate app changes, and since 2026-10-08 also roll the Go daemon and gateway themselves; only `deploy/core/` changes and the runners still wait for the desktop ([../deploy/self-deploy.md](../deploy/self-deploy.md)).

## Live transcript stream (2026-10-05)

While a runner or mirror session is queued or running and its screen is open,
both apps follow `GET <session URL>/live`. That is the session GET's URL
(owner or org route) with `/live` added, sent with the same bearer token. The
reply is a `text/event-stream`. The server side and the frame grammar are in
[agents/README.md](../agents/README.md#live-stream-to-the-phone).

Both apps handle the frames the same way:
- A `text` frame replaces the live text.
- An `append` frame extends it. An append is ignored until a `text` frame has
  arrived on the same connection.
- `end` closes the stream and runs the screen's normal re-read, which brings
  the final history.
- When the server closes the stream cleanly (after 5 min), the app reconnects
  after about 1 s.
- Network errors, 5xx, 408 and 429 back off from 2 s, doubling, up to 30 s.
  The server answers 503 `live_unavailable` when the relay cannot be reached.
- Any other non-2xx, or a reply that is not an event stream, turns streaming
  off for that screen visit.
- A re-read keeps the streamed text unless the session's `live.at` is newer.

**iOS** (`FMApi/Services/AgentLiveStream.swift`,
`AgentControlCenter.followCloudLive`, `LaunchedAgentSessionModel.followLive`).
URLSession already treats `*.forkmesh.com` as HTTP/3-capable (`HTTP3Hosts`),
so the stream rides QUIC. The run screen and the launched-session screen
both follow it. The run screen also re-reads a runner session when a
`cloud-agents` ring moves its row; before, it read a cloud run only once.

**Android** (`core/.../agents/AgentLiveStream.kt`,
`AgentControlCenter.cloudLive`). It uses the existing `HttpURLConnection`, so
it gets HTTP/1.1 or HTTP/2 and never HTTP/3; Cronet was not added. It runs on
a background thread with a 45 s read timeout. `UnifiedAgentScreen` keeps its
3 s session poll as the fallback and slows it to 30 s while the stream is
live. `LaunchedAgentSessionScreen` skips `cloud-agents-live` re-reads while
the stream is live.

## Continue on desktop (2026-09-30)

Asked from the phone: "Add a button in mobile on the agent detail to transfer
this agent from the runner over to desktop because sometimes the runner fails
or doesn't complete the job, so we want the desktop to be able to pick it up
and finish it."

The run screen of a runner (or other node) session now has **Continue on
desktop** (the desktop icon; iOS `AgentRunScreen` toolbar, Android
`UnifiedAgentScreen` status row) whenever the phone knows the account's
desktop (`hub.desktop`), the session names a repository and it is not merged.
It is offered for a running, queued, failed, stopped or finished run.

After a confirmation, `AgentControlCenter.continueOnDesktop` (iOS
`FMControl/AgentControl.swift`, Android `control/AgentControl.kt`):

1. Reads the session from the App (the full history).
2. Asks the desktop `agents.takeover` with `check: true`. The desktop only
   resolves the repository and answers; nothing starts. A desktop without a
   checkout (`no_checkout`) or an older desktop (`unsupported`) stops the move
   here, so the runner is never stopped for nothing.
3. Stops the session on the runner (`POST {"action": "stop"}`) if it is still
   queued or running, and reads it again. If the stop fails, nothing moves.
4. Sends `agents.takeover` for real and shows the desktop's answer ("Continuing
   on studio as agent #42, on agent/9-fix-login.").
5. Deletes the runner's copy (`DELETE …/agent-sessions/<sid>`, the same call as
   **Delete**) and leaves the run screen. Also done when the check answers
   `already`.

Step 5 was added on 2026-10-01, asked from the phone: "When we click continue
on desktop on mobile, make sure to delete the one that was on the runner."
Before, the session stayed in the relay's list, so the Agents tab showed the
desktop agent and the stopped runner run side by side. Only the session record
goes; the branch on the cores stays, since the desktop continues on it. A `404`
counts as deleted. Any other failure keeps the move (the desktop already has
the run): `RunnerDesktopMove.runnerKept` is set, the message ends "Its run on
the runner could not be deleted; delete it from Agents.", and the screen stays
open so **Delete** is at hand.

### `agents.takeover` (desktop local control)

Params: `cloudSession` (the relay session id), `owner`, `name`, `branch`,
`base`, `runner`, `task`, `title`, `provider`, `model`, `check`. Advertised in
`status.capabilities`. `MainWindowLocalControl.cpp`:

- `cloudSession` must match `takeoverSessionValid`, `branch` (when given)
  `handoffBranchValid`, `base` `handoffRefNameValid`
  (`LocalControlHandoff.h`); `task` is 1 to 8000 characters.
- If a local agent already stands for that cloud session
  (`AgentSession::webSessionId`), it answers that agent with `already: true`
  and starts nothing, so a second tap is harmless.
- The repository is resolved like the desktop's cloud list files runs under
  local repositories (`takeoverRepoIndex`): the exact owner and name, else the
  only checkout with that name. A runner session is often addressed by its
  core (`fmnext-n1/forkmesh`) rather than the owner the desktop knows.
- It fetches `branch` from the repository's served mirror or clone URL
  (`fetchServedBranch`) and starts an agent on it with
  `startAgentOnExistingBranch`, so the desktop's commits land on top of the
  runner's. When the runner never pushed the branch (a run that failed at
  sign-in, for example) or the fetch fails, it starts an ordinary agent from
  the default branch and answers `fresh: true`; the agent log says which.
- The new agent's `webSessionId` is the cloud session, so the desktop's Agents
  list shows the local run instead of the relay's row, the automatic cloud-run
  move skips it, and a later `agents.takeover` answers `already`. The relay row
  is dropped at once.
- Answer: `{id, branch, fresh}`. Errors: `bad_session`, `bad_task`,
  `bad_branch`, `no_checkout`, `not_started`.

The task (`RunnerDesktopHandoff.task`, both phones) names the runner, the
session and how it ended (with `diagnostic` or `lastError`), the branch and
base, the original prompt (up to 3000 characters), then "What happened on the
runner": each later turn on one line (`Me:` / `Agent:`, up to 1200 characters
each), newest kept, all under 7600 characters.

| Desktop answer | Phone says |
| --- | --- |
| `unsupported` (older desktop) | Update ForkMesh on the desktop to move runner agents to it. |
| `no_checkout` | The desktop has no working copy of owner/repo. Clone it there first, then move the agent. |
| `not_started` | The desktop could not start the agent, then the desktop's `message`. Without one: It says why in its Agents list. |

Since 2026-10-03 a `not_started` answer also carries `message`: the error
toast the desktop raised while trying to start the agent (for example "Branch
agent/9 is not on this node, so there is nothing for an agent to work on. Fetch
it first."). `flashMessageImpl` keeps the last `local` error toast in
`m_lastLocalOperationError`; `agents.start`, `agents.handoff` and
`agents.takeover` clear it before starting and send it with
`LocalControlServer::respondError(…, error, message)`. Both phones keep it on
`LocalControlException` (`message` on iOS, `reason` on Android) and show it
after "The desktop could not start the agent." in place of "It says why in its
Agents list", so the reason shows on the phone as well as on the desktop. A
desktop that sends no message keeps the old wording.

A failure after the stop adds "The run on fmnext-r1 is stopped; Continue
restarts it there."

Tests: desktop `test_local_control_handoff.cpp` (session ids, repository
resolution); iOS `FMControlTests/RunnerDesktopHandoffTests.swift`; Android
`control/RunnerDesktopHandoffTest.kt` (task text and trimming, who may move,
failure wording, check → stop → takeover order, a refused check leaving the
runner alone).

## Web

The `/<owner>/<repo>/agents` detail of a cloud session
(`renderOrgAgentSessionDetail`, `app/public/dashboard/js/06-repo-content.js`)
has the desktop session's buttons and wording: **Diff** (toggles to the paged
diff above, 25 files a page, **Load more**), **Stop** while queued or running,
**Continue**, **Merge & clean up** and **Clean up**, plus a **Runner · <name>**
badge, `branch → base` with files and +/− under the title, and Runner, Changes
and Merge rows in Details. `runOrgAgentAction` sends the requests with no
browser confirm for Merge & clean up or Clean up (since 2026-09-30); a merge or
cleanup repeats the same body (same `requestId`) once a second while the App
answers 202, every 2 seconds for up to 3 minutes
(`ORG_AGENT_MERGE_POLLS` × `ORG_AGENT_MERGE_POLL_MS`), then says the core is
still publishing. While the session's `merge.state` is `merging` the button
reads **Finish merge** and the Merge row **Merging…**; pressing it resumes the
same merge. The notice line shows the
merge in the desktop celebration's words ("Merged agent/x into main and cleaned
up. 3 commits · 5 files +40 −2."), the conflicting files, or the App's
message. The helpers sit before `orgAgentProviderLabel` because
`test_org_agent_bots.py` forbids timers inside the session renderers; the merge
poll's one-second wait only runs after a click. Test:
`app/tests/test_web_runner_agent_actions.py`.

### Web merge stuck on "Merge & clean up…" (2026-10-03)

Asked from the web: "Fix merging on web, it's stuck at this and doesn't merge.
It should merge and then any associated actions should run and we have the
option to run them individually too."

- Cause: once the executor had landed the merge, the gateway's job thread
  (`_run_merge_job`, `tools/mirror_gateway.py`) reloaded the gateway with
  `reload_from_disk()`, whose default warms every cache (stats, sizes and
  history walk the whole repository) before the job may answer `merged`. On
  a 1 vCPU core serving `forkmesh/forkmesh` that outlasted the page's 20
  one-second polls, the page gave up, and the App kept the session `merging`
  without learning the result. Nothing after the merge ran: no state pin, no
  mirror wake, no Actions.
- Fix: the job reloads with `prewarm=False` and warms the caches on a
  separate thread (`_prewarm_after_merge`), as the live reload path has done
  since 2026-09-12. The page waits up to 3 minutes, shows **Finish merge**
  while a merge is `merging`, and the App finishes a stale `merging` attempt
  on its core (above).
- Actions after the merge: a merged session's detail shows **Actions on
  <base>** (`renderOrgAgentMergeActions`), read from the same
  `GET /api/repo/<o>/<r>/actions/runs` the Actions tab reads. It lists every
  workflow that runs on a push or by hand, with the run for the merge commit
  (`merge.mergeOid`, or a run on the base since `mergedAt`) and its status.
  Push workflows start by themselves when the merge reaches the executor
  (`scanRefs`, `server/actions.go`); each row has its own **Run**
  (`workflow_dispatch` on the base), **Re-run**, **Stop** or **Approve**, and
  **Run all** dispatches every manual workflow. The buttons send
  `POST …/actions/control` to the executor node and show only when that node
  advertises `actions-control` and the viewer may manage the session. The
  panel loads after a merge lands (or on **Show Actions**) and re-reads every
  5 seconds, at most 60 times, while a push workflow has no run yet, a run is
  live, or a control is unanswered. **Open Actions** switches to the Actions
  tab.
- Deploying the merge: the core's Actions panel never shows "Deploy nodes",
  because cores run no Actions. The source desktop runs it once the source
  publisher has taken the merge into the checkout. The publisher watches the
  cores' `main` every 20 s for that (2026-10-03,
  [../tools/source-publisher.md](../tools/source-publisher.md#merges-made-on-the-cores-2026-10-03)).

## Tests

- App: `app/tests/test_agent_branches.py` (patch parsing, projection, diff
  paging and errors, a run that pushed nothing (empty diff, merge that
  finishes without a core), merge and clean up with the 202 poll, a second tap
  resuming the same merge, a lagging core left pending then cleaned, conflict
  with its files, stale head, the cleanup guard, running sessions, the owner
  gate, a follow-up carrying the branch, NNN-slug names, the relay's result
  check), `app/tests/test_runner_leases.py` (a follow-up moving to another
  runner once the first is gone or revoked, the named start ref, the report
  naming the runner), `app/tests/test_agent_branch_gateway.py` (gateway body
  and result validation, job flow, failures per operation, routing sets),
  `app/tests/test_web_runner_agent_actions.py` (web page).
- Go: `server/merge_branch_test.go` (a merge commit even when fast-forward
  would do, cleanup after saving the tip, conflict and stale refs, cleanup
  refuses unmerged work, non-agent branches and mixed pull/branch requests
  refused, reconcile does not fetch a cleaned-up branch back but replicates
  new work on the same name, advertising only with a gateway that knows the
  operations), `server/runner_steer_test.go` (a follow-up the runner never
  ran resumes on the branch with the preamble; diff stats in the report).
- Phones: iOS `FMApiTests/RunnerAgentOpsTests.swift`,
  `FMControlTests/AgentControlTests.swift`; Android
  `agents/RunnerAgentOpsTest.kt`, `control/UnifiedAgentRowBranchTest.kt`.

## Runner accounts on iOS (2026-10-01)

The iPhone app switches the Claude Code or Codex account a runner uses, as the
desktop's usage menu does (**Runner accounts**,
[../desktop/agents-ui.md](../desktop/agents-ui.md)). It uses the endpoints in
[../RUNNERS.md](../RUNNERS.md) "Several accounts per provider, and usage
limits" and nothing new on the server.

- Screen: **Runner accounts** (`RunnerAccountsSheet`,
  `ForkMesh/Features/Agents/RunnerAccountsViews.swift`) lists one card per
  active runner, and in it each provider whose CLI is installed or that has
  saved accounts. Each account shows its name (label, else email, else
  "Account"), its plan, a check on the active one and the desktop's usage line
  ("5-hour session 42% (resets in 1 h 0 min) · Weekly 100%", or its wording
  for a usage error). Tapping another account posts `activate`; while the
  runner has not applied it the row reads **Switching…** and cannot be tapped.
  **Add … account…** (or **Sign in …** when the provider has none) opens the
  existing runner sign-in sheet, `RunnerSignInSheet`, on top of this one: the
  shell's `runnerSignInPresenter` cannot present while another sheet is up.
  When the sign-in ends the list is read again now, after 6 s and after 14 s,
  because the runner adds the new account on its next check-in. Remove was
  not offered at first, as in the desktop menu; since 2026-10-06 it is **Log
  out…** in each account's menu (below).
- Entry points: since 2026-10-09 the runner accounts sit inside the usage
  sheet that the Agents usage chip opens (the phone's counterpart of the
  desktop's usage menu), under the desktop's providers, as
  `RunnerAccountsPanel`: every switch, sign-in, log out and terminal of the
  screen, without opening a second sheet. The Agents list no longer has its
  own **Runner accounts** row. With no desktop usage client, a person icon in
  the Agents tool row opens `RunnerAccountsSheet` instead. Both show only when
  a listed runner has at least one account, so accounts that may not read
  `/api/runners` see nothing.
- Reads: `RunnerAccountsModel` (`FMApi/Runners/RunnerAccountsModel.swift`,
  registered by `AgentsFeature.register()`) reads `GET /api/runners` when the
  Agents list or the usage sheet appears, at most once per 5 minutes there
  (no polling, as for the sign-in banner), and on every opening or pull of
  the screen. 401 and 403 empty the list rather than showing an error. After a
  `202` it reads again at once and then every 2 seconds while any account
  request is `pending`, at most 30 times, the desktop's rule; the outcome
  ("fmnext-r1 now runs Claude Code as …", or the runner's failure message)
  replaces the "Switching …" line. A refused request shows the server's
  `message` (for example `404 unknown_account`, `429`), else the HTTP status.
- Parsing, wording and paths (`FMApi/Runners/RunnerAccounts.swift`) follow
  `parseRunners`, `runnerAccountUsageText` and `usageErrorText` in
  `desktop/src/RunnerLogin.cpp`: account and request ids must be 32 lowercase
  hex characters, usage percentages are clamped to 0–100, a usage window
  without an id is dropped, and of several requests for one account the
  newest (`createdAt`) decides the outcome.
- Tests: `RunnerAccountsParsingTests` and `RunnerAccountsModelTests` in
  `ForkMeshCore/Tests/FMApiTests/RunnerAccountsTests.swift`
  (`FM_ONLY=FMApi swift test --filter RunnerAccounts`, 10 tests). The SwiftUI
  files are parse-checked only on Linux.

### Usage, log in, log out and a terminal on iOS (2026-10-06)

Asked for from the phone: the screen showed the usage line as text only, could not log an account out, and could not reach the CLI itself.

- Each account row now shows one bar per usage window (`UsageMeterRow`, the desktop usage card's meter, from `UsageWindowMeter(runner:)`: amber from 70 %, red from 90 %, "resets in …") and a line under them, `RunnerAccounts.usageNote`: the usage error ("Usage refreshes after its next run", "Usage limits need a subscription sign-in", …) or "No usage limits reported", and how old the reading is ("read 3 min ago").
- A **⋯** menu per account: **Use this account** (inactive accounts), **Open Claude Code terminal** (the active account, when the runner can), **Log in again…** (the existing `RunnerSignInSheet`; signing the same account in renews its saved login, which is the fix for "sign it in again" and for an API-key login without subscription limits) and **Log out…**, which asks first and posts `{action: "remove"}` (`RunnerAccountsModel.remove`) with the same re-read rule as a switch. While it is pending the row reads **Logging out…**; the outcome is "<account> is logged out of Claude Code on <runner>." or "Could not log the account out: <message>".
- A **Terminal** button beside each provider opens `RunnerTerminalScreen` full screen: the terminal grid, status line and key bar of the node sign-in terminal (`TerminalGridView`, `TerminalStatusLine`, `TerminalKeyBar`) on a `RunnerTerminal` (`FMAgents/RunnerTerminal.swift`), a `StreamedTerminal` over the runner relay. See [../RUNNERS.md](../RUNNERS.md#runner-terminals-2026-10-06). It attaches to a terminal already open on that runner (`terminals[]` in `GET /api/runners`), sends its measured size when it starts and on every settled layout change, polls output every second, sends typed keys in chunks of at most 4096 bytes and the line box's Enter as a chunk of its own, and cancels the session when the screen closes. A runner that does not report the `terminal` feature shows "Terminals open here once <runner> runs a newer runner build." instead of the button.
- Tests: `RunnerAccountsTerminalTests` and `testRemoveLogsTheAccountOut` in `RunnerAccountsTests.swift`; `RunnerTerminalTests` and `RunnerUsageMetersTests` in `ForkMeshCore/Tests/FMAgentsTests/RunnerTerminalTests.swift`. Android does not have these yet.

## Runner accounts on Android (2026-10-01)

The Android app does the same as the iPhone app above, with the same
endpoints, wording and rules; only the names differ.

- Screen: **Runner accounts** (`RunnerAccountsScreen`,
  `app/.../feature/agents/RunnerAccountsScreen.kt`) shows, per active runner,
  one card per provider whose CLI is installed (`binaryFound`) or that has
  saved accounts. Each account row has a check on the active one, its name
  (label, else email, else "Account") and plan, and the desktop's usage line.
  Tapping another account posts `activate`; while the request is `pending`
  the row shows a spinner and **Switching…** and cannot be tapped. The last
  row, **Add Claude account on <runner>…** (or **Sign in … on <runner>…** with
  no account; disabled when the CLI is not installed), pushes the existing
  `RunnerSignInScreen`. When a sign-in ends, `refreshAfterSignIn` re-reads the
  account list with the sign-in banner (now, after 6 s, after 14 s), because
  the runner adds the account on its next check-in. Remove is not offered.
- Entry points: a **Runner accounts** row (`RunnerAccountsEntry`) under the
  runner sign-in banner on the Agents screen, and a **Runner accounts** button
  in the desktop usage card (`AgentUsageCard`, the counterpart of the
  desktop's usage menu). Both show only when a listed runner has at least one
  account.
- State: `RunnerAccountsFeature` keeps one `RunnerAccountsStore`
  (`core/.../runners/RunnerAccounts.kt`) per `ApiService` and publishes its
  `RunnerAccountsState` as a `StateFlow`; it is reset with the other agent
  caches when the account changes. The Agents screen reads `GET /api/runners`
  at most once a minute (no polling); opening the screen always reads. 401 and
  403 hide the list instead of showing an error. After a `202` the store reads
  again at once and then every 2 seconds while any account request is
  `pending`, at most 30 times; the newest request for the account (by
  `createdAt`) replaces "Switching …" with "<runner> now runs Claude Code as
  …" or "Could not change the runner account: <message>". A refused request
  shows the server's `message`, else a wording for 401, 403, 404
  (`unknown_account`), 429 or the HTTP status.
- API: `RunnerAttentionApi.activateAccount(runner, provider, account)` posts
  `{provider, account, action: "activate"}` to
  `/api/runners/<runner>/accounts` with the bearer session, `Content-Type`
  and `Origin`, like the sign-in calls.
- Tests: `core/src/test/.../runners/RunnerAccountsTest.kt` (parsing, sections,
  usage text, POST body and headers, pending re-reads and the 30-read cap,
  outcomes, 401 hiding); `./gradlew :core:coreTest -Pfilter=runners`.

## Runner agents look like desktop agents (2026-10-02)

Asked from the phone: "Make this look / work exactly like the desktop agents",
then "Agent detail screen use the same one as we use for desktop and also diff
viewer". The runner run (fmnext-r1, `agent/5-make-this-look-work-exactly`) hit
the weekly usage limit before writing anything; the desktop finished it.

- iOS: the runner actions menu on `AgentRunScreen` became the desktop's
  top-bar buttons, `RunnerDiffScreen` dropped its facts line so it reads like
  `AgentDiffScreen`, and every cloud or runner session in the Agents tab opens
  `AgentRunScreen` (`AgentLines.runRow`). See the iOS list above.
- Android: `LaunchedSessionRow` opens `UnifiedAgentScreen` for every cloud or
  runner session with a session id, not only runner rows and cloud rows with
  a branch. Its runner buttons already matched the desktop's.
- Test: `AgentLinesTests.testEveryCloudRunOpensTheAgentRunScreen`.

## Pull requests, and the same actions on every surface (2026-10-04)

Asked: "improve the experience of the cloud runners to be more like desktop so
we can see transcripts, merge branches, view diffs and manage pull requests
from mobile, web and desktop seamlessly just like a desktop agent".

Before this, a runner run never had a pull request: `runnerBackend.openPull`
returns 0 because a runner holds no key that may write the pull ledger, and
nothing in the App opened one. The web start form sends `createPr: false`,
and the web detail still said "Opens on finish". The desktop could not merge,
clean up or stop a cloud run from its detail, and hid Create PR on cloud rows.

### App

- Session JSON: top-level `prNumber` (the linked pull request, else the
  runner-reported `agentInfo.prNumber`) and `canOpenPull` (an `agent/` branch,
  the run idle, not merging or merged, no pull request yet, and the run
  published something: not `published_nothing`). `agent_branches.projection_extras`.
- `POST …/<sid>` `{"action": "link-pull", "number": N}` records the pull
  request on the session (`record["pull"]`, kept apart from `agentInfo`,
  which every runner report replaces) and rings `cloud-agents`. `200
  {"ok": true, "status": "linked", "prNumber": N, "message"}`; a second link
  of the same number changes nothing; another number answers `409
  pull_exists`; `400 invalid_pull` (1 to 10,000,000); `409 no_branch`. Owner
  only, like merge.
- The pull request itself is opened by the client, signed by its own key,
  through the existing inbox (`POST /api/repo/<o>/<r>/pulls`, which reserves
  and returns the number): the browser key on the web, the device key on the
  phones, the desktop's node key (a branch-backed pull from the synced
  branch). The App holds no key that may write a pull, so it only links.
- Signed desktop writes: merge, cleanup and link-pull take the new proof
  `forkmesh-owner-agent-branch-v1` (see [../agents/README.md](../agents/README.md));
  the diff route accepts the read proof on its own path
  (`agent_branches._load` passes the signed account through).

### Web

The runner detail (`renderOrgAgentSessionDetail`) has **PR #N** (opens the
pull request) and **Create PR** (`orgAgentOpenPull`: `submitWebPullOpen` for
`branch` into `baseBranch` with the session's title and a description naming
the runner and the branch, followed by the agent's last answer, then
`link-pull`). A branch with nothing against its base, a branch the cores do
not have yet, or one too large for a web pull request each get their own
sentence. The PR row reads "branch merged" after **Merge & clean up**, and
"Opens on finish" only for a mirror run still going. The agent's turns in the
transcript render as markdown (`renderRecordMarkdown`, the escaping renderer
issue bodies use); your own turns stay plain text.

### Desktop

See [../desktop/agents-ui.md](../desktop/agents-ui.md), "Cloud runs merge,
stop and open pull requests like desktop agents": Merge & clean up, Clean up,
Stop and Create PR on a cloud row, all signed.

### Phones

Both apps put the pull request in the runner run's actions, next to Merge &
clean up:

- **PR #N** while the session has a `prNumber` opens the pull request (iOS
  `PullDetailScreen`, Android the agents feature's `openPull`).
- **Create PR** while `canOpenPull` and the run is idle: re-read the session
  (an existing pull request is reused, never a second one), take its
  `baseBranch` or the repository's default, read the comparison, open the
  pull request through the signed inbox with the device key, then send
  `link-pull`. The wording matches the web: "<branch> has no changes against
  <base>, so there is nothing to open a pull request for.", "<branch> is not
  on ForkMesh yet, so no pull request was opened.", and, when only the link
  failed, "Opened pull request #N, but the run could not be linked to it: …".
  The description is the web's (`RunnerPullDraft` on both).
- The inbox call now returns the number the App reserved (iOS
  `InboxService.openNewPull`, which `submitNewPull` wraps; Android
  `submitNewPull` returns it and signs `commits` when it has them, as the web
  and iOS do; without commits the signed bytes are unchanged).

iOS: `RunnerPullModel` (`FMAgents/Remote/RunnerAgentOpsModels.swift`),
`ApiService.linkRunnerAgentPull`, `UnifiedAgentRow.canOpenPull` /
`offersPullOpen`, the top bar in `AgentRunScreen` (`RunnerPullLabel`). Android:
`RunnerAgentOps.linkPull` / `openPull`, `UnifiedAgentRow.canCreatePull`,
`RunnerPullDevice` (`RunnerAgentViews.kt`), the run screen in
`AllAgentsScreen.kt` (result in the line under the action buttons, with
**Open**).

Known limits: a pull request opened on a phone goes to the run's own
repository (the web sends a fork's pull request to its parent). If the link
fails after the pull request opened, the run still offers Create PR; the
message names the pull request that exists (iOS also keeps showing it as
**PR #N** until the screen closes; Android names it in the line under the
action buttons).

Rollout: the App first. An older App has no `canOpenPull` (no client shows
Create PR) and refuses the desktop's signed merge, clean up and link as
`invalid_session`.

Tests: `app/tests/test_agent_branches.py` (projection, link-pull, a link
outliving the runner's report, the signed diff read),
`app/tests/test_owner_agent_signed_sessions.py` (the branch proof binds the
whole body, owner route only), `app/tests/test_web_runner_agent_actions.py`
(Create PR signs then links, wording, markdown transcript),
`desktop/tests/test_agent_cloud_sessions.cpp`; iOS
`FMApiTests/RunnerAgentOpsTests.swift`, `FMApiTests/InboxServiceTests.swift`,
`FMControlTests/AgentControlTests.swift`,
`FMAgentsTests/RunnerPullModelTests.swift`; Android
`agents/RunnerAgentOpsTest.kt`, `api/InboxContractTest.kt`,
`control/UnifiedAgentRowBranchTest.kt`.

## Branches, updates, the task and Actions after a merge (2026-10-04)

Asked next: "also allow us to fully manage branches and merge directly from
them like we can on desktop so task all the way to a merged and deployed code
should work as it does on desktop including actions".

Before this, the website's Branches page queued every merge, update and
delete for the owner's desktop and nothing ran while it was offline; the
phones could not merge, update or delete a branch at all; the cores merged
only `agent/` branches and could not update a branch from its base; a cloud
merge never touched its task; and only the web showed the Actions a merge
started.

- **Cores** ([../server/merge-executor.md](../server/merge-executor.md), "Any
  branch, and updates from the base"): the branch merge and delete take any
  user branch; an update is the merge with the roles swapped; the default
  branch is never deleted.
- **App** ([../app/branch-ops.md](../app/branch-ops.md), "Run on the cores
  first"): a single branch merge, update or delete runs on a core and answers
  with the outcome (202 while working); the desktop queue stays the fallback.
  A runner session gets `POST …/<sid> {"action": "update"}` (merge its base
  into its branch; `202` while working, `updated`/`already`, `409
  merge_conflict` with the files, which sets the session's merge state to
  `conflict`). The desktop signs it with the branch proof.
- **Web**: the Branches and compare pages show the cores' outcome at once
  (spinner, message, conflicting files, **Open Actions** after a merge into
  the default branch); the batch buttons run one branch at a time on the
  cores and stop at the first error, or queue when the cores do not serve the
  repository. The runner detail has **Update from <base>**; after **Merge &
  clean up** the linked task is completed (below) and **Actions on <base>**
  follows the merge commit's runs as before. `orgAgentPostUntilDone` keeps
  polling through `503 merge_status_pending`.
- **iOS / Android**: the Branches screen has Merge into <default>, Merge &
  delete, Update from <default> and Delete (asking first), with the same
  resume ids, outcome and conflicts (**Check conflicts**), and **Actions**
  after a merge into the default branch. A runner run has **Update from
  <base>** (conflicts offer **Fix with agent**). The runner merge result
  (iOS `MergeLandedScreen`, Android "Merged!") has **Actions**, which opens
  the Actions screen filtered to the merge commit (**Show all runs** clears
  it). iOS `BranchOps.swift`, `ApiService+BranchOps.swift`,
  `RunnerMergeModel.update()`; Android `code/BranchOps.kt`,
  `RunnerAgentOps.update`, the commit filter in `ci/Actions.kt`.
- **The task**: after a runner merge every client completes the linked task
  the way the desktop does after a local merge: `POST
  /api/tasks/<id>/complete` with "Merged <branch> into <base> (<abc1234>).
  <message>", the id from `taskKey` (`task:<32 hex>`) or a leading
  `[task:<id>]` in the title or first prompt. Best-effort: a refusal is a
  secondary note and the merge still stands.
- **Deployed**: the deploy is the push workflows the merge starts on the
  base (on the cores); every surface now leads from the merge to those runs.
  There is still no single "deployed" state on a run or a task.

Tests: `server/merge_branch_test.go` (any branch, update, default branch kept,
plain-branch tombstone), `app/tests/test_agent_branch_gateway.py`,
`app/tests/test_agent_branches.py` (core merge, update, delete, stale head,
fallbacks, session update, conflicts), `app/tests/test_repo_branch_management.py`
(cores first, 202, batch and `queue`, the web's resume ids and outcomes),
`app/tests/test_web_runner_agent_actions.py`,
`desktop/tests/test_agent_cloud_sessions.cpp`, iOS `BranchOpsTests`,
`RunnerMergeModelTests`, Android `BranchOpsTest`, `RunnerAgentOpsTest`.

Rollout: App and cores together is safest. Cores first changes nothing (the
App still sends only `agent/` branches); the App first keeps queueing plain
branches for the desktop until the cores accept them (an older core refuses
the request and the App falls back).

## A follow-up on a run no runner started (iOS, 2026-10-06)

A run stopped or failed before a runner approved it has no `localAgentId`,
so the relay refuses a follow-up (`agent_not_ready`) and the iOS run screen
hid its composer: the only way on was the Retry button, with nowhere to say
anything more. `AgentLaunchSession.restartsOnPrompt` (failed or stopped,
retryable, never claimed) now also makes `UnifiedAgentRow.canPrompt` true.
The composer's hint reads "Follow-up (runs the task again)", and sending goes
through `ApiService.retryAgentSession(_:_:_:followUp:attachments:)`: the task
it was started with, a blank line, then the follow-up, queued as a new run
with the same provider, target and model. A claimed run still takes the
follow-up as a steer on its own session.

Retry sent the session's `requestedModel`, which the App stores resolved
(`claude-opus-5-5`), while the launch route only took aliases (`opus`), so
every retry of a run with a chosen model failed with "Choose a model offered
for the selected agent provider." The launch route now resolves the model
with `_agent_handoff_model`, which takes either form for the provider and
still refuses anything else.

Tests: `AgentControlTests.testAFailedRunNoRunnerClaimedTakesAFollowUpAsARunOfItsTaskAgain`,
`ApiAgentsTests.testAFollowUpOnARetryResumesWithItOrRunsTheTaskAgainWithIt`,
`app/tests/test_org_agent_attachments.py::test_launch_takes_a_resolved_model_id_a_retry_sends_back`.
Android still hides the composer on such a run.

## Every run opens the desktop agent's page (2026-10-06)

Asked, with a screenshot of a run queued for an online desktop: "use the same
page that we use for the desktop agent detail … keep the top information, but
get rid of the steps and show the live streaming terminal only like we do with
the other desktop agents". A session sent to a desktop (no runner, not a
cloud target) still opened `LaunchedAgentSessionScreen`: the chips, the
five-step task card and the terminal-style run log under it.

Now any session with a session id opens the run screen (iOS `AgentRunScreen`,
Android `UnifiedAgentScreen`): the title, badge, repository and status line
on top, then the streamed transcript, with no step card.

- iOS: `AgentLines.runRow` no longer takes `cloud` and only needs a session
  id. When a desktop row has claimed the run (`AgentLines.claimingDesktop`,
  the same match `claimedByDesktop` uses: `localAgentId`, node, repository)
  it returns that row, so the page follows the desktop's own stream; else it
  builds `UnifiedAgentRow.fromCloud`. The `.launchedAgent` route (opened
  after a quick start) goes through it too.
- Android: the Agents tab's launched rows and `AgentsFeature.launchedSessionScreen`
  (the quick start's "open run") push `UnifiedAgentScreen` for any session
  with an id.
- `LaunchedAgentSessionScreen` is left only for a session the relay has not
  given an id yet, or on iOS when the control center is not set up.

Tests: `AgentLinesTests.testEveryCloudRunOpensTheAgentRunScreen` and
`testADesktopQueuedRunOpensTheAgentRunScreen`.

Asked again at 11:45 PM the same day, with a screenshot of a runner run
(`fmnext-r1`) still on the step page: "make it so this page is the same as if
we launched an agent to the desktop, remove these steps, update the top
buttons". The phone was on ad hoc build 180, built from `13c41a8962`. The
routing above reached main later, at 11:18 PM, in `221b4789fd`, and the Mac
builds between 7:12 and 10:13 PM all failed to archive, so no installed build
had it yet. To make the old page match as well:

- The step card is gone: iOS `AgentTaskChainCard` / `AgentStageRow` and
  Android `AgentTaskChainCard`, together with the terminal-style run log under
  them (`AgentRunTerminal` on both). `AgentTaskChain` and `AgentRunLog` stay in
  the cores for the list rows and their tests. Android's launched screen no
  longer reads `publishedPulls`, which only fed the card.
- The fallback page's top bar has one button, Delete (a trash icon, iOS
  `agent-run-delete` as on `AgentRunScreen`). It still asks "Drop this
  session?" first. The Refresh button is gone, and pull to refresh still
  re-reads.

## Rollout

Order matters, because each layer refuses what the one before it does not know
yet:

1. **App first** (`app/deploy.sh` / the usual core app deploy). It must know
   `merge-agent-branch` and `delete-agent-branch` in
   `edge_routing.PUBLIC_OPERATIONS` before any core advertises them:
   `operations_sha256` rejects a proof listing an unknown operation, and the
   App would then route around that core for every read. The new routes
   answer `merge_node_unavailable` until the cores are rolled; nothing else
   changes for old clients.
2. **Core roll** (daemon step, which ships the Go daemon with
   `tools/mirror_gateway.py`). Each core then advertises the two operations
   (only when its gateway script knows them) and skips trashed `agent/`
   tips on reconcile. Roll all three before relying on clean up: a core
   still on the old daemon keeps the branch and hands it back.
3. **Runners** by hand, as usual:
   `python3 tools/next_fleet.py install r1 --steps daemon,start --execute`.
   Before that, follow-ups still reach only the runner that ran the session,
   and named branches still work (the App picks the push ref); diff numbers
   in the rows appear once the runner reports them (or after a Changes read).
4. **Phones**: the iOS (Codemagic/Mac) and Android builds. Their new calls
   need step 1; merge and clean up need step 2.

## Queue position and countdown on a queued run (2026-10-08)

Asked, with a screenshot of a runner run (`fmnext-r1`) sitting at `queued`:
"show a countdown of what the actual timers are so we'll know how long we
should wait before the job gets picked up and … what number in the queue we
are in and how many are in the queue."

**App.** `GET …/agent-sessions/<sid>` (and the organization `agent-bots`
route) adds `session.queue` while the session is `queued` or
`security_pending` and has a `queued` row in `org_agent_jobs`
(`_org_agent_queue_projection` in `app/src/route_handlers.py`). The list
routes do not carry it, so the Agents list costs nothing more.

| field | meaning |
|---|---|
| `position`, `ahead`, `waiting` | this job's place among the `queued` jobs of the same target node (ordered by job id, the order `_lease_jobs` scans), and how many wait in total |
| `running` | jobs on that target that are `leased` or `running` |
| `repoBusy` | another job on the same repository is `leased`; `_lease_jobs` leases one job per repository at a time |
| `queuedAt`, `now` | when the job was queued, and the App's clock, so the phone corrects its own clock skew |
| `checkEveryMs`, `leaseTimeoutMs` | `CLAIM_WAIT_MAX_MS` (20 s) and `JOB_TIMEOUT_MS` (2 h) from `runner_leases.py` |
| `runner` | for a runner session only: `name`, `online`, `busy`, `activeJobs` (its unexpired leases), `lastSeenAt` (the start of its last claim), `nextCheckAt`, `busySince`, `freeBy` |

The timers are the real ones in [../RUNNERS.md](../RUNNERS.md):

- **Idle runner.** Its claim is held for up to 20 s and answers within 250 ms
  of a change in the queue, then it claims again. `nextCheckAt` is
  `lastSeenAt + 20 s`, the end of the claim it is holding. A run that is
  still queued while its runner is idle is not leasable yet (the repository
  is busy, or the provider or owner does not match), so the countdown shows
  when the runner looks again. A runner whose `nextCheckAt` is more than
  30 s past is `online: false`.
- **Busy runner.** A runner with no free slot does not claim, so
  `lastSeenAt` goes stale. It takes the next job as soon as its current job
  ends (the runner re-polls at once). The countdown is to `freeBy`, the
  earliest expiry of its leases: the latest moment a slot frees, when the 2 h
  lease times out and the job is requeued.

**iOS.** `AgentRunQueue` (`FMApi/Models/AgentRunQueue.swift`) parses the
block and computes the lines; `AgentTranscriptDelta.queue` carries it to
`AgentRunScreen`, which shows `RunnerQueueCard` under the status line: the
place ("#2 in queue · 2 others waiting · 1 running"), a once-a-second
countdown ("Next claim check in 14s", which rolls over every 20 s, or "Slot
frees by lease timeout in 1h 02m 05s"), what the runner is doing, and how
long the run has waited. While the card shows, the screen re-reads the
session when the countdown ends, no sooner than 5 s and no later than 30 s after the last read;
it stops once the run leaves the queue.

Tests: `app/tests/test_org_agent_queue_position.py`,
`ForkMeshCore/Tests/FMApiTests/AgentRunQueueTests.swift`.

## Related

- [DESKTOP_AGENTS.md](DESKTOP_AGENTS.md), [../RUNNERS.md](../RUNNERS.md),
  [../server/agent-jobs.md](../server/agent-jobs.md),
  [../server/merge-executor.md](../server/merge-executor.md)
