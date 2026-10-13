# Agent tools inventory

Every tool a ForkMesh agent can call, by where the agent runs, and what is
missing. Written 2026-10-02 from the code; file references are to that
revision. The run lifecycle is in [README.md](README.md); the desktop UI is in
[../desktop/agents-ui.md](../desktop/agents-ui.md).

## Summary

| Where the agent runs | Runtime | Tools it gets | What limits them |
| --- | --- | --- | --- |
| Desktop, Claude Code provider | `claude` stream-json (`desktop/src/ClaudeStreamSession.cpp:183-212`) | All Claude Code built-in tools; MCP servers from the worktree's `.mcp.json` and the user's own config; the ForkMesh IDE bridge | Permission mode, `--disallowedTools` per switched-off family, external-call gate hook, Landlock folder lock, optional KVM |
| Desktop, Codex provider | `codex app-server` (`desktop/src/CodexAppServerSession.cpp:261-283`) | All Codex built-in tools (shell, apply_patch, web search, view_image, image generation) | Approval policy, Landlock folder lock. Sandbox is always `danger-full-access` |
| Desktop, legacy `claude` (API) provider | `forkmesh_claude_agent.py` (`desktop/src/ClaudeAgentScript.h`) | One `bash` tool | Landlock, jail, 60 turns |
| Desktop, `local-llama` and `cloudflare-ai` providers | `tools/forkmesh_coding_agent.py` (embedded in `CloudflareAgentScript.h`) | 18 ForkMesh coding tools | Landlock; the script's own path guardrails are off by default |
| Desktop, `openai` and other legacy providers | `codex -a never exec --sandbox danger-full-access` | Codex built-ins | Landlock only |
| Mirror node or runner | `server/agents.go:1514-1571` | All Claude Code built-ins (`--dangerously-skip-permissions`) or Codex built-ins (`-a never`, `danger-full-access`); Gemma runs as `codex exec --oss` | Runner: systemd scope, `forkmesh-job` uid, private networks blocked. Mirror: nothing |
| iOS and Android coding agent | `FMAgents/CodingAgent.swift`, `AgentTools.kt` | 7 file and Git tools, no shell | Plan mode refuses edits; 24 turns |
| iOS Genie and on-device chat | `FMAssistant/` | 10 Genie tools; 20 ForkMesh account tools | Approval on start, message, stop and delete |
| Desktop teammates (Hearth, Reddit, CFO, …) | Claude Code or Codex desktop runs | Same as the provider's run, plus their CLIs through the shell | Plan forced unless `modify-code` or `run-unattended` |
| Any MCP client | `tools/forkmesh_mcp_server.py` | 50 ForkMesh tools | Connector token for writes, CRM and tasks; desktop socket for agents, windows and releases |
| Remote MCP client | `app/src/remote_mcp.py` at `/mcp` | 3 organization task tools | Org bot bearer scopes |

## Desktop: Claude Code runs

Launch: `claude --output-format stream-json --input-format stream-json
--verbose --include-partial-messages`, then:

- Permission mode from the composer label (`MainWindowInternal1.cpp:929-947`):
  Auto is `--dangerously-skip-permissions`, Plan is `--permission-mode plan`,
  Edit is `acceptEdits`, Ask is `default`. Mode changes mid-run go through a
  `set_permission_mode` control request; a run not launched in Auto cannot be
  raised to it.
- `--model`, `--effort`, `--fallback-model`, `--resume`.
- `--disallowedTools` for families the operator turned off under Tool
  efficiency (`agents/disabledToolFamilies`, `desktop/src/AgentToolPolicy.cpp:17-43`):

  | Family | Claude tools removed |
  | --- | --- |
  | read | Read, NotebookRead, WebFetch |
  | write | Write |
  | edit | Edit, MultiEdit, NotebookEdit |
  | grep | Grep |
  | glob | Glob |
  | bash | Bash, BashOutput, KillShell, KillBash |
  | task | Task, Agent |
  | hub | SendMessage, ListAgents, TaskOutput, TaskStop, Monitor, ScheduleWakeup, CronCreate, CronDelete, CronList |
  | todo | TodoWrite, TodoRead |
  | ask | AskUserQuestion |
  | web_search | WebSearch |
  | manage_skill | Skill |
  | computer | Computer |

  `ToolSearch` is never removed: it is how deferred tools load.
