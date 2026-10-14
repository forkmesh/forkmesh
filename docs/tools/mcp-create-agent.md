# MCP connector: `create_agent`

`tools/forkmesh_mcp_server.py` can add an agent to the ForkMesh desktop's
Agents list, so an agent working through the connector can hand a follow-up
task to a new agent, running at once or parked until the user starts it.

## The tool

| Argument | Required | Meaning |
| --- | --- | --- |
| `repository` | yes | `owner/name` of a repository the desktop has checked out |
| `prompt` | yes | The task, at most 8000 characters (`kMaxPromptChars`) |
| `title` | no | The name in the Agents list, at most 200 characters; derived from the prompt otherwise |
| `branch` | no | Continue on this existing branch (`startAgentOnExistingBranch`) |
| `worktree` | no | Absolute path of an existing agent worktree of this repository to reuse; its branch must equal `branch` when both are given |
| `paused` | no | `true` lists the agent without running it. Default `false` |
| `priority` | no | Queue priority 1-99, 1 runs first. Default 50 |
| `provider`, `model` | no | Default to the desktop's defaults |

It returns the new session id, `state`, `paused`, repository, branch, title,
provider, model and priority. Refusals carry the desktop's code
(`no_checkout`, `branch_not_found`, `protected_branch`, `worktree_mismatch`,
`worktree_not_agent`, `rate_limited`, `unauthorized`, ...). `create_agent` is
in `WRITE_TOOLS`, so a published connector needs `FORKMESH_MCP_TOKEN`;
`whoami` reports `canCreateAgents`.

## Transport and authentication

The MCP server runs inside an agent the desktop launched and inherits its
environment, which is the channel:

- The desktop owns one `QLocalServer` (`forkmesh::agentintake::Server`,
  `desktop/src/AgentIntake.*`) in `$XDG_RUNTIME_DIR` (0700), named
  `forkmesh-agent-intake-<pid>-<random 64 bits>`, created with
  `UserAccessOption`.
- Every Claude Code and Codex launch gets `FORKMESH_AGENT_INTAKE=<socket>` and
  `FORKMESH_AGENT_INTAKE_TOKEN=<session id>.<256-bit random secret>`
  (`MainWindow::agentIntakeEnv`). Secrets live only in desktop memory and are
  reminted on restart; in a KVM guest or without a socket both variables are
  removed.
- The MCP server refuses unless both are set and the socket belongs to the
  calling user. It sends one JSON line `{"token", "method": "agents.create",
  "params"}` and reads one line back.
- The desktop accepts one request per connection (at most 64 KiB, 10 s idle
  timeout) and compares the secret in constant time. There is no TCP
  listener.

A token proves the caller runs inside an agent this desktop launched: the
same trust boundary as the external-call gate.

## Validation on the desktop

The desktop re-validates everything (`parseCreateRequest`,
`MainWindow::createAgentFromIntake`):

- `repository` must have a local checkout; field lengths and characters are
  checked; `priority` is an integer 1-99.
- `branch` must pass `handoffRefNameValid`; `main`, `master`, `trunk`,
  `HEAD`, `refs/...`, `release/...` and the checkout's own branch are refused
  (`protected_branch`). A missing branch is fetched from the served mirror
  without moving an existing local branch.
- A `worktree` must be the worktree git lists for that branch, directly under
  the repository's agent worktree root.
- At most 12 agents per 10 minutes per desktop run (`RateLimiter`).

Each created session's log names the creating agent, and the desktop files a
ping and an inline notice. Settings > Pings > **Ping when a new agent is
started** (`notifications/agentStart`, on by default) turns that off.

## Activating it

Rebuild and restart the desktop, then start the calling agent from the
desktop so its MCP server inherits the two variables.
