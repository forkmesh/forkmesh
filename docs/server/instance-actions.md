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

The grant is a runner grant (`forkmesh-runner-grant-v1`, same message layout
and 3-hour TTL cap as `push`) with access `actions-push` and ref
`refs/heads/<branch>`, where the branch matches
`^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$` and has no `..`, `//` or `@{` and does
not end in `/`, `.lock` or `.`. The gateway (`tools/mirror_gateway.py`,
`runner_branch_ref`, `enforce_runner_push_scope`) serves the receive-pack
advertisement and receive-pack for it like a `push` grant, but before git
runs it refuses the push unless it holds exactly one command for the granted
ref whose old id is the branch's current tip: creates, deletes, other refs and
stale bases are refused. Git's own `receive.denyNonFastForwards` then refuses
a new commit that does not descend from the old one (the pack is not in the
repository before git runs, so the gateway cannot check ancestry itself).

The gateway never calls the app. An accepted Actions push is reported the way
runner pushes are: the `hosted-pushes.json` sidecar for the refresh cycle (no
new field; that file's schema is closed) and the `X-ForkMesh-Ref-Updates`
response header, which for an `actions-push` grant carries
`"actions": true` next to `total` and `updates`; the gateway's access log
entry carries `"actions": true` too. The push reaches the runner's git, not
the app, so the app records it from the runner's report (`pushed_commit`)
and never sees it in `core_push.record_accepted_push`; any proxy that relays
the header must skip `push` triggers when `actions` is true.

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

App side (`app/src/actions_job_api.py`, routed for `/api/actions/job/*` in
`app_runtime.py`): the token's SHA-256 must match `token_hash` of a job in
`leased`, `running` or `cancelling`. The bot key is derived from
`ACTIONS_BOT_SEED` (64 hex characters, the raw Ed25519 seed; WebCrypto
imports it as PKCS#8 and exports the public half). Each change is a v2
issue event (`forkmesh-issue-event-v2\n<owner>/<repo>\n...`, owner being the
repository's node owner) with `authorName: <org>-actions`, handed to
`_accept_issue_submission` in `entry.py` — the same validation, inbox row and
side effects as a web submission (it is the former POST body of
`issues_handler`). The job token is the authority, so structural events
(labels, status) skip the web's session check. `labels` reads the issue's
current labels from the repository and sends the full new list. Reading an
issue goes through the repository's gateway (`blobs` of
`.forkmesh/issues/{open,closed}/<n>/issue-<n>.json`). A repository whose
`issues` feature is off refuses every write, like the web.

The bot is a `node`-kind account holding the derived public key, with the
organization role `maintainer` (repository permission `maintain`, which is at
least write). A user-owned instance (`ownerType: user`) gets the bot account
but no membership, and its bot name is `forkmesh-actions`.

`tools/forkmesh-actions.sh` wraps these calls for workflows
(`forkmesh-actions comment 12 "Thanks!"`); `install-instance.sh` installs it
as `/usr/local/bin/forkmesh-actions`. Subcommands: `event` (prints
`$FORKMESH_EVENT_PATH`, else `GET event`), `issue <n>`, `comment <n>
<body|->`, `label <n> --add a,b --remove c`, `close <n>`, `reopen <n>`,
`open-issue <title> <body|-> [labels]` (`-` reads standard input). It needs
curl and builds JSON with jq, or python3 when jq is missing
(`FORKMESH_ACTIONS_JSON=python` forces that path, for tests). The token is
passed to curl through a process-substitution header file, never on its
command line; a non-2xx answer prints the body to stderr and exits 1, bad
arguments exit 2. Test: `app/tests/test_forkmesh_actions_helper.py`.

## Secrets

`GET/PUT/DELETE /api/repo/<owner>/<repo>/actions/secrets[/<NAME>]` and
`/api/org/<org>/actions/secrets[/<NAME>]` (`app/src/actions_secrets.py`),
for the repository owner (or the account that owns its node) or an owner or
admin of an organization linked to the repository; organization secrets for
that organization's owners and admins. `GET` answers
`{ok, secrets: [{name, updatedAt, updatedBy}]}`, `PUT` takes `{value}` and
answers `{ok, name, updatedAt}`, `DELETE` takes no body. At most 100 secrets
per repository or organization. Names match `^[A-Z_][A-Z0-9_]{0,63}$`, values are up to
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

- Jobs are keyed by the repository's node owner (`forkmesh` on an
  instance), as pushes record them; the lease also accepts a runner whose
  `allowed_owners` lists the organization the node's repository is linked to,
  and that organization names the bot and `repository.full_name`.
- Push jobs keep the per-commit dedupe (also against `pushed_commit`, the
  loop guard) and queued coalescing, now per branch; every other event
  inserts its own row and must name a workflow (one path). At most one
  leased, running or cancelling job per `(owner, repo, workflow)`; push jobs
  share the empty workflow.
- A job with an empty `commit` (schedule, dispatch, issues) runs the branch
  tip; the runner reports the commit it ran.
- The lease adds to the job object: `event`, `workflow`, `before`,
  `eventPayload`, `inputs`, `secrets`, `pushGrant`, `pushRef`, `jobToken`,
  `apiUrl`, `trusted`, `botName`, `botEmail`, plus `ref` and `actor`.
  `secrets`, `jobToken`, `apiUrl`, `botName`, `botEmail`, `pushGrant` and
  `pushRef` are present only when `trusted` is true (the job row's `trusted`,
  the runner row's `actions_trusted`, and an event other than
  `pull_request`). A trusted job always gets the push grant; the runner uses
  it only when the workflow asks for `contents: write`. The grant lives as
  long as the lease (90 minutes). `apiUrl` is `PUBLIC_BASE_URL` (else
  `APP_ORIGIN`); `botEmail` is `<org>-actions@users.noreply.<host of
  apiUrl>`. The token is also kept in the sealed payload so the report can
  mask it.
- The report adds `catalog` (`[{path, name, events, schedules, inputs}]`,
  every workflow at the commit that parses, at most 64; each input is
  `{name, description, required, default, type, options}`; stored in
  `repo_action_workflows`) and per run `event` and `log` (full log, 1 MiB
  cap: the first 768 KiB and the last 256 KiB with an omitted-bytes line
  between; stored in `repo_action_logs`). `logTail` stays the last 16 KiB.
  The runner reports every 5 seconds while a workflow runs; those progress
  reports are sent once, without retries. `/api/runners/actions-report`
  accepts bodies up to 8 MiB (other runner routes keep 64 KiB). A run
  without `log` keeps its stored log. The app masks secret values (and each
  line of a multi-line one) of four or more bytes and the job token in `log`,
  `logTail` and `message`. Runs may carry `workflowPath`, used by re-run. The
  catalog is stored only from jobs on the default branch; it replaces the
  repository's catalog but keeps each workflow's `last_slot`. Schedules that
  do not parse or fire more often than every 5 minutes are dropped. An
  optional `pushedCommit` is stored in `pushed_commit`. A terminal report
  clears `token_hash`. The answer is `{ok: true, cancel}`: `cancel: true`
  means the job was cancelled from the web and the runner should stop it and
  send its final report (the job then ends `cancelled`).
- `snapshot` returns `controlAvailable: true` when `ACTIONS_INSTANCE_RUNNER`
  is set, each run's `event` and `workflows` (the catalog, inputs as a list
  of `{name, description, required, default, type, options}`).
  With `ACTIONS_INSTANCE_RUNNER` set,
  `POST /api/repo/<owner>/<repo>/actions/control` with
  `{action: "dispatch", workflow, ref, inputs}`, `{action: "cancel", runId}`
  or `{action: "rerun", runId}` is handled locally (`repository_actions.py`,
  for the owner or an organization writer; other keys such as
  `sessionToken` are ignored). `ref` is a branch name or `refs/heads/...`;
  inputs are strings or booleans, checked against the catalog (required,
  choice options, `true`/`false`; values up to 64 KiB). Success is
  `202 {ok, status: "applied", action, runId}`; errors are `{error, message}`
  with `input_required`, `input_invalid`, `workflow_not_found`, `not_manual`,
  `ref_not_found`, `run_not_found`, `run_active` (re-run of a run still
  queued or active) or `not_cancellable`. Dispatch needs the workflow in the
  catalog, so a workflow becomes dispatchable after the first job on the
  default branch has reported. Cancel marks a queued job `cancelled` and an
  active one `cancelling`; re-run copies the job (one workflow when the run
  names its `workflowPath`). Run ids are `job id × 100 + n` (n from 1; the
  placeholder of a job with no runs yet is `job id × 100`).
  `GET /api/repo/<owner>/<repo>/actions/runs/<id>/log` returns
  `{ok, runId, log, updatedAt}` with the same access as the runs list.

