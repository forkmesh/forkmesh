# Agent access rules

What every desktop agent may reach and run, and where each rule is enforced.
Added 2026-10-02 (branch `1324-fix-global-rules`) in answer to the "Safety and
consistency" list in the agent tools inventory (branch
`1323-create-document-all-tools`, `docs/agents/tools.md`). The run lifecycle is
in [README.md](README.md).

## Global rules and project rules

There are two layers, edited in the same form (`AgentAccessEditor.*`):

- **Global agent rules**: Settings > Agents > Global agent rules…, also reached
  from a project's settings and from the transcript's Commands and permissions
  dialog (`MainWindowGlobalAgentRules.cpp`). Stored in
  `<home project>/config/agent-rules.json` (`forkmesh::projects::loadGlobalRules`,
  `globalRulesPath`). The home project's `config/` is one of the private roots
  the folder lock never grants, so an agent cannot rewrite the rules that hold it.
- **Project rules**: the gear beside a project, stored under `agentAccess` in its
  `project.json`, as before.

A project follows the global rules unless it was saved with its own settings.
A project with no `agentAccess` key follows them, and so does one saved with
`"followGlobal": true` (the "Follow the global agent rules" box). Projects saved
before this change have their own settings and keep them.

`effectiveAgentAccess()` combines the layers at each launch:

| Setting | Following project | Project with its own settings |
| --- | --- | --- |
| Folder boundary, shared tools, external calls (ask, block, allow) | Global | Project |
| Extra folders, approved destinations | Global and project lists merged | Global and project lists merged |
| Blocked destinations | Global, always | Global, always |
| Destructive commands (ask, block, allow), tool allowlist | Global | Global |
| Destructive kinds allowed without asking | Global and project lists merged | Global and project lists merged |
| Folders where recursive deletes run without asking | Global and project lists merged | Global and project lists merged |

With no rules file the global defaults are the old project defaults (locked to
the project, every external call asks), and destructive commands ask. "Always
allow" on an external-call prompt and "Grant folder" on an access-failure
prompt write to the project's own lists, so a following project keeps following.

`forkmesh::agentrules` (`AgentRunRules.*`) turns the rules, plus any teammate
permissions, into one decision per command: run, ask or deny. The Claude Code
hook, the Codex approval handler, the legacy script providers and the
transcript's command chips all use it, so they agree.

## What each rule covers

### Tool allowlist

With "Only let agents use the tool families checked below" on, Claude Code
launches with `--tools` naming only the built-in tools of the checked families,
plus `ToolSearch`, `EnterPlanMode` and `ExitPlanMode`. A tool a newer Claude
Code adds stays off until someone allows it. The families outside the list are
also added to the switched-off families, so Codex and the script providers
remove what they can (`AgentToolPolicy::familiesOutsideAllowlist`). The Tool
efficiency switches still apply on top. MCP tools are not built-in tools, so
`--tools` does not limit them; teammates lose the MCP tools their permissions
do not cover (below).

### Destructive commands

`agentnet::classifyCommandRisk()` (`AgentNetworkGate.cpp`) flags recursive
deletes (`rm -r`, `find -delete`, `find -exec rm`, `shutil.rmtree` in
`python -c`), force pushes and remote ref deletes (`--force`,
`--force-with-lease`, `--mirror`, `--delete`, `+ref`, `:ref`),
`git reset --hard`, `git clean -f`, `git branch -D`, `git stash drop|clear`,
history rewrites (`filter-branch`, `filter-repo`, `reflog expire`,
`gc --prune=now`, `update-ref -d`), disk wipes (`dd of=/dev/…`, `mkfs`,
`wipefs`, `shred`) and `gh repo delete`. A recursive delete whose every target
is under `/tmp` or `$TMPDIR` does not count; one proven to stay inside the
agent's own scratch folder is the separate `scratch delete` kind (see
[Variables and scratch deletes](#variables-and-scratch-deletes-2026-10-07)). The global rule is Ask (default),
Block or Allow. Ask holds the agent on a "Destructive command" prompt with
Allow once, Allow for this run (that kind of command, this run only), Always
allow (that kind, in this project or in every project) and Deny.

The prompt (`promptAgentDestructiveCommand`, a plain `QDialog` since
2026-10-06) is built so the user can decide from it alone:

- **The whole command, sized to fit.** A long `&&` chain or a heredoc commit
  message once hid the part that tripped the rule behind a 300-character cut,
  and a 14-line cap then hid the rest behind a scroll bar. The view's height
  now comes from the laid-out document at the dialog's width (up to 55% of the
  screen; only longer commands scroll, with the match scrolled into view). The
  wrapped labels' narrow-width minimum heights made the dialog twice as tall as
  its content, so the layout has no size constraint and the dialog is resized
  to `totalHeightForWidth` instead.
