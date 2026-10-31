# GitHub Actions: watching the mirror's checks

The GitHub copy of the repository (github.com/forkmesh/forkmesh) runs the
workflows in `.github/workflows/` on every push to `main`. GitHub's only signal
for a failure is an email, and job logs need a GitHub login, so the workflows
once stayed red for days unseen. Three pieces make failures visible: a watcher
that pings admins, a command-line check, and failures written as annotations
anyone can read. Home: [docs/README.md](../README.md).

## Code map

| Piece | Where |
| --- | --- |
| Watcher (cron phase `github-actions`, every 5 minutes at minute % 5 == 1) | `app/src/github_actions_watch.py`; wired in `_cron_due_phases` / `_run_cron_phase` (`app/src/entry.py`) |
| Tests | `app/tests/test_github_actions_watch.py`, `app/tests/test_cron_runner.py` |
| Command-line check | `tools/github_actions_status.py` |
| Gate annotations | `run_profile`, `github_annotation` in `tools/quality_gate.py`; `app/tests/test_quality_gates.py` |
| Playwright annotations | `github` reporter in `app/browser_tests/playwright.config.js` and `world/browser_tests/playwright.config.js` |

## Pings when a workflow breaks

The cron on the cores reads one page of GitHub's public runs API
(`/repos/<repo>/actions/runs?branch=main`, 100 runs) every 5 minutes. For each
workflow, the newest completed `push`, `schedule` or `workflow_dispatch` run
that passed or failed decides its state. Cancelled, skipped and neutral runs
are ignored, so a run cancelled by a newer push is not a recovery.

- **Hourly sweep.** 100 runs cover only the last few pushes, so a daily
  workflow or one whose recent runs were all cancelled would never appear.
  On the tick in the first 5 minutes of each hour (`sweep_due`) the watcher
  lists the active workflows (`/actions/workflows`) and reads 50 runs for each
  workflow missing from the page (`unseen_workflows`, at most 12). Between
  sweeps a workflow off the page keeps its stored state.
  `tools/github_actions_status.py` sweeps on every call.
- **State.** One row per workflow in `repository_monitor_state`, id
  `github-actions:<repo>:<workflow path>`, holding `is_up`, `changed_at` and
  `pinged_state`. The `status:` monitors share the table and never read these
  rows.
- **When it pings.** A workflow that turns red, or is red when first seen,
  files one `operational_alert` ping to every platform admin; turning green
  files one "passes again" ping; first seen green stays quiet. Dedupe key:
  `github-actions:<repo>:<path>:<up|down>:<changed_at>`. `pinged_state` moves
  only after a ping was filed, so a ping that could not be filed (no admin, or
  every admin muted Operational alerts) retries next tick under the same key
  and never doubles.
- **What the ping says.** The failed job and step, the commit, the first useful
  failure annotation (the runner's "Process completed with exit code" line is
  skipped), how many runs in a row failed ("at least" when the streak runs off
  the page), and a link to the run in the body. `href` stays `/status` because
  the phone apps open only relative links.
- **Rate limits.** Unauthenticated, a tick costs 1 request plus up to 3 per new
  failure (jobs and annotations); the hourly sweep adds 1 plus one per missing
  workflow (3 today). About 16 an hour against GitHub's limit of 60 per IP.

| Setting (app environment) | Default | Effect |
| --- | --- | --- |
| `GITHUB_ACTIONS_WATCH_REPO` | `forkmesh/forkmesh` | Repository to watch; `off` turns the watcher off (for self-hosts) |
| `GITHUB_ACTIONS_WATCH_BRANCH` | `main` | Branch whose runs count |
| `GITHUB_ACTIONS_WATCH_TOKEN` | unset | Optional read-only token, for a higher rate limit |

## Checking by hand

```sh
python3 tools/github_actions_status.py            # exit 1 while anything fails
python3 tools/github_actions_status.py --json
python3 tools/github_actions_status.py --repo owner/name --branch release
```

Same rules as the watcher; sends `GITHUB_TOKEN` or `GH_TOKEN` if set.

## Failures readable without a login

Check-run annotations (`/repos/<repo>/check-runs/<job id>/annotations`) need
no login, so both suites write their failures as annotations:

