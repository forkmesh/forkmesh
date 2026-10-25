# Agent tools inventory

Every tool a ForkMesh agent can call, by where the agent runs. The run
lifecycle is in [README.md](README.md); access rules in
[access-rules.md](access-rules.md); the desktop UI in
[../desktop/agents-ui.md](../desktop/agents-ui.md).

## Summary

| Where the agent runs | Runtime | Tools it gets | What limits them |
| --- | --- | --- | --- |
| Desktop, Claude Code provider | `claude` stream-json (`ClaudeStreamSession.cpp`) | Claude Code built-ins; the ForkMesh MCP server (`--mcp-config`), `.mcp.json` once approved and the user's config; the IDE bridge | Permission mode, tool families, folder lock, gate hook, optional allowlist |
| Desktop, Codex provider | `codex app-server` (`CodexAppServerSession.cpp`) | Codex built-ins (shell, apply_patch, web search, view_image) | Approval policy `never`, folder lock, tool families Codex can switch off |
| Desktop, legacy `claude` (API) provider | `ClaudeAgentScript.h` | One `bash` tool | Landlock, jail, gate, 60 turns |
| Desktop, `local-llama` and `cloudflare-ai` | `tools/forkmesh_coding_agent.py` | 18 ForkMesh coding tools | Landlock, gate, tool families, path guardrails |
| Desktop, `openai` legacy provider | `codex -a never exec` | Codex built-ins | Landlock only |
| Mirror node or runner | `server/agents.go` | Claude Code built-ins (`--dangerously-skip-permissions`) or Codex built-ins (`danger-full-access`) | Runner sandbox (`server/sandbox.go`); mirror runs as the node user |
| iOS and Android coding agent | `FMAgents/CodingAgent.swift`, `AgentTools.kt` | 7 file and Git tools, no shell | Plan mode refuses edits; 24 turns |
| iOS Genie and on-device chat | `FMAssistant/` | Genie tools and ForkMesh account tools | Approval on start, message, stop and delete |
| Desktop teammates | Claude Code or Codex desktop runs | The provider's tools plus their CLIs | Teammate permissions ([access-rules.md](access-rules.md#teammates)) |
| Any MCP client | `tools/forkmesh_mcp_server.py` | ForkMesh MCP tools | Connector token for writes, CRM, tasks, windows and agent tools; desktop socket for agents, windows, widgets, runners and releases |
| Remote MCP client | `app/src/remote_mcp.py` at `/mcp` | 3 organization task tools | Org bot bearer scopes |

## Desktop: Claude Code runs

Launch: `claude --output-format stream-json --input-format stream-json
--verbose --include-partial-messages`, then:

- Permission mode from the composer: Auto is
  `--dangerously-skip-permissions`, Plan `--permission-mode plan`, Edit
  `acceptEdits`, Ask `default`. Mid-run changes use a `set_permission_mode`
  control request; a run not launched in Auto cannot be raised to it.
- `--model`, `--effort`, `--fallback-model`, `--resume`.
- `--disallowedTools` for families turned off under Tool efficiency
  (`agents/disabledToolFamilies`, `AgentToolPolicy.cpp`):

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
- `--settings` with the gate hooks: PreToolUse on `Bash|WebFetch|WebSearch`
  asks the desktop over a local socket and denies when it is gone;
  PostToolUse and PostToolUseFailure report refusals so the desktop can offer
  Retry, Grant folder or Add to /etc/hosts. Not added inside the KVM
  workspace.
- `--tools` only when the global tool allowlist is on.

Never passed: `--allowedTools`, `--strict-mcp-config`,
`--append-system-prompt`, `--add-dir`.

Tool guidance reaches the agent through the prompt:
`AgentRunner::toolUseInstruction()` is appended to every CLI prompt (bounded
searches, reading long files by range and docs by headings, anchored edits,
background waits instead of foreground sleeps, test output piped through
`tail`), and the guardrail wording names the folders the lock allows.

The tool efficiency ledger (`AgentToolImprovementLog.cpp`) classifies every tool result into
outcomes, empty probes, oversized results and known refusals, each with a
suggested fix; finished runs are logged to `agent-runs.jsonl`
(`AgentRunEfficiencyLog`).

Environment: `ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN`,
`ANTHROPIC_ADMIN_KEY` and `CLAUDE_CODE_OAUTH_TOKEN` are removed; the agent
intake socket (`FORKMESH_AGENT_INTAKE`, `FORKMESH_AGENT_INTAKE_TOKEN`) and the
IDE bridge port are added off the VM.

### MCP servers

