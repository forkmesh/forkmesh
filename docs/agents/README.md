# Agent runs

ForkMesh runs coding agents (Claude Code and Codex CLIs, plus a few legacy
script providers) against repositories in four places: locally in the
desktop app, on a mirror node or runner, on another of the account's desktops
(a job queued from the website or phone), and on the phone itself. This page
follows a run through every layer. The desktop UI for agents is described in
[../desktop/agents-ui.md](../desktop/agents-ui.md).

## Code map

| Layer | Files |
| --- | --- |
| Desktop local runtimes | `desktop/src/AgentRuntimeRegistry.*`, `AgentRuntimeSession.*`, `ClaudeStreamSession.*` (Claude Code stream-json), `CodexAppServerSession.*` (Codex app-server), `AgentRunner.*` (legacy script providers) |
| Desktop sessions and storage | `desktop/src/AgentStore.*`, `AgentWorktree.h`, `AgentScratch.h`, `MainWindowAgent*.cpp` |
| Desktop memory and CPU containment | `desktop/src/AgentResources.*`, `AgentResourceGovernor.*` ([memory.md](memory.md)) |
| Desktop sandboxing | `desktop/src/ProjectAgentAccess.*`, `MainWindowAgentAccess.cpp`, `AgentNetworkGate.*`, `AgentRunRules.*`, `CommandVariables.*`, `AgentJail.h`, `AgentGuardrails.*`, `AgentPermissionController.*`, `VirtualMachineRuntime.*` ([access-rules.md](access-rules.md)) |
| Desktop claiming website jobs | `desktop/src/MainWindowAgentSync.cpp`, `OrgAgentJobAttachments.*` |
| Desktop cloud rows and Move to cloud | `desktop/src/AgentCloudSessions.*`, `MainWindowAgentCloud.cpp`, `MainWindowAgentEvents.cpp` |
| Mirror node and runner | `server/agents.go`, `server/agent_errors.go`, `server/agent_attachments.go`, `server/agent_live.go`, `server/agentstream.go` |
| Host CLI install and refresh | `desktop/src/ControlNode.{h,cpp}`, `MainWindowHostInstall.cpp`, `MainWindowHosts.cpp` |
| Relay | `app/src/route_handlers.py` (org/owner agent routes, job results), `app/src/entry.py`, `app/src/urls.py`, `app/src/agent_streams.py`, `app/src/agent_live_relay.py`; tables in `app/migrations/0093_org_agent_bots.sql`, `0191_org_agent_attachments.sql`, `0232_agent_session_streams` |
| Web UI | `app/public/dashboard/js/06-repo-content.js` (session detail), `app/public/dashboard-composer.js` ([../app/web-composer.md](../app/web-composer.md)) |
| Phone | `native/ios/ForkMesh/Features/Agents/`, `AgentWatch/`; `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/agents/`, `agentwatch/` |

## Local runs (desktop)

- **Sessions.** A run is an `AgentSession` stored under `data/agents/`
  (`AgentStore`, `meta.json` per session; deleted sessions go to a recycle
  bin with a `deleted-at.json` stamp). `AgentSession::name` is the
  repository name; the task title is `issueTitle`.
- **Titles.** A run starts with `promptTitle()` (`AgentWorktree.h`, up to 8
  words); the branch and worktree slug (`titleSlug()`) is 4 words / 30
  characters. In parallel the agent's own CLI may reword the title
  (`requestAgentTitle()`; Settings > Agents > "Agent titles"): a one-shot,
  tool-less call that replies `{"title": ...}`, at most 2 at once, killed
  after 45 s. An optional Workers AI namer uses the desktop's locally stored
  Cloudflare token directly. `AgentSession::titledBy` records who reworded
  the title. Naming requests go through `WorkspaceNameGate` (2 in flight,
  `Retry-After` honoured up to 1 hour).
