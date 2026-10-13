# Agent jobs on mirror nodes

How a managed mirror runs the Claude Code, Codex and Gemma sessions the website
(or a desktop's **To cloud**) queues for it. Operator view:
[mirror-node.md, "Running website agent jobs"](../../www/docs/operations/mirror-node.md#running-website-agent-jobs).
Code: `server/agents.go`, `server/agent_errors.go`,
`server/agent_attachments.go`, `server/agent_live.go`, `server/terminal.go`.

## Lifecycle

1. **Advertise.** `CatalogPublisher.agentProviders()` (`server/catalog.go`)
   lists `claude`/`codex` when found on `PATH`, plus `local-llama` (Gemma)
   when both `codex` and `ollama` are, unless `disableAgentJobs` is set. That
   puts the node in the website's "Run on" list.
2. **Wake.** The relay pushes an `org-agents` event on the node socket; a
   5-minute safety read (`agentJobSafetyCheck`) covers a lost push and results
   the relay has not accepted yet.
3. **Claim.** Up to 8 jobs per repository per read (`agentJobClaimsPerRepo`)
   over the signed `org-agent-jobs` lease, signed with the node key.
4. **Validate.** `agentJobRunner.validate`: session id, prompt of at most 8,000
   characters, provider in `agentWebsiteProviders`, model in
   `agentWebsiteModels`, kind `start`/`continue`/`steer`, valid attachments,
   and owner/repository matching this node. Anything else fails with
   "Malformed or unsafe job envelope."
5. **Prepare.** A per-session clone under `mirror-gateway/agent-jobs/work/<hash>`
   (`prepareAgentWorkspace`, `server/agent_workspace.go`, see
   [The session clone](#the-session-clone)); `start` branches
   `agent/web-<job>-<slug>` off the default branch, or the job's own
   `branch` (under `agent/`) off its `baseBranch` when the start named them
   (2026-09-30, [runner agents](../native/RUNNER_AGENTS.md)); `continue` waits up to 10
   minutes for the moved branch (`agentHandoffBranchWait`); `steer` resumes the
   stored session.
6. **Attach images.** Up to 5 images, 5 MiB each, fetched over the lease,
   checked, and written to `.forkmesh/attachments/<uuid>.<ext>` (listed in
   `.git/info/exclude`). A runner fetches them with
   `POST /api/runners/attachment` instead, and refuses a symlinked folder
   ([../RUNNERS.md](../RUNNERS.md#prompt-images-2026-10-04)).
7. **Run.** At most 2 runs at once (`agentMaxConcurrentRuns`), 2 hours each.
   Claude: `claude -p --output-format stream-json --verbose
   --dangerously-skip-permissions [--model M] [--resume S]`, prompt on stdin.
   Codex: `codex -a never [-m M] exec --sandbox danger-full-access -o <file> -`.
   Gemma: the same Codex command with `exec --oss`, which points Codex at the
   node's own Ollama; the model defaults to `gemma4:e4b` because a bare
   `--oss` would pick Ollama's gpt-oss instead.
   A Claude follow-up whose conversation the CLI no longer has is retried once
   without `--resume`.
   While a Claude run goes, the transcript so far is sent as a `live: true`
   running report, every 2.5 s at most, or every 250 ms while the App answers
   `watched: true` because a phone has the run's live stream open
   ([../agents/README.md](../agents/README.md#live-stream-to-the-phone)).
8. **Publish and report.** Push the branch, open a node-signed pull if asked,
   report running/completed/failed with transcript, tokens and cost. State is
   in `mirror-gateway/agent-jobs/state.json`, so a re-leased job is
   re-reported rather than run twice; a run cut off by a restart is reported
   failed.

## Gemma (`local-llama`)

Added for issue #591 so the web composer's provider dropdown can offer Gemma.
Desktops no longer publish their own catalog record, so every website target
is a Go node and Gemma has to run there. Codex already drives the agent loop
(tools, sandbox, last-message file), and `--oss` lets it talk to a local
Ollama, so no separate runner was written. The relay offers the Gemma 4 models
from `marketplace/packages/gemma-4` (`ORG_AGENT_MODEL_ALIASES["local-llama"]`,
alias = model id), and the node's `agentWebsiteModels` accepts the same ids.
The floating composer (`app/public/dashboard-composer.js`) lists the Gemma
models in its Model popup (`/agent gemma`), and the new-agent modal and Agents tab get a
"Gemma (local)" provider. The iOS composer's model menu lists the same Gemma
4 models (`AgentModelCatalog.nodeModels`, provider `local-llama`) whenever
the relay offers them. Picking one sends provider `local-llama` and moves the
run to the first online node that advertises it, the same way a Codex pick
does; with no such node the usual fallback to this iPhone still applies. Runner pools do not advertise Gemma: they are shared sandboxes with no local
Ollama. There is no login step, so the web composer hides its node sign-in
button for Gemma. The issue composer's "Assign to agent" passes `local-llama`
through to the owner's desktop, which runs it with the Gemma package.

There is no model safety preflight (removed 2026-09-13, a7dfa7936); do not
add one back. `agentOwner` in `mirror-node.json` is parsed and ignored. The
lease's `securityCheck` object is no longer declared in `agentJob`
(2026-10-02): nothing read it, and `json.Unmarshal` skips the field, so
relays that still send it keep working.

## What the agent process gets

Until 2026-10-02 a mirror ran `claude`/`codex` with the daemon's whole
environment and `HOME`, as the node's service user, next to the node key: every
`FORKMESH_*` variable, the operator's tokens, `CLAUDE_CONFIG_DIR`, and a `HOME`
whose `.claude/settings.json` and shell files the agent could rewrite for the
next job. Now `newMirrorAgentJobs` (`server/sandbox.go`) runs the provider CLI
through the same `jobSandbox` the runner uses:

- **Environment.** Only the runner's allowlist (`jobEnvironmentAllowed`: `PATH`,
  `LANG`, `LANGUAGE`, `TZ`, `TERM`, `LC_*`, proxy variables and the provider keys
  `ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN`, `ANTHROPIC_BASE_URL`,
  `CLAUDE_CODE_OAUTH_TOKEN`, `OPENAI_API_KEY`, `OPENAI_BASE_URL`) plus, for
  mirrors, `OLLAMA_HOST`, `CODEX_OSS_BASE_URL`, `CODEX_OSS_PORT`,
  `SSL_CERT_FILE`, `SSL_CERT_DIR` and `NODE_EXTRA_CA_CERTS`
  (`mirrorJobEnvironmentAllowed`). No `FORKMESH_*`, `CLAUDE_CONFIG_DIR`,
  `CODEX_HOME` or `XDG_*` of the daemon reaches the agent, except
  `XDG_RUNTIME_DIR` and `DBUS_SESSION_BUS_ADDRESS` in scope mode, which
  `systemd-run --user` needs. `USER`, `LOGNAME` and `GIT_TERMINAL_PROMPT=0` are
  set.
- **HOME.** `HOME` is `agent-jobs/work/.homes/<hash>`, one per session (so
  `--resume` still finds its conversation), with `TMPDIR` and
  `CLAUDE_CODE_TMPDIR` at `<home>/tmp` (new for every job) and
  `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME`
  under it. Before the run only `.claude/.credentials.json` and
  `.codex/auth.json` are copied in, from the daemon's `HOME`, or from
  `CLAUDE_CONFIG_DIR` / `CODEX_HOME` when the daemon has them; after the run a
  refreshed login is copied back and the copies and `tmp` are deleted
  (`openSession`/`closeSession`, `server/jobhome.go`). The operator's
  `~/.claude/settings.json`, `~/.claude.json` and `~/.codex/config.toml` no
  longer apply to website jobs; a Gemma setup that needs a non-default Ollama
  address uses `OLLAMA_HOST` or `CODEX_OSS_BASE_URL`. The first follow-up of a
  session started before this change cannot `--resume` (the conversation is in
  the old `HOME`) and starts fresh on the branch, as any lost conversation does.
- **Sandbox.** When the daemon is not root and its systemd user manager is
  reachable (`XDG_RUNTIME_DIR/systemd/private`), each CLI runs in a
  `systemd-run --user --scope` unit `fmnext-job-<random>`, stopped when the CLI
  exits, so nothing the agent started in the background outlives the job. The
  scope sets no `MemoryMax`, `CPUQuota` or `TasksMax`: the runner's limits are
  sized for small runner hosts and would kill builds mirrors run today. Without
  a user manager, or when the daemon is root, the CLI runs directly with the
  same environment and `HOME`. The daemon logs which one it uses at startup.
- **Key folder.** A job is refused when `agent-jobs/work` is the node key's
  folder, inside it, or contains it (`agentWorkOutsideNodeKey`); the bootstrap
  layout (`identity/` beside `mirror-gateway/`) never is.

What this does not change: the agent still runs as the node's own user, so it
can open the node key, `mirror-node.json`, the connector token or
`/proc/<daemon>/environ` by path. A mirror cannot switch to the `forkmesh-job`
user the way a runner does: its session clones share objects with the served
repositories (`clone --shared`) and the daemon runs git in them to publish,
which the runner only does through a job-made bundle. Owners whose agents you
do not trust belong on a runner ([../RUNNERS.md](../RUNNERS.md)). Tests:
`TestMirrorAgentRunsWithAnAllowlistedEnvironmentAndPrivateHome`,
`TestMirrorJobSandboxPicksUserScopeOrRunsDirectly` and
`TestAgentWorkMustNotShareTheNodeKeyFolder` in `server/sandbox_test.go`.

## Error reporting

`claudeRunError` / `codexRunError` (`server/agent_errors.go`) produce one
`lastError`, capped at 600 runes (`agentErrorLimit`):

1. the Claude result event's `errors` list or `result` text;
2. an assistant message flagged as an API error (`isApiErrorMessage`, an
   `error` field, or text starting "API Error");
3. the last non-empty lines of stderr;
4. "Claude Code exited with status N".

The stream-json init event is never quoted. The error is not appended to the
transcript; clients show `lastError` beside it. Desktop rows from older nodes
that still carry the appended trailer have it stripped
(`desktop/src/AgentCloudSessions.cpp`).

**Outdated CLI.** "Claude Code X does not support this model; version Y or
newer is required" (or Codex's "requires a newer version of Codex") becomes
"Claude Code on <node> is X; <model> needs Y or newer. Update the host (Hosts →
Update)." `agentCLIFloor` remembers the refusal per provider and model in
memory; later runs for that model check `--version` first and fail at once
until the CLI moves past it. It is not persisted, so a daemon restart forgets
it.

## Keeping the CLIs current

The CLIs live in `/usr/local/bin`, owned by root; the service (`forkmesh-node`)
cannot replace them. The desktop's host update pipelines append
`agentCliServiceRefreshScript()` (`desktop/src/ControlNode.cpp`) after a
successful update: for each CLI already in `/usr/local/bin`, run the official
installer as root, `install` the result to `/usr/local/bin/<name>.new`, then
`mv -f` it over the old copy so running jobs keep their binary. Failures print
a line and leave the old copy; the command still exits with the update's status.
The first install uses the same `.new` + rename step
(`agentCliServicePublishScript()`).

## Sign-in terminal

`server/terminal.go` runs exactly `claude auth login` or `codex login` on a
pseudo-terminal, one session at a time, at most 30 minutes, with frames relayed
over the node socket to the owner's browser. `disableAgentLogin` refuses it.

## The session clone

`<hash>` is the first 12 bytes of the session id's SHA-256, so every job of a
session uses the same folder. Until 2026-09-29 each job deleted it and cloned
again (`removeOwnCloneUnder`), which lost any commit or edit the previous job
had not pushed: a run that failed before `publish`, or a failed push. Now:

- **Missing or empty folder:** `git clone --shared --no-checkout` from the
  served repository, then `checkout -B <branch> <start>`, as before.
- **A usable clone** (its own `.git` folder, and `rev-parse --show-toplevel`
  is the folder itself): it is kept. `fetch --prune origin`, then the old
  state is saved before anything moves:
  - `HEAD` with commits on no remote-tracking ref goes to
    `refs/forkmesh/trash/agent/<hash>/<unix-seconds>`;
  - a local `<branch>` that differs from `HEAD` and has unpushed commits goes
    to `<unix-seconds>-branch`;
  - a dirty index or working tree (tracked edits and untracked files, not
    ignored ones) becomes one commit built with a temporary index in `.git`
    (`GIT_INDEX_FILE`: `read-tree HEAD`, `add -A`, `write-tree`,
    `commit-tree -p HEAD`), stored as `<unix-seconds>-wip`. The shared stash
    and the clone's own index are never touched.

  Each ref is written with `update-ref <ref> <commit> ""` (must not exist yet)
  and read back. Only then `checkout --force -B <branch> <start>` and
  `clean -fd` run (ignored files such as `node_modules` stay). If the fetch or
  any save step fails, the job fails ("could not save the unpushed work in
  agent workspace ..., so it was left as it was and the job did not run") and
  nothing in the folder changes.
- **A broken folder** (no `.git` folder, a `.git` file, or git cannot read
  it): renamed to `<hash>.replaced-YYYYmmdd-HHMMSS` (`moveAside` in
  `server/safedelete.go`, never deleted), then cloned fresh. The node log says
  where it went.

Trash refs are bounded to the newest 20 per session (`agentTrashKeep`,
`pruneAgentTrash`): after a save, older ones are removed with
`update-ref -d`. Nothing else in the Go daemon prunes trash refs; the
desktop's `refs/forkmesh/trash/sync/` and `trash/tasks/` refs are C++ and
never pruned automatically. A same-second collision takes `<seconds>-2`,
`-3`, and so on.

To recover work on a node:

```
git -C <dir>/work/<hash> for-each-ref refs/forkmesh/trash/agent/
git -C <dir>/work/<hash> branch rescued refs/forkmesh/trash/agent/<hash>/<ts>-wip
```

A `-wip` commit sits on top of the old `HEAD`; `git reset --soft HEAD~1` on a
branch made from it turns it back into uncommitted changes.

The runner tier (`server/runner.go`) still uses a fresh folder per job and
removes it afterwards: it publishes through a bundle before release, and its
folder lives in a per-job sandbox.

Tests: `server/agent_workspace_test.go` (unpushed commit kept, dirty tree
kept, broken clone moved aside with its files, a failed save resets nothing,
newest 20 trash refs kept).

## Traps

- The mirror-35-38 outage of 2026-09-24: a CLI frozen at 2.1.248 was refused
  for `claude-opus-5-5`, and the old error path quoted the stream-json init
  event. Both are fixed; if the model allowlist gains a model, the fleet's CLIs
  may need a host update first.
- The model allowlist is duplicated: `agentWebsiteModels` in `server/agents.go`
  must match `allowedWebsiteModels` in `desktop/src/MainWindowAgentSync.cpp`.

## Related

- [Agents across desktop, web and phone](../agents/README.md)
- [server/README.md](README.md)