Every run gets `--mcp-config <data>/core/agent-mcp/session-<id>.json`
(mode 0600, rewritten on each launch by `MainWindow::addAgentRunMcpConfig`):
the `forkmesh` server (`forkmesh::mcp::configJson`, the same file the
terminal tabs get) with the run's `FORKMESH_AGENT_INTAKE` and
`FORKMESH_AGENT_INTAKE_TOKEN` in its `env`. Without it a run had no ForkMesh
tools at all: a non-interactive `claude` skips a checked-in `.mcp.json` until
the user approves it in an interactive session (`enabledMcpjsonServers`), and
other repositories have no `.mcp.json`. So the terminal, window, browser and
hub tools ([../tools/mcp-claude-code-desktop.md](../tools/mcp-claude-code-desktop.md))
reach every run, as Claude Code's desktop app gives its sessions their panes.
The flag adds to the user's own servers (no `--strict-mcp-config`), and a run
with guardrails gets the config file and the server's folder as read-only
paths. Runs in the KVM workspace, which have no intake socket, get no file.

### IDE bridge

`desktop/src/ClaudeIdeBridge.*` makes the desktop Claude Code's IDE: a
localhost WebSocket MCP server announced through
`~/.claude/ide/<port>.lock` (Claude runs only, not in the VM). `openDiff`
shows a modal Accept/Reject dialog; `openFile` shows an inline notice;
`getWorkspaceFolders` returns the worktree; selection, editors and
diagnostics are empty.

## Desktop: Codex runs

`codex [-c overrides] app-server`, wrapped like Claude. Threads use approval
policy `never`; Plan sets collaboration mode `plan`. Fallback approval
requests are accepted for the session without a question. Tool families map
to two switches: web_search sets `web_search="disabled"` and
`tools.web_search=false`; read sets `tools.view_image=false`. Codex gets no
MCP server from ForkMesh (it does not read `.mcp.json`).

## Desktop: legacy script providers

- `claude` (Claude API): one `bash` tool, 600 s per command, 60 turns,
  credentials scrubbed, private HOME.
- `local-llama` and `cloudflare-ai`: `tools/forkmesh_coding_agent.py`, 120
  turns, model from `marketplace/packages/gemma-4/content/agent.json`. Tools:
  `bash`, `read_file`, `write_file`, `replace_text`, `apply_patch`,
  `undo_edit`, `search_files`, `grep`, `list_directory`, `execute_command`,
  `git_status`, `git_diff`, `git_commit`, `git_log`, `git_branch`,
  `git_restore`, `run_tests`, `lint_project`. The Cloudflare path calls
  Workers AI directly with the desktop's locally stored token.
- `openai` and fallbacks: `codex -a never exec --sandbox danger-full-access`.

## Mirror node and runner runs

`server/agents.go`:

- Claude Code: `claude -p --output-format stream-json --verbose
  --dangerously-skip-permissions --model <m> [--resume <id>]`.
- Codex: `codex -a never [-m <m>] exec --sandbox danger-full-access -o <file> -`.
- Gemma (`local-llama`): Codex with `exec --oss` against the node's Ollama;
  runner pools do not offer it.

No allowlist, MCP config, tool-family policy or call gate. Images land in
`.forkmesh/attachments/`. The daemon, not the agent, commits, pushes and
opens a node-signed pull request only when the job asked.

- Mirror role: allowlisted environment and private HOME, but the node's user
  ([../server/agent-jobs.md](../server/agent-jobs.md)).
- Runner role (`server/sandbox.go`): `systemd-run --scope` as `forkmesh-job`
  with memory, CPU and task caps; loopback, link-local and private ranges
  blocked; environment allowlist; Git hooks off.

## Phone agents

The on-device coding agent (iOS `FMAgents/AgentTools.swift`, Android
`AgentTools.kt`) has `read_file`, `write_file`, `replace_text`,
`list_directory`, `grep`, `git_status`, `git_commit` and no shell. iOS also
has Genie (`FMAssistant/GenieToolbox.swift`: screen navigation, app actions,
search, `start_agent` with approval, hand-off) and a ForkMesh account toolbox
(`FMAssistant/ForkMeshToolSpecs.swift`; destructive and messaging tools need
approval, `draft_email` never sends).

## ForkMesh MCP server

`tools/forkmesh_mcp_server.py`: JSON-RPC over stdio, protocol `2025-06-18`,
tools only. Tool names are pinned by `tools/test_forkmesh_mcp_server.py`.

Access: with no `mcp/connector.json` everything is open; with one, a
matching `FORKMESH_MCP_TOKEN` gives write access and anything else is
read-only. CRM tools and `list_tasks`/`get_task` need the token even to read.
Agent, window and release tools need the desktop's intake socket
(`FORKMESH_AGENT_INTAKE`).