- **Highlighted bash with links** (`destructive::commandHtml`,
  `DestructiveCommandReview.cpp`): programs, git/gh subcommands, flags,
  strings, variables, operators, redirects and comments are coloured. Programs
  and subcommands link to their manual (git-scm.com, cli.github.com, man7.org,
  Python, pytest), and paths that exist relative to the call's working folder
  open the file or folder. The simple command that matched
  (`CommandRisk::matchStart`/`matchLength`) is drawn on amber.
- **Why it is destructive** (`destructive::review`): what exactly that part
  deletes or rewrites, naming its targets, and how (or whether) it can be
  undone.
- **Reasons to allow / to deny and a lean**, read from the command itself. For
  `git stash drop $n` after `git stash push -m <tag>`, a lookup by that tag and
  `git stash apply`, it leans allow (the agent drops only its own entry), and
  still lists that an unquoted `$n` that comes out empty turns into a plain
  `git stash drop` of someone else's newest entry, and that `>/dev/null` hides
  the id git prints for recovery. Recursive deletes check each target for `/`,
  home, `.`, unquoted variables (and what the path becomes when they are
  empty), `..`, globs, ForkMesh data, `.git`/worktrees, scratch folders and
  regenerable build output; force pushes check shared branch names and
  `--force-with-lease`; `git clean` checks `-n`, `-x` and `-X`. When the Claude
  call carries a `description`, it is shown as "The agent says".
