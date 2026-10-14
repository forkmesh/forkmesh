# Runner agents on the phone

A runner agent is a cloud session (`/api/repo/<owner>/<repo>/agent-sessions`)
that a runner or a mirror runs on its own branch on the cores
(`refs/heads/agent/<...>`, [RUNNERS.md](../RUNNERS.md)). A desktop agent is a
session on one of the account's desktops, reached over local control or the
relay ([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)). The phones, the web and the
desktop give both kinds the same row, run screen, diff, merge and clean up,
pull request and follow-up actions. This page covers what the App serves for
runner sessions and how the clients use it.

## What the App serves for a runner session

All routes are the owner route `/api/repo/<owner>/<repo>/agent-sessions/<sid>`
(and the organization route `/api/orgs/<org>/repos/<repo>/agent-bots/<sid>`),
authorised by the account session and, for POST and DELETE, an allowed
`Origin`. Signed desktop writes (merge, cleanup, link-pull, update) use the
proof `forkmesh-owner-agent-branch-v1` ([../agents/README.md](../agents/README.md)).
Code: `app/src/agent_branches.py`, imported lazily (listed in `LAZY_MODULES`,
[app/worker-footprint.md](../app/worker-footprint.md)).

### Session JSON

| Field | Meaning |
| --- | --- |
| `branch` | the session's branch on the cores, `""` until known |
| `baseBranch` | the branch it was cut from and merges into |
| `runner` | the runner's name when a runner runs it, else `""` |
| `diffStats` | `{files, added, removed, ahead, behind, conflicted, known}` from the last diff read or the runner's report |
| `merge` | `{state, base, mergeOid, mergedAt, branchDeleted, cleanupPending, conflicts, message}`; `state` is `""`, `merging`, `merged`, `conflict` or `failed` |
| `canMerge` | a branch exists, the run is idle, not merged yet |
| `prNumber` | the linked pull request, else the runner-reported one |
| `canOpenPull` | an `agent/` branch, idle, not merging or merged, no pull request, and the run published something |
| `queue` | single-session GET only, while queued (see "Queue position") |

### Diff: `GET …/<sid>/diff?page=0&pageSize=25`