- **Worktree and scratch.** Each run gets a Git worktree at
  `.worktrees/agent-<session>-<slug>` (bare mirrors and KVM runs use a temp
  directory) and a scratch folder `.worktrees/.scratch/<worktree>`
  (`AgentScratch.h`). Runs launch with `FORKMESH_AGENT_SCRATCH` naming it and
  `TMPDIR`/`TMP`/`TEMP` pointing at its `tmp/`; the agent is told to keep
  build trees, logs and throwaway files there.
- **Resuming.** A resumed ad-hoc run reuses its saved branch and worktree. If
  an older workspace naming update left those saved names out of sync, the
  desktop recovers the one unambiguous worktree whose folder and checked-out
  branch start with that session id; it never removes an existing worktree to
  make a replacement.
- **Deleting an agent deletes its area.** Every worktree removal also removes
  its scratch folder; deleting a session removes leftovers under the session
  id in `builds/` and `/tmp`. All deletes go through
  `safedelete::removeTree`, which refuses a git checkout.
- **Parity alert.** `MainWindowAgentParity.cpp` checks every local checkout
  1 minute after start and every 3 minutes (`git worktree list --porcelain`,
  `git for-each-ref refs/heads/`); `AgentParity.h` reports agents with no
  branch or worktree, orphan worktrees under the agent root and orphan agent
  branches. An issue must show in two checks in a row before one local
  warning ping. Tests: `forkmesh-agent-parity-tests`.
- **Runtimes.** Claude Code runs as a stream-json session; Codex through its
  app-server protocol. Both report provider-neutral events
  (`session.lifecycle`, `output.text.streaming`, `tool.lifecycle`, `usage`;
  `AgentRuntimeSession.h`) feeding the transcript, the tool efficiency ledger
  (`data/agents/tool-efficiency.json`) and the activity window.
- **External and cloud rows.** External Claude Code sessions on the same
  machine are rows with ids `<= -1000`, matched by the first `cwd` in their
  transcript; cloud rows use ids `<= -1000000`. Neither is written to the
  store.
- **Merge & clean up** deletes only the merged branch's session, its
  worktree, branch and the PR records whose head is that branch. The merge
  runs in the main checkout; it proceeds past unstaged or untracked edits the
  branch does not touch and refuses (naming files) when the branch touches a
  dirty file, anything is staged or the checkout is not on the base branch.

## Desktop agent sandboxing

The rules, per-provider enforcement and teammate permissions are in
[access-rules.md](access-rules.md); memory and CPU limits in
[memory.md](memory.md). In short:

- **Folder lock (Linux, default on).** Landlock restricts the agent to its
  worktree and the project's top-level folders except `data/`, `config/`,
  `cache/`, `state/`, `logs/`, `ssh/`, plus shared tool and scratch paths.
- **External-call gate.** `AgentNetworkGate.*` classifies Bash, WebFetch and
  WebSearch calls (and scripts that import network modules); for Claude Code a
  PreToolUse hook in `--settings` asks the desktop over a local socket, and an
  unreachable socket denies. Refusals prompt the user (`AgentAccessFailure.*`).