- `--settings` carrying the external-call gate hooks
  (`AgentNetworkGate.cpp:771-891`): PreToolUse on `Bash|WebFetch|WebSearch`
  asks the desktop over a local socket and denies when the socket is gone;
  PostToolUse and PostToolUseFailure report folder and network refusals so the
  desktop can offer Retry, Grant folder or Add to /etc/hosts. Not added inside
  the KVM workspace.

Never passed: `--allowedTools`, `--mcp-config`, `--strict-mcp-config`,
`--append-system-prompt`, `--add-dir`. `--tools` is passed only when the
global tool allowlist is on (since 2026-10-02); otherwise an agent has every
built-in tool its Claude Code version ships (Read, Write, Edit, MultiEdit,
NotebookEdit, Glob, Grep, Bash and its background helpers, WebFetch,
WebSearch, Task/Agent, TodoWrite, AskUserQuestion, Skill, Monitor,
SendMessage, ListAgents, ScheduleWakeup, Cron*, Enter/ExitPlanMode,
ToolSearch, Workflow) minus the switched-off families.

Instructions about tools reach the agent only through the prompt:
`AgentRunner::toolUseInstruction()` (`desktop/src/AgentRunner.cpp:965-1029`)
is appended to every CLI prompt, and the guardrail wording
(`AgentGuardrails.cpp:626-653`) names the folders the lock allows.

The instruction goes to Codex runs as well, which have no Glob, Grep or Read.
The tool-efficiency log for 2026-09-29 to 2026-10-06 showed what that cost:
Codex shell output was ~1.77M of the ~3.9M tokens at stake. Most of it came
from repo-wide `rg`, and from `sed -n` or `head` on network logs, JSONL and
`pgrep -af` output, whose lines are long enough that bounding the line count
did not help. Claude Code's 468 whole-file Reads (~2.9M tokens) were mostly
3K–12K-token source files and `docs/*.md` pages read whole before a short
append. Since 2026-10-06 the instruction has a Codex bullet (`rg -l` first,
`rg -m`/`--max-columns`, `cut -c` for long lines, `git show --stat` before a
diff), says a whole-file Read is for files under ~300 lines and that a docs
page is read by its `^#` headings, asks for anchored edits instead of rewrites,
and sends waits on work the agent did not start to a background until-loop
or Monitor. On the detector
side (`AgentToolImprovementLog.cpp`, mirrored in
`desktop_go_new/internal/insights/shape.go`), a failing `go build`, `go vet`,
`cargo build`/`check`, `cmake --build`, `ninja` or `swift build` counts as an
outcome, like a failing test run. These had been reported as "Recurring
failures… worth fixing in the runtime prompt".

The log for 2026-10-06 19:23 to 2026-10-07 13:58 (121 signals, ~683K tokens)
added these changes on 2026-10-07:

- Codex wraps every command in `/bin/bash -lc '…'`, so its calls all grouped
  as `bash`. The detector now unwraps the launcher (single or double quoted,
  `'\''` escapes included) before taking the shape, and `timeout -k N`,
  `env -u VAR` and `sudo -u user` drop their option arguments.
- A merge stopped by conflicts, and a check script that prints `FAIL:`,
  `not ok N`, `--- FAIL` or a compiler `file:line:col: error:` and exits
  1–123, are outcomes, like a failing test run. A refused `rm` on a `$VAR` path
  ("Dangerous rm operation") is an "unguarded delete" that points at
  `fm_safe_rm_tree`; `tail -2 a b` ("option used in invalid context") gets its
  own `tail -n N` fix; "requires approval" is a refusal.