The `AgentDiff` shape the phones parse for a desktop diff, plus paging and the
merge preview: `branch`, `base`, `baseOid`, `headOid`, `mergeBaseOid`,
`commits`, `ahead`, `behind`, `clean`, `conflicts`, `merged`, `files`,
`added`, `removed`, `page`, `pageSize` (1 to 100), `pageCount`, `committed`
(this page's patches), `uncommitted`, `untracked`, `hasWorktree`, `truncated`.

- Errors: `409 no_branch`, `404 branch_not_found`, `413 diff_too_large`,
  `503 mirror_unavailable`. A read also stores `diffStats` on the session.
- The diff, merge and cleanup address the repository by the name the cores
  serve: a repository linked to an organization is served as `<org>/<repo>`,
  found through `org_repos` (`_gateway_owner`). `_mirror_context` keeps the
  node's routing context and sets `routeOwner` to the organization.
- A session a mirror ran merges on that mirror first (`session_home` passed as
  `sticky` to `_https_mirror_candidates`), since it already holds the branch.

### Merge and clean up: `POST …/<sid>` `{"action": "merge", "cleanup": true, "requestId": "<id>"}`

A `git merge --no-ff` of the session branch into its base on the repository's
core, through the owner merge executor (`merge-agent-branch`,
[server/merge-executor.md](../server/merge-executor.md)). With `cleanup`, the
branch is removed in the same ref transaction after its tip is saved as
`refs/forkmesh/trash/agent-branch/<branch>/<request token>`, then on the other
cores.

- `requestId` is optional (12 to 80 of `[A-Za-z0-9_-]`); the App makes one.
- The attempt is kept in the session (`merge.state: "merging"`). For 10
  minutes (`PENDING_MERGE_MS`) every merge POST resumes it and answers with the
  original `requestId`. Later, a new `requestId` first asks the core the
  attempt went to (the executor's job record outlives the gateway's memory);
  exact-OID pins make a duplicate merge fail as `stale_base`.
- Only public repositories the cores serve; a private one answers `404
  not_found` and uses the desktop.
- `202 processing`: repeat the same body about once a second.
- `200 merged` with `branch`, `base`, `mergeOid`, `branchDeleted`,
  `cleanupPending`, `stats` (`base`, `commit`, `commits`, `files`,
  `insertions`, `deletions`) and `message`.
- `409 merge_conflict` with `conflicts` (sets `merge.state` to `conflict`),
  `409 stale_base`/`stale_head` (retry with a new id), `409 session_running`,
  `409 no_branch`, `409 already_merged`, `403 forbidden`, `503
  merge_node_unavailable`.
- A merge from the iPhone carries `deployApproval` (`{publicKey, ts,
  signature}` over `approval_message` in `app/src/deploy_approvals.py`), so the
  cores deploy an owner-approved merge while the desktop is offline. The key
  ("Deploy approval key" on Identity and recovery) goes in `approver_keys`
  under `[self_deploy]` in `deploy/fleet.toml`
  ([../deploy/self-deploy.md](../deploy/self-deploy.md)).

**A run that pushed nothing.** A runner pushes its branch only when it
committed something. `published_nothing` decides a missing branch has nothing
to merge when the run is idle, `lastError` has no `publish branch:` failure,
and no node ever reported or the runner reported 0 files and 0 commits ahead.
The diff then answers `200` with no files and the merge answers `200 merged`
with `mergeOid: ""` and `branchDeleted: true` without asking a core. Otherwise
`404 branch_not_found` explains whether the push failed or is still pending.

### Other actions

| Request | Effect |
| --- | --- |
| `{"action": "cleanup", "requestId"}` | Removes an already-merged branch (trash ref first) on every core; `200 cleaned`, `202`, `409 not_merged`; lagging cores stay in `cleanupPending` |
| `{"action": "cleanup", "discard": true}` | Removes the branch merged or not; the phones' Delete sends this before `DELETE` |
| `{"action": "update"}` | Merges the base into the branch; `202`, `updated`/`already`, `409 merge_conflict` |
| `{"action": "link-pull", "number": N}` | Records the pull request (`record["pull"]`); `200 linked`, `409 pull_exists`, `400 invalid_pull`, `409 no_branch` |
| `{"action": "stop"}` | Requests a stop (`stopRequestedAt`) |
| `{"prompt": "…"}` | Follow-up (below) |
| `DELETE` | Removes the session record, never the branch |

### Follow-ups

A follow-up carries the session's branch and base. Any eligible runner may take
it when the one that ran the session is gone, and a runner that no longer has
the session resumes it on the same branch. A follow-up on a `runner_pool`
session carries the start's `runnerOwner` as `organization`
(`agent_branches.runner_job_owner`), and `_lease_jobs` treats the runner
holding the session's latest lease as pinned, so a personal runner takes the
follow-ups of its own runs. The phones' **Continue** sends `Continue where you
left off.`

A run that failed or stopped before any runner claimed it has no
`localAgentId`; the iOS composer then reads "Follow-up (runs the task again)"
and `ApiService.retryAgentSession(...followUp:)` queues a new run with the
original task plus the follow-up. The launch route resolves models with
`_agent_handoff_model`, which accepts aliases or resolved ids.

### Start: `POST …/agent-sessions`

Optional `baseBranch` (must exist; default the repository's default branch)
and `branchName` (a slug; the App makes it `agent/<n>-<slug>` with `<n>` one
more than the highest among the repository's sessions).

### Live transcript stream

While a session is queued or running and its screen is open, both apps follow
`GET <session URL>/live`, a `text/event-stream`
([agents/README.md](../agents/README.md)):

- `text` replaces the live text; `append` extends it (ignored before a `text`
  on the same connection); `end` closes and triggers a normal re-read.
- A clean server close (after 5 min) reconnects after about 1 s; network
  errors, 5xx, 408 and 429 back off from 2 s to 30 s; other errors turn
  streaming off for that visit. A re-read keeps the streamed text unless
  `live.at` is newer.
- iOS: `FMApi/Services/AgentLiveStream.swift` over HTTP/3. Android:
  `core/.../agents/AgentLiveStream.kt` on `HttpURLConnection` with a 45 s read
  timeout; the 3 s session poll slows to 30 s while the stream is live.

### Queue position

The single-session GET adds `session.queue` while the session is `queued` or
`security_pending` (`_org_agent_queue_projection`):

| Field | Meaning |
|---|---|
| `position`, `ahead`, `waiting` | place among the target node's queued jobs (job id order, as `_lease_jobs` scans) |
| `running` | jobs on that target that are leased or running |
| `repoBusy` | another job on the same repository is leased (one per repository at a time) |
| `queuedAt`, `now` | queue time and the App's clock, for skew correction |
| `checkEveryMs`, `leaseTimeoutMs` | `CLAIM_WAIT_MAX_MS` (20 s) and `JOB_TIMEOUT_MS` (2 h) |
| `runner` | `name`, `online`, `busy`, `activeJobs`, `lastSeenAt`, `nextCheckAt`, `busySince`, `freeBy` |

An idle runner's `nextCheckAt` is `lastSeenAt + 20 s`; past 30 s it is
offline. A busy runner's countdown is to `freeBy`, the earliest lease expiry.
iOS `AgentRunQueue` drives `RunnerQueueCard` on `AgentRunScreen` (place,
once-a-second countdown, runner state, wait time) and re-reads when the
countdown ends, between 5 s and 30 s after the last read.

## Phones

Runner rows look like desktop rows: status glyph, title, branch, files and
`+added −removed` from `diffStats`, merged state, and a **Runner · <name>**
badge (`memory_outlined` glyph on Android). Any session with a session id
opens the run screen (iOS `AgentRunScreen`, Android `UnifiedAgentScreen`);
`LaunchedAgentSessionScreen` remains only for sessions without an id.

## Every run opens the desktop agent's page

- iOS `AgentLines.runRow` needs only a session id. When a desktop row has
  claimed the run (`AgentLines.claimingDesktop`: `localAgentId`, node,
  repository) it returns that row so the page follows the desktop's stream;
  otherwise `UnifiedAgentRow.fromCloud`.
- Android pushes `UnifiedAgentScreen` from the Agents tab and from the quick
  start for any session with an id.
- The run screen shows title, badge, repository and status, then the streamed
  transcript. There is no step card.

Run screen actions, all starting on the tap without a confirm:

- **View branch diff** (`RunnerDiffScreen`, the desktop diff view, paged with
  **Load more**, with **Merge & clean up** in its top bar).
- **Merge & clean up**, then the Landed numbers (iOS `MergeLandedScreen`
  counts down five seconds back to the list; Android "Merged!" with
  `MergeStatsBlock`), with **Actions** filtered to the merge commit.
- **Fix conflicts with agent** while the branch conflicts; **Update from
  <base>**; **Clean up** when merged but the branch remains; **Delete**.
- **PR #N** or **Create PR**: re-reads the session, compares the branch with
  its base, opens the pull request through the signed inbox with the device
  key (`InboxService.openNewPull`), then sends `link-pull`
  (`RunnerPullDraft`, `RunnerPullModel`).
- **Continue**, the steer composer, **Stop**.
- After a merge, every client completes the linked task (`POST
  /api/tasks/<id>/complete`, id from `taskKey` or a leading `[task:<id>]`),
  best effort.
- The start form offers a base branch when the repository has several and
  sends `branchName` from the title (`RunnerAgentText.branchSlug`,
  `RunnerBranchName.slug`: 4 words, 30 characters).

Android `RunnerAgentOps` re-POSTs a merge or cleanup once a second while 202
(up to 20 times) and retries `stale_base`/`stale_head` once with a new id.

The Branches screen on both phones offers Merge into <default>, Merge & delete,
Update from <default> and Delete with the same resume ids and outcomes (iOS
`BranchOps.swift`, Android `code/BranchOps.kt`).

Code: iOS `FMApi/Services/ApiService+RunnerAgents.swift`,
`FMApi/Models/RunnerAgentOps.swift`, `FMAgents/Remote/RunnerAgentOpsModels.swift`,
`FMControl/AgentControl.swift`, `ForkMesh/Features/Agents/AllAgents/`; Android
`core/.../agents/RunnerAgentOps.kt`, `AgentQueueApi.kt`,
`control/AgentControl.kt`, `app/.../feature/agents/AllAgentsScreen.kt`,
`RunnerAgentViews.kt`.

## Continue on desktop

The run screen of a runner or node session offers **Continue on desktop**
whenever the phone knows the account's desktop (`hub.desktop`), the session
names a repository and it is not merged. `AgentControlCenter.continueOnDesktop`:

1. Reads the session's full history.
2. Asks the desktop `agents.takeover` with `check: true`; `no_checkout` or
   `unsupported` stops here, so the runner is never stopped for nothing.
3. Stops the session on the runner if queued or running; a failed stop moves
   nothing.
4. Sends `agents.takeover` and shows the desktop's answer.
5. Deletes the runner's session record (also when the check answers
   `already`); the branch stays. If the delete fails the move stands
   (`RunnerDesktopMove.runnerKept`) and the screen stays open for Delete.

### `agents.takeover` (desktop local control)

Params: `cloudSession`, `owner`, `name`, `branch`, `base`, `runner`, `task`,
`title`, `provider`, `model`, `check`. Advertised in `status.capabilities`.

- Validation: `takeoverSessionValid`, `handoffBranchValid`,
  `handoffRefNameValid` (`LocalControlHandoff.h`); `task` 1 to 8000
  characters.
- A local agent already standing for the session (`AgentSession::webSessionId`)
  answers `already: true`.
- The repository resolves by exact owner and name, else the only checkout with
  that name (`takeoverRepoIndex`).
- It fetches `branch` (`fetchServedBranch`) and starts on it
  (`startAgentOnExistingBranch`); if the branch is missing it starts from the
  default branch and answers `fresh: true`.
- Answer `{id, branch, fresh}`; errors `bad_session`, `bad_task`,
  `bad_branch`, `no_checkout`, `not_started` (with the desktop's last local
  error as `message`).

The task text (`RunnerDesktopHandoff.task`) names the runner, session, how it
ended, branch and base, the original prompt (up to 3000 characters) and later
turns (up to 1200 each, newest kept, under 7600 total).

## Runner accounts on iOS

The iPhone switches the Claude Code or Codex account a runner uses, using the
endpoints in [../RUNNERS.md](../RUNNERS.md).

- `RunnerAccountsPanel` sits in the usage sheet the Agents usage chip opens;
  without a desktop usage client a person icon opens `RunnerAccountsSheet`.
  Both show only when a runner has at least one account.
- One card per active runner and provider. Each account shows name, plan,
  active check, usage meters (`UsageMeterRow`, amber from 70%, red from 90%)
  and `RunnerAccounts.usageNote`.
- Per-account menu: **Use this account** (`activate`), **Open Claude Code
  terminal**, **Log in again…** (`RunnerSignInSheet`), **Log out…**
  (`{action: "remove"}`). Pending rows read **Switching…** or **Logging
  out…**.
- **Terminal** per provider opens `RunnerTerminalScreen`: a `RunnerTerminal`
  (`FMAgents/RunnerTerminal.swift`) over the runner relay that reattaches to an
  open terminal, sends its size, polls output every second and sends keys in
  chunks of at most 4096 bytes.
- `RunnerAccountsModel` reads `GET /api/runners` on appear (at most every 5
  minutes) and on open or pull; 401/403 empty the list. After a `202` it
  re-reads every 2 s while a request is pending, at most 30 times. A sign-in
  is followed by re-reads now, after 6 s and after 14 s.
- Parsing follows `desktop/src/RunnerLogin.cpp`: ids are 32 lowercase hex,
  percentages clamp to 0-100, the newest request per account decides.
- Tests: `RunnerAccountsTests.swift`, `RunnerTerminalTests.swift`.

## Runner accounts on Android

`RunnerAccountsScreen` with the same endpoints, wording and rules (switch and
sign-in; no log out or terminal). Entry points: a row under the runner sign-in
banner and a button in `AgentUsageCard`. `RunnerAccountsStore`
(`core/.../runners/RunnerAccounts.kt`) per `ApiService`, read at most once a
minute; `RunnerAttentionApi.activateAccount` posts
`{provider, account, action: "activate"}` to `/api/runners/<runner>/accounts`.
Tests: `./gradlew :core:coreTest -Pfilter=runners`.

## Web

The `/<owner>/<repo>/agents` detail of a cloud session
(`renderOrgAgentSessionDetail`, `app/public/dashboard/js/06-repo-content.js`)
has **Diff**, **Stop**, **Continue**, **Merge & clean up** (or **Finish merge**
while `merging`), **Clean up**, **Update from <base>**, **PR #N** / **Create
PR**, a **Runner · <name>** badge and markdown transcripts. Merges poll every
2 s for up to 3 minutes (`ORG_AGENT_MERGE_POLLS`, `ORG_AGENT_MERGE_POLL_MS`).
A merged session shows **Actions on <base>** (`renderOrgAgentMergeActions`)
with per-workflow Run, Re-run, Stop or Approve and **Run all**, sent as
`POST …/actions/control` to the executor node. Timers must stay out of the
session renderers (`test_org_agent_bots.py`).

The gateway's merge job reloads with `prewarm=False` and warms caches on a
separate thread (`_prewarm_after_merge`), so the result is answered promptly.

## Rollout

Each layer refuses what the one before it does not know:

1. **App first**: it must list `merge-agent-branch` and `delete-agent-branch`
   in `edge_routing.PUBLIC_OPERATIONS` before any core advertises them.
2. **Cores** (daemon step with `tools/mirror_gateway.py`): all of them before
   relying on clean up, since an old core hands a cleaned branch back.
3. **Runners**: `python3 tools/next_fleet.py install r1 --steps daemon,start --execute`.
4. **Phones**.

## Tests

- App: `app/tests/test_agent_branches.py`, `test_runner_leases.py`,
  `test_agent_branch_gateway.py`, `test_web_runner_agent_actions.py`,
  `test_owner_agent_signed_sessions.py`, `test_repo_branch_management.py`,
  `test_org_agent_queue_position.py`.
- Go: `server/merge_branch_test.go`, `server/runner_steer_test.go`.
- Desktop: `test_local_control_handoff.cpp`, `test_agent_cloud_sessions.cpp`.
- iOS: `FMApiTests/RunnerAgentOpsTests.swift`, `AgentRunQueueTests.swift`,
  `FMControlTests/AgentControlTests.swift`, `RunnerDesktopHandoffTests.swift`,
  `FMAgentsTests/RunnerPullModelTests.swift`, `BranchOpsTests`.
- Android: `agents/RunnerAgentOpsTest.kt`, `control/UnifiedAgentRowBranchTest.kt`,
  `control/RunnerDesktopHandoffTest.kt`, `BranchOpsTest`.

## Related

[DESKTOP_AGENTS.md](DESKTOP_AGENTS.md), [../RUNNERS.md](../RUNNERS.md),
[../server/agent-jobs.md](../server/agent-jobs.md),
[../server/merge-executor.md](../server/merge-executor.md)
