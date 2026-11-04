# MCP connector: `create_agent`

Part of [the tools docs](README.md); the MCP server itself is in
[agent tools](../agents/tools.md#forkmesh-mcp-server).

`create_agent` on `tools/forkmesh_mcp_server.py` adds an agent to the
desktop's Agents list, so an agent can hand a follow-up task to a new agent,
running at once or parked until the user starts it. The agent-intake socket
described here also carries the window, hub, terminal and release tools
([mcp-desktop-tools.md](mcp-desktop-tools.md),
[mcp-releases.md](mcp-releases.md)).

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
| `task` | no | 32-hex id of an organization task (`list_tasks`) to hand to the new agent |
| `handoff` | no | `true` hands over the task the calling agent is linked to. Default `false` |

It returns the new session id, `state`, `paused`, repository, branch, title,
provider, model, priority, `task` (the linked task id, empty when none) and
`handedOff`. Refusals carry the desktop's code (`no_checkout`,
`branch_not_found`, `protected_branch`, `worktree_mismatch`,
`worktree_not_agent`, `bad_task_id`, `no_task`, `task_mismatch`,
`rate_limited`, `unauthorized`, ...). `create_agent` is
in `WRITE_TOOLS`, so a published connector needs `FORKMESH_MCP_TOKEN`;
`whoami` reports `canCreateAgents`.

## Handing off a task

`task` links the new agent to an organization task the same way the Tasks
page's **Start agent on task** does (`orgTaskId` with `orgTaskAttached`
false): the agent reports its status and work item on the task, the Agents
detail shows the task chip, and the task is completed when the agent
finishes. On a `branch` run, which `startAgentOnExistingBranch` starts
without a task, the link is added afterwards through `recordOrgTaskFields`.

`handoff` is for an agent that is itself linked to a task and wants another
agent to finish it. The desktop takes the task id from the calling session
(the token names it), so the caller need not know it; a `task` that differs
is refused (`task_mismatch`) and a caller with no task gets `no_task`. Once
the new agent exists the caller is unlinked (`unlinkAgentSessionFromOrgTask`,
shared with **Detach from task**), so its end no longer completes the task and
deleting it no longer deletes the task. Both sessions' logs record the
handoff. Write the prompt as a brief (what is done, what is left and where):
the new agent sees only that prompt.

To hand over a branch as well, pass it as `branch` and stop working on it:
the new agent reuses the agent worktree that holds it.

## Linking a branch to your own session: `link_branch`

A session the desktop starts directly in the checkout (`directCheckout`) has
no branch (`branchName` empty), so when its agent later moves the work to a
branch (`git switch -c ...` in the checkout, or a worktree it added) the
desktop cannot see it: the agent detail shows no **Branch** button, and
**Code**, the merge actions and the task link have nothing to work on. The
session lives in memory and `AgentStore::saveSession()` rewrites
`meta.json` whole, so editing the file does not stick. `link_branch` is the
way to attach the branch.

| Argument | Required | Meaning |
| --- | --- | --- |
| `branch` | yes | The branch, valid per `handoffRefNameValid` and present locally in the session's repository |
| `worktree` | no | Absolute path of the worktree of that repository that has `branch` checked out |

The MCP tool sends `agents.link_branch` and is listed only when the desktop's
intake variables are set. The desktop (`parseLinkBranchRequest`,
`linkBranchRefusal`, `MainWindow::linkBranchFromIntake`) applies it to the
calling session, named by the token, so an agent can only link its own
session:

- The session must exist and be local (`no_session`) and running or waiting
  on approval (`not_running`).
- `main`, `master`, `trunk`, `HEAD`, `refs/...`, `release/...`, the
  session's base branch and, when the session has none, the repository's
  default branch are refused (`protected_branch`). The checkout's current
  branch is allowed, because that is where a direct-checkout agent usually
  creates its branch.
- A session the desktop started in its own worktree keeps that branch: any
  other branch is refused (`has_branch`). A direct-checkout session can link
  again to move to another branch.
- The branch must exist locally (`branch_not_found`); nothing is fetched.
- A `worktree` goes through the same checks as `create_agent`'s
  (`verifyIntakeWorktree`): it must be a directory with a branch checked out
  (`bad_worktree`), that branch must be `branch` and git must list it as the
  worktree holding it (`worktree_mismatch`). Unlike `create_agent`, it need
  not be under the agent worktree root.

On success the desktop sets `branchName`, `worktreeName` (the worktree's
folder name) when a worktree is given, and `baseBranch` when it was empty
(the open repository's configured default, else `main` or `master`). It
leaves `directCheckout` and the running process's working directory alone,
saves the session, logs `==> Linked branch X`, refreshes the list and detail
pane and pushes the sessions to phones. If the session is linked to an
organization task, the branch (and worktree, if any) is attached to the task
as a work item, as for worktree sessions. The reply is
`{"ok": true, "branch", "worktree"}`. Other refusals: `bad_branch`,
`bad_request`, `no_checkout`, `not_saved`.

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
- `task` must be 32 lowercase hex characters after trimming and lowering
  (`taskIdValid`); `handoff` must be a boolean.
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