- Non-shell errors: "while in plan mode" folds into one "refused in plan mode"
  signal (10 Edits and 2 Writes in the window), and "File does not exist" into
  "guessed path", each with its own fix instead of the generic text.
- A slow shell call is named by its first command that is not a quick probe
  (`tail`, `grep`, `ls`, `git status`…), and a poll loop wrapped in
  `timeout N bash -c 'until …'` is a "foreground wait": the sleep guard does
  not see it, but it blocks the turn just the same.
- The whole-file Read floor rose from 2000 to 3000 tokens, which is about the
  ~300-line limit the prompt allows a whole read.
- The prompt adds `git status --short | head -50` (an unbounded status in a
  conflicted worktree returned 104K tokens), `tail -n N` with several files,
  that plan mode refuses every Edit and Write, the `rm`-on-`$VAR` refusal and
  `fm_safe_rm_tree`, and that a `timeout bash -c` until-loop counts as a
  foreground wait.

The log for 2026-10-06 19:27 to 2026-10-12 18:16 (200 signals from 1275
calls, ~4.30M tokens) added these changes on 2026-10-12:

- Codex appends its exit code as a last line, `Exit code: N`
  (`CodexAppServerSession::toolOutput`), while Claude Code leads with
  `Exit code N`. The classifier only read the leading form, so every Codex
  failure had no exit code: failing pytest runs, test binaries and
  `git diff --check` showed up as "Recurring failures… worth fixing", and a
  `sed -n … && rg …` chain whose last search found nothing was a "Tool error"
  carrying the whole printed file. The trailing form is read now, and the
  ledger keeps the first 1792 and last 256 characters of an error result
  (it kept the first 2048), so the exit line and the error at the end of a
  long chain survive. Replaying 99 failed shell calls from
  `data/agents/**/events.jsonl` moved 21 of the 31 that had fallen into the
  generic bucket into a named class.
- A chain made only of read-only commands (`sed`, `cat`, `rg`, `grep`, `ls`,
  `[`, `git show`/`log`/`diff`…) that exits 1 is "probe found nothing", named
  by its last search, even when the file it printed contains the word "error".
  A binary named `*-tests` is a test run; `git diff --check` findings
  (`file:N: trailing whitespace.`) are outcomes; exit 127 or "command not
  found" is its own class pointing at `command -v`.
- A failing test run is still an outcome, but if it returned 8K tokens or more
  it is also an oversized result (topic "test run": `pytest -q --tb=short`,
  `tail -n 80`). One `pytest -vv -x` run had returned ~216K tokens.
- Oversized shell results that came from a search get the topic "search" and
  say that `rg -m` and `grep -m` cap matches per file, not in total: the
  largest results (90K–262K tokens) were repo-wide `rg -m 80`/`-m 100` runs.
- Non-shell errors: "File has not been read yet" folds into "write before
  read" (one Write lost ~29K tokens of content) and "Concurrent subagent limit
  reached" into "subagent limit" (22 refusals).
- An abandoned call is named by its first command that is not a quick probe,
  like a slow call, so a `timeout 580 pytest … | grep` stopped mid-run reads as
  pytest rather than grep.
- The prompt says to pipe every search through `head -n 60` because `rg -m`
  is per file, to keep a Codex call to ~300 printed lines in total, to Write
  only new files, to run at most ten subagents at once, to probe a tool with
  `command -v`, that `sleep N; tail …` is a refused foreground sleep too, and
  to run tests with `pytest -q --tb=short` piped through `tail -n 80`.

The Go mirror (`desktop_go_new/internal/insights/shape.go`, `improvements.go`,
`ledger.go`) carries the same rules; `testdata/golden.cpp` has a case for
each and `golden.json` was regenerated with `testdata/gen.sh`.