- **Destructive commands** (`classifyCommandRisk`) ask by default.
- **Codex** keeps approval policy `never`; web search is off unless allowed.
- **KVM workspace (optional, `VirtualMachineRuntime.*`)**: the guest is the
  boundary
  ([www/docs/qt-client.md](../../www/docs/qt-client.md#kvm-isolated-coding-workspace)).
- **Jail** (`AgentJail.h`) is a resource fence, not a security sandbox.
- Not covered: MCP tools and the `openai` legacy provider. The gate works per
  tool call; it is not a network sandbox.

### Legacy provider scripts

The legacy providers run Python scripts: `tools/forkmesh_coding_agent.py`
(embedded as `desktop/src/CloudflareAgentScript.h` by
`tools/sync_coding_agent_header.py`; `local-llama` and `cloudflare-ai`) and
the one-tool script in `desktop/src/ClaudeAgentScript.h` (Claude API). The
desktop passes:

| Variable | Meaning |
| --- | --- |
| `FORKMESH_AGENT_GATE` | Gate socket path. Every shell-running tool asks first with one JSON line `{"v":1,"session","label","tool":"Bash","input":{"command"},"cwd","env":{"values","hidden"}}`; a denial returns to the model as the tool result |
| `FORKMESH_AGENT_GATE_SESSION`, `FORKMESH_AGENT_GATE_LABEL` | Copied into each request |
| `FORKMESH_AGENT_DISABLED_TOOL_FAMILIES` | Comma-separated family ids from `AgentToolCatalog.cpp`; those tools are not offered and refuse if called |
| `FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES` | When non-empty, only listed families are offered |
| `FORKMESH_AGENT_ALLOWED_DIRECTORIES` | JSON list of repository-relative directories the file tools may touch; `[]` allows none; default `["."]` |
| `FORKMESH_AGENT_GUARDRAILS` | `off` turns path guardrails off, only when no allowlist is given |

`git_*` tools send their equivalent command line to the gate; `run_tests`
and `lint_project` send the configured command. Commands the model runs do
not inherit `FORKMESH_AGENT_GATE*`. Tests: `app/tests/test_coding_agent.py`,
`tools/test_forkmesh_coding_agent_guardrails.py`.

`cloudflare-ai` runs call Cloudflare directly. `agentConfigForProvider`
(`MainWindowAgentLaunch.cpp`) passes the token from Settings as
`FORKMESH_AGENT_API_TOKEN` and the account as `FORKMESH_AGENT_ACCOUNT_ID`; the
script builds `https://api.cloudflare.com/client/v4/accounts/<id>/ai/v1/chat/completions`
from them and stops with "Cloudflare endpoint or account ID is required" when
neither an endpoint nor an account is set. The model picker reads
`/ai/models/search` and the workspace namer calls the same chat endpoint. The
older relay path, a per-run ticket in `FORKMESH_AI_AGENT_AUTH` signed as
`forkmesh-ai-agent-v1` for the relay's `POST /api/ai/agent`, is no longer used
by the desktop; the relay route stays for older clients.

`app/tests/test_forkbot.py` runs the embedded script end to end
(`_run_cloudflare_agent`): it extracts the Python from the header and drives it
against a local stub of the chat-completions endpoint in a throwaway Git
repository with a fake `HOME`. Small Workers AI models loop on one command,
stop before committing, or answer with an empty turn, so the repeat guard
(adhoc #279, #1633), steering from stdin, retries of a 5xx (adhoc #1633),
the uncommitted-work check and the empty-turn check are exercised rather than
grepped. On 15 Oct 2026 these tests still assumed the relay ticket and failed;
they now use the token path. The script now echoes a tool call in the
OpenAI `tool_calls` shape, where the relay-era script wrote it as assistant
text because Workers AI chat templates behind the relay dropped `tool_calls`
(adhoc #1683); whether the chat-completions endpoint keeps them needs checking
against a live model.

## Website- and phone-queued jobs

1. The web or phone queues a prompt for one repository and one target node
   (owner routes `/api/repo/<owner>/<repo>/agent-sessions[/<id>]`;
   organization bots `/api/orgs/<org>/repos/<repo>/agent-bots[/<id>]`). Rows
   live in `org_agent_sessions` and `org_agent_jobs`; image attachments in
   `org_agent_attachments`.
2. The target node claims with a signed
   `GET /api/repo/<node>/<repo>/org-agent-jobs` and receives leased jobs (a
   desktop in `MainWindowAgentSync.cpp`, a mirror in `server/agents.go`).
3. The node validates the envelope and fails closed: session id, prompt
   length, provider (`claude-code` or `codex`), model from an allowlist
   (`agentWebsiteModels`), job kind (`start`, `continue`, `steer`),
   attachments and repository owner.
4. The node runs the CLI, publishes a branch (and a pull only when asked) and
   reports to `POST .../org-agent-jobs/<id>/result`. Attachments are fetched
   with the lease from `.../org-agent-jobs/<id>/attachments/<attachmentId>`,
   checked for size, SHA-256 and magic bytes, saved under
   `<data>/agent-images/` and named on an `Attached image: <path>` line; a
   failed fetch fails the job.
5. The relay rings the account event socket (topic `cloud-agents`). The
   desktop also reads the account queue with a key-signed
   `GET /api/account/agent-sessions` on catch-up floors.

There is no model safety preflight for queued jobs; jobs are queued with
security state `not_required`. Do not reintroduce one.

### Access rules on the relay

- Queueing a run or follow-up requires the account to own the target node
  (`_account_owns_node`) or the organization linking the repository
  (`_org_owner_of_linked_repo`). Reading, stopping and deleting only need the
  row to be in the account's own queue.
- The account list also carries organization-scope runs where the account
  has `organization.agents.read` and the required team membership. Every
  active run is listed (up to 500); finished runs are capped (40 account-wide,
  50 per repository) and `finishedBefore` marks the cut. Every client keeps a
  row missing from a cut answer only when it is finished and at or before
  `finishedBefore`.
- Desktop requests are key-signed instead of cookie-authenticated:
  `forkmesh-owner-agent-queue-v1` (binds path and the SHA-256 of
  `provider\nmodel\ncreatePr(1|0)\nfromDevice\nprompt`),
  `forkmesh-owner-agent-read-v1` (bound to the read path),
  `forkmesh-owner-agent-stop-v1` (owner route only, body exactly
  `{"action":"stop"}`) and `forkmesh-owner-agent-branch-v1` (merge, cleanup,
  link-pull, update). A signed queue never names its target; the relay picks
  the freshest owned `headless` or `runner-pool` node advertising the
  provider, or answers `no_cloud_agent_node` (409).

### Prompt images

A launch or follow-up carries up to five images. Turns record
`attachments: [{id, name, mediaType, size}]`; sealed bytes stay in
`org_agent_attachments` for a week (`ORG_AGENT_ATTACHMENT_KEEP_MS`).
`GET .../agent-sessions/<sid>/attachments/<id>` (or the `agent-bots`
equivalent) serves one image: signed in or with a read proof for that exact
path; on the organization route only the member who attached it may read it
(`403 attachment_private`); expired or mismatched rows answer
`410 attachment_expired`. The body is `application/octet-stream` with
`nosniff`. The web, desktop and phones cache thumbnails and do not retry
refusals.

## Live transcript on the web

The website's session page follows a running Claude Code run.

- **Node** (`server/agent_live.go`): `agentLiveReporter` sends the transcript
  so far at most once per 2.5 s (`agentLiveReportInterval`; 250 ms while
  watched), skipping unchanged text, as a `running` report with `live: true`.
  It stops before the closing report. Codex runs report only at the end.
- **Relay** (`_apply_org_agent_live_transcript`): stores
  `session.live = {text, at}` with a compare-and-set on `updated_at`, returns
  `live` only while `running`, and rings `cloud-agents-live` (not
  `cloud-agents`), so clients that re-read the whole queue are not woken every
  step.
- **Web** (`02-helpers.js`, `06-repo-content.js`): both topics and every
  reconnect fire `forkmesh:cloud-agents`; with the Agents tab open the page
  re-reads the list and patches the open session in place, keeping scroll and
  any draft. Nothing polls.

Text arrives per assistant message, not per token.

### Pinned last prompt on the web

The cloud and desktop session pages (`06-repo-content.js`) and World's agent
sessions (`world.js`) pin the last prompt above the transcript, like the
phone and the desktop (asked for on 2026-10-22). It is one line, with edges
peeking out below for earlier prompts and `2/5` with ‹ › buttons when there
are several. Swipe or drag the card sideways, use a trackpad's sideways
scroll, or press ← → while it has focus: right goes back to the previous
prompt, left goes forward to the next. Click the text to read all of it.

- Prompts are the `user` history entries (cloud and World) or the desktop's
  `_local_user` events, with `Attached image:` lines dropped.
- `promptDecks` / `worldPromptDecks` keep the prompt you are reading per
  session across live patches. A new prompt resets it to the latest.
- A live patch swaps the deck (`data-…-detail-deck`) only when its markup
  changed and nobody is dragging it, so a read-back is not interrupted.
- The dashboard's markup uses only classes already in the committed
  `tailwind.css` (see `docs/app/content-first-pings.md`). The drag and
  `touch-action: pan-y`, which keeps vertical scrolling, are inline styles.

### Live stream to the phone

`GET /api/repo/<owner>/<repo>/agent-sessions/<id>/live` (or the `agent-bots`
equivalent) is a `text/event-stream` (HTTP/3 where supported): `retry: 1000`,
then `event: live` frames carrying the whole transcript (`{"at","text"}`)
first and appends (`{"at","append"}`) after, `: keepalive` every 15 s, and
`event: end` (`{"status"}`) when the run ends. A stream closes after 5
minutes. The relay (`app/src/agent_live_relay.py`) lives in memory in the
session creator's `ForkMeshNodes` object, reached by internal
`/api/nodes/agent-live/publish|watch` paths clients cannot call; limits are
64 watched sessions per owner object and 16 watchers per session (429). While
watched, the runner reports every 250 ms but stored writes and rings stay at
2.5 s.

### Live output of cloud runs on the desktop

For the one cloud session a desktop has open, the runner or mirror
(`server/agentstream.go`) streams raw stream-json lines: validated, shortened,
token-scrubbed, kept in a 256 KiB backlog, posted as numbered chunks of at
most 8 KiB about once a second while watched (a 5 s heartbeat otherwise) to
`POST /api/runners/stream` or `.../org-agent-jobs/<id>/stream`. The relay
(`app/src/agent_streams.py`) accepts only the current lease holder, keeps the
newest 256 chunks, rings `cloud-agent-stream` to at most 8 watchers, and
deletes chunks 2 minutes after the terminal report. The viewer reads
`GET .../agent-sessions/<id>/stream?after=<seq>` with a read proof bound to
`/stream` (answer `{chunks, nextSeq, lastSeq, job, stream, reset, truncated,
more, live, status}`, 64 chunks a page); a read marks the session watched for
30 s. Codex runs send nothing. Desktop side:
[../desktop/agents-ui.md](../desktop/agents-ui.md#live-output-of-a-cloud-run).

Runner transcripts arrive as plain text entries (prose, or `▸ <Tool>:
<summary>` lines); `cloudAgentTranscriptEvents` turns tool lines into
`tool_use` blocks with `_summaryOnly: true` so they draw like a local run.
Prompt images are cached under `<data>/agent-images/cloud/` after size and
magic-byte checks.

## Moving runs between places

- **Continue in cloud** (`agentCloudHandoff`) commits the session's tree,
  pushes its branch into the served mirror and posts
  `/api/repo/<owner>/<repo>/agents/<id>/handoff` with branch, base, runtime
  and task; the transcript does not travel. The mirror waits up to 10 minutes
  for the branch. The desktop signs as its account; the relay accepts the
  node's key or the key of an account that owns the node or the linking
  organization.
- **Continue here** on a queued owner row posts
  `.../agent-sessions/<id>/claim` and reports on
  `.../org-agent-jobs/<id>/result`, signed as the account. Organization-scope
  jobs cannot be claimed.
- **Ended or running cloud runs** (`cloudAgentActions()`):

  | run | actions |
  |---|---|
  | queued, no node started it | **Continue here** claims it |
  | running, or queued after a node started it | follow-ups, or "Stop the cloud run and continue here" |
  | ended | **Continue here**, **Retry in cloud**, follow-ups |
  | ended and `cloudAgentNeedsRunnerSignIn()` | **Sign in runner** first |

  Continue here on an ended or stopped run fetches the run's `agent/` branch
  from the served mirror (`fetchServedBranch`), picks a start with
  `cloudAgentContinuePlan()` (the tip, else the base branch with a notice),
  and starts a local agent on a new branch cut from it, seeded by
  `cloudAgentBranchContinuationPrompt()`. Retry in cloud re-queues the
  original prompt with the same provider, model and `createPr`.
- **Phone to desktop:** the phone pushes its `agent/` branch and the desktop
  continues on it (local-control `agents.handoff`; see
  [../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).
- The relay never writes `stopped`: a stopped run ends `failed` with
  `stopRequestedAt` and a stop turn, which clients read as stopped.

## Mirror runs

`server/agents.go`: at most 2 concurrent runs, 2 hour timeout, 8 claims per
repository per pass; state in `agent-jobs/state.json` beside the gateway
config. `availability()` checks that the CLI is on `PATH` and credentials
exist (`CLAUDE_CODE_OAUTH_TOKEN`, `ANTHROPIC_AUTH_TOKEN`,
`ANTHROPIC_API_KEY` or `~/.claude/.credentials.json` for Claude Code;
`OPENAI_API_KEY` or a Codex login for Codex) and the node advertises provider
availability in the catalog. See [../server/agent-jobs.md](../server/agent-jobs.md).

## Pull requests are explicit

An agent run never opens a pull request unless the person starting it asked
for one for that run. `createPr` is false everywhere by default; the
finish-time hooks (`maybeCreatePullForStreamSession`, `openPull` in
`server/agents.go`) act only when it is true. Teammates have no pull-request
permission. `desktop/tests/validate_agent_branch_only_contract.py` and
`app/tests/test_owner_agent_sessions.py` pin the defaults.

## Error surfacing

- **Node** (`server/agent_errors.go`): `lastError` comes, in order, from the
  result event's errors, an API-error assistant message, stderr's last
  non-empty lines, then the exit status; capped at 600 runes.
- **Outdated CLI:** "does not support this model" becomes "Claude Code on
  <node> is <ver>; <model> needs <X> or newer. Update the host (Hosts ->
  Update)."; the runner remembers the refused version per provider and model
  (`agentCLIFloorKey`).
- The error is shown beside the transcript, never appended to it. A failed
  run with no reply records "The run stopped before the agent replied.".

## Agent CLI refresh on hosts

Every host update pipeline ends with a root-only, never-fatal refresh of the
agent CLIs the host serves (`withAgentCliServiceRefresh()`,
`agentCliServiceRefreshScript()` in `desktop/src/ControlNode.cpp`): run the
official installers, install to `<name>.new` and `mv -f` over the old copy so
running jobs keep their binary. The update's exit status is unchanged.

## Operations

- Mirror CLI sign-in: **Network → Hosts** has an agent-CLI installer and a
  sign-in shell per host.
- A job stuck unclaimed: check the target node is online and advertises the
  provider.
- A terminal report the relay keeps refusing (6 conflicts) is dropped
  (`agentReportConflictLimit`).

## Traps

- Two desktop execution paths (legacy `AgentRunner` pool and stream/Codex
  sessions) have separate completion hooks. `anyAgentRunning()` must cover
  both, or Rebuild & restart waits forever.
- A guard written as `sessionId < 0` also hits external and cloud rows; use
  `isExternalSession()`/`isCloudSession()`.
- New agent launch paths must apply the project access policy
  (`applyProjectAgentAccess`), take tool options from
  `agentRunLaunchOptions(sid, codex)` and the gate environment from
  `agentNetworkGateEnv(sid)`, and use the memory containment calls
  (`applyEnvironment`, `wrapShellCommand`, `contain`).

## Related docs

- [tools.md](tools.md): every tool each kind of agent can call
- [access-rules.md](access-rules.md), [memory.md](memory.md)
- [../desktop/agents-ui.md](../desktop/agents-ui.md)
- [../server/README.md](../server/README.md) and [../server/agent-jobs.md](../server/agent-jobs.md)
- [www/docs/operations/organization-agent-offline-queue.md](../../www/docs/operations/organization-agent-offline-queue.md)
- [www/docs/mobile/mobile-agent-access.md](../../www/docs/mobile/mobile-agent-access.md)