Web UI (`app/public/dashboard/js/06-repo-actions.js`): a runner snapshot
with `controlAvailable: true` (schema version 1 or 2) turns on the controls.
Selecting a workflow from `snapshot.workflows` (normalized from the catalog;
`inputs` may be a `{name: spec}` map or a list of `{name, ...}`) whose events
include `workflow_dispatch` shows Run workflow with a branch picker and one
field per input: `string` a text field, `choice` a select of `options`,
`boolean` a checkbox, with `default` filled in and `required` checked before
sending. The page posts `{action: "dispatch", workflow: <path>, ref: <bare
branch name>, inputs: {name: string | boolean}}`, `{action: "cancel",
runId}` and `{action: "rerun", runId}` without a `requestId` or `node` (those
belong to the mirror contract); a 2xx answer counts as done, an optional
`runId` in it opens that run, and an error body's `error` code is shown in
words (`run_active`, `input_required`, `input_invalid`, `workflow_not_found`
and others). Each run shows its `event` as a badge. Opening a run loads
`GET .../actions/runs/<id>/log`, which must answer JSON `{ok: true, log:
"<text>"}`; while it loads, or if it fails, the run's `logTail` is shown. A
live run's log is re-read on each poll. The repository Settings tab has an
Actions secrets card and the organization page (owners and admins) an
Actions secrets section; both expect `GET` to answer `{secrets: [{name,
updatedAt}]}` (milliseconds) and send `PUT .../<NAME>` with `{value}` and
`DELETE .../<NAME>`; the value field is cleared after every attempt. Like every
dashboard call through `orgAgentRequest`, JSON bodies also carry
`sessionToken` when the session is a bearer token; the endpoints must ignore
it.