Each finished run is also logged to `agent-runs.jsonl` beside the ledger
(`AgentRunEfficiencyLog`), with the tool families it launched with, so runs
with and without families switched off can be compared; see
[../desktop/agents-ui.md](../desktop/agents-ui.md#tool-efficiency-runs-view-2026-10-12).

Environment: `ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN`,
`ANTHROPIC_ADMIN_KEY` and `CLAUDE_CODE_OAUTH_TOKEN` are removed; the agent
intake socket (`FORKMESH_AGENT_INTAKE`, `FORKMESH_AGENT_INTAKE_TOKEN`) and the
IDE bridge port are added off the VM (`MainWindowAgentCliSession.cpp:924-1006`).

### MCP servers

The desktop does not hand an MCP config to its runs. Claude Code loads:

- the checked-in `.mcp.json`: one server, `forkmesh`, run as
  `python3 tools/forkmesh_mcp_server.py` (relative path, no token), so it works
  only in ForkMesh's own worktrees;
- whatever the user registered themselves, for example with the
  `claude mcp add forkmesh --scope user …` command Settings > MCP copies
  (`desktop/src/McpConnector.cpp:150-166`).

### IDE bridge

`desktop/src/ClaudeIdeBridge.*` makes the desktop Claude Code's IDE: a
localhost WebSocket MCP server announced through `~/.claude/ide/<port>.lock`.
Claude runs only, not in the VM. Tools (`ClaudeIdeBridge.cpp:616-663`):

| Tool | Behaviour |
| --- | --- |
| `openDiff` | Modal Accept/Reject dialog; Accept writes the file |
| `openFile` | Shows an inline notice ("Claude Code opened <file>") at the top of the page in view; opens nothing |
| `getWorkspaceFolders` | The run's worktree |
| `getCurrentSelection`, `getLatestSelection`, `getOpenEditors` | Always empty: nothing calls `setActiveFile` or `setSelection` |
| `getDiagnostics` | Always `[]` |
| `closeAllDiffTabs` | Signal is never connected |
| `checkDocumentDirty`, `saveDocument` (not advertised) | Always false; no-op |

## Desktop: Codex runs

Launch: `codex [-c overrides] app-server`, wrapped like Claude. Every thread
and turn uses sandbox `danger-full-access` and approval policy `never`,
including when a project's external-call gate is enabled. Plan still sets
collaboration mode `plan`. Autonomous developer instructions arrive in the
thread and every turn.

If a Codex thread receives a fallback command, file, or permission approval
request, the desktop accepts it for the session without showing a question.

Tool families map to Codex only for two switches (`AgentToolPolicy.cpp:49-58`):
web_search sets `web_search="disabled"` and `tools.web_search=false`; read sets
`tools.view_image=false`. A switched-off bash family is enforced by denying
every `process.execute` approval, which only works when Codex asks.

Codex gets no MCP server from ForkMesh (it does not read `.mcp.json`), only
what is in the user's `~/.codex/config.toml`.

## Desktop: legacy script providers

Everything except `claude-code` and `codex` runs through `AgentRunner`
(`MainWindowAgentLaunch.cpp:2720-2793`). These get Landlock, the VM and the
jail, but no external-call gate, no tool-family policy and no IDE bridge.

- `claude` (Claude API): one `bash` tool, 600 s per command, 60 turns,
  credentials scrubbed, private HOME (`ClaudeAgentScript.h`).
- `local-llama` and `cloudflare-ai`: `tools/forkmesh_coding_agent.py`,
  120 turns, model from `marketplace/packages/gemma-4/content/agent.json`.
  Tools (`forkmesh_coding_agent.py:2041-2058`): `bash`, `read_file`,
  `write_file`, `replace_text`, `apply_patch`, `undo_edit`, `search_files`,
  `grep`, `list_directory`, `execute_command`, `git_status`, `git_diff`,
  `git_commit`, `git_log`, `git_branch`, `git_restore`, `run_tests`,
  `lint_project`. The Cloudflare path calls the account's Workers AI
  OpenAI-compatible endpoint directly with the desktop's locally stored
  token and account ID.
- `openai` and fallbacks: `codex -a never exec --sandbox danger-full-access`.

## Mirror node and runner runs

`server/agents.go`:

- Claude Code: `claude -p --output-format stream-json --verbose
  --dangerously-skip-permissions --model <m> [--resume <id>]`, default
  `claude-opus-5-5`.
- Codex: `codex -a never [-m <m>] exec --sandbox danger-full-access -o <file> -`.
- Gemma (`local-llama`): the Codex command with `exec --oss` against the
  node's Ollama (`gemma4:e4b`). It gets Codex's tools, not the ForkMesh coding
  tools the desktop's Local Llama uses. Runner pools do not offer it.

There is no allowlist, MCP config, tool-family policy or call gate. Images
land in `.forkmesh/attachments/` and the prompt says to open them with the
file-reading tool. The agent does not open pull requests itself: the daemon
commits, pushes and opens a node-signed one only when the job asked
(`agents.go:1675-1777`).

- Mirror role: the agent inherits the daemon's whole environment and user,
  which also holds the node key.
- Runner role (`server/sandbox.go`): `systemd-run --scope` as `forkmesh-job`
  with memory, CPU and task caps; loopback, link-local and private ranges
  blocked; public internet open; environment allowlist; Git hooks off.

## Phone agents

The phones run agents on the device as well as queueing jobs to nodes.

- Coding agent, iOS (`native/ios/ForkMeshCore/Sources/FMAgents/AgentTools.swift:107-190`)
  and Android (`native/android/core/.../agents/AgentTools.kt:67-117`):
  `read_file`, `write_file`, `replace_text`, `list_directory`, `grep`,
  `git_status`, `git_commit`. No shell by design. Providers: Anthropic and
  OpenAI with the user's key on both; on-device Gemma (MLX) on iOS only.
- Genie, iOS only (`FMAssistant/GenieToolbox.swift:175-207`): `open_screen`,
  `find_app_actions`, `run_app_action`, `search_app`, `open_search`,
  `start_agent` (approval), `list_capabilities`, `say`, `ask_user`,
  `hand_off_to_agent`; up to 5 rounds. Calls by an alias name are mapped by
  `GenieAliases` (`docs/native/GENIE.md`).
- ForkMesh toolbox, iOS only (`FMAssistant/ForkMeshToolSpecs.swift:6-90`), also
  used by the on-device chat: `get_account`, `list_tasks`, `read_notes`,
  `list_repositories`, `list_issues`, `list_pull_requests`, `read_chat`,
  `list_agents`, `message_agent` (approval), `stop_agent` (approval),
  `list_pings`, `list_calendar`, `crm_contacts`, `create_crm_contact`,
  `update_crm_contact`, `delete_crm_contact` (approval), `draft_email` (never
  sends), `search_web`, `read_web_page`, `search_forkmesh`.

## ForkMesh MCP server

`tools/forkmesh_mcp_server.py`: JSON-RPC over stdio, protocol `2025-06-18`,
tools only (no resources or prompts). The 50 names are pinned by
`tools/test_forkmesh_mcp_server.py`.

Access: with no `mcp/connector.json` everything is open; with one, a matching
`FORKMESH_MCP_TOKEN` gives write access and anything else is read-only. The
CRM tools and `list_tasks`/`get_task` need the token even to read. Agent,
window and release tools need the desktop's intake socket
(`FORKMESH_AGENT_INTAKE`), which only desktop Claude Code and Codex runs have.

| Group | Tools | Notes |
| --- | --- | --- |
| Identity and code | `whoami`, `list_repos`, `list_repositories`, `list_files`, `read_file`, `search_code` | Local Git reads; `read_file` up to 1 MiB, `search_code` up to 200 hits |
| Notes | `create_note`, `get_note`, `list_notes`, `search_notes`, `update_note`, `create_cloud_note` | Signed files; repository notes are committed under `.forkmesh/notes`. `create_cloud_note` writes to the Notes page instead (below) |
| Chat | `read_chat_messages`, `post_chat_message` | mainnode/forkmesh #general only |
| Issues and planning | `search_issues`, `create_issue`, `comment_on_issue`, `create_milestone`, `update_milestone`, `create_project`, `update_project` | Signed tracker writes |
| Pull requests | `open_pr_from_branch`, `get_pr_diff` | The PR carries the MCP marker that makes it a payable microjob |
| CRM | `crm_list_organizations`, `crm_get_pipeline`, `crm_list_contacts`, `crm_get_contact`, `crm_list_activity`, `crm_update_contact`, `crm_find_duplicates`, `crm_check_accounts`, `crm_merge_contacts` | Signed `/api/crm`; `crm_merge_contacts` is destructive |
| Tasks | `list_tasks`, `get_task`, `create_task`, `update_task`, `complete_task`, `reopen_task`, `attach_work_item`, `detach_work_item`, `delete_task`, `restore_task` | Signed `/api/tasks`; `delete_task` soft-deletes into a 30-day trash |
| Agents | `create_agent` | Desktop socket; 12 per 10 minutes |
| Releases | `list_releases`, `create_release`, `publish_release` | Desktop socket; create and publish share 6 per 10 minutes |
| Windows | `show_window`, `read_window`, `close_window`, `open_browser_window`, `read_browser_page` | Desktop socket; not token-gated; the user can stop a page being shared |

Details: [../tools/mcp-create-agent.md](../tools/mcp-create-agent.md),
[../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md),
[../tools/mcp-releases.md](../tools/mcp-releases.md).

### Notes page writes (2026-10-07)

`create_note` writes signed Markdown files: personal ones go to
`<data>/notes/<slug>.md`, beside the desktop's `notes.json` but in a format
the Notes page never reads. So an agent asked to "add a note to our notes"
had no way to make one the user could see. `create_cloud_note` (`title`,
`content`) closes that gap: it sends `POST /api/notes` with the
`{"title","markdown"}` body, signed by the account key as
`forkmesh-notes-request-v1\n<account>\nPOST\n/api/notes\n<sha256 of the exact body>\n<ts>`
(the proof the desktop's `requestNotes()` uses,
[../desktop/notes.md](../desktop/notes.md#authentication-fixed-2026-09-27)).
The relay encrypts the note at rest, and it is listed on the web, desktop and
phone Notes pages on their next refresh. It is a write tool, so it needs the
connector token once one is published. The Tasks signer is now
`signed_relay_request(..., service=)`, shared by both.

## Remote MCP

`app/src/remote_mcp.py`, served at `https://app.forkmesh.com/mcp` (Streamable
HTTP, POST only), authenticated by an organization bot bearer token:
`list_org_tasks` and `get_org_task` (`organization.tasks.read`),
`complete_org_task` (`organization.tasks.write`). Genie on the desktop mints
the credential from `/api/genie/credential` and tells its agent to register
the server as `forkmesh-tasks`.

## Teammates and special agents

Teammates (`.forkmesh/teammates/*.json`) are desktop Claude Code or Codex runs,
so they get that provider's tools. The JSON has no tool list. The provider
and model follow the newest non-teammate run (`TeammateStore.cpp:1417-1423`).

Permissions (`TeammateStore.cpp:1802-1826`) are mostly prompt text. Code
enforces three: `read-repositories` to start, `process-payouts` for the payout
workflow, and `modify-code` or `run-unattended` to run in the teammate's own
mode; without either, the run is forced to Plan
(`MainWindowTeammates.cpp:6970-6986`). An empty `mode` falls back to the
global composer mode, which defaults to Auto
(`MainWindowAgentCliSession.cpp:431-440`).

| Teammate | Mode | Permissions | Extra tools |
| --- | --- | --- | --- |
| Hearth | Auto | read-repositories, manage-issues, external-tools, run-unattended | `tools/discord_community.py` (`channels`, `room`, `post`, `react`; posts only with `--send`) |
| Reddit agent | Auto | read-repositories, external-tools, run-unattended | `tools/reddit_scout.py` (`scan`, `thread`, `rules`, `account`, `link`; no posting) |
| Marketing | Auto | modify-code and others, plus run-git, run-tests, read-forkmesh-web ([access rules](access-rules.md#teammates)) | — |
| UX | Auto | modify-code and others, plus run-unattended, run-git, run-tests, read-forkmesh-web | Weekday evidence-backed UX repairs |
| Y Combinator News agent | Auto | modify-code and others | — |
| Code Explainer, Code Steward, Error Fixer, Integrator, Mobile Developer, Qt Optimizer, Quality, Sentinel | empty (global mode) | modify-code and others | — |
| CEO, COO, CFO, CPA, Sales, Reaper | forced Plan | no modify-code | CFO: payouts through the desktop workflow only |

Skills in `.forkmesh/skills/`: `cfo-payout-operations`, `community-engagement`,
`discord-quality-intake`, `reddit-engagement`, `security-review`. The repo has
no `.claude/` or `.codex/` folder (no project settings, subagents, commands or
hooks).

## Server-side AI

No relay code gives an LLM tools to run on the server.

- Desktop coding-agent traffic goes directly to Workers AI. It does not use
  `/api/ai/agent`, `/api/ai/workspace-name`, or `/api/forkbot/models`.
- ForkBot chat: JSON-schema intent classification, not function calling.
  Intents: `create_issue`, `list_issues`, `count_issues`, `search_issues`,
  `show_issue`, `start_agent`, `help`, `none`.
- Tool-less calls: `/api/ai/ask`, username moderation, Discord bug duplicate
  review, microjob review.
- Marketplace package workers (`_PluginApiRuntime`, `app/src/entry.py`):
  `outbound_text`, `outbound_json`, `ai_json`, `notify`, `seal_with`,
  `open_with`, `send_mailtrap`, `account_exists`, `account_evidence`. None
  gives an LLM tools.
- ForkMesh Apps ([../APPS.md](../APPS.md)): `auth.test`, `chat.postMessage`,
  `issues.create`, `issues.comment`, `pulls.comment`, `pings.send`,
  `agents.run`.

## What is missing

Ordered by how much each would change what agents can do or how safely.

### Reach

1. **Codex runs have no ForkMesh tools.** Codex does not read `.mcp.json` and
   the desktop injects no `mcp_servers`, yet it exports the intake socket to
   Codex and `toolUseInstruction()` tells every agent to load "ForkMesh MCP"
   with ToolSearch. [../tools/mcp-create-agent.md](../tools/mcp-create-agent.md)
   says Codex starts the server from `.mcp.json`; it does not.
2. **Agents outside the ForkMesh repository have no ForkMesh tools** unless
   the user ran the Settings > MCP command. The desktop could pass
   `--mcp-config` with the absolute script path and the connector token.
3. **Mirror and runner agents have no ForkMesh tools at all**: no tasks,
   issues, notes or CRM, so a cloud agent cannot update the task it was
   started from. Whether headless `claude -p` loads the scratch clone's
   `.mcp.json` was not verified; it would lack the identity key to sign writes
   anyway.
4. **The built-in Claude terminal** gets no intake environment, so
   `create_agent`, windows and releases fail there.
5. **Android has no on-device model, Genie or ForkMesh toolbox**; iOS has all
   three.
6. **Phone coding agents have no shell**, so they cannot run tests or builds.
7. **Planned tool families with no runtime path** (`AgentToolCatalog.cpp:143-208`):
   `checkpoint`, `rewind`, `context_notes`, `new_context`, `learn`, `goal`.
   Package-only families (`ast_edit`, `ast_grep`, `eval`, `lsp`, `debug`,
   `security_scan`, `github`, `browser`, `computer`, `tts`, `retain`, `recall`,
   `reflect`, `memory_edit`) depend on a package being installed.
8. **The IDE bridge is mostly stubs**: no diagnostics, selection or open
   editors, and `openFile` opens nothing. Real diagnostics from the build would
   be the most useful addition.
9. **No project Claude Code config**: no `.claude/settings.json`, subagents,
   slash commands or hooks, so the `AGENTS.md` destructive-action rules are
   prompt text only (see Safety, item 3).

### Safety and consistency

All twelve gaps found here were addressed on 2026-10-02 (branch
`1324-fix-global-rules`); see
[README.md](README.md#desktop-agent-sandboxing) and
[access-rules.md](access-rules.md). What remains:

1. **Tool allowlist** (Settings > Agents > Global agent rules) is optional and
   off by default. With it on, Claude Code gets `--tools`; Codex and the
   script providers only switch off what they can.
2. **Codex keeps `danger-full-access` on Linux** inside the Landlock lock
   (Codex's own sandbox blocks loopback servers); off Linux it uses
   `workspace-write`.
3. **Destructive commands** are caught at the Claude gate
   (`classifyCommandRisk`) and ask by default; they are a pattern list, not a
   sandbox.
4. **Codex web search** is off unless external calls are allowed or
   `web-search` is approved. Codex access failures prompt like Claude's, but
   the answer arrives as a steering message because Codex does not wait.
5. **In the VM** neither provider is gated.
6. **Legacy providers and the terminal** now get the gate, the tool policy and
   (scripts) path guardrails on by default. The `openai` legacy provider
   (`codex -a never exec`) still only gets the folder lock.
7. **Scripts that use the network** (`python3 tools/*.py`, `node x.mjs`)
   classify as external, so Hearth's and the Reddit agent's calls reach the
   gate.
8. **Teammates**: an empty mode is Edit (Auto only with `run-unattended`), and
   `external-tools`, `manage-issues`, `create-branches`, `review-prs` and
   `merge` are enforced.
9. **Mirror-role agents** get an allowlisted environment, a private HOME and a
   systemd user scope when available, but still run as the node's user, so the
   node key is readable by path ([../server/agent-jobs.md](../server/agent-jobs.md)).
10. **The auto-model triage call** runs with `--tools ''` and
    `--strict-mcp-config`.
11. **Org bot scopes** nothing checked were retired
    ([../app/organization-bot-scopes.md](../app/organization-bot-scopes.md)).
12. **MCP window tools** need the connector token once a connector exists, and
    the desktop checks the intake token before every method.

### Stale text

- Settings > MCP "How agents use it" (`MainWindowMcp.cpp:194-228`) omits the
  task, agent, release and window tools and two CRM tools.
- The dashboard's organization MCP prompt names its HTTP server `forkmesh`,
  the same name as the local stdio server (`app/public/dashboard.js:5403`).
- [README.md](README.md) lists the mirror providers as `claude-code` or
  `codex`; `local-llama` is accepted too.

## Related docs

- [README.md](README.md): agent runs and sandboxing
- [../security/agent-host-boundaries.md](../security/agent-host-boundaries.md): what each provider and mode can read, run and reach, with a runnable check per row
- [../desktop/agents-ui.md](../desktop/agents-ui.md): teammates and the Tool efficiency page
- [../server/agent-jobs.md](../server/agent-jobs.md), [../RUNNERS.md](../RUNNERS.md): mirror and runner jobs
- [../tools/README.md](../tools/README.md): the MCP server's home