- **Answers:** Allow once, Allow *kind* for this run, Deny (default), and
  "Always allow *kind*" with "In this project" (writes `allowedDestructive` in
  the project's `agentAccess`) or "In every project" (writes it into the global
  `defaults`). `effectiveAgentAccess` merges both lists like approved
  destinations, and `agentrules::decide` runs a listed kind without asking when
  the rule is Ask; Block still refuses it. Both settings dialogs list every
  kind with a checkbox (`AgentAccessEditor`), so an "always" can be taken back,
  and the run's permissions summary shows the merged list. For a recursive
  delete, "In folders matching a rule…" saves a folder rule instead (see
  [Folder rules](#folder-rules-for-recursive-deletes-2026-10-07)).

This is a tool-layer filter like the external-call gate: it catches the
commands an agent types, not a deletion hidden inside a program it wrote.

#### Variables and scratch deletes (2026-10-07)

A prompt for `S=$FORKMESH_AGENT_SCRATCH/desktop && rm -rf $S/marketplace`
leaned deny with "`$S/marketplace` depends on a variable; if it is empty or
unset the path becomes `/marketplace`", although `S` was set two words
earlier, and offered no way to let agents clean their own scratch folder
without also allowing every `rm -r`.

- **The hook sends variable values.** For a Bash call the PreToolUse hook adds
  `env: {values, hidden}`: the agent's value of every `$NAME` the command uses,
  plus `FORKMESH_AGENT_SCRATCH` and `HOME` (64 names, 4 KB each at most). Names
  that look secret (`TOKEN`, `SECRET`, `PASSW`, `KEY`, `AUTH`, `CREDENTIAL`,
  `COOKIE`, `SESSION`, `PRIVATE`) go in `hidden` without a value. The gate
  keeps them on `Request::environment` (`commandvars::Environment`).
- **Variables in the prompt.** Under the command the dialog lists each
  variable (`commandvars::variables`, `CommandVariables.cpp`): set in the
  command (its text and what it comes to), the agent's environment value,
  "not set, so it expands to nothing", withheld as secret, or unknown when an
  older hook sent no values. The review resolves targets with the values in
  effect where the delete runs (`commandvars::valuesAt`), so "Why this is
  destructive" names `$S/marketplace (/…/.scratch/1705-…/desktop/marketplace)`
  and the "if it is empty" reason only appears when the value is not known.
- **The `scratch delete` kind.** `classifyCommandRisk(command, scratch)`
  reports `scratch delete` instead of `recursive delete` when
  `commandvars::deletesOnlyInside` proves every `rm -r` in the command stays
  inside the agent's own scratch folder. Like every kind it asks by default;
  "Always allow scratch delete" (or its checkbox in the agent settings) lets
  agents clean their scratch without asking while other recursive deletes
  still ask. Any other destructive part of the same command outranks it.

The proof is strict and gives up (falling back to `recursive delete`) on
anything it cannot follow:

- The scratch root is trusted only when `FORKMESH_AGENT_SCRATCH` is a clean
  absolute folder named like `<agent root>/.scratch/<worktree>` whose agent
  root has the ForkMesh `.gitignore` (`commandvars::trustedScratch`). Other
  environment values are shown but never used in the proof, since the shell
  snapshot Claude Code sources may change them.
- The command must lex completely: no `$(…)`, backticks, `||`, background
  `&`, subshells, groups, brace expansion, heredocs, process substitution,
  `$1`/`$?`-style parameters or `${…}` operators.
- Assignments count only when they certainly ran: a `NAME=` that follows
  another command in an `&&` chain is unknown after the next `;`, one inside a
  pipeline is unknown, prefix assignments (`X=1 cmd`) make `X` unknown
  afterwards, and assigning bash's special or read-only names (`IFS`,
  `GLOBIGNORE`, `PWD`, `UID`, `BASH*`, …) fails or is unknown.
- Before the last checked `rm -r` only harmless commands may run (`cd`,
  `mkdir`, `touch`, `echo`, `ls`, `cat`, `printf` without `-v`, `set`,
  `export NAME=…`, plain `rm`, …): nothing that could create a symlink, change
  a variable behind the parser's back (`read`, `unset`, `eval`, `source`) or
  wrap an `rm` (`sudo`, `xargs`, `env`). After it, anything may run.
- Every target must expand fully to an absolute path, unquoted expansions must
  not split or glob, there is no trailing `/`, no empty, `.` or `..`
  component, and a glob (`*`, `?`) is allowed only in the last component and
  not after a leading dot. The target's parent must be the scratch root or
  inside it, so the root itself is never a scratch delete, and the deepest
  existing part of the parent must resolve (symlinks followed) inside the
  scratch root.

Tests: `testScratchDelete` and the hook round trip in `testServer`
(`forkmesh-agent-access-tests`).

#### Folder rules for recursive deletes (2026-10-07)

A prompt for a long build command that ended in
`rm -rf $FORKMESH_AGENT_SCRATCH/mainbase && mkdir -p … && git archive main | tar -x …`
counted as a plain `recursive delete`: the `( time … )` subshell and `$?`
earlier in the line make the scratch proof above give up. The only answers
were to allow every recursive delete or to keep answering. Folder rules let
the user say "deletes under these folders are fine", for example anything in
an agent's scratch folder or under `~/projects/forkmesh/src/.worktrees/*/`.

- **The rule.** `allowedDeletePaths` in `agentAccess` (a project) or in the
  global `defaults`, merged like the other lists. Each entry is a path
  pattern: `*` matches one name, `**` any number of folders (at the end, at
  least one, so `~/a/**` covers what is inside `~/a` but not `~/a` itself).
  It starts with `/`, `~`, `$HOME` or `$FORKMESH_AGENT_SCRATCH`, which is
  filled in with the requesting agent's trusted scratch folder, so one rule
  covers every agent's own scratch. No other variables, no `.` or `..`, and a
  pattern with no named folder below the home folder before its first
  wildcard (`/**`, `~/**`, `/home/*/…`) is refused as too broad
  (`commandvars::deletePatternProblem`).
- **When it applies.** `agentrules::decide` runs a `recursive delete` or
  `scratch delete` without asking when the rule is Ask, every destructive part
  of the command is an `rm -r` (`CommandRisk::onlyRemoves`; a `find -delete`,
  `rmtree` or any other kind still asks), and every path those `rm`s delete
  matches a rule. Block still refuses.
- **Never covered.** `/`, the home folder and its parents, any path with a
  `.git` component, `~/projects`, `~/projects/forkmesh` and its `src`,
  `.worktrees`, `.worktrees/.scratch` and each worktree folder itself, the
  ForkMesh data folders (`data`, `config`, `ssh`, `state`, `logs`,
  `~/.forkmesh`, `~/.config/ForkMesh`, `~/.local/share/ForkMesh`) and
  `~/projects/forkmesh-backup` (`commandvars::protectedDeletePath`), whatever
  the rules say.
- **Where to set them.** The agent settings (project and global) have a
  "Folders where recursive deletes run without asking" list. On the
  "Destructive command" prompt, "Always allow … > In folders matching a rule…"
  opens a small dialog listing what the command deletes, with suggested
  patterns (`$FORKMESH_AGENT_SCRATCH/**`, `<…>/.worktrees/.scratch/*/**`,
  `<…>/.worktrees/*/**`, the common parent folder), a live "covers / does not
  cover" check and the project or global scope. Saving answers the prompt.
  The run's permissions summary lists the merged rules.

How the deleted paths are worked out (`commandvars::removedPaths`, filled into
`CommandRisk::removedPaths` by the gate from the hook's variable values) is
looser than the scratch proof about the rest of the line, since a rule is the
user's own choice, but strict about the `rm` itself:

- Every `rm -r` the classifier flagged must be found and read; a wrapper
  (`sudo rm`, `xargs rm`), a function definition, `eval`, `source` or `.`
  anywhere in the line, or any mention of `IFS` gives up.
- The `rm` and its words must be clean: a `$(…)`, backtick, `$1`/`$?`-style
  parameter, `${…}` operator, unclosed quote, or a `(`, `{` or backtick that
  ends the `rm` (brace expansion, process substitution) gives up.
- Values come only from `$FORKMESH_AGENT_SCRATCH` when it is trusted, `HOME`
  (and `~`) when the agent's value is the user's home folder, and assignments
  in the command when the whole line lexes strictly (the same
  certainly-ran rules as above). A variable used in a target whose name also
  appears anywhere else in the line in a non-`$` form (`read X`, `unset X`,
  `declare -n`, an assignment in a subshell) is unknown, and an unknown or
  word-splitting value gives up.
- Each target must be absolute with no empty, `.` or `..` component; a glob
  only in the last component and not after a leading dot. A target name with
  `*` or `?` is matched only by a `*` or `**` pattern component.
- The path is checked both as written and with the deepest existing folder
  resolved through symlinks (the whole path for a trailing `/`), and both must
  match. An `ln`, `cp` or `mv` before the `rm` in the same line gives up, since
  it could put a link in place; a program the line runs before the `rm` could
  still do that, which is the usual limit of a tool-layer filter.

Codex commands (`decideCommand`) and the transcript's command chips resolve
`~` with the user's home folder; the chips also use the session's scratch
folder.

Tests: `testDeleteRules` (`forkmesh-agent-access-tests`).

### External calls

Unchanged in kind ([README.md](README.md#desktop-agent-sandboxing)), with two
additions:

- **Blocked destinations** are refused for every project, even one that allows
  all external calls; a non-empty list keeps the gate on.
- **Scripts that use the network.** `python3 script.py`, `./script.py`,
  `node script.js` and `deno run` read the script (up to 2 MB). A Python import
  of `urllib.request`, `requests`, `httpx`, `aiohttp`, `http.client`, `smtplib`,
  `ftplib`, `websockets` or `paramiko`, or a Node `fetch(`/`https`/`net`
  import, makes the call external, aimed at the first non-local URL host in the
  file. A script whose URLs are all loopback stays local. Hearth's
  `tools/discord_community.py` and the Reddit agent's `tools/reddit_scout.py`
  used to classify as local and so bypassed the gate.

## Per provider

| Runtime | Folder lock | External calls and destructive commands | Tool policy |
| --- | --- | --- | --- |
| Claude Code (stream-json) | Landlock | PreToolUse hook on Bash, WebFetch, WebSearch whenever any rule needs it | `--disallowedTools`, `--tools` with the allowlist |
| Claude Code in the built-in terminal | Landlock (the terminal command is wrapped like other launches) | Same hook through `--settings` | `--disallowedTools=`, `--tools=` (the `=` form, so the prompt argument after them is not read as a tool name) |
| Codex app-server | Landlock on Linux; elsewhere Codex's `workspace-write` sandbox over the same folders, with network on | Approval policy `never`; Codex does not wait for host command approval | `-c` overrides |
| Legacy script providers (`claude` API, `local-llama`, `cloudflare-ai`) | Landlock | The scripts ask the same gate socket before each shell or git tool call, failing closed | `FORKMESH_AGENT_DISABLED_TOOL_FAMILIES`, `FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES` |

Codex details:

- `danger-full-access` stays on Linux because Landlock already holds the
  folders and `workspace-write` blocks loopback sockets (dev servers,
  Playwright). Codex keeps approval policy `never` regardless of external-call
  or destructive-command rules.
- **Web search** cannot be asked about per call, so it is switched off
  (`web_search="disabled"`) unless the project allows all external calls or
  approves `web-search`, and `web-search` is not blocked.
- **Access-failure prompts** now reach Codex runs too: each command output goes
  through `detectAccessFailure`, and the user's answer (Retry, Grant folder,
  pin the host) is steered into the running turn. Codex does not wait for the
  answer the way the Claude hook does.

The `openai` legacy provider (`codex -a never exec`) still has only the folder
lock and the tool families Codex can switch off through its command setting.

### Inside the KVM workspace

The VM is the boundary for both providers: the Claude hook is not applied
there, and Codex web search follows the user's tool families only.

## Teammates

`TeammateStore` permissions that the run now enforces
(`agentrules::deniedTeammatePermissions`):

| Permission missing | Effect |
| --- | --- |
| `external-tools` | Every external call is refused except the project's approved destinations; WebFetch and WebSearch are removed |
| `create-branches` | `git push`, `git branch <new>`, `git checkout -b`, `git switch -c`, `git worktree add`, `gh pr create` and the MCP `open_pr_from_branch` are refused |
| `manage-issues` | `gh issue create/comment/edit/close/…` and the MCP issue, milestone and project tools are refused |
| `review-prs` | `gh pr review/comment/edit/close/reopen/ready` are refused |
| `merge` | `gh pr merge` is refused |

Mode (`agentrules::teammateRunMode`): without `modify-code` or
`run-unattended` a teammate plans, as before. An empty mode no longer falls
back to the composer's global mode (Auto by default): it is Auto only with
`run-unattended`, otherwise Edit. A teammate set to Auto without
`run-unattended` ("Run tools without asking") runs in Edit.

Three narrower grants pre-approve specific commands instead of every tool
(`agentrules::teammateAllowedToolRules`, added 2026-10-04 when the operator
let Marketing run git, pytest and node and read its own public site without
a prompt, rather than giving it `run-unattended`):

| Permission held | Claude Code allow rules | ForkMesh gate |
| --- | --- | --- |
| `run-git` ("Run git without asking") | `Bash(git:*)` | unchanged |
| `run-tests` ("Run pytest and node without asking") | `Bash(python3 -m pytest:*)`, `Bash(pytest:*)`, `Bash(node:*)` | unchanged |
| `read-forkmesh-web` ("Read public forkmesh.com pages without asking") | `WebFetch(domain:…)` and `Bash(curl -s https://…/:*)` for `forkmesh.com`, `www.`, `app.` and `world.` | `*.forkmesh.com` joins the run's approved destinations |

The rules reach Claude Code as `permissions.allow` inside the `--settings`
JSON next to the gate hooks (`agentrules::claudeSettingsWithAllowRules`),
for both the stream session and the terminal launch. Whenever the gate hook
is on (any enforced permission missing, as for every shipped special agent,
or project rules that ask or block) it still runs first, so a destructive
git command still asks under the global rules and `git push` still needs
`create-branches`. A rule containing a
quote or other shell-unsafe character is dropped. Codex runs get the
approved destinations but not the allow rules. A teammate generated from an
agent's reply loses all three, like `run-unattended`, and the special-agent
designer prompt does not offer them: only the operator grants them.

The grant is recorded for the session when the teammate starts it
(`m_teammateRunGrants`) and again before a continued run, so the gate knows
which run it is answering for.

### App log access for log monitors

A teammate with an active log monitor (`teammates::watchesAppLog`; Fixer, and
Quality's stall monitor) gets the implicit `read-app-logs` grant
(`agentrules::kReadAppLogsGrant`) and two read-only rules
(`agentrules::appLogReadRules`):

- `Read(//<data root>/network_log.txt)`
- `Read(//<data root>/network_log.d/**)`

Added 2026-10-07. A Fixer crash run (SIGSEGV in `QObjectPrivate::connectImpl`
during action run 1266) could not read the log its assignment pointed at:
Claude Code refused Read, Grep and shell `grep` on both paths because they are
outside the worktree. The assignment's copy of the crash had a cut-off
backtrace and none of the log lines before the crash, so the run could not
find the crash's cause. The rules name the two log locations only.
`--add-dir` on the data root would also have exposed the node identity key,
settings and agent history.
Paths become Claude Code's POSIX form (`C:\…` becomes `//c/…`). An empty,
relative or root path grants nothing. A path with a character that
`claudeSettingsWithAllowRules` rejects drops its rule rather than widening it.
Both repair assignments (`errorLogRepairAssignment`,
`crashRepairAssignment`) tell the run to open the logs with the Read and Grep
tools. A shell `grep` on the same paths may still ask. Codex runs already read
outside the workspace and need no rule.

## Other fixes in the same change

- **Auto-model triage** (`runClaudeAutoTriageRung`) runs
  `claude -p … --tools '' --strict-mcp-config`: no built-in or MCP tools, where
  before only its prompt said not to use them.
- **Mirror-role jobs** no longer inherit the daemon's environment or HOME
  ([../server/agent-jobs.md](../server/agent-jobs.md)).
- **Organization bot scopes** that no route checked were retired
  ([../app/organization-bot-scopes.md](../app/organization-bot-scopes.md)).
- **MCP window tools** need the intake token
  ([../tools/mcp-agent-windows.md](../tools/mcp-agent-windows.md)).
