# MCP connector: `create_agent` (2026-09-30)

`tools/forkmesh_mcp_server.py` can add an agent to the ForkMesh desktop's
Agents list. The owner asked for "the connector can create agents in the agents
desktop list": an agent working through the connector can hand a follow-up task
to a new agent, either running at once or parked until the user starts it.

## The tool

| Argument | Required | Meaning |
| --- | --- | --- |
| `repository` | yes | `owner/name` of a repository the desktop has checked out (not a preview-only repository). |
| `prompt` | yes | The task, at most 8000 characters (the same `kMaxPromptChars` as local control's `agents.start`). |
| `title` | no | The name in the Agents list, at most 200 characters. Without it the desktop derives one from the prompt, as for a typed prompt. |
| `branch` | no | Continue on this existing branch instead of cutting a new one (the `agents.handoff` path, `startAgentOnExistingBranch`). |
| `worktree` | no | Absolute path of an existing ForkMesh agent worktree of this repository to reuse. Its checked-out branch is used when `branch` is absent, and must equal `branch` when both are given. |
| `paused` | no | `true` lists the agent without running it until the user starts it. Default `false`. |
| `priority` | no | Queue priority 1-99, 1 runs first. Default 50. |
| `provider`, `model` | no | Default to the desktop's default provider and, for Claude Code, its configured model. |

It returns the new session id, `state` (`paused`, `queued`, `starting`,
`running`, ...), `paused`, the repository, branch, title, provider, model and
priority. Refusals come back as a tool error carrying the desktop's code
(`no_checkout`, `branch_not_found`, `protected_branch`, `worktree_mismatch`,
`worktree_not_agent`, `rate_limited`, `unauthorized`, ...).

`create_agent` is in `WRITE_TOOLS`, so once the node publishes a connector the
call needs a matching `FORKMESH_MCP_TOKEN` like every other write, and it is
listed under "Writes" in the server's `initialize` instructions. `whoami`
reports `canCreateAgents`.

## Transport and authentication

The MCP server is not started by the desktop directly: Claude Code (or Codex)
starts it from `.mcp.json` inside an agent the desktop launched, and it inherits
that agent's environment. That environment is the channel:

- The desktop owns one `QLocalServer` (`forkmesh::agentintake::Server`,
  `desktop/src/AgentIntake.*`), started the first time it launches an agent.
  The socket lives in `$XDG_RUNTIME_DIR` (a 0700 directory; the temp directory
  when there is none), named `forkmesh-agent-intake-<pid>-<random 64 bits>`, and
  is created with `QLocalServer::UserAccessOption`, so only the same user can
  connect. This is the same pattern as the external-call gate
  (`AgentNetworkGate`).
- Every Claude Code and Codex launch (`MainWindowAgentCliSession.cpp`) gets
  `FORKMESH_AGENT_INTAKE=<socket path>` and
  `FORKMESH_AGENT_INTAKE_TOKEN=<session id>.<256-bit random secret>`
  (`MainWindow::agentIntakeEnv`). The secret is minted per agent session, held
  only in the desktop's memory, and reused when that session relaunches. A
  desktop restart mints new ones; in a KVM guest, or when the socket cannot
  listen, both variables are removed instead.
- The MCP server refuses unless both variables are set, the path is a socket,
  and the socket belongs to the calling user. It sends one JSON line
  `{"token", "method": "agents.create", "params"}` and reads one line back.
- The desktop accepts one request per connection (at most 64 KiB, idle
  connections dropped after 10 s), looks the session id up in its token table
  and compares the secret in constant time. Anything else gets `unauthorized`.
  There is no TCP listener and nothing reachable from another user or machine.

What this does and does not prove: a caller holding the token is a process
running inside an agent this desktop launched (the MCP server, or the agent's
own shell, which inherits the same variables). That is the same trust boundary
as the external-call gate and the agent's existing write tools; it keeps out
other local users, other programs of this user that were not started by the
desktop, and anything off the machine.

## Validation on the desktop

The desktop re-validates everything (`forkmesh::agentintake::parseCreateRequest`
plus `MainWindow::createAgentFromIntake`); the server's checks are only an early
answer:

- `repository` must match a repository record with a local checkout.
- `prompt` 1-8000 characters, `title` at most 200, `model` and `provider` limited
  to id characters, `provider` must be one the desktop knows, `priority` an
  integer 1-99, `worktree` a clean absolute path.
- `branch` must pass `forkmesh::localcontrol::handoffRefNameValid`. Desktop agent
  branches are `<id>-<slug>`, not `agent/...`, so the stricter
  `handoffBranchValid` (phone hand-offs, `agent/` only) would refuse the
  branches this tool is for. Instead `main`, `master`, `trunk`, `HEAD`,
  `refs/...`, `release/...` and the branch the repository's own checkout has
  open are refused (`protected_branch`). A branch missing locally is fetched
  from the served mirror without moving an existing local branch
  (`fetchServedBranch(..., update=false)`).
- A `worktree` must be a directory, have a branch checked out, be the worktree
  git lists for that branch in this repository, and sit directly under the
  repository's agent worktree root. Any other checkout is the operator's and is
  never adopted.
- At most 12 agents per 10 minutes are created this way per desktop run
  (`RateLimiter`), so a looping agent cannot flood the list.

Each created session's log says which agent created it, and the desktop files
a ping ("Agent #7 started an agent for owner/repo.") on the Pings page and shows
the same line as an inline notice at the top of the page in view, in normal
layout rather than as a floating card. Until 2026-10-09 this was a toast; the
user asked for no toasts anywhere, with the information shown inline.

Settings > Pings > **Ping when a new agent is started** (`notifications/agentStart`,
on by default) turns that ping and notice off. With it off, the line goes to
the Log only. It was added on 2026-10-07 because agents that start agents made a ping
for every run, and it is switched off on the ForkMesh team's own desktop.

## Activating it

The desktop has to be rebuilt and restarted, and the agent that will call the
tool has to be (re)started from the desktop after that, so its MCP server
inherits the two variables. An already-running Claude Code session keeps its
old MCP server and environment.
