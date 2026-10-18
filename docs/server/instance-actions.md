# Actions on a self-hosted instance

An instance (installed with `www/public/install-instance.sh`) runs its
repositories' workflows on the instance server itself, so maintainers can
push, merge, run CI, run scheduled jobs and deploy from the website alone.
It builds on the runner Actions path ([runner-actions.md](runner-actions.md)):
one runner daemon is installed next to the core and leases every Actions job.

What this adds over runner Actions:

| Feature | Runner Actions | Instance Actions |
| --- | --- | --- |
| Triggers | push to the default branch | push (any branch, `paths`), `schedule`, `workflow_dispatch` with inputs, `issues`, `issue_comment` |
| Desktop check | queues only while the owner's desktop is offline | always queues (`ACTIONS_INSTANCE_RUNNER` set) |
| Step options | `run`, workflow `env` | plus job/step `env`, `working-directory`, `if`, `timeout-minutes` |
| Secrets | none | repository and organization secrets, masked in logs |
| Push back | never | fast-forward push to the triggering branch with an `actions-push` grant |
| Job API | none | read the event, comment on, label, open and close issues |
| Logs | 16 KiB tail | full log up to 1 MiB, reported every 5 seconds |
| Web control | none | Run workflow (with inputs), cancel, re-run |

## Workflow files

Workflows stay in `.forkmesh/<name>.yml` (`server/actions_workflow.go`).
Supported keys, in GitHub Actions spelling:

```yaml
name: Refresh data
on:
  push:
    branches: [main]
    paths: ["bolt-jobs/**"]
  schedule:
    - cron: "17 3 * * *"
  workflow_dispatch:
    inputs:
      project:
        description: Project to deploy
        required: true
        default: all
        type: choice          # string | choice | boolean
        options: [all, bolt-c]
  issues:
    types: [opened, labeled, edited]
  issue_comment:
    types: [created]
permissions:
  contents: write            # ask for the push grant
env:
  TZ: UTC
jobs:
  refresh:
    runs-on: runner
    timeout-minutes: 30
    if: github.event_name != 'issues' || contains(github.event.issue.labels.*.name, 'hackathon')
    defaults:
      run:
        working-directory: bolt-jobs
    env:
      NODE_ENV: production
    steps:
      - name: Build
        run: npm ci && npm run build
      - name: Deploy
        if: github.ref == 'refs/heads/main'
        working-directory: bolt-jobs/worker
        env:
          CLOUDFLARE_API_TOKEN: ${{ secrets.CLOUDFLARE_API_TOKEN }}
        run: npx wrangler deploy
```

- Steps of every job are still flattened into one ordered list; `needs`
  orders whole workflows as before. `uses:` steps are ignored (the workspace
  is already a checkout; install toolchains with `run:`).
- `working-directory` is relative to the checkout and may not be absolute
  or contain `..`. Step values override job values; job values override
  workflow values.
- Cron is 5-field UTC (`*`, lists, ranges, steps, `@hourly`, `@daily`,
  `@weekly`, `@monthly`). Scheduled runs use the default branch tip. The
  smallest interval is 5 minutes.
- Expressions (`${{ }}`, `if:`) support `==`, `!=`, `&&`, `||`, `!`,
  parentheses, string and number literals, `contains()`, `startsWith()`,
  `endsWith()`, `success()`, `failure()`, `always()`, and the contexts
  `github.event_name`, `github.ref`, `github.ref_name`, `github.sha`,
  `github.repository`, `github.actor`, `github.event.*` (`.*.name` maps over
  arrays), `inputs.*`, `secrets.*` and `env.*`. An unknown context is the
  empty string.
- In `run:` an expression becomes a reference to an environment variable
  holding its value (`"$FM_EXPR_1"`), so issue titles and inputs can never
  inject shell. `${{ secrets.X }}` becomes `"$X"` with `X` exported (trusted
  jobs; otherwise it is an empty `FM_EXPR_n`). Inside a double-quoted string
  the reference is written `${FM_EXPR_1}` and inside a single-quoted one
  `'"$FM_EXPR_1"'`, so the value lands where GitHub would put it; the runner
  tracks quotes and `#` comments only, not heredocs. Numbering restarts at 1
  for every step.
- `if:` follows GitHub: without `success()`, `failure()`, `always()` or
  `cancelled()` it is `success() && (<if>)`; an empty `if:` is `success()`.
  A failed step makes later steps skip unless their `if:` asks for
  `failure()` or `always()`; the run is then `failed`. A job's `if:` is
  evaluated before its first step; a false one logs `Skipped: <step>` for
  each of its steps. An unparsable expression fails the step.