Triggers:

| Event | Where |
| --- | --- |
| push | `core_push.record_accepted_push` for every branch, with `before`, whether or not a desktop is online, when `ACTIONS_INSTANCE_RUNNER` is set (else the old default-branch, desktop-offline rule) |
| schedule | cron phase `actions-schedule` (every minute, `app/src/actions_cron.py`) over `repo_action_workflows`, deduped by `last_slot` (a compare-and-set claim, and at least 5 minutes between two runs of one workflow); queued on the default branch with an empty commit and payload `{schedule}` |
| workflow_dispatch | the control endpoint (owner or organization writer) |
| issues, issue_comment | after an issue event is accepted (`open`→`opened`, `labels`→`labeled`/`unlabeled`, `edit`/`title`→`edited`, `status`→`closed`/`reopened`, `comment`→`issue_comment` `created`), one job per catalog workflow listing the event, on the default branch; `types` are filtered by the runner |

Actions-grant pushes go from the runner to the gateway directly and never
reach `record_accepted_push`, so they cannot queue `push` jobs. As a second
guard, a push report whose `X-ForkMesh-Ref-Updates` document has
`"actions": true` queues nothing, and a commit a job reported as
`pushedCommit` is never queued as a push job. The app learns the new tip the
way it learns agent-branch pushes: from the gateway's own hosted-push record
and refresh cycle.

## Install

`install-instance.sh` installs a runner on the instance by default (setting
`actions`, default `true`): it runs `install-runner.sh --preregistered` with
the instance's daemon binary, registers it with
`python -m forkmesh_host instance-runner --name <runner> --public-key <key>
--owner <owner>` (idempotent upsert of the `runner_nodes` row:
`scope=pool`, `allowed_owners=[<owner>]`, `registered_by=<owner>`,
`status=active`, `actions_trusted=1`; it takes the same `--env-file`,
`--db` and `--data-dir` options as `instance-bootstrap`), sets
`RUNNER_POOL_TARGET_NODES=<node>` and `ACTIONS_INSTANCE_RUNNER=<runner>`,
generates `ACTIONS_BOT_SEED` (which `instance-bootstrap` reads from its
environment or the app env to create the bot), and installs git, curl, jq, python3-venv,
Node.js and build tools for jobs. Jobs run as `forkmesh-job` in a systemd
scope (memory, CPU and task limits) and cannot reach loopback or private
addresses, so they talk to the instance only through its public origin.

Installer details: the job packages are added to the `packages` step only
when `actions` is on; `ACTIONS_BOT_SEED` is `openssl rand -hex 32`, kept
across runs; the runner is `<node>-runner` (`forkmesh-runner`) under
`/etc/forkmesh-runner/accounts/<owner>` and
`/var/lib/forkmesh-runner/accounts/<owner>`, unit
`forkmesh-runner@<owner>.service`. `install-runner.sh --preregistered`
prints `FM_RUNNER_PUBLIC_KEY=<key>` (base64url, no padding, of the raw 32-byte
Ed25519 public key, 43 characters, the same form `runner_nodes.public_key`
holds) and the installer passes that value to `instance-runner`, run through
the same `host_cli` wrapper as `instance-bootstrap` (so it also receives
`--env-file`, `--db` and `--data-dir`). The runner step runs inside the `repo`
step (the launch page has a fixed step list) and only warns on failure.
`--update` keeps `actions` from `instance.json` (absent counts as `true`).

## Tests

`server/actions_workflow_test.go`, `server/actions_expr_test.go`,
`server/runner_actions_test.go`, `server/runnergrant_test.go`,
`app/tests/test_instance_actions.py`, `app/tests/test_actions_cron.py`,
`app/tests/test_core_push.py`, `app/host/tests_unit/test_instance_bootstrap.py`,
`app/tests/test_actions_job_api.py`, `app/tests/test_actions_secrets_api.py`,
`app/tests/test_mirror_gateway_runner_grants.py`,
`app/tests/test_install_instance_script.py`,
`app/tests/test_install_runner_script.py`,
`app/tests/test_repo_actions_tab_frontend.py`,
`app/tests/test_forkmesh_actions_helper.py`.
