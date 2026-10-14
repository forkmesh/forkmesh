# Agent jobs on mirror nodes

How a mirror runs the Claude Code, Codex and Gemma sessions the website (or a
desktop's **To cloud**) queues for it. Operator view:
[mirror-node.md, "Running website agent jobs"](../../www/docs/operations/mirror-node.md#running-website-agent-jobs).
Code: `server/agents.go`, `agent_errors.go`, `agent_attachments.go`,
`agent_live.go`, `agent_workspace.go`, `sandbox.go`, `jobhome.go`,
`terminal.go`.

## Lifecycle

1. **Advertise.** `CatalogPublisher.agentProviders()` lists `claude`/`codex`
   when found on `PATH`, plus `local-llama` (Gemma) when `codex` and `ollama`
   are, unless `disableAgentJobs` is set.
2. **Wake.** The relay pushes an `org-agents` event on the node socket; a
   5-minute safety read (`agentJobSafetyCheck`) covers a lost push.
3. **Claim.** Up to 8 jobs per repository per read (`agentJobClaimsPerRepo`)
   over the node-signed `org-agent-jobs` lease.
4. **Validate.** `agentJobRunner.validate`: session id, prompt ≤ 8,000
   characters, provider in `agentWebsiteProviders`, model in
   `agentWebsiteModels`, kind `start`/`continue`/`steer`, valid attachments,
   owner/repository matching this node; otherwise "Malformed or unsafe job
   envelope."
5. **Prepare.** The per-session clone (below). `start` branches
   `agent/web-<job>-<slug>` off the default branch, or the job's own `agent/`
   branch off its `baseBranch`; `continue` waits up to 10 minutes for a moved
   branch (`agentHandoffBranchWait`); `steer` resumes the stored session.
6. **Attach images.** Up to 5 images of 5 MiB, fetched over the lease, checked
   and written to `.forkmesh/attachments/<uuid>.<ext>` (excluded from Git).
   Runners use `POST /api/runners/attachment` and refuse a symlinked folder.
7. **Run.** At most 2 runs at once (`agentMaxConcurrentRuns`), 2 hours each.
   Claude: `claude -p --output-format stream-json --verbose
   --dangerously-skip-permissions [--model M] [--resume S]`, prompt on stdin.
   Codex: `codex -a never [-m M] exec --sandbox danger-full-access -o <file> -`.
   Gemma: Codex with `exec --oss` against the node's Ollama (default model
   `gemma4:e4b`). A Claude follow-up whose conversation is gone is retried once
   without `--resume`. Live transcript reports go out at most every 2.5 s, or
   every 250 ms while watched
   ([../agents/README.md](../agents/README.md#live-stream-to-the-phone)).
8. **Publish and report.** Push the branch, open a node-signed pull only if
   asked, report with transcript, tokens and cost. State lives in
   `mirror-gateway/agent-jobs/state.json`, so a re-leased job is re-reported,
   not rerun; a run cut off by a restart is reported failed.

There is no model safety preflight; do not add one. `agentOwner` is parsed
and ignored. Gemma models come from `marketplace/packages/gemma-4`
(`ORG_AGENT_MODEL_ALIASES["local-llama"]`); runner pools do not offer Gemma.

## What the agent process gets

`newMirrorAgentJobs` (`server/sandbox.go`) runs the CLI through the runner's
`jobSandbox`:

- **Environment.** Only the allowlist (`jobEnvironmentAllowed`: `PATH`,
  locale, `TZ`, `TERM`, proxy variables and the provider keys) plus, on
  mirrors, `OLLAMA_HOST`, `CODEX_OSS_BASE_URL`, `CODEX_OSS_PORT` and CA
  variables (`mirrorJobEnvironmentAllowed`). No `FORKMESH_*` or daemon config
  directories reach the agent.
- **HOME.** `agent-jobs/work/.homes/<hash>`, one per session (so `--resume`
  works), with a fresh `tmp` per job and XDG folders under it. Only
  `.claude/.credentials.json` and `.codex/auth.json` are copied in, and a
  refreshed login is copied back afterwards (`openSession`/`closeSession`).
  Operator CLI settings do not apply to website jobs.
- **Sandbox.** With a reachable systemd user manager and a non-root daemon,
  each CLI runs in a `systemd-run --user --scope` unit `fmnext-job-<random>`,
  stopped when the CLI exits; otherwise directly. No resource limits are set
  on mirrors.
- **Key folder.** A job is refused when the work folder overlaps the node key's
  folder (`agentWorkOutsideNodeKey`).

The agent still runs as the node's user and can open the node key by path; a
mirror cannot switch users because session clones share objects with the
served repositories. Untrusted owners belong on a runner
([../RUNNERS.md](../RUNNERS.md)). Tests: `server/sandbox_test.go`.

## Error reporting

`claudeRunError` / `codexRunError` produce one `lastError` (at most 600 runes)
from, in order: the result event's errors, an API-error assistant message,
stderr's last non-empty lines, the exit status. The error is shown beside the
transcript, never appended to it.

**Outdated CLI.** "does not support this model; version Y or newer is
required" becomes "Claude Code on <node> is X; <model> needs Y or newer.
Update the host (Hosts → Update)." `agentCLIFloor` remembers the refusal per
provider and model in memory and fails later runs for it at once.

## Keeping the CLIs current

The CLIs live in `/usr/local/bin`, owned by root. The desktop's host update
appends `agentCliServiceRefreshScript()`: run the official installer, install
to `<name>.new`, then `mv -f` over the old copy so running jobs keep their
binary. Failures leave the old copy and never change the update's status.

## Sign-in terminal

`server/terminal.go` runs exactly `claude auth login` or `codex login` on a
pseudo-terminal, one session at a time, at most 30 minutes, relayed over the
node socket to the owner's browser. `disableAgentLogin` refuses it.

## The session clone

`work/<hash>` (first 12 bytes of the session id's SHA-256) is reused by every
job of a session, and unpushed work is never lost:

- **Missing or empty:** `git clone --shared --no-checkout` from the served
  repository, then `checkout -B <branch> <start>`.
- **Usable clone:** `fetch --prune origin`, then save before moving anything:
  unpushed `HEAD` commits to `refs/forkmesh/trash/agent/<hash>/<unix-seconds>`,
  a differing local branch to `<unix-seconds>-branch`, and a dirty tree as one
  commit built with a temporary index (`<unix-seconds>-wip`). Each ref is
  created with `update-ref <ref> <commit> ""` and read back; only then
  `checkout --force -B` and `clean -fd` run. If any save fails, the job fails
  and nothing changes.
- **Broken folder:** renamed to `<hash>.replaced-YYYYmmdd-HHMMSS`
  (`moveAside`), never deleted, then cloned fresh.

The newest 20 trash refs per session are kept (`agentTrashKeep`). Recover with
`git branch rescued refs/forkmesh/trash/agent/<hash>/<ts>-wip`. Runners use a
fresh folder per job instead. Tests: `server/agent_workspace_test.go`.

## Traps

- When the model allowlist gains a model, host CLIs may need an update first.
- `agentWebsiteModels` in `server/agents.go` must match
  `allowedWebsiteModels` in `desktop/src/MainWindowAgentSync.cpp`.

## Related

- [Agent runs](../agents/README.md), [server/README.md](README.md)