- `on.push.branches` absent means every branch (GitHub's meaning; the
  desktop and mirror runners still follow only the default branch).
  `on.push.paths` is applied to `git diff --name-only <before> <commit>`;
  with no usable `before` (empty or all zeros) every path counts as changed.
- `timeout-minutes` on a job sets the workflow's limit (the largest job value,
  capped at 90 minutes; 60 by default); on a step it limits that step.

## Job environment

Every step gets the runner Actions environment plus:

| Variable | Value |
| --- | --- |
| `FORKMESH_BEFORE` | the push's previous commit (`before`), empty when unknown |
| `FORKMESH_EVENT`, `GITHUB_EVENT_NAME` | `push`, `schedule`, `workflow_dispatch`, `issues`, `issue_comment` |
| `FORKMESH_EVENT_PATH`, `GITHUB_EVENT_PATH` | JSON event payload file |
| `FORKMESH_TOKEN`, `GITHUB_TOKEN` | job token for the job API (trusted jobs only) |
| `FORKMESH_API_URL` | instance origin, e.g. `https://git.example.org` |
| `GITHUB_REPOSITORY` | `<owner>/<repo>` |
| `GITHUB_SHA`, `GITHUB_REF`, `GITHUB_REF_NAME` | commit and ref |
| `GITHUB_WORKSPACE` | the checkout |
| `GITHUB_ACTOR` | the pusher, dispatcher or issue author |
| `INPUT_<NAME>` | each `workflow_dispatch` input |
| secrets | each repository/organization secret by name (trusted jobs only) |

Payloads (`FORKMESH_EVENT_PATH`) follow GitHub's shapes for the fields used:

- push: `{ref, before, after, repository:{full_name}, pusher:{name}}`
- schedule: `{schedule: "<cron>"}`
- workflow_dispatch: `{inputs:{...}, ref, sender:{login}}`
- issues: `{action, issue:{number, title, body, state, labels:[{name}], user:{login}}, label:{name}, sender:{login}}`
- issue_comment: `{action, issue:{...}, comment:{id, body, user:{login}}, sender:{login}}`

### Checkout and pushing back

Every run gets a real clone (`git clone --no-hardlinks` from the runner's
repository cache, full history and every branch as `origin/*`), with the
job's branch checked out at the job's commit (`git checkout -B <branch>
<commit>`), so `git diff $FORKMESH_BEFORE $GITHUB_SHA` works. `origin` is the
lease's runner clone URL, with no credential, and `user.name`/`user.email`
are the Actions bot when the lease names one. The clone is owned by the job
user.

Only a trusted job whose workflow sets `permissions: contents: write` (or
`write-all`, at the workflow or a job) and whose lease carries an
`actions-push` grant for exactly `refs/heads/<branch>` (this runner, owner and
repository) gets the credential: `http.extraHeader` is
`X-ForkMesh-Runner-Grant: <grant>` (the header agent-branch pushes use), and
`branch.<branch>.remote`/`.merge` point at `origin`. A grant that does not
match is left out silently and the run continues without it.
`git push origin HEAD:<branch>` is accepted when it fast-forwards that branch.
A push made with an Actions grant does not trigger `push` workflows again.

The runner masks the job token and the push grant as well as the secrets.

### Which commit runs

A `push` to the default branch runs only a commit that is an ancestor of the
branch tip (as runner Actions always did). A `push` to another branch runs
the pushed commit when the cache has it, even after a force-push. Other
events run `commit` when the lease names one the cache has, else the branch
tip. `issues` and `issue_comment` runs need the payload's `action` in the
workflow's `types` (no `types` means every action).

## Job API

`Authorization: Bearer $FORKMESH_TOKEN`. The token is random, stored as a
SHA-256 hash on the job row, valid while the job is leased or running, and
reaches only that job's repository. Responses are JSON.

| Method and path | Body | Does |
| --- | --- | --- |
| `GET /api/actions/job/event` | | the job's event payload |
| `GET /api/actions/job/issues/<n>` | | `{number, title, body, state, labels, author}` |
| `POST /api/actions/job/issues` | `{title, body, labels}` | opens an issue, returns `{number}` |
| `POST /api/actions/job/issues/<n>/comments` | `{body}` | comments |
| `POST /api/actions/job/issues/<n>/labels` | `{add: [], remove: []}` | changes labels |
| `POST /api/actions/job/issues/<n>/state` | `{state: "open" \| "closed"}` | opens or closes |

Issue changes are signed by the instance's Actions bot account
(`<owner>-actions`, an organization writer created at install). Issue events
the bot makes do not trigger workflows.