| Group | Tools | Notes |
| --- | --- | --- |
| Identity and code | `whoami`, `list_repos`, `list_repositories`, `list_files`, `read_file`, `search_code` | Local Git reads; `read_file` up to 1 MiB, `search_code` up to 200 hits |
| Notes | `create_note`, `get_note`, `list_notes`, `search_notes`, `update_note`, `create_cloud_note` | Signed files; `create_cloud_note` posts to `/api/notes` signed with `forkmesh-notes-request-v1` |
| Chat | `read_chat_messages`, `post_chat_message` | One fixed channel |
| Issues and planning | `search_issues`, `create_issue`, `comment_on_issue`, `create_milestone`, `update_milestone`, `create_project`, `update_project` | Signed tracker writes |
| Pull requests | `open_pr_from_branch`, `get_pr_diff` | The PR carries the MCP marker |
| CRM | `crm_list_organizations`, `crm_get_pipeline`, `crm_list_contacts`, `crm_get_contact`, `crm_list_activity`, `crm_update_contact`, `crm_find_duplicates`, `crm_check_accounts`, `crm_merge_contacts` | Signed CRM API |
| Tasks | `list_tasks`, `get_task`, `create_task`, `update_task`, `complete_task`, `reopen_task`, `attach_work_item`, `detach_work_item`, `delete_task`, `restore_task` | Signed `/api/tasks`; delete is soft |
| Agents | `create_agent` | Desktop socket; 12 per 10 minutes; `task` / `handoff` hand an organization task to the new agent |
| Releases | `list_releases`, `create_release`, `publish_release` | Desktop socket; 6 per 10 minutes |
| Ledger | `record_expense` | Signed `POST /api/ledger/expenses`; the signed-in account must be an administrator with two-factor sign-in ([../finance/cfo.md](../finance/cfo.md#recording-expenses)) |
| Email | `send_email` | Sends through the local Zoho Mail account set up with `tools/zoho_mail.py`, no confirmation step, then logs outside recipients in the CRM's Initiatives list ([../tools/zoho-mail.md](../tools/zoho-mail.md)) |
| Windows | `show_window`, `read_window`, `close_window`, `open_browser_window`, `read_browser_page`, `browser_action`, `show_widget`, `suggest_session` | Desktop socket; the user can stop a page being shared; widgets run sandboxed with no network; a suggested session starts only when the user presses Start session |
| Agents and runners | `list_agents`, `send_agent_message`, `list_runners`, `notify_user` | Desktop socket; messages only reach desktop agents whose mode allows no more than the sender's; 30 messages and 10 notifications per 10 minutes |

Details: [../tools/mcp-create-agent.md](../tools/mcp-create-agent.md),
[../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md),
[../tools/mcp-claude-code-desktop.md](../tools/mcp-claude-code-desktop.md)
(how these map to Claude Code desktop's own tools),
[../tools/mcp-releases.md](../tools/mcp-releases.md).

## Remote MCP

`app/src/remote_mcp.py` at `https://app.forkmesh.com/mcp` (Streamable HTTP,
POST only), authenticated by an organization bot bearer token:
`list_org_tasks`, `get_org_task` (`organization.tasks.read`),
`complete_org_task` (`organization.tasks.write`).

## Teammates

Teammates (special agents: the Special agent step of a
`.forkmesh/workflows/*.json` workflow, or a legacy `.forkmesh/teammates/*.json`
file) are desktop Claude Code or Codex runs and get that provider's tools; the
JSON has no tool list. Permissions
and modes are enforced as in [access-rules.md](access-rules.md#teammates).
Some teammates use repository CLIs through the shell (for example
`tools/discord_community.py`, which posts only with `--send`, and
`tools/reddit_scout.py`, read-only); these classify as external calls.
Skills live in `.forkmesh/skills/`.

## Server-side AI

No relay code gives an LLM tools to run on the server: ForkBot chat uses
JSON-schema intent classification, other AI calls are tool-less, and
marketplace package workers and ForkMesh Apps ([../APPS.md](../APPS.md))
expose fixed APIs, not LLM tools.

## Known gaps

- Codex runs, agents outside the ForkMesh repository (without the Settings >
  MCP command) and mirror/runner agents have no ForkMesh MCP tools.
- Phone coding agents have no shell; Android has no on-device model or Genie.
- Several planned tool families have no runtime path; package-only families
  depend on a package being installed.
- The IDE bridge returns no diagnostics, selection or open editors.
- The tool allowlist is optional and off by default; destructive-command
  detection is a pattern list, not a sandbox; neither provider is gated in
  the VM.

## Related docs

- [README.md](README.md): agent runs and sandboxing
- [../security/agent-host-boundaries.md](../security/agent-host-boundaries.md)
- [../desktop/agents-ui.md](../desktop/agents-ui.md)
- [../server/agent-jobs.md](../server/agent-jobs.md), [../RUNNERS.md](../RUNNERS.md)
- [../tools/README.md](../tools/README.md)