- **`tools/quality_gate.py run`** runs every gate in the profile instead of
  stopping at the first failure. Under `GITHUB_ACTIONS=true` each failed gate
  prints `::error title=Quality gate <id> failed::` and its last 40 output
  lines (at most 3,500 characters). The step still fails and lists every
  failed gate.
- **Playwright** adds the `github` reporter under `GITHUB_ACTIONS`, so each
  failing browser test becomes an annotation.

## Rules the CI runs taught

- **Load-sensitive Go tests.** `TestThreePeopleHoldACallThroughImpairedLinks`
  (`server/rtc/fmmserver/call_test.go`) checks 3 seconds of real-time media
  and fails ("heard only 7 of 150 audio frames") when the process is starved.
  It is in `GO_LOAD_SENSITIVE_TESTS` (`tools/run_critical_tests.py`), so the
  retry-once rule for timeouts ([docs/server/README.md](../server/README.md))
  covers it; a second failure still fails the step.
- **CI installs only `tools/requirements-ci.txt`.** The local machine and the
  ForkMesh Actions runner have more packages, so a test importing a
  third-party package can pass locally and fail only on GitHub. The file pins
  `cryptography` (with `cffi`, `pycparser`) and `httpx` (with `anyio`,
  `certifi`, `h11`, `httpcore`, `idna`, `typing_extensions`) at the
  `app/host/requirements.txt` versions; raise them in both files together, and
  refresh the Docs package's dependency snapshot
  (`marketplace/tools/docs_dependencies.py --offline`, then
  `docs_mobile_bundle.py`). To reproduce a CI run, install only that file in a
  fresh venv and run `python tools/quality_gate.py run --profile core`.
- **Terminology and claims gates read public copy.** "peer to peer" is a
  discouraged term on primary surfaces (`docs/terminology.json`);
  `app/tests/test_public_security_claims.py` checks that mirroring stays a
  desktop or headless-node job.
- **Third-party headers.** A vendored file with a license header naming an
  outside domain needs a `documentation` reference in the external dependency
  manifest, then a re-render of the published map.
- **The issue-sync workflow** skips with a notice when the
  `FORKMESH_ISSUE_SYNC_DESTINATION` repository variable is unset on a scheduled
  run; a manual run without it still fails. Set the variable and the tokens in
  [automation-and-cloudflare-bootstrap.md](../operations/automation-and-cloudflare-bootstrap.md)
  to turn it on.
- **World browser E2E** cancels in progress only on pull requests; on `main` a
  newer push waits, and GitHub keeps only the newest waiting run. The job limit
  is 40 minutes (the full App suite takes about 10). The mobile camera samples
  warm up with two drags under SwiftShader
  ([docs/world/README.md](../world/README.md)), and the workflow installs
  ffmpeg for `chat-view-once`'s fake camera clip.

## The browser-test server

The App browser specs run against a local Workers runtime:
`world/browser_tests/run-browser-worker.sh` serves `app/dist` with
`pywrangler dev` and `world/browser_tests/wrangler.browser-tests.toml`. This is
a test harness only; production runs the app on the cores
([docs/host/README.md](../host/README.md),
[architecture](../architecture.md)). Its traps:

- **Off-site redirects.** `build_site_assets.py cutover` copies
  `www/public/_redirects` into `app/dist`, and wrangler applies its 308s to
  `https://app.forkmesh.com` and `https://world.forkmesh.com`, which made specs
  test production instead of the build. Production never applies
  `_redirects`, so in full-suite mode the launcher drops every rule that leaves
  the test server.
- **Static folders.** `run_worker_first = "/*/*"` must exclude static folders
  (`fmm`, `dashboard-plugins`, `instance-themes` and the rest), or a module
  such as `/fmm/fmm-local.js` comes back as a repository page.
- **Keep the test config in step with the app.** `/signup` and `/signup/` run
  the app first (the one-use `contact_…` email field); `APP_ORIGIN` is
  `http://127.0.0.1:4179`, because the ingress lets a WebSocket upgrade through
  only when its origin equals `APP_ORIGIN` (otherwise 426 and "Disconnected");
  the `FORKMESH_NOTE_ROOM`, `FORKMESH_CRON_WATCHDOG`, `FORKMESH_CRON_RUNNER`,
  `FORKMESH_DISCORD_GATE` and `FORKMESH_ORG_OFFICE` bindings match the app
  (migration tag `browser-tests-v2`).
