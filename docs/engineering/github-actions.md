# GitHub Actions: watching the mirror's checks

The GitHub copy of the repository (github.com/forkmesh/forkmesh) runs the
workflows in `.github/workflows/` on every push to `main`. From 2026-10-21 to
2026-10-23, "ForkMesh quality gates" and "ForkMesh World browser E2E" failed
on every push and nobody saw it: the only signal was GitHub's "Some jobs were
not successful" email, and the logs need a GitHub login. This page covers the
three changes that make these failures visible.

## Code map

| Piece | Where |
| --- | --- |
| Watcher (cron phase `github-actions`, every 5 minutes at minute % 5 == 1) | `app/src/github_actions_watch.py`; wired in `_cron_due_phases` / `_run_cron_phase` (`app/src/entry.py`) |
| Tests | `app/tests/test_github_actions_watch.py`, `app/tests/test_cron_runner.py` |
| Command-line check | `tools/github_actions_status.py` |
| Gate annotations | `run_profile`, `github_annotation` in `tools/quality_gate.py`; `app/tests/test_quality_gates.py` |
| Playwright annotations | `github` reporter in `app/browser_tests/playwright.config.js` and `world/browser_tests/playwright.config.js` |

## Pings when a workflow breaks

The relay's cron reads one page of GitHub's public runs API
(`/repos/<repo>/actions/runs?branch=main`, 100 runs) every 5 minutes. For each
workflow, the newest completed `push`, `schedule` or `workflow_dispatch` run
that passed or failed decides its state. Cancelled, skipped and neutral runs
are ignored, so a run cancelled by a newer push does not count as a recovery.

- **Where the state lives.** Each workflow has a row in
  `repository_monitor_state` with id `github-actions:<repo>:<workflow path>`,
  holding `is_up`, `changed_at` and `pinged_state`. The `status:` monitors use
  the same table, and their queries never read these rows.
- **When it pings.** A workflow that turns red, or is red the first time it is
  seen, files one `operational_alert` ping to every platform admin. Turning
  green again files one "passes again" ping. A workflow seen green for the
  first time stays quiet. The dedupe key is
  `github-actions:<repo>:<path>:<up|down>:<changed_at>`, and `pinged_state`
  moves only after a ping was filed. A ping that could not be filed (no admin,
  or every admin muted Operational alerts) is tried again on the next tick
  under the same key, so it never doubles.
- **What the ping says.** The failed job and step, the commit, and the first
  useful failure annotation of the failed jobs (the runner's own
  "Process completed with exit code" line is skipped). It also counts how many
  runs in a row failed, saying "at least" when the streak runs off the
  fetched page, and links the run on GitHub. The link is in the body:
  `href` stays `/status`, because the phone apps open only relative links.
- **Rate limits.** With no token, one tick costs 1 request, plus up to 3 for
  each new failure (jobs and annotations). That is about 12 an hour against
  GitHub's unauthenticated limit of 60 an hour per IP.

| Setting (Worker env) | Default | Effect |
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

It uses the same rules as the watcher. If `GITHUB_TOKEN` or `GH_TOKEN` is set,
it sends it.

## Failures readable without a login

Job logs and artifacts need a GitHub login, but check-run annotations do not
(`/repos/<repo>/check-runs/<job id>/annotations`). So both suites now write
their failures as annotations:

- **`tools/quality_gate.py run`.** Every gate in the profile runs; the run no
  longer stops at the first failure, so one CI run shows every broken gate.
  Under `GITHUB_ACTIONS=true`, each failed gate prints
  `::error title=Quality gate <id> failed::` followed by the last 40 output
  lines (at most 3,500 characters). The step still fails, and the error lists
  every failed gate.
- **Playwright.** Under `GITHUB_ACTIONS`, both configs add the `github`
  reporter, so each failing browser test becomes an annotation with its error.

## The flaky call test

`TestThreePeopleHoldACallThroughImpairedLinks`
(`server/rtc/fmmserver/call_test.go`) checks real-time audio and video
delivery over 3 seconds. On a loaded machine the test process is starved, and
it reports "heard only 7 of 150 audio frames" or "received 0 video frames".
That happened once in 15 local runs with other suites running at the same
time, and in the first local quality-gate run on 2026-10-23. It is now listed
in `GO_LOAD_SENSITIVE_TESTS` (`tools/run_critical_tests.py`), so the existing
retry-once rule for timeouts (docs/server/README.md) covers it too. A second
failure still fails the step.

## Why World browser E2E never passed

The workflow had failed all 91 of its GitHub runs, at the "Enforce daily
interaction and accessibility budgets" step
(`app/browser_tests/tests/daily-interface.spec.js`). Six tests no longer
matched the dashboard:

| Test | Cause | Fix |
| --- | --- | --- |
| Offline state retries | `fetchJson` now retries a 503 itself (3 attempts), and the page's early catalog read uses up another request, so one mocked failure never reached the error state | The mock stays offline until the test calls `requestCount.reconnect()` |
| Clone commands | Since the canonical short addresses (2026-10-17), `cloneUrl()` builds the URL from the site origin, not the catalog's `cloneUrl` | Expect `git clone <origin>/alpha/engine` |
| Command palette focus | Text fields have had a 1px focus outline instead of the 3px ring since 2026-10-15 (`docs/app/dashboard-components.md`) | Check that the outline is visible (style not `none`, at least 1px) |
| Reduced motion | "New repository" became a link | Look it up by the link role |
| Touch budget | Under `pointer: coarse` the shell set only `min-height: 44px`, so the icon-only menu button was 32px wide | `[data-mobile-menu-toggle] { min-width: 44px; }` in the coarse block of `app/public/dashboard/shell.html` |
| Contrast | White on the dark-mode `primary` (`68 147 248`) is 3.1:1, under the 4.5:1 budget, on solid blue buttons such as Publish advisory and the chat Send button | Dark `primary-foreground` is `1 4 9` (6.6:1), see `docs/app/dashboard-colors.md` |

These tests mock every request with `page.route`, so they run against a static
server on any free port (see below). The suite's next step, the full
`npm test`, had never run on GitHub because this step came first; its failures
will show up as annotations.

## Reproducing the browser suites locally

`npm run test:daily-interface` (App browser tests) starts the Python Worker
with `pywrangler dev`. On the office network the router's DNS does not resolve
`pyodide-capnp-bin.edgeworker.net`, and connections to it time out, so the
Workers runtime cannot start. Port 4179 is often taken by another agent's
server too. `daily-interface.spec.js` needs no Worker: run it with a scratch
Playwright config that spreads `app/browser_tests/playwright.config.js` and
sets `baseURL` and `webServer` to `python3 -m http.server <port>` in
`app/dist`. Rebuild the assets first (`build_dashboard_assets.py`, then
`build_site_assets.py cutover`). The startup budgets miss under heavy host
load, and CI's single retry absorbs that. For the suites that need the
Worker, read the failing tests from the annotations of the GitHub run
(`tools/github_actions_status.py` prints the first one).
