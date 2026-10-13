# Agent runs

ForkMesh runs coding agents (Claude Code and Codex CLIs, plus a few legacy
script providers) against repositories, in four places: locally in the desktop
app, on a managed mirror node, on another of the account's desktops (a job
queued from the website or phone), and on the phone itself. This page follows
a run through every layer. The desktop UI for agents is described in
[../desktop/agents-ui.md](../desktop/agents-ui.md).

## Code map

| Layer | Files |
| --- | --- |
| Desktop local runtimes | `desktop/src/AgentRuntimeRegistry.*`, `AgentRuntimeSession.*`, `ClaudeStreamSession.*` (Claude Code stream-json), `CodexAppServerSession.*` (Codex app-server), `AgentRunner.*` (legacy script providers) |
| Desktop sessions and storage | `desktop/src/AgentStore.*`, `AgentWorktree.h`, `MainWindowAgent*.cpp` |
| Desktop memory and CPU containment | `desktop/src/AgentResources.*`, `AgentResourceGovernor.*` ([memory.md](memory.md)) |
| Desktop sandboxing | `desktop/src/ProjectAgentAccess.*`, `MainWindowAgentAccess.cpp`, `AgentNetworkGate.*`, `CommandVariables.*`, `AgentGuardScript.h`, `AgentJail.h`, `AgentGuardrails.*`, `AgentPermissionController.*`, `VirtualMachineRuntime.*` |
| Desktop claiming website jobs | `desktop/src/MainWindowAgentSync.cpp` |
| Desktop cloud rows and Move to cloud | `desktop/src/AgentCloudSessions.*`, `MainWindowAgentCloud.cpp`, `MainWindowAgentEvents.cpp` |
| Mirror node runner | `server/agents.go`, `server/agent_errors.go`, `server/agent_attachments.go`, `server/agent_live.go` (live transcript) |
| Live output of cloud runs | `server/agentstream.go` (runner/mirror side), `app/src/agent_streams.py` (relay), `desktop/src/AgentCloudStream.*` and `MainWindowAgentCloud.cpp` (viewer) |
| Host CLI install and refresh | `desktop/src/ControlNode.{h,cpp}`, `MainWindowHostInstall.cpp`, `MainWindowHosts.cpp` |
| Relay (Worker) | `app/src/route_handlers.py` (org/owner agent routes, job results), `app/src/entry.py` (`repo_org_agent_jobs_handler`), `app/src/urls.py`, tables in `app/migrations/0093_org_agent_bots.sql`, `0191_org_agent_attachments.sql` |
| Web UI | `app/public/dashboard/js/06-repo-content.js` (session detail), `app/public/dashboard-composer.js` (prompt dock on every page, [../app/web-composer.md](../app/web-composer.md)) |
| Phone | `native/ios/ForkMesh/Features/Agents/`, `native/ios/ForkMesh/Features/AgentWatch/`, `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/agents/`, `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/agentwatch/` |

## Architecture

### Local runs (desktop)

- A run is an `AgentSession` stored under `data/agents/` (`AgentStore`,
  `meta.json` per session; deleted sessions go to a recycle bin with a
  `deleted-at.json` stamp). `AgentSession::name` is the repository name; the
  task title is `issueTitle`.
- A typed prompt's title comes from `promptTitle()` (`AgentWorktree.h`, up to
  8 words), then, when a naming model is set in Settings, from a direct
  Workers AI chat-completions call. The desktop uses its locally stored
  `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`; the prompt never passes
  through ForkMesh. The branch and worktree slug built from the same text
  (`titleSlug()`) stays at 4 words / 30 characters.
- Since 2026-09-29 the agent's own CLI writes the list title
  (`requestAgentTitle()`, `MainWindowAgentLaunch.cpp`; Settings > Agents >
  "Agent titles", on by default). `promptTitle()` cut run-on prompts into
  fragments, and the Cloudflare namer was opt-in and rate-limited per account.
  The run starts at once with the `promptTitle()` title, and in parallel a
  one-shot, tool-less call reads the whole prompt and replies with
  `{"title": ...}` (`agentTitleRequest()`, `parseAgentTitle()`,
  `forkmesh-agent-prompt-title-tests`). Claude Code runs
  `claude -p --model haiku --max-turns 1 --tools ''` with no settings or MCP
  servers. Codex runs `codex exec --ephemeral --sandbox read-only` at low
  reasoning effort. Both run in the temp dir, as the run's active agent
  account. Measured on 2026-09-29: 4 to 10 s for Haiku, about 4 s for Codex at
  low effort and 14 s at its default effort. That is too slow to hold the
  launch, so the branch and worktree keep their prompt-derived name, and only
  the title changes when the answer lands (`applyAgentTitle()`). A run already
  linked to an issue keeps the issue's title. At most 2 title calls run at
  once, each killed after 45 s. Launches past that limit, other providers,
  and failed or unparseable replies keep the `promptTitle()` title. When
  Cloudflare naming is also on, it names the branch and worktree, and it only
  sets the title if the agent has not replied yet.
- Since 2026-10-08 the run records who reworded its title in
  `AgentSession::titledBy` (`titledBy` in `meta.json`): "Claude Code (Haiku)"
  or "Codex" from `applyAgentTitle()`, "Cloudflare AI" from the workspace
  namer. The agent detail shows it as a "Title by" row, so a title that no
  longer matches the prompt's first words says where it came from. A Genie
  task title clears it, and a run linked to an issue hides the row, since its
  title is the issue's.
- Naming requests go through `WorkspaceNameGate` (`AgentWorkspaceNameGate.h`,
  `forkmesh-agent-workspace-name-tests`): at most 2 in flight, and after a
  429 no request until `Retry-After` (capped at 1 hour); after a timeout or
  other failure, none for 60 s. A launch the gate turns away starts at once
  with the `promptTitle()` name.
- Deleting a cloud session that the relay answers `404 not_found` for drops the
  row as already gone ("The relay no longer had this cloud agent session.",
  `cloudAgentDeleteFoundNothing` in `AgentCloudSessions.cpp`) and reloads the
  list, which brings the row back if the session still exists under another
  route. The 19 such 404s (2026-09-27 18:14 to 18:52) came from a build that
  still addressed runs by repository owner instead of `relayRepoOwner()`
  (fixed by 999cc56671), and every `mirror35` 403 stopped once 97c7d847e8
  deployed: the last `mirror35` sessions were deleted with 200 on 2026-09-28
  12:58. Follow-ups to a session on a node the account no longer owns still
  answer 403 by design.
- Each run gets a Git worktree inside the project at
  `.worktrees/agent-<session>-<slug>` (`AgentWorktree.h`). Bare mirrors and
  KVM runs use a temporary directory instead.
- **Each agent stays in its own area (2026-10-06, task 1416).** Next to its
  worktree every run gets a scratch folder, `.worktrees/.scratch/<worktree>`
  (`AgentScratch.h`), for build trees, logs, test output and throwaway
  scripts. Claude Code, Codex and the legacy runner launch with
  `FORKMESH_AGENT_SCRATCH` naming it and `TMPDIR`/`TMP`/`TEMP` pointing at its
  `tmp/`, and `AgentRunner::toolUseInstruction()` tells the agent to keep
  everything there instead of `/tmp`, `~/.cache` or a shared builds folder.
  With the jail or guardrails on, the jail's `tmp/` and `cache/` live in the
  same folder, and the folder lock allows it read-write. Only a worktree whose
  parent carries the agent-root `.gitignore` marker gets one, so a run in the
  main checkout or a hand-made worktree does not. Before this, agents put
  build trees in `~/projects/forkmesh/builds/agent-<session>*` and scratch in
  `/tmp/fm<session>*`, and deleting the agent removed only the worktree: on
  2026-10-06 `builds/` held 16 GB of trees and logs for long-gone agents and
  the disk was full.