`tools/forkmesh-actions.sh` wraps these calls for workflows
(`forkmesh-actions comment 12 "Thanks!"`).

## Secrets

`GET/PUT/DELETE /api/repo/<owner>/<repo>/actions/secrets[/<NAME>]` and
`/api/org/<org>/actions/secrets[/<NAME>]`, for the repository owner or an
organization admin. Names match `^[A-Z_][A-Z0-9_]{0,63}$`, values are up to
48 KiB, encrypted with the `DATA_KEY` envelope (`encrypt_row`), and listed by
name and update time only. Every change is audited. Repository secrets win
over organization secrets with the same name. The repository Settings page
has an Actions secrets section.

Secrets, the job token and the push grant go only to **trusted** jobs: every
event except a `pull_request` from someone without write access, on a runner
row with `actions_trusted=1`. The runner masks every secret value of four or
more bytes as `***` line by line, and the app masks again on report.

## Queue, lease, report

Migration `0272_actions_instance.sql` adds to `repo_action_jobs`: `event`
(default `push`), `workflow` (one path for non-push events, empty for push),
`ref`, `actor`, `before_oid`, `payload` (sealed `{event, inputs}`),
`token_hash`, `trusted`, `pushed_commit`; to `runner_nodes`:
`actions_trusted`; and the tables `repo_action_workflows` (catalog:
`owner, repo, path, name, events, schedules, inputs, commit_oid, last_slot,
updated_at`), `repo_action_secrets` (`owner, repo` (empty for an
organization), `name, value, updated_by, updated_at`) and `repo_action_logs`
(`job_id, run_index, data, updated_at`).

- Push jobs keep the per-commit dedupe and queued coalescing; every other
  event inserts its own row. At most one leased or running job per
  `(owner, repo, workflow)`.
- The lease adds to the job object: `event`, `workflow`, `before`,
  `eventPayload`, `inputs`, `secrets`, `pushGrant`, `pushRef`, `jobToken`,
  `apiUrl`, `trusted`, `botName`, `botEmail`.
- The report adds `catalog` (`[{path, name, events, schedules, inputs}]`,
  every workflow at the commit that parses, at most 64; each input is
  `{name, description, required, default, type, options}`; stored in
  `repo_action_workflows`) and per run `event` and `log` (full log, 1 MiB
  cap: the first 768 KiB and the last 256 KiB with an omitted-bytes line
  between; stored in `repo_action_logs`). `logTail` stays the last 16 KiB.
  The runner reports every 5 seconds while a workflow runs; those progress
  reports are sent once, without retries.
- `snapshot` returns `controlAvailable: true` and each run's `event`.
  `POST /api/repo/<owner>/<repo>/actions/control` with
  `{action: "dispatch", workflow, ref, inputs}`, `{action: "cancel", runId}`
  or `{action: "rerun", runId}` is handled locally for runner snapshots.
  `GET /api/repo/<owner>/<repo>/actions/runs/<id>/log` returns the full log.

Triggers:

| Event | Where |
| --- | --- |
| push | `core_push.record_accepted_push` for every branch, with `before` |
| schedule | cron phase `actions-schedule` (every minute, `app/src/actions_cron.py`) over `repo_action_workflows`, deduped by `last_slot` |
| workflow_dispatch | the control endpoint (owner or organization writer) |
| issues, issue_comment | after an issue event is accepted (`open`, `labels`, `edit`, `title`, `status`, `comment`) |

## Install

`install-instance.sh` installs a runner on the instance by default (setting
`actions`, default `true`): it runs `install-runner.sh --preregistered` with
the instance's daemon binary, registers it with
`python -m forkmesh_host instance-runner` (`scope=pool`,
`allowed_owners=[<owner>]`, `actions_trusted=1`), sets
`RUNNER_POOL_TARGET_NODES=<node>` and `ACTIONS_INSTANCE_RUNNER=<runner>`,
generates `ACTIONS_BOT_SEED`, and installs git, curl, jq, python3-venv,
Node.js and build tools for jobs. Jobs run as `forkmesh-job` in a systemd
scope (memory, CPU and task limits) and cannot reach loopback or private
addresses, so they talk to the instance only through its public origin.

## Tests

`server/actions_workflow_test.go`, `server/actions_expr_test.go`,
`server/runner_actions_test.go`, `server/runnergrant_test.go`,
`app/tests/test_instance_actions.py`, `app/tests/test_actions_cron.py`,
`app/tests/test_actions_job_api.py`, `app/tests/test_actions_secrets_api.py`,
`app/tests/test_mirror_gateway_runner_grants.py`,
`app/tests/test_install_instance_script.py`.
