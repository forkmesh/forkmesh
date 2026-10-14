# Agent access rules

What every desktop agent may reach and run, and where each rule is enforced.
The run lifecycle is in [README.md](README.md).

## Global rules and project rules

Two layers, edited in the same form (`AgentAccessEditor.*`):

- **Global agent rules**: Settings > Agents > Global agent rules…
  (`MainWindowGlobalAgentRules.cpp`), stored in
  `<home project>/config/agent-rules.json` (`loadGlobalRules`,
  `globalRulesPath`). The home project's `config/` is never granted by the
  folder lock, so an agent cannot rewrite the rules that hold it.
- **Project rules**: the gear beside a project, stored under `agentAccess` in
  its `project.json`.

A project with no `agentAccess` key, or with `"followGlobal": true`, follows
the global rules. `effectiveAgentAccess()` combines the layers at each
launch:

| Setting | Following project | Project with its own settings |
| --- | --- | --- |
| Folder boundary, shared tools, external calls (ask, block, allow) | Global | Project |
| Extra folders, approved destinations | Global and project lists merged | Global and project lists merged |
| Blocked destinations | Global, always | Global, always |
| Destructive commands (ask, block, allow), tool allowlist | Global | Global |
| Destructive kinds allowed without asking | Global and project lists merged | Global and project lists merged |
| Folders where recursive deletes run without asking | Global and project lists merged | Global and project lists merged |

With no rules file the defaults lock agents to the project, ask for every
external call and ask for destructive commands. `forkmesh::agentrules`
(`AgentRunRules.*`) turns the rules plus any teammate permissions into one
decision per command (run, ask or deny), used by the Claude Code hook, the
Codex approval handler, the legacy script providers and the transcript's
command chips.

## What each rule covers

### Tool allowlist

With the allowlist on, Claude Code launches with `--tools` naming only the
built-in tools of the checked families plus `ToolSearch`, `EnterPlanMode` and
`ExitPlanMode`, so tools a newer Claude Code adds stay off. Families outside
the list are switched off for Codex and the script providers as far as they
can (`AgentToolPolicy::familiesOutsideAllowlist`). MCP tools are not limited
by `--tools`.

### Destructive commands

`agentnet::classifyCommandRisk()` (`AgentNetworkGate.cpp`) flags recursive
deletes (`rm -r`, `find -delete`, `find -exec rm`, `shutil.rmtree` in
`python -c`), force pushes and remote ref deletes, `git reset --hard`,
`git clean -f`, `git branch -D`, `git stash drop|clear`, history rewrites
(`filter-branch`, `filter-repo`, `reflog expire`, `gc --prune=now`,
`update-ref -d`), disk wipes (`dd of=/dev/…`, `mkfs`, `wipefs`, `shred`) and
`gh repo delete`. A recursive delete whose every target is under `/tmp` or
`$TMPDIR` does not count. The global rule is Ask (default), Block or Allow.

The "Destructive command" prompt (`promptAgentDestructiveCommand`,
`DestructiveCommandReview.cpp`) shows the whole command with bash
highlighting and manual links, the matched part highlighted, what it deletes
or rewrites and whether it can be undone, reasons to allow and to deny, and
the agent's stated description. Answers: Allow once, Allow *kind* for this
run, Deny (default), "Always allow *kind*" in this project
(`allowedDestructive` in `agentAccess`) or in every project, and for
recursive deletes "In folders matching a rule…". Both settings dialogs list
every kind with a checkbox so an "always" can be revoked.

This is a tool-layer filter: it catches commands an agent types, not a
deletion hidden inside a program it wrote.

### Variables and scratch deletes

- **The hook sends variable values.** For a Bash call the PreToolUse hook
  adds `env: {values, hidden}`: the agent's value of every `$NAME` the
  command uses, plus `FORKMESH_AGENT_SCRATCH` and `HOME` (64 names, 4 KB each
  at most). Secret-looking names (`TOKEN`, `SECRET`, `PASSW`, `KEY`, `AUTH`,
  `CREDENTIAL`, `COOKIE`, `SESSION`, `PRIVATE`) are listed without a value.
  The prompt lists each variable and resolves targets with the values in
  effect where the delete runs (`commandvars::valuesAt`).
- **The `scratch delete` kind.** Reported instead of `recursive delete` when
  `commandvars::deletesOnlyInside` proves every `rm -r` stays inside the
  agent's own scratch folder. It asks by default but can be allowed on its
  own.

The proof is strict and falls back to `recursive delete` on anything it
cannot follow: the scratch root must be a trusted
`<agent root>/.scratch/<worktree>`; the command must lex completely (no
command substitution, `||`, background jobs, subshells, heredocs or `${…}`
operators); assignments count only when they certainly ran; only harmless
commands may precede the `rm`; targets must expand to absolute paths inside
the scratch root after resolving symlinks. Tests: `testScratchDelete`
(`forkmesh-agent-access-tests`).

### Folder rules for recursive deletes

`allowedDeletePaths` in `agentAccess` or the global `defaults` lists path
patterns where recursive deletes run without asking:

- `*` matches one name, `**` any number of folders (at least one). Patterns
  start with `/`, `~`, `$HOME` or `$FORKMESH_AGENT_SCRATCH` (the requesting
  agent's trusted scratch folder). Over-broad patterns (`/**`, `~/**`,
  `/home/*/…`) are refused (`commandvars::deletePatternProblem`).