- **`www/public/_headers`.** `tools/cutover_rehost.py` restores `www.` only in
  quoted, comma-separated lists, not in space-separated policy lists, so both
  `/world` `frame-ancestors` rules name `https://www.forkmesh.com` by hand.

Run one spec with
`CI=1 FORKMESH_BROWSER_FULL=1 npx playwright test tests/<spec> --retries=0`
in `app/browser_tests`.

## Writing specs that hold

- **Catch a promise you await later.** A spec that starts
  `const framePromise = page.evaluate(...)` and awaits it after other steps
  must call `framePromise.catch(() => {})` at once. Otherwise, when an
  assertion fails first, Playwright closes the page, the promise rejects
  unobserved, and the runner counts "1 error was not a part of any test",
  which exits 1 even when every retry passed.
- **Poll for the final count.** `expect.poll` returns the first time it sees
  the count, so poll for the total after the last frame (for
  `direct-messages`: `RETAINED_KEY_ADVERTISEMENTS + 5`; the sealed key
  advertisement sent with a room's first message is retained so a reloaded
  page can read the history, and deletes are retained but do not notify).
- **Poll for desktop calls.** `showAgentLoginModal` draws the title before
  `createAgentLoginTerminal` starts the terminal, so specs poll for
  `terminal.start` with `expect.poll`.
- **Accounts confirm their email before they write.** Specs confirm through
  `/api/accounts/verify-email` with a link token built from the test
  `DATA_KEY`; the test server sends no mail.
- **Recorder stages.** In `chat-view-once`, the face's voice stage starts
  before the face stage (its worklet must load within `MODULE_TIMEOUT_MS`,
  6 seconds, in `chat-call-voice.js`; the face stage has 20 seconds), a failure
  names the stage that threw, and `openCapture`
  (`app/public/chat-view-once-recorder.js`) turns a failure from a stale
  generation into `superseded`, so only the current attempt changes the
  status. The harness reads the newest stage still running.

The page fixes behind past spec failures are in their subsystem pages, for
example [daily-interface-validation.md](daily-interface-validation.md),
[web-composer.md](../app/web-composer.md) and
[branch-ops.md](../app/branch-ops.md).

## Reproducing the browser suites locally

`daily-interface.spec.js` mocks every request with `page.route`, so it needs no
Workers runtime: use a scratch Playwright config that spreads
`app/browser_tests/playwright.config.js` and sets `baseURL` and `webServer` to
`python3 -m http.server <port>` in `app/dist`. Rebuild first
(`build_dashboard_assets.py`, then `build_site_assets.py cutover`). Port 4179
is often taken by another agent. Startup budgets miss under heavy load; CI's
single retry absorbs that.

The other suites need `pywrangler dev`. Where DNS does not resolve
`pyodide-capnp-bin.edgeworker.net` (the office network), the runtime cannot
start; read the failures from the GitHub run's annotations instead
(`tools/github_actions_status.py` prints the first one), or seed the Pyodide
bundle cache: set `MINIFLARE_WORKERD_PATH` to a wrapper that adds
`--pyodide-bundle-disk-cache-dir=<dir>` after `serve`, and download
`pyodide_<version>_<revision>_<backport>.capnp.bin` (for example
`pyodide_314.0.6_2026-08-17_8.capnp.bin`) from that host into the folder. The
versions and revisions are in the `workerd` binary's strings next to the
`pythonWorkers2026…` flag names; probe backport numbers with a one-byte range
request. Overriding `/etc/hosts` in a private mount namespace does not help:
`workerd`'s own connect still times out.

`build_site_assets.py cutover` deletes and rebuilds `app/dist`, and a running
`pywrangler dev` then answers 500 for every static file until it restarts. To
rebuild while it runs, serve a copy of `dist` (a scratch config whose
`[assets] directory` points at it) and `rsync -a --delete` the new build into
that copy.