- **Deleting an agent deletes its area.** Every worktree removal (Merge &
  clean up, delete agent, Delete worktree/branch/agent, the Cleanup package,
  the legacy runner's end-of-run teardown) also removes the worktree's scratch
  folder and sweeps any scratch folder whose worktree is gone. Deleting the
  session (`deleteStoredAgentSession`) removes, on a worker, the leftovers
  older runs left under the session id: `builds/agent-<id>`,
  `builds/agent-<id>-*`, `builds/agent-<id>.*`, `/tmp/fm<id>`,
  `/tmp/fm<id>[-_.]*`, `/tmp/_tmp_fm<id>*` and the jail folders
  `/tmp/forkmesh-agent-jails/s<id>` and `s<id>-triage`. All of it goes through
  `safedelete::removeTree`, which refuses a git checkout, so a clone an agent
  made in `/tmp` stays. Claude Code's own subagent worktrees under the main
  checkout's `.claude/worktrees` are not tied to a session and are not removed.
- **Branch, worktree and agent parity alert (2026-10-07, task 1663).** The
  operator asked to be told when agents, their branches and their worktrees
  fall out of step. `MainWindowAgentParity.cpp` checks every repository with a
  local checkout on a worker 1 minute after startup and every 3 minutes after
  (`git worktree list --porcelain` and `git for-each-ref refs/heads/`), and
  `AgentParity.h` decides what is wrong:
  - an agent with no branch: a local, non-cloud agent that is not queued,
    cleared, merged, **No worktree** (`directCheckout`) or association-only;
  - an agent with no worktree, or one Git lists whose folder is gone: the same
    agents while running or waiting, and finished Claude Code and Codex runs
    (they keep their worktree; Auto runs that are not vibe, and other
    providers' finished runs, remove it by design);
  - a worktree directly under the agent root (`.worktrees` with the ForkMesh
    `.gitignore` marker, hidden folders such as `.scratch` skipped) whose
    branch and folder belong to no agent;
  - a branch named like an agent branch (`<id>-<slug>` or
    `agent-<id>-<slug>`) that no agent claims and no worktree has checked
    out. An agent in the recycle bin still claims its branch, since it can be
    restored, but not its worktree.
  An issue must show in two checks in a row before it alerts, so a launch or a
  Merge & clean up in progress does not. The alert is one warning ping, kept
  on this machine (`kLocalOperationNoticeKind`), listing up to 8 new issues; an
  issue alerts once and again only after it was gone for a check. The watch
  does not run in window tests. Tests: `forkmesh-agent-parity-tests`.
- Runtimes: Claude Code runs as a stream-json session; Codex runs through its
  app-server protocol. Both report provider-neutral events
  (`session.lifecycle`, `output.text.streaming`, `tool.lifecycle`, `usage`, …;
  `AgentRuntimeSession.h`), which feed the transcript, the tool efficiency
  ledger (`data/agents/tool-efficiency.json`) and the activity window.
- External Claude Code sessions running on the same machine are surfaced as
  rows with ids `<= -1000`; cloud rows use ids `<= -1000000`. Neither is
  written to the store.
- An external session is matched by the folder it was launched in (the first
  `cwd` in its transcript), not the last one. Claude Code rewrites `cwd` on
  every line after a `cd`, so until 2026-09-24 an agent that `cd`'d into a
  subfolder of its worktree stopped matching the worktree and was listed a
  second time as an external session. A session inside a ForkMesh agent's
  worktree or any of its subfolders is the agent's own; a no-worktree agent
  claims only the checkout root. Transcripts whose folder is gone (a worktree
  cleaned up after its run) are skipped.
- **Merge & clean up** deletes only the merged branch's session and repaints
  with `refreshAgentListAfterRemoval()`; stop paths reload from cache. Neither
  rescans every session.
- **Merge & clean up deletes the branch's PR** (2026-10-04, task 1366). Once
  the worktree, branch and Agent record are gone, `deletePullsOfMergedBranch`
  removes every PR record whose head is the merged branch (or `pr/<N>`, or a
  deleted session's `prNumber`) from `.forkmesh/pulls/` and reloads the PR
  list, so the merged PR no longer lingers under Merged. The merge is still
  recorded first (`reconcileMergedPulls`) while the branch resolves. Until then
  the PR was kept with status merged; the user wants it gone like the branch.
  The PR page's **Merge + delete** now also removes the head branch's agent
  worktree and Agent record before deleting the PR, so both buttons leave
  nothing of the PR behind.
- **Merge & clean up with a dirty main checkout.** The merge runs in the
  repository's main checkout, not the branch's worktree, so edits there (another
  agent, a manual change) are invisible in the branch's Changes list. The merge
  goes ahead when those edits are unstaged or untracked and the branch changes
  none of their files (`git diff base...branch`); Git leaves them in place and
  the log names them. It refuses, naming the checkout path and up to five files,
  when the branch touches a dirty file, anything is staged, the checkout is not
  on the base branch, or Git can't list either side (issue #654).

### Desktop agent sandboxing

- **Global agent rules (2026-10-02).** Settings > Agents > Global agent rules
  (`MainWindowGlobalAgentRules.cpp`) edits
  `~/projects/forkmesh/config/agent-rules.json` (`GlobalAgentRules` in
  `ProjectAgentAccess.*`). It lives in the home project's `config/` folder,
  which no agent's folder lock grants, so an agent cannot loosen its own rules.
  It holds:
  - default folder boundary, shared tools and external-call setting for every
    project that follows the global rules (a project with no `agentAccess`
    key, or with `"followGlobal": true`; the project gear has a "Follow the
    global agent rules" box);
  - extra folders and approved destinations that apply to every project;
  - blocked destinations no project can allow;
  - the destructive-command rule: ask (default), block or allow;
  - an optional tool allowlist of tool families.
  `effectiveAgentAccess` merges a project's policy with these, and
  `agentrules::decide` (`AgentRunRules.*`) turns the result, the destructive
  rule and the run's teammate permissions into run, ask or deny. The Claude
  hook, the Codex approval path, the legacy scripts and the transcript chips
  all use it, so one setting means the same thing for every provider. The
  full table, teammate permissions included, is in
  [access-rules.md](access-rules.md).
- **Destructive commands.** `classifyCommandRisk` (`AgentNetworkGate.cpp`)
  flags recursive deletes (outside `/tmp`), `find -delete`, force pushes and
  remote branch deletes, `git reset --hard`, `git clean -f`, `git branch -D`,
  `git stash drop/clear`, history rewrites, disk wipes and `gh repo delete`.
  With the rule on Ask the agent waits on a prompt (Allow once, Allow for this
  run, Always allow in this project or every project, Deny) that shows the
  highlighted command, why it is destructive and reasons to allow or deny it
  ([access-rules.md](access-rules.md#destructive-commands)). The PreToolUse hook is therefore installed for almost every
  Claude run; local commands that are not destructive are answered at once.
- **Tool allowlist.** With it on, Claude Code launches with `--tools` naming
  the allowed families' built-in tools plus `ToolSearch` and the plan-mode
  tools, so a tool a newer Claude Code adds stays off. Codex and the script
  providers get every family outside the list switched off as far as they
  can. Tool efficiency switches still apply on top.
- **Codex.** Codex runs keep approval policy `never`, including when external
  calls or destructive commands are gated. Codex web search is switched off unless the network
  setting is Allow or `web-search` is an approved destination, because Codex
  never asks before searching. Where the folder lock cannot apply (not Linux),
  Codex uses its own `workspace-write` sandbox with the project's folders as
  writable roots and network on; on Linux it keeps `danger-full-access` inside
  the Landlock lock, because Codex's sandbox also blocks loopback servers.
  Codex command output goes through the same access-failure detector; the
  answer reaches the agent as a steering message, since Codex does not wait.
- **KVM workspace.** Inside the VM Claude is not gated because its hook cannot
  reach the host socket;
  the guest is the boundary.
- **Built-in Claude terminal.** Gets the folder lock, the gate hook, the tool
  policy and the agent intake environment, like stream-json runs. Inserted
  list options use `--opt=value` so they cannot swallow the prompt argument.
- **Auto-model triage.** `claude -p` for model routing runs with `--tools ''`
  and `--strict-mcp-config`, so it has no tools at all.

- **Folder lock (Linux, default on).** Per-project policy in `project.json` key
  `agentAccess` (`ProjectAgentAccess.*`); the gear beside a project in the
  project picker edits it. Landlock restricts the agent to its worktree, the
  project's top-level folders except `data/`, `config/`, `cache/`, `state/`,
  `logs/`, `ssh/`, plus a few shared tool and scratch paths.
- **External-call gate.** `AgentNetworkGate.*` classifies Bash, WebFetch and
  WebSearch calls. For Claude Code a PreToolUse hook, inlined into
  `--settings`, asks the desktop over a local socket; an unreachable socket
  denies. When an agent is refused access to a folder or
  host, the desktop prompts the user (`AgentAccessFailure.*`).
  The hook sends the output of every failed tool call, so the detector has to
  tell a real resolver or connect error from text that only quotes one. Since
  2026-09-28 a network match is ignored when the tool only reads or writes
  files (`Read`, `Grep`, `Glob`, `LS`, `Edit`, `MultiEdit`, `Write`,
  `NotebookEdit`, `NotebookRead`, `TodoWrite`), when the captured host is
  followed directly by `"`, `\`, `&`, `<` or a backtick (a string literal or
  an HTML-escaped echo), and when the host is not a valid DNS name or IP
  address. Agent #459 had grepped `tests/test_agent_access.cpp`; line 348,
  `expectFailure(QStringLiteral("curl: (6) Could not resolve host:
  cdn.jsdelivr.net"),`, raised an "Agent network access" prompt, and agent
  #684 then got one for `cdn.jsdelivr.net&amp;quot` from the echoed report.
  Folder permission errors still count for every tool.
- **KVM workspace (optional).** Agents run in a Lima QEMU/KVM guest
  (`VirtualMachineRuntime.*`); the folder lock is skipped there too. See
  [www/docs/qt-client.md](../../www/docs/qt-client.md#kvm-isolated-coding-workspace).
- **Jail.** `AgentJail.h` is a resource fence (private TMPDIR/cache, memory
  cap), not a security sandbox.
- **Memory containment (Linux, default on).** Every agent runs in its own
  systemd scope inside `forkmesh.slice`, which is capped below physical
  memory so the desktop and the rest of the system keep a reserve; builds an
  agent starts with `systemd-run --user` join the same slice, and under a
  sustained memory stall the newest such build is frozen until the others
  finish. The slice's CPU is capped below the core count and lowered while
  other programs need the CPU. See [memory.md](memory.md).
- Legacy `AgentRunner` providers (Claude API, local Llama, Cloudflare AI)
  ask the same gate from their Python scripts, and honour the tool-family
  settings and path guardrails; see
  [Legacy provider scripts](#legacy-provider-scripts-gate-tool-families-guardrails).
- Not covered by the gate: MCP tools, and the `openai` legacy provider
  (`codex -a never exec`), which only gets the folder lock. The MCP window
  tools need the per-agent intake token and, once a connector is published,
  `FORKMESH_MCP_TOKEN`
  ([../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md)). The gate
  works per tool call; it is not a network sandbox.
- Scripts an agent runs (`python3 tools/x.py`, `./x.py`, `node x.mjs`) count
  as external when their source imports a network module; the target is the
  first non-local URL in the file. That is how Hearth's
  `discord_community.py` and the Reddit agent's `reddit_scout.py`, which used
  to classify as local, now reach the gate.

### Legacy provider scripts: gate, tool families, guardrails

The legacy providers run Python scripts rather than an agent CLI:
`tools/forkmesh_coding_agent.py` (embedded as
`desktop/src/CloudflareAgentScript.h` by `tools/sync_coding_agent_header.py`;
`local-llama` and `cloudflare-ai`) and the one-tool script in
`desktop/src/ClaudeAgentScript.h` (Claude API). Until 2026-10-02 they skipped
the external-call gate and the tool-family settings, and the coding agent's
path guardrails were off unless the desktop sent an allowlist. The desktop
passes these environment variables; the scripts implement them:

| Variable | Meaning |
| --- | --- |
| `FORKMESH_AGENT_GATE` | Gate socket path. When set, every shell-running tool asks before it runs: one JSON line `{"v":1,"session","label","tool":"Bash","input":{"command"},"cwd","env":{"values","hidden"}}` (`env`: the variables the command uses, see [access-rules.md](access-rules.md#variables-and-scratch-deletes-2026-10-07)), one JSON line back with `decision` `allow` or `deny` and a `reason` (the Claude Code hook's protocol, `AgentNetworkGate.cpp` `hookScript()`), 880 s timeout. Unreachable, timed out, or an answer that is not `allow`/`deny` denies. Unset: nothing is asked. |
| `FORKMESH_AGENT_GATE_SESSION`, `FORKMESH_AGENT_GATE_LABEL` | Copied into each request. |
| `FORKMESH_AGENT_DISABLED_TOOL_FAMILIES` | Comma-separated family ids from `AgentToolCatalog.cpp`, case-insensitive. Tools of these families are not offered to the model and refuse if called anyway. |
| `FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES` | Same format. When non-empty, only tools whose family is listed are offered (and still not if disabled). Empty or unset: no allowlist. |
| `FORKMESH_AGENT_ALLOWED_DIRECTORIES` | JSON list of repository-relative directories the file tools may touch. `[]` (or unparsable) allows none. |
| `FORKMESH_AGENT_GUARDRAILS` | `off` (or `0`, `false`, `no`, `disabled`) turns the path guardrails off, only when no allowlist is given. |

Coding agent (`forkmesh_coding_agent.py`):

- Families: `bash`, `execute_command`, `run_tests`, `lint_project` and every
  `git_*` tool are `bash`; `read_file` is `read`; `write_file` is `write`;
  `replace_text`, `apply_patch`, `undo_edit` are `edit`; `grep`,
  `search_files` are `grep`; `list_directory` is `glob`
  (`TOOL_FAMILIES`). Withheld tools are named to the model in a developer
  message and printed at start. The end-of-run test check goes through
  `run_tests`, so it is skipped when `bash` is off and asks the gate when it
  runs; a denied check is reported as not run, never as "Tests pass".
- Gate: `bash`/`execute_command` send their command; `run_tests` and
  `lint_project` send the configured command (nothing is asked when none is
  configured); `git_*` tools send the equivalent command line, e.g.
  `git status --short --branch` or `git add -A && git commit -m <message>`.
  A denial comes back to the model as the tool result,
  `Not run: ForkMesh did not allow ...` plus the reason. The harness's own
  `git status`/`git ls-files` reads (planner, unfinished-work check) are not
  tool calls and are not gated.
- Guardrails default on. With no `FORKMESH_AGENT_ALLOWED_DIRECTORIES` the
  allowlist is `["."]`, the repository root, so structured tool calls
  (including the paths inside an `apply_patch` patch) are checked against the
  root; `AgentConfig` defaults match. An explicit allowlist always wins over
  `FORKMESH_AGENT_GUARDRAILS=off`, and `[]` keeps meaning "no directories",
  as in `AgentGuardrails.cpp` `policyDescription()`.
- Commands the model runs do not inherit `FORKMESH_AGENT_GATE*`.

Claude API script (`ClaudeAgentScript.h`): its only tool is `bash`, so the
run stops before any API call with a message when `bash` is disabled or
missing from a non-empty allowlist. Each `bash` call asks the gate first
(`run_tool`), with the script's working directory as `cwd`.

Tests: `app/tests/test_coding_agent.py` (gate allow/deny/unreachable/garbled/
timeout against a temporary `AF_UNIX` server, family filters, guardrail
defaults, the Claude API script), `tools/test_forkmesh_coding_agent_guardrails.py`.
`app/tests/test_forkbot.py` runs the embedded script as a subprocess and drops
the gate variables from its environment, since the desktop exports them to
the agents it launches.

### Website- and phone-queued jobs

1. The web or phone queues a prompt for one repository and one target node
   (owner routes `/api/repo/<owner>/<repo>/agent-sessions[/<id>]`; organization
   bots `/api/orgs/<org>/repos/<repo>/agent-bots[/<id>]`). Rows live in D1
   `org_agent_sessions` and `org_agent_jobs`; image attachments in
   `org_agent_attachments`.
2. The target node claims with a signed
   `GET /api/repo/<node>/<repo>/org-agent-jobs` and receives leased jobs. A
   desktop target claims in `MainWindowAgentSync.cpp`; a mirror target claims
   in `server/agents.go` (`agentJobRunner.claim`).
3. The node validates the envelope and fails closed: session id, prompt length
   (8000 runes on the mirror), provider (`claude-code` or `codex`), model from an
   allowlist (`agentWebsiteModels` in `server/agents.go`; the desktop keeps the
   same list in `MainWindowAgentSync.cpp`, which accepts `claude-opus-5-5`), job
   kind (`start`, `continue`, `steer`), attachments, and repository owner.
4. The node runs the CLI on a scratch clone, publishes a branch (and a pull
   when asked), and reports to `POST .../org-agent-jobs/<id>/result`. Attachments
   are fetched from `.../org-agent-jobs/<id>/attachments/<attachmentId>` with
   the lease. A desktop target does the same for `start` and `steer` jobs
   before the run starts (`src/OrgAgentJobAttachments.*`): each image is
   checked for size, SHA-256 and magic bytes, saved as
   `<data>/agent-images/relay-<uuid>.<ext>`, and named on an
   `Attached image: <path>` line after the prompt, so the CLI, the transcript
   and the phone see it like a pasted image. An image that cannot be fetched
   or saved fails the job rather than running the prompt without it. Before
   this, the desktop ignored `attachments`, and a phone prompt with a photo
   ran as text only. An issue-numbered `start` still runs from the issue
   record, not the queued prompt.
5. The relay rings the account event socket (topic `cloud-agents`) so the
   desktop and phone update without polling. The desktop also reads the whole
   account queue with a key-signed `GET /api/account/agent-sessions`, with
   catch-up floors of 5 minutes (2 minutes while a run is in flight).

#### A session on a node the account no longer owns

An owner session is stored under the account that queued it (`org_bi` is the
account's blind index), but its route is addressed by the repository node,
e.g. `/api/repo/mirror35/forkmesh/agent-sessions/<sid>`. Queueing a run or a
follow-up still requires the account to own that node
(`_account_owns_node` or `_org_owner_of_linked_repo`). Reading a session,
reading its images, stopping it and deleting it only need the row to be in the
account's own queue (`own_session` in `_org_agent_member_context`).
Before this, retiring a mirror (e.g. `mirror35`, see `STATUS_RETIRED_MIRRORS`)
left its sessions in the account-wide list, but every action on them returned
`403 forbidden` / `use_owner_account`. That included the desktop's Delete and
its clean-up after moving cloud runs back to the desktop.

#### Prompt images on the transcript

A launch or follow-up can carry up to five images. The session turn records
`attachments: [{id, name, mediaType, size}]`; the job also records each
image's `sha256`. The sealed bytes stay in `org_agent_attachments` for a week
(`ORG_AGENT_ATTACHMENT_KEEP_MS`). They used to be deleted as soon as the node
reported on the job, which left the transcript with names but no pictures.
Rows older than a week are swept the next time anybody stores images, unless
a queued, leased or running job of the session still needs them. Deleting
the session deletes them at once.

`GET /api/repo/<owner>/<repo>/agent-sessions/<sid>/attachments/<id>` (owner
route) and `GET /api/orgs/<org>/repos/<repo>/agent-bots/<sid>/attachments/<id>`
(organization route) serve one image back (`org_agent_session_attachment_handler`):

- The caller is signed in, or signs a read proof for that exact path. A proof
  for the session itself does not open its images.
- On the owner route the session is the account's own. On the organization
  route only the member who attached the image may read it (`403
  attachment_private`); other members see the name and size.
- An id the session does not list, a row older than a week, or bytes whose
  length does not match the turn answer `410 attachment_expired`.
- The body is `application/octet-stream` with `nosniff`, so a link opened
  directly never renders in the relay's origin. The client labels the bytes
  with the turn's media type.

The web Agents tab shows each prompt's images as thumbnails under the turn
(`orgAgentAttachmentsHtml`, `hydrateOrgAgentAttachments` in
`app/public/dashboard/js/06-repo-content.js`). It fetches them with the
session's bearer token and keeps at most 40 as blob URLs. A click opens the
image full size in a dialog. A refusal shows its reason in place of the
thumbnail. A 403, 404 or 410 is remembered for the page's lifetime. The live
transcript patches the open session every few seconds
(`patchOrgAgentDetail`), and each patch reloads the thumbnails from that
cache, so an expired image is not requested again on every patch. The phone reads the same routes ([../native/AGENT_ATTACHMENTS.md](../native/AGENT_ATTACHMENTS.md)).

The account list also carries the organization-scope runs (queued on the web
through `/agent-bots`) of every organization the account belongs to, up to 20
organizations, but only where the org route itself would let the account read
them: a role with `organization.agents.read` and membership of the Engineering
floor team (`_account_org_agent_rows`). Each row says `scope`
(`owner` or `organization`); the list is merged newest first. Every active
run is listed (up to 500); finished runs are capped at 40, and when the cap cut
some off the answer's `finishedBefore` is the `updatedAt` of the oldest finished
row it kept (0 when nothing was cut). The per-repository list
(`GET .../agent-sessions` and `.../agent-bots`, `org_agent_bots_handler`) reads
the same way through `_account_agent_scope_rows`: every active run, the 50
newest finished ones, and `finishedBefore`. It used to answer the 50 newest
rows of any status, so a repository with 50 or more runs in flight listed no
finished run at all.

The web dashboard (`orgAgentEndpoint` in
`app/public/dashboard/js/06-repo-content.js`) only calls the organization route
when the catalog row names an organization owner (`logicalOwners`, or no
owner fields at all on an older row). A repository owned by a person, such as
`jett/test` viewed by another account, has no organization behind
`/api/orgs/jett/...`, so every pull request page and Agents tab visit used to
answer 404 and trip the 404 alert. Such a repository now gets the same
`not_found` error locally, with no request.

Every client applies one rule to a cut answer: a row it already shows that the
answer left out stays when it is finished and its `updatedAt` is at or before
`finishedBefore`; any other missing row was deleted and goes. The desktop does
this in `cloudAgentBeyondListedWindow`, the website in
`keepRepoAgentsBeyondWindow` (`app/public/dashboard/js/06-repo-content.js`),
iOS in `AgentSessionList.keeping` (FMApi) and Android in
`AgentsList.keptCloudRuns` (core).
The phone Agents screens apply it to each repository's queued runs, and the
iOS and Android All agents lists (`AgentControlCenter`) to the account answer.
The phone lists also keep a repository's rows, and the All agents cloud rows,
when a read fails, instead of emptying them until the next read works. The iOS
Agents tab now also refreshes on the `cloud-agents` account topic, as Android
already did.
The desktop marks org rows (`CloudAgentSession::organization`) and reads,
steers and deletes them on `/api/orgs/<org>/repos/<repo>/agent-bots/<id>`, which
accepts the same key-signed proofs as the owner routes (list, read, prompt,
delete; never queue). A signed triple is then the only credential, as on the
owner routes. Org rows cannot be claimed with "Continue here"; the node serving
the repository drains them.

There is no model safety preflight. The Haiku ALLOW/DENY gate for website jobs
was removed on 2026-09-13 at the user's request (merge `1c2f2c2f5`); the Worker
queues every job with security state `not_required`. Do not reintroduce it.
Organization members' prompts therefore run on another account's node with no
model screen; that is a deliberate trade-off.

### Live transcript on the web

Added 2026-09-27. The website's session page used to show a run only once it
had ended: the node reported `running` at the start and the transcript at the
end, and the page redrew only on **Refresh**. Now the page follows a Claude
Code run as it happens.

- **Node** (`server/agent_live.go`): `claudeStream` calls back on every
  assistant text block or tool call; `agentLiveReporter` sends the transcript
  so far at most once per 2.5 s (`agentLiveReportInterval`; 250 ms while a
  phone watches, see [Live stream to the phone](#live-stream-to-the-phone)), skipping
  unchanged text, as a `running` report with `live: true` on the same result
  route (mirror) or `/api/runners/report` (runner tier). It stops, and waits
  for any report in flight, before the closing report, so a live report never
  lands after it. Codex runs still report only at the end.
- **Relay** (`_apply_org_agent_live_transcript` in
  `app/src/route_handlers.py`): a live report on a running job and session
  stores `session.live = {text, at}` and appends nothing to `history`. The
  write is a compare-and-set on `updated_at`, so a follow-up prompt or stop
  written meanwhile is never overwritten; a lost live report is replaced by
  the next one. Every other report drops `live`. The projection returns `live`
  only while the session is `running`. It rings `cloud-agents-live`, not
  `cloud-agents`, so desktops and phones (which re-read the whole account
  queue on `cloud-agents`) are not woken every step, and the closing
  `cloud-agents` ring is not coalesced away by a live one.
- **Web** (`02-helpers.js`, `06-repo-content.js`): both topics, and every
  socket reconnect, fire `forkmesh:cloud-agents`. With the Agents tab open the
  page re-reads the session list (one read in flight, one queued), redraws
  the list, and patches the open session in place: the header and transcript
  are replaced, the scroll stays pinned to the bottom when it was there, and
  the follow-up composer is kept while it holds a draft or focus. The live
  text is drawn as a last "streaming" agent entry and as the list row's
  preview. Nothing polls.

The phones follow the same live text in the run screen's transcript; the
terminal-style log under the queued session's step card was removed on
2026-10-06 ([runner agents](../native/RUNNER_AGENTS.md), "Every run opens the
desktop agent's page"). They re-read on
`cloud-agents-live` only while that screen is open on an active session, and
since 2026-10-05 follow the run's `/live` stream instead
([Live stream to the phone](#live-stream-to-the-phone)).

Text arrives per assistant message, not per token: the CLI runs without
`--include-partial-messages`, which older host CLIs refuse.

### Live stream to the phone

Added 2026-10-05 ("have the phone connect directly to the runner so agents
start and update instantly through QUIC"). Before this a phone saw a runner
step 3 to 6 s late: the runner reported at most every 2.5 s, the App rang
`cloud-agents-live` (itself coalesced to one ring per 2 s), and the phone then
re-read the whole session, or Android polled it every 3 s.

**Why not a direct phone-to-runner connection.** The user chose the
App-relayed stream (option A) over a direct QUIC connection. A direct link
would have needed a public UDP port, a TLS name and a new sign-in check on
the one machine that runs untrusted agent code. Runners accept only SSH
(`render_runner` in `deploy/core/render.py`), and personal runners sit behind
home routers anyway. Starting is already fast: a runner holds `/api/runners/claim`
open and picks a new job up within about 250 ms.

**Route.** `GET /api/repo/<owner>/<repo>/agent-sessions/<id>/live`, or
`/api/orgs/<org>/repos/<repo>/agent-bots/<id>/live`. Both share the `/stream`
regexes in `app/src/urls.py`, so the same handler
(`org_agent_bot_stream_handler`) authorizes them, and the owner READ proof
covers them. The response is `text/event-stream`, carried over HTTP/3 where
the client supports it. The cores' Caddy serves `h1 h2 h3` and proxies with
`flush_interval -1`.

| Frame | Meaning |
|---|---|
| `retry: 1000` | Reconnect delay hint. |
| `event: live`, `data: {"at","text"}` | The whole live transcript. Always the first live frame on a connection. |
| `event: live`, `data: {"at","append"}` | Text to add to what this connection already sent. |
| `: keepalive` | Sent every 15 s. |
| `event: end`, `data: {"status"}` | The run is no longer queued or running. The client re-reads the session for its history, and the server closes the stream. |

A stream closes after 5 minutes. A client that still sees an active run
reconnects. A finished session gets `end` at once.

**Relay** (`app/src/agent_live_relay.py`). The relay lives in memory inside
the session creator's `ForkMeshNodes` object, the same per-owner object as
the account event socket. Cluster placement therefore puts every publisher
and watcher of a session on one core, and the mesh proxies the streamed body
from there. Two internal paths reach it, `/api/nodes/agent-live/publish` and
`/api/nodes/agent-live/watch`. The public route forwards only
`/api/nodes/events` to the object, so clients cannot call either path. The
watch is seeded from the stored `session.live`, so a new viewer sees text at
once.

Limits:
- 64 watched sessions per owner object, and 16 watchers per session (429).
- A session's entry is dropped when its last watcher leaves.
- A publish with no watchers is a no-op that answers `watchers: 0`.

**Pace.** `_apply_org_agent_live_transcript` publishes every live report to
the relay. The App answers the runner `watched: true` while the relay has a
watcher, and `agentLiveReporter` (`server/agent_live.go`) then reports every
250 ms (`agentLiveWatchedInterval`) instead of every 2.5 s. While watched, the
App still writes `session.live` and rings `cloud-agents-live` only once per
2.5 s (`PERSIST_MS`), so database writes and socket rings stay at the old
rate. The closing report publishes `{status}`, which ends every open stream.

**What it does not change.** Text still arrives per assistant message or tool
call, not per token (see above). The `/stream` route (raw stream-json chunks
for the desktop) is separate and unchanged. The dashboard keeps its
ring-and-re-read path.

### Runner transcripts on the desktop

Added 2026-10-03 ("make runner output look nice like desktop output"). A
runner or mirror reports a Claude Code run as plain text (`claudeStream` in
`server/agents.go`): entries separated by a blank line, each either what the
agent said or a tool line `▸ <Tool>: <summary>`. The desktop used to wrap a
whole reply in one assistant text block, so those tool lines showed as prose.

`cloudAgentTranscriptEvents` (`desktop/src/AgentCloudSessions.cpp`) now splits
each assistant turn into events: runs of prose become text blocks, and runs of
tool lines become `tool_use` blocks whose input carries the summary under the
key the transcript view reads for that tool (`command` for Bash, `file_path`
for Read/Edit/Write, `pattern` for Grep/Glob, `query`, `url`, `path`, else
`description`), plus `_summaryOnly: true`, which tells
`ClaudeTranscriptView::toolBody` to draw no body (an Edit with no strings
would otherwise be an empty diff, and an unknown tool a JSON dump of the
summary). The view then draws them as it draws a local run: a terminal
panel per command, a captioned row per tool. There are no results to attach,
since the runner never reports tool output. A turn that ends on tool calls
gets an empty closing text block so the view's activity indicator stops.
Tool ids are `cloud-<session>-<turn>-<n>`. A turn is now several events, but
`MainWindowAgentCloud.cpp` counts the events of the turns it has seen, so
only new turns are fed to the sonifier.

The node lists `pattern` before `path` in `claudeToolSummary`, so a Grep with
a path is captioned by its pattern. Codex runs report only their final text
and are unchanged.

### Prompt images in cloud transcripts on the desktop

Added 2026-10-06. A prompt sent from a phone or the website with images showed
on the desktop as text only: `parseCloudAgentRows` kept a turn's `role`,
`author`, `text` and `at` and dropped its `attachments`. That happened whether
the desktop was online or not, so a desktop that was off when the prompt was
sent never showed the image once it came back.

The relay already keeps each prompt image for a week
(`ORG_AGENT_ATTACHMENT_KEEP_MS`) behind
`GET /api/repo/<owner>/<repo>/agent-sessions/<id>/attachments/<attachmentId>`
(or `/api/orgs/<org>/repos/<repo>/agent-bots/<id>/attachments/<attachmentId>`),
signed with the account key and the read proof, and the session payload lists
each user turn's `attachments` (`id`, `name`, `mediaType`, `size`). Now:

- `CloudAgentTurn::images` holds a user turn's attachments: an id of 32 hex
  characters, a PNG, JPEG, GIF or WebP media type, and a size up to 5 MB.
  Anything else is dropped. A user turn that has images and no text is kept.
- `MainWindow::fetchCloudAgentImages` runs after each cloud-session refresh and
  after a full session read. Up to three downloads run at a time. Each file
  goes to `<data>/agent-images/cloud/cloud-<session>-<attachment>.<ext>`
  (`cloudAgentImageFileName`; the session id is reduced to `[A-Za-z0-9_-]`).
  It is written with `QSaveFile` only after its size and magic bytes match
  (`orgagentjobs::checkImage`). The turn metadata has no digest, so the
  digest is not checked. The refresh happens when the desktop starts and
  whenever it reconnects, so images sent while it was off arrive then.
- Turns older than a week are skipped, since the relay no longer has their
  images. A 403 (an organization image attached by another member), 404, 410
  or a bad or oversized file is not retried until the desktop restarts. A network error
  or any other status is retried on the next refresh.
- `cloudAgentTranscriptEvents(cloud, imageDirectory)` adds an
  `Attached image: <path>` line under the prompt for each image already on
  disk. `ClaudeTranscriptView::addUserTurn` draws those lines as thumbnails,
  as it does for a local prompt. When a download finishes for the open
  session, the transcript is drawn again.

Tests: `desktop/tests/test_agent_cloud_sessions.cpp` covers which images are
parsed, image-only turns, the cache file name (including a hostile session
id), and the transcript with and without a cached file.

### Move to cloud

**Continue in cloud** in the desktop's agent detail
(`MainWindowAgentEvents.cpp`, `agentCloudHandoff`) commits the session's tree,
pushes its branch into the served mirror, and posts
`/api/repo/<owner>/<repo>/agents/<id>/handoff`. The job carries branch, base,
runtime and task; the transcript does not travel in it (it is owner-sealed on
the agent snapshot transport). The mirror waits up to 10 minutes for the
branch to arrive (`agentHandoffBranchWait`).

The desktop signs the handoff as its signed-in account, not as the repository
owner. An organization repository's owner is the org alias (`forkmesh`), which
no desktop can sign for; the relay rewrites the alias to the node serving the
repository and `_agent_handoff_signer` accepts the node's own key or, when the
query's `owner` names another account, that account's key if it owns the node
(`_account_owns_node`) or owns the organization that links the repository
(`_org_owner_of_linked_repo`), the same rule as the owner agent-session route.
The session row is stored under the signer's blind index so it shows in that
account's list.

### Continue on this desktop

"Continue here" on a queued owner row posts
`/api/repo/<owner>/<repo>/agent-sessions/<id>/claim`; the desktop then reports
the run on `.../org-agent-jobs/<id>/result`. Both requests are signed as the
signed-in account, like the handoff. Until 2026-09-27 they were signed as the
repository owner, so on an organization repository (`forkmesh/forkmesh`) the
proof named the org alias while the relay checked it against the backing
node, and every claim failed with 401 `relink_desktop` even though the
desktop was logged in. `_authorized_agent_claimant_key` accepts the node's own
proof (a mirror's claimed node key) or, when the query's `owner` names another
account, that account's key under the handoff rule: it owns the node or owns
the organization that links the repository.

The claim then checks that the job belongs to the claimant. A job's
`organization` is normally the node the run was filed under, the URL owner
after the org alias rewrite. A job queued for a `runner-pool` target is
different: `_org_agent_target_mirror` sets its `organization` to the queuing
account (`runnerOwner`) so the runner's `allowed_owners` check passes. Until
2026-09-27 the claim compared `organization` only with the URL owner, so the
account that queued a runner job (for example `jett` on `forkmesh/forkmesh`,
served by `fmnext-n1`) got 403 "This agent session was not queued by this
account." when it moved that job to its desktop. `_agent_job_queued_by_signer`
now also accepts the claim when all of these hold: `organization` names the
signing account (the query's `owner`); that account proves its key; it owns
the node or the organization linking the repository; and the job row's
`org_bi` is that account's blind index. Organization-scope jobs are still
refused, because their `org_bi` is the organization's blind index, not an
account's.

### Continue a phone agent on the desktop

The phone's on-device agent can be moved to one of the account's desktops:
the phone commits and pushes its `agent/` branch, and the desktop fetches it
and continues on it with a prompt that carries the phone's conversation
(local-control `agents.handoff`). See
[../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md), "Moving a phone
agent to the desktop".

### Cloud launch from the desktop composer

With the composer's cloud toggle on, the desktop queues a new run on the same
owner route the website uses, `POST /api/repo/<owner>/<repo>/agent-sessions`,
but key-signed instead of cookie-authenticated (a desktop that authenticated
silently holds the account key and no session token). The proof is
`forkmesh-owner-agent-queue-v1`; its canonical binds the path and the SHA-256
of `provider\nmodel\ncreatePr(1|0)\nfromDevice\nprompt`, and the body must
carry exactly those five keys, so a captured proof cannot be replayed with a
different task, runtime or repository. The browser-origin check is waived only
for a signed request, and then the triple is the only credential
(`_org_agent_member_context`).

A signed queue never names its target. The relay picks it in
`_owner_agent_cloud_node`: the freshest owned node serving the repository whose
runtime is `headless` or `runner-pool` and which advertises the provider,
skipping `fromDevice`. With none, it answers `no_cloud_agent_node` (409)
rather than falling back to a desktop, because the toggle means "not on a
desktop". The model may be a resolved id or an alias; anything else falls back
to the provider default (`_agent_handoff_model`), as for a handoff.

### Continuing, retrying and unblocking ended cloud runs

Added 2026-09-27, when every queued run on the runner `fmnext-n1` failed with
an expired Claude login (diagnostic `provider_credential_missing`, "OAuth
session expired") and the desktop had no way forward: **Continue here** only
took runs no node had started, so it was greyed out on all of them.

What a cloud row offers now comes from `cloudAgentActions()` in
`desktop/src/AgentCloudSessions.cpp` (tested in
`desktop/tests/test_agent_cloud_sessions.cpp`):

| run | actions |
|---|---|
| queued / `security_pending`, no node started it | **Continue here** claims it (unchanged) |
| running, or queued after a node started it | follow-ups, or **Continue here** as "Stop the cloud run and continue here" (since 2026-09-30) |
| ended (`failed`, `rejected`, `completed`, `cancelled`, `stopped`, `expired`, `timeout`) | **Continue here**, **Retry in cloud**, follow-ups while the relay still takes them |
| ended and `cloudAgentNeedsRunnerSignIn()` | **Sign in runner** first, then the above |

`cloudAgentNeedsRunnerSignIn()` is true for the relay's
`provider_credential_missing` code, and for a failed run whose diagnostic or
`lastError` reads like a provider login failure ("OAuth session expired",
"Please run /login", "not logged in", "Invalid API key",
"authentication_error"). The relay does not set that code on runs that
failed mid-run, only when the node's availability snapshot says the login is
missing.

- **Continue here** on an ended run (`continueFinishedCloudAgentHere`) does
  not claim anything: the relay has no lease to hand over. It starts a local
  ad-hoc agent in this desktop's working copy through
  `startAdHocAgentForRepo`, seeded by `cloudAgentContinuationPrompt()`: which
  run it continues and why it stopped, the original prompt (the first user
  turn, or the title when the history was truncated), then the rest of the
  conversation, newest turns kept within 24 KB. The local session's
  `webSessionId` is set to the cloud run's id, so the Agents list shows the
  local run in place of the cloud row. The website row is not updated by this
  local run: it has no lease to report on. The blockers of the claim path still
  apply (working copy, signing for the account, organization queue).
- **Retry in cloud** (`retryCloudAgentInCloud`) queues the original prompt
  again, with the run's provider, model and `createPr`, on the owner queue
  route through `postCloudAgentQueue`, the same key-signed
  `forkmesh-owner-agent-queue-v1` request the composer's cloud toggle sends
  (`startQuickAddCloudAgent` now uses it too). When the run needs a runner
  sign-in, a question offers **Sign in runner, then retry**: the runner
  sign-in dialog opens and the retry is queued when that sign-in ends
  `succeeded`. Organization runs are retried from the website.
- **Sign in runner** opens the runner sign-in dialog
  ([../RUNNERS.md](../RUNNERS.md#desktop-app-integration)) for the run's
  provider. The runner is the one named by the run's `targetNode`, else the
  only active runner, else the only one whose CLI for that provider needs a
  sign-in (`runnerForTarget()` in `desktop/src/RunnerLogin.cpp`). Otherwise the
  desktop opens **Network → Hosts**, where the Runners list shows them all.

#### Continue here on the cloud run's branch (2026-09-30)

Until 2026-09-30 **Continue here** on an ended run started the local agent
from the checkout's current branch, so a runner run's commits on
`agent/<session>` were not in the new worktree; the phone's **Continue on
desktop** (`agents.takeover`, [../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md#continue-on-desktop-2026-09-30))
already continued on that branch. A run that was running, or queued with
follow-ups after a node started it, was greyed out with "fmnext-n1 has
already started this run... Send it a follow-up instead", while the phone
could stop it and move it.

Now `continueCloudRunOnItsBranch` (`desktop/src/MainWindowAgentCloud.cpp`):

1. Reads the branch from the row: `agentInfo.branchName`, else the runner
   projection's top-level `branch` (`app/src/agent_branches.py`,
   `projection_extras`: `branch`, `baseBranch`, `runner`, `diffStats`,
   `merge`, `canMerge`, all parsed into `CloudAgentSession`).
2. Fetches it from the repository's served mirror or clone URL
   (`fetchServedBranch`, the same fetch `agents.takeover` uses). Only
   `agent/` branches are fetched (`cloudAgentContinueBranch`), so a row
   naming `main` never fast-forwards a local branch the operator uses.
3. Picks the start with `cloudAgentContinuePlan()`
   (`desktop/src/AgentCloudSessions.cpp`): the fetched tip; or, when the
   branch is missing on the mirror, merged (`merge.state == "merged"` or
   `agentInfo.merged`), or absent, the run's `baseBranch` (else the
   checkout's own branch) with a notice saying which and why. A missing
   branch is flashed as an error; the notice is also in the agent log and in
   the prompt ("None of the cloud run's changes are in this working copy").
4. Starts an ordinary ad-hoc agent with `startAdHocAgentForRepo`, passing the
   start branch as its base, so the agent gets its usual `NNN-slug` branch
   and worktree cut from the cloud tip (`baseRef` is that tip). After the
   start, `baseBranch` is set to the run's own base (`main`) when it resolves,
   so a pull request targets that base and, being branch-backed, carries the
   cloud commits and the desktop's. The agent log records the upstream:
   "Continuing cloud run <id> from <runner> on top of agent/<id> (upstream
   agent/<id> at <sha>)". `handoffFromProvider` is not set: it would make the
   launch replace the prompt with the local log.
5. Carries the run's provider (default Claude Code) and model (the
   `agentInfo.model` the node reported, else `requestedModel`, for example
   `claude-opus-5-5`), and links the local session with `webSessionId` as
   before.

The prompt is `cloudAgentBranchContinuationPrompt()`: the continuation
prompt above with a paragraph after its first line saying where the work is
("Its work so far was pushed to branch agent/x (started from main). It is 2
commits ahead, with 3 files changed (+40/-2). This working copy is a new
branch cut from that branch's tip...").

This differs from `agents.takeover` in one way: takeover checks out
`agent/<session>` itself (`startAgentOnExistingBranch`); **Continue here**
cuts the desktop's own `NNN-slug` branch from its tip, so the cloud branch
stays as the runner left it.

**Stop the cloud run and continue here.** For a run a node has started that
is still `queued`, `running` or `security_pending`
(`cloudAgentStoppable()`), **Continue here** asks first (non-modal question
`agentCloudStopContinueQuestion`, button "Stop the cloud run and continue
here"). On yes, `postCloudRunStopThenContinue` sends the web Stop,
`POST .../agent-sessions/<id>` with exactly `{"action":"stop"}`, key-signed
with the new proof `forkmesh-owner-agent-stop-v1` (canonical: proof, signer,
path, time; no body digest, because the body can only be that one value).
The relay (`_owner_agent_signed_session`) accepts it only on the owner route
`/api/repo/<owner>/<repo>/agent-sessions/<id>`, never on an organization
`agent-bots` path, and only with that exact body; the handler then runs
`_org_agent_stop_session` exactly as for the website's Stop: follow-ups not
yet claimed are withdrawn, a `kind:"stop"` job goes to the node, and the
audit is `owner.agent_stop`. An account with an unconfirmed email is refused
like every other signed write. If the stop fails, nothing is started and the
inline notice says why. If it succeeds, the desktop re-reads the session (read
proof) for the newest branch, then continues as above with the status shown
as `stopped`. As on the phone, commits the runner pushes after the stop are
not in the new worktree.

The bulk **Move cloud runs to this desktop** is unchanged; its confirmation
already says each run's copy on the relay is deleted.

**Branch actions (2026-10-04).** A desktop without a session token merges,
cleans up and links a pull request for a cloud run with the proof
`forkmesh-owner-agent-branch-v1`, on the owner route only. The body may carry
only `action` (`merge`, `cleanup` or `link-pull`), `cleanup`, `discard`
(booleans), `requestId` (string) and `number` (integer); the canonical binds
the SHA-256 of those five values joined by newlines, each empty when absent
(`_owner_agent_branch_text`); `update` (merge the base into the run's
branch) joined the actions on 2026-10-04. The branch diff,
`GET …/agent-sessions/<id>/diff`, is read with the read proof bound to the
diff path. See [../desktop/agents-ui.md](../desktop/agents-ui.md), "Cloud runs
merge, stop and open pull requests like desktop agents".

Tests: `desktop/tests/test_agent_cloud_sessions.cpp` (runner projection
parsing; tip present, missing, merged, absent and non-`agent/` branches;
stoppable states; the stop canonical),
`desktop/tests/validate_cloud_agent_claim_contract.py` (fetch before start,
confirm before stop, stop before continue),
`app/tests/test_owner_agent_signed_sessions.py` and
`app/tests/test_org_agent_stop.py` (the signed stop is owner-only, binds its
path and body, and runs the web Stop with its audit).

The three are rail buttons in the detail header (`agentContinueHereButton`,
`agentSignInRunnerButton`, `agentRetryInCloudButton`) and entries in the cloud
row's right-click menu; a disabled one says why in its tooltip.

The phone apps' Agents tab has the same retry for relay-queue runs, from the
list row and the session screen (`retryAgentSession()` in
`native/ios/ForkMeshCore/Sources/FMApi/Services/ApiService+Agents.swift` and
`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/agents/AgentQueueApi.kt`).
A run a node claimed gets its original task back as a follow-up, which resumes
it on its branch; a run no node started is queued again with the same node,
provider, model, issue and `createPr`. The relay never writes a `stopped`
status: a stopped run ends as `failed` with `stopRequestedAt` set and a "Stop
this run." user turn (state `stopping`) in its history. `AgentLaunchSession`
reads that marker on both platforms (`isStopped`), so the app labels the run
stopped and its run log ends "run stopped" rather than "run failed", and the
retry prompt skips the stop turn, falling back to the title when the history
was trimmed down to it. Tests: `testARelayStoppedRunIsStoppedAndRetriesItsTask`
in `native/ios/ForkMeshCore/Tests/FMApiTests/AgentLaunchTests.swift` and
`testRelayStoppedRunIsStoppedAndRetriesItsTask` in
`native/android/core/src/test/kotlin/com/forkmesh/nativeapp/agents/AgentLaunchTest.kt`.

### Live output of cloud runs

A cloud run used to reach the desktop only as whole history entries: the
running report ("Prompt dispatched.") and, at the end, the transcript. The
Agents view now shows the run's events as they happen, but only for the one
cloud session a person has open, and nothing is sent for any other run.

This is separate from the web's live transcript (above). That one sends the
readable transcript text of every running Claude Code job as a `live` report
every 2.5 s, whether or not anyone is looking, and the website draws it as one
"streaming" entry. This one sends the raw stream-json events, only while a
desktop has the session open, so the desktop can draw tool calls and tool
results the way a local run's are drawn. The two were built at the same time
(2026-09-27) and run side by side in `agentJobRunner.run`. They share
`runClaudeAgent`'s line sink and nothing else.

1. **Runner or mirror** (`server/agentstream.go`). `runClaudeAgent` taps every
   stream-json line it already parses (`withAgentStreamTap`). Each line is
   validated as JSON, shortened when it is over 32 KiB (every string cut to
   4 KiB, which keeps big tool results readable; a line still too long is
   dropped), scrubbed of token-shaped text with the web sign-in's
   `scrubTokens`, and kept in a backlog of at most 256 KiB, cut at line
   boundaries. While nobody watches, the run posts only a heartbeat
   (`seq: 0`, no data) every 5 s. When the answer says `watched: true`, it
   posts the backlog and then new output as numbered chunks of at most 8 KiB,
   about once a second (up to 8 posts in one tick while catching up). A
   rejected post (lease gone, run ended, out of order) stops the stream for
   that job; a network error retries the same chunk with the same number. On
   exit the stream flushes for at most 5 s, then the terminal report goes out
   as before. A runner posts runner-signed to `POST /api/runners/stream`
   (`{jobId, leaseId, seq, data}`); a mirror posts to
   `POST /api/repo/<node>/<repo>/org-agent-jobs/<id>/stream` with its signed
   collaboration query and the lease id. Codex runs have no stream-json and
   send nothing, and neither do runs a desktop claimed.
2. **Relay** (`app/src/agent_streams.py`, migration
   `0232_agent_session_streams`). A post is accepted only from the holder of
   the job's current lease. `agent_stream_state` holds the streamed job, that
   job's last accepted runner sequence, and the relay's own session sequence.
   Chunks are scrubbed again and stored in `agent_stream_chunks`, and only the
   newest 256 of a session are kept. A newer job of the same session (a
   follow-up) deletes the older job's chunks. Each stored chunk rings
   `cloud-agent-stream` on the account event socket of every account reading
   the stream (`agent_stream_watchers`, at most 8; `notify_account_event`
   coalesces to one frame per 2 s). The terminal result report marks the
   stream ended (`_apply_org_agent_job_result`); its chunks are hidden at once
   and deleted 2 minutes later. A stream with no post and no read for 3 hours
   is swept, and deleting the session deletes its stream.
3. **Viewer route.** `GET /api/repo/<owner>/<repo>/agent-sessions/<id>/stream?after=<seq>`
   and, for organization sessions,
   `GET /api/orgs/<org>/repos/<repo>/agent-bots/<id>/stream?after=<seq>`.
   Authorization is the session read's: the same member context, the same
   session lookup, and for a desktop the `forkmesh-owner-agent-read-v1` proof,
   bound to the `/stream` path, so a read proof for the session cannot be
   replayed here or the other way round. A read of a `queued` or `running`
   session marks it watched for 30 s. The answer is
   `{chunks: [{seq, job, data}], nextSeq, lastSeq, job, stream, reset, truncated, more, live, status}`,
   with 64 chunks a page. `stream` identifies the state row, so a reader
   notices when it was recreated. `reset` means the cursor was past the
   stream, and `truncated` means chunks between the cursor and the first
   chunk returned have already left the ring.
4. **Desktop.** See
   [../desktop/agents-ui.md](../desktop/agents-ui.md#live-output-of-a-cloud-run).

Why a heartbeat rather than the claim poll: a runner's claim poll is every
~20 s and a mirror has no poll of its own on this path, so a viewer would wait
up to 20 s for output. A 5 s heartbeat per running job is one small signed
request, and it is the only traffic an unwatched run makes.

### Mirror runs

`server/agents.go`: at most 2 concurrent runs, 2 hour timeout, 8 claims per
repository per pass; state in `agent-jobs/state.json` beside the gateway
config. Before running, `availability()` checks that the CLI binary is on
`PATH` and that credentials exist (`CLAUDE_CODE_OAUTH_TOKEN`,
`ANTHROPIC_AUTH_TOKEN`, `ANTHROPIC_API_KEY` or `~/.claude/.credentials.json`
for Claude Code; `OPENAI_API_KEY` or Codex login for Codex). The mirror
advertises provider availability in the catalog so the web can offer it.

### Pull requests are explicit

An agent run never opens a pull request unless the person starting it asked
for one for that run. `createPr` is false everywhere by default: the desktop's
"Create a PR" boxes start unticked, the web composer's green dot and the
dashboard's "Create a pull request when the run finishes" box start off (the
dot turns itself off again after each start), and the phone apps send
`createPr: false` for new agents and issue `@model` mentions. The finish-time
hooks (`maybeCreatePullForStreamSession` on the desktop, `openPull` in
`server/agents.go`) only act on a run whose `createPr` is true. Everywhere
else a pull request is a click: the rail's Create PR button, the branch
view's Open pull request, or `open_pr_from_branch`, whose MCP description and
the desktop agent prompt both tell agents to call it only when the user asks.
Teammates have no pull-request permission at all and are told to leave their
work on the branch ([agents-ui.md](../desktop/agents-ui.md)); the repository's
teammate shortcuts say the same.
`desktop/tests/validate_agent_branch_only_contract.py` and
`app/tests/test_owner_agent_sessions.py` pin the defaults.

## Error surfacing

Since 2026-09-24 (`a8815d8c4`, `fe5a131cb`, `de8481362`, merged in
`dc5c41af7`) a failed run's error is shown once, everywhere.

- **Mirror** (`server/agent_errors.go`): `lastError` is taken, in order, from
  the result event's errors/result text, an API-error assistant message,
  stderr's last non-empty lines, and only then "Claude Code exited with status
  N"; capped at 600 runes; the stream-json init event is never quoted. Codex
  uses the same tail-of-stderr order.
- **Outdated CLI.** "does not support this model; version X or newer is
  required" becomes "Claude Code on <node> is <ver>; <model> needs <X> or
  newer. Update the host (Hosts -> Update)." (Codex: "requires a newer version
  of Codex".) The runner remembers the refused version per provider and model
  (`agentCLIFloorKey`) and fails the next run for that model at once until the
  host's CLI is newer. No resume retry for an outdated CLI.
- The error is no longer appended to the transcript or substituted for the
  result. Clients show `lastError` beside the transcript.
- **Relay** (`repo_org_agent_job_result_handler` in `app/src/route_handlers.py`):
  a failed run with no reply records "The run stopped before the agent
  replied." instead of "Prompt dispatched.".
- **Web**: the diagnostic built from `lastError` is skipped when it only
  repeats `lastError` (`06-repo-content.js`).
- **Requested PR on merge** (2026-09-28): "Merge & clean up" on an agent that
  asked for a PR first creates it. When that fails the inline notice now says why
  (`m_agentPullFailures`, filled by `maybeCreatePullForStreamSession` and
  `landAgentPullForSession`), and a `git diff` that failed or timed out is
  logged as a failure instead of "No code changes"
  ([../desktop/README.md](../desktop/README.md#recent-changes)).
- **Web Copy / Send to prompt** (2026-10-04): every dashboard error, the
  session's `lastError` box included, carries Copy and Send to prompt buttons
  ([../app/web-composer.md](../app/web-composer.md#errors-copy-and-send-to-prompt)).
- **Desktop cloud rows**: mirrors older than this change appended `lastError`
  to the transcript; `AgentCloudSessions.cpp` drops that trailer from the last
  assistant turn, since the desktop already shows the error as the Attention
  row.

## Agent CLI refresh on hosts

The host installer used to copy Claude Code into `/usr/local/bin` once, so
mirrors kept a frozen CLI that the API later refused for new models. Since
`de8481362` every host update pipeline (per-host **Update**, **Install
(binary)**, fleet runs, and the verified native mirror-node update) ends,
after a successful update, with a root-only, never-fatal refresh of the CLIs
the host already serves to the `forkmesh-node` service
(`withAgentCliServiceRefresh()`, `agentCliServiceRefreshScript()` in
`desktop/src/ControlNode.{h,cpp}`):

- run the official installers, then install to `<name>.new` and `mv -f` over
  the old copy (`agentCliServicePublishScript()`), so running jobs keep the
  binary they opened;
- the update's exit status stays the update command's;
- the first-install bootstrap uses the same `.new` + rename swap.

Symptom it fixes: runs failing with the "Update the host (Hosts -> Update)"
message above.

## Operations

- Mirror CLI sign-in: **Network → Hosts** has an agent-CLI installer and a
  sign-in shell per host (`ControlNode.cpp`).
- A job stuck unclaimed: check the target node is online and advertises the
  provider; the relay only offers nodes whose catalog row lists it.
- A terminal report the relay keeps refusing (6 conflicts) is treated as
  belonging to a deleted session and dropped (`agentReportConflictLimit`).

## Traps

- Two desktop execution paths (legacy `AgentRunner` pool and stream/Codex
  sessions) have separate completion hooks. Changes to "is any agent running"
  (`anyAgentRunning()`) must cover both, or Rebuild & restart waits forever.
- Any guard written as `sessionId < 0` also hits external and cloud rows; use
  `isExternalSession()`/`isCloudSession()`.
- A mirror older than the change that wraps a run's error still appends it to
  the transcript; the desktop parser handles it, other clients show it twice.
- New agent launch paths must apply the project access policy
  (`applyProjectAgentAccess` in `MainWindowAgentAccess.cpp`), take their tool
  options from `agentRunLaunchOptions(sid, codex)` and the gate environment
  from `agentNetworkGateEnv(sid)` ([access-rules.md](access-rules.md)), and the memory
  containment calls (`applyEnvironment`, `wrapShellCommand`, `contain` in
  `AgentResources.h`, see [memory.md](memory.md)).

## Recent changes

- 2026-10-04: images attached to a web or phone prompt reach agents on the
  runner tier. The claim answer now carries the job's `attachments`, and the
  runner downloads each image with the runner-signed
  `POST /api/runners/attachment` (lease holder only), writes it under
  `.forkmesh/attachments/` for the job user and names it in the prompt. Before
  this, runner jobs ran with the prompt text only (see
  [RUNNERS.md, Prompt images](../RUNNERS.md#prompt-images-2026-10-04)).
- 2026-10-03: runner and mirror transcripts draw on the desktop like a local
  run, with tool rows and command panels instead of `▸ Tool:` prose (see
  Runner transcripts on the desktop).
- 2026-10-02: global agent rules. One file of rules every agent follows in
  every project (defaults for projects that follow them, global folders and
  destinations, blocked destinations, a destructive-command rule, a tool
  allowlist), enforced the same way for Claude Code, Codex, the legacy
  scripts and the built-in terminal; teammate permissions enforced in code;
  Codex web search gated and Codex access failures prompted; network-using
  scripts classified as external; the triage call runs with no tools. See
  [Desktop agent sandboxing](#desktop-agent-sandboxing).
- 2026-10-02: the legacy provider scripts ask the external-call gate before
  every shell or git command, honour the tool-family allow and deny lists,
  and the coding agent's path guardrails default to the repository root (see
  [Legacy provider scripts](#legacy-provider-scripts-gate-tool-families-guardrails)).
- 2026-09-30: pull requests are explicit. The web composer and dashboard
  agent form, the dashboard task-agent form, and the iOS and Android launchers
  requested a pull request by default, so agents started from the web or a
  phone opened pull requests nobody asked for, and scheduled teammates were
  allowed and told to open them. All default to off and teammates never open
  one now (see
  [Pull requests are explicit](#pull-requests-are-explicit)).
- 2026-09-29: agent memory containment. Agents run in per-agent scopes in
  `forkmesh.slice` with a machine-wide memory ceiling and low CPU weight,
  agent-started `systemd-run --user` builds join the slice through a PATH
  shim, build tools get job caps and a 10-minute Gradle daemon idle timeout,
  and a governor freezes the newest build under a sustained memory stall
  (see [memory.md](memory.md)).
- 2026-10-07: agent CPU cap. `forkmesh.slice` gets a `CPUQuota` of the
  cores minus a 20 % reserve, and the governor lowers it by whatever other
  programs are using (see [memory.md](memory.md)).
- 2026-09-29: photos attached on the phone reach a desktop agent. The
  shell composer no longer refuses them for the desktop, and the follow-up
  composers on a desktop run take photos. They are uploaded over local
  control (`agents.image.begin`/`chunk`, LAN or relay) and named by
  `agents.start`/`agents.prompt`, which save them under `agent-images` and
  add `Attached image:` lines like a relay job's images (see
  [../native/AGENT_ATTACHMENTS.md](../native/AGENT_ATTACHMENTS.md), Photos in
  phone prompts to a desktop agent).
- 2026-09-27: live output of the cloud run open in the Agents view
  (Live output of cloud runs, above). Needs a runner daemon release to take
  effect ([../RUNNERS.md](../RUNNERS.md#live-agent-output)).
- 2026-09-27: the dashboard's Agents page lists every run the account can
  see across repositories, organizations and nodes, including sealed desktop
  sessions by repository ([../app/agents-page.md](../app/agents-page.md)).
- 2026-09-27: finished cloud runs stay in the web, iOS and Android agent
  lists as well as the desktop's (per-repository list split into active
  and finished reads, `finishedBefore`; see Website- and phone-queued jobs).
- 2026-09-27: the iOS Agents tab is one list of every
  session instead of separate Cloud and Desktop tabs; each row's icon and the
  first word of its second line say Desktop or Cloud (Cloud when the relay
  calls the run's target node a mirror, Desktop otherwise). The iOS approvals,
  bulk actions, usage and saved-permissions controls moved above the list.
- 2026-09-27: images attached to a prompt show on the web Agents tab
  transcript; the relay keeps them a week and serves them back to the person
  who attached them (see Prompt images on the transcript).
- 2026-09-27: the website's session page streams a running Claude Code
  run's transcript (see Live transcript on the web).
- 2026-09-27: web-queued organization runs show in the desktop Agents list, and
  Move to cloud works on organization repositories served by the account's
  node (see Website- and phone-queued jobs, Move to cloud).
- 2026-09-25: Watch shows every agent CLI running on the machine, including
  ones started outside ForkMesh (`MachineAgentScan`, see
  [../desktop/agents-ui.md](../desktop/agents-ui.md)).
- 2026-09-24: web prompt dock on every dashboard page (new, add, task),
  see [../app/web-composer.md](../app/web-composer.md).
- 2026-09-24: error surfacing and CLI refresh (above); image attachments on
  website-queued jobs; `claude-opus-5-5` accepted on desktop-claimed web jobs;
  phone meets the relay's 10 MB photo total (`f94d8c896`).
- 2026-10-04: Merge & clean up deletes the merged branch's PR; PR-page
  Merge + delete removes the head's agent worktree and record.
- 2026-09-22: Merge & clean up removes only the merged agent; Watch window and
  phone Watch.
- 2026-09-21: per-project agent access (folder lock, external-call gate);
  phone control of desktop agents over the LAN.
- 2026-09-18: cloud (mirror-run) sessions shown in the desktop Agents list.
- 2026-09-13: Haiku preflight removed.

## Related docs

- [tools.md](tools.md): every tool each kind of agent can call, and what is missing
- [../desktop/agents-ui.md](../desktop/agents-ui.md)
- [../server/README.md](../server/README.md) and [../server/agent-jobs.md](../server/agent-jobs.md): mirror runner details
- [www/docs/operations/organization-agent-offline-queue.md](../../www/docs/operations/organization-agent-offline-queue.md)
- [www/docs/mobile/mobile-agent-access.md](../../www/docs/mobile/mobile-agent-access.md)


Codex runs use full access with approval prompts bypassed, including when an external-call gate is configured, and an explicit instruction to complete tasks autonomously, resolve routine choices, and request only essential missing information. Desktop app-server threads and turns receive the same instruction, including resumed sessions; repository data protections still apply.

The desktop session transcript in the web dashboard formats JSON log events as readable messages and structured fields. Questions appear as touch-sized choice cards with optional typed responses, secret fields, and a submit action using `agents.answer`; multiple answers preserve their question mapping. Answer drafts survive live transcript refreshes, and failed submissions retain the choices for retry. The streamed terminal also offers touch arrow keys, Tab, Enter, Escape and a text response field for mobile CLI menus.

Native iOS uses its own transcript views. JSON blocks and tool details now render as wrapped, labelled fields, including question text and options, while the existing live question cards submit through `agents.answer`. Secret responses use secure fields. iOS and desktop require an answer for every field before sending a multi-question request; desktop continues to route Codex answers through its active app-server session.

Codex app-server starts with a non-login shell so the PATH prepared by ForkMesh survives startup. Login profiles can reset that PATH and hide an installed Codex binary, producing exit 127 repeatedly on continuation. Continue keeps the session branch and resume identifier, clears the failed-launch retry count, and starts with the prepared environment. A genuinely missing CLI or missing authentication still needs installation or sign-in.