- Applies when the rule is Ask, every destructive part of the command is an
  `rm -r` (`CommandRisk::onlyRemoves`) and every deleted path matches. Block
  still refuses.
- **Never covered:** `/`, the home folder and its parents, any path with a
  `.git` component, `~/projects`, `~/projects/forkmesh` and its `src`,
  `.worktrees`, `.worktrees/.scratch` and each worktree folder itself, the
  ForkMesh data folders (`data`, `config`, `ssh`, `state`, `logs`,
  `~/.forkmesh`, `~/.config/ForkMesh`, `~/.local/share/ForkMesh`) and
  `~/projects/forkmesh-backup` (`commandvars::protectedDeletePath`).
- Deleted paths are worked out by `commandvars::removedPaths`, strict about
  the `rm` itself and checked both as written and with symlinks resolved.
  Tests: `testDeleteRules`.

### External calls

`AgentNetworkGate.*` classifies Bash, WebFetch and WebSearch calls
([README.md](README.md#desktop-agent-sandboxing)).

- **Blocked destinations** are refused for every project, even one that
  allows all external calls.
- **Scripts that use the network.** `python3 script.py`, `./script.py`,
  `node script.js` and `deno run` read the script (up to 2 MB). A Python
  import of `urllib.request`, `requests`, `httpx`, `aiohttp`, `http.client`,
  `smtplib`, `ftplib`, `websockets` or `paramiko`, or a Node
  `fetch(`/`https`/`net` import, makes the call external, aimed at the first
  non-local URL host in the file. Loopback-only scripts stay local.

## Per provider

| Runtime | Folder lock | External calls and destructive commands | Tool policy |
| --- | --- | --- | --- |
| Claude Code (stream-json) | Landlock | PreToolUse hook on Bash, WebFetch, WebSearch whenever any rule needs it | `--disallowedTools`, `--tools` with the allowlist |
| Claude Code in the built-in terminal | Landlock | Same hook through `--settings` | `--disallowedTools=`, `--tools=` (the `=` form) |
| Codex app-server | Landlock on Linux; elsewhere Codex's `workspace-write` sandbox over the same folders | Approval policy `never`; access failures are steered into the turn | `-c` overrides |
| Legacy script providers (`claude` API, `local-llama`, `cloudflare-ai`) | Landlock | The scripts ask the gate socket before each shell or git tool call, failing closed | `FORKMESH_AGENT_DISABLED_TOOL_FAMILIES`, `FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES` |

- Codex keeps `danger-full-access` on Linux because Landlock already holds
  the folders and `workspace-write` blocks loopback sockets.
- Codex web search is switched off unless the project allows all external
  calls or approves `web-search` (and it is not blocked).
- The `openai` legacy provider (`codex -a never exec`) has only the folder
  lock and the families Codex can switch off.
- Inside the KVM workspace the VM is the boundary; the hook is not applied.

## Teammates

Teammate permissions enforced by the run
(`agentrules::deniedTeammatePermissions`):

| Permission missing | Effect |
| --- | --- |
| `external-tools` | Every external call is refused except approved destinations; WebFetch and WebSearch are removed |
| `create-branches` | `git push`, `git branch <new>`, `git checkout -b`, `git switch -c`, `git worktree add`, `gh pr create` and MCP `open_pr_from_branch` are refused |
| `manage-issues` | `gh issue …` and the MCP issue, milestone and project tools are refused |
| `review-prs` | `gh pr review/comment/edit/close/reopen/ready` are refused |
| `merge` | `gh pr merge` is refused |

Mode (`agentrules::teammateRunMode`): without `modify-code` or
`run-unattended` a teammate plans. An empty mode is Auto only with
`run-unattended`, otherwise Edit.

Narrow grants pre-approve specific commands
(`agentrules::teammateAllowedToolRules`):

| Permission held | Claude Code allow rules | ForkMesh gate |
| --- | --- | --- |
| `run-git` | `Bash(git:*)` | unchanged |
| `run-tests` | `Bash(python3 -m pytest:*)`, `Bash(pytest:*)`, `Bash(node:*)` | unchanged |
| `read-forkmesh-web` | `WebFetch(domain:…)` and `Bash(curl -s https://…/:*)` for `forkmesh.com`, `www.`, `app.` and `world.` | `*.forkmesh.com` joins the approved destinations |

The rules reach Claude Code as `permissions.allow` in the `--settings` JSON
(`agentrules::claudeSettingsWithAllowRules`); the gate hook still runs first.
Rules containing shell-unsafe characters are dropped. Teammates generated
from an agent's reply never get these grants. The grant is recorded per
session (`m_teammateRunGrants`).

### App log access for log monitors

A teammate with an active log monitor (`teammates::watchesAppLog`) gets the
implicit `read-app-logs` grant (`agentrules::kReadAppLogsGrant`) and two
read-only rules (`agentrules::appLogReadRules`):

- `Read(//<data root>/network_log.txt)`
- `Read(//<data root>/network_log.d/**)`

Only those two paths are granted, never the data root, which holds the node
identity key. Paths use Claude Code's POSIX form; an empty, relative or root
path grants nothing.

## Related

- Auto-model triage runs `claude -p … --tools '' --strict-mcp-config`.
- Mirror-role jobs get an allowlisted environment
  ([../server/agent-jobs.md](../server/agent-jobs.md)).
- [Organization bot scopes](../app/organization-bot-scopes.md).
- MCP window tools need the intake token
  ([../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md)).
