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

- **Hourly sweep.** One page of 100 runs covers only the last few pushes, so
  the daily "GitHub issue sync" and a workflow whose recent runs were all
  cancelled (World browser E2E) never appeared on it, and their failures
  were never pinged (found 2026-10-25). On the tick in the first 5 minutes of
  each hour (`sweep_due`), the watcher also lists the repository's active
  workflows (`/actions/workflows`) and reads one page of 50 runs on the
  branch for each workflow missing from the page (`unseen_workflows`, at most
  12). Between sweeps, a workflow off the page keeps its stored state.
  `tools/github_actions_status.py` sweeps on every call.

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
  each new failure (jobs and annotations), and the hourly sweep adds 1 plus
  one per missing workflow (3 today: the issue sync, World E2E and the
  always-skipped GitHub bridge). That is about 16 an hour against GitHub's
  unauthenticated limit of 60 an hour per IP.

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

## Python packages on the CI runner

GitHub runners install only `tools/requirements-ci.txt`, while the local
machine and the ForkMesh Actions runner have more packages preinstalled. So a
test that imports a third-party package passes locally and fails only on
GitHub. On 2026-10-23 the `service-role-contract` gate failed every run: its
`independent_gateway` fixture loads `app/tests/test_mirror_dashboard.py`,
which signs health reports with `cryptography`'s Ed25519 keys. The file now
pins `cryptography` and its `cffi` and `pycparser` dependencies at the
versions `app/host/requirements.txt` uses; raise them in both files together.
To reproduce a CI run, create a fresh venv, install only
`tools/requirements-ci.txt`, and run
`python tools/quality_gate.py run --profile core` with it.

The same runs also failed `terminology-contract`: the 2026-10-22 /features
update described one-to-one calls as "peer to peer", a discouraged term on
primary surfaces (`www/docs/terminology.json`). The card now says "device to
device", which is still accurate because one-to-one calls skip the media server
(`docs/server/rtc.md`).

## Why World browser E2E never passed

The workflow had failed all 91 of its GitHub runs, at the "Enforce daily
interaction and accessibility budgets" step
(`app/browser_tests/tests/daily-interface.spec.js`): six checks no longer
matched the dashboard (fetch retries, clone URLs from the page origin, the
text-field focus outline, the New repository link, the 44px menu button on
touch screens, and dark-theme text contrast on solid `primary`). The fixes
and what the check relies on are in
[daily-interface-validation.md](../../www/docs/engineering/daily-interface-validation.md).

These tests mock every request with `page.route`, so they run against a static
server on any free port (see below). The suite's next step, the full
`npm test`, had never run on GitHub because this step came first; its failures
will show up as annotations.

## The 2026-10-25 failures

The sweep turned up three red workflows on `main`:

- **Quality gates, `security-privacy-contracts`.** The README now says phones
  are mobile nodes with their own key and Git engine, so
  `test_retired_bounty_and_phone_mirror_advertising_stays_removed`
  (`app/tests/test_public_security_claims.py`) no longer finds "phones are
  access clients". It now checks the claim that matters: mirroring remains a
  desktop or headless-node job.
- **World browser E2E.** The mobile camera samples under SwiftShader carried
  a second slow drag; the warm-up is now two drags
  ([docs/world/README.md](../world/README.md)). The workflow also cancelled
  its own `main` runs on every push (57 of the last 60), so it almost never
  finished. Only pull-request runs cancel in progress now; on `main` a newer
  push waits for the running one, and GitHub keeps only the newest waiting run.
- **Daily one-way GitHub issue sync.** The `FORKMESH_ISSUE_SYNC_DESTINATION`
  repository variable is not set on github.com/forkmesh/forkmesh, so every
  scheduled run failed its configuration check. An unconfigured scheduled run
  now writes a notice and skips the sync and upload steps; a manual run without
  a destination still fails. Set the variable (and the tokens in
  [automation-and-cloudflare-bootstrap.md](../../www/docs/operations/automation-and-cloudflare-bootstrap.md))
  to turn the sync on.

## The 2026-10-29 failures

- **Quality gates, `security-guard-contracts`.** The security sweep added
  three `app/tests/host/test_repo_fastpath.py` tests to
  `tools/run_critical_tests.py`. They build an `httpx.AsyncClient`, and the
  runner had no `httpx`. `tools/requirements-ci.txt` now pins `httpx` and its
  dependencies (`anyio`, `certifi`, `h11`, `httpcore`, `idna`,
  `typing_extensions`) at the `app/host/requirements.txt` versions. The Docs
  package's dependency snapshot and the phones' Docs bundle were refreshed
  with it (`marketplace/tools/docs_dependencies.py --offline`, then
  `docs_mobile_bundle.py`).
- **External dependency map.** The agent-summary preload script
  (`desktop/src/AgentSummaryPreloadScript.h`) carries an MIT header naming
  `flamerix.ai`. It is now a `documentation` reference in the manifest, and
  the published map was re-rendered.
- **World browser E2E never finished.** Every run was cancelled at the
  20-minute limit inside the App browser step, and the cause was the test
  server, not the runner. `run-browser-worker.sh` serves `app/dist` with
  `pywrangler dev`, and `build_site_assets.py cutover` copies
  `www/public/_redirects` into it. The www site's handoff rules (`/chat`,
  `/dashboard/*`, `/login`, `/world/*` and the rest) 308 to
  `https://app.forkmesh.com` and `https://world.forkmesh.com`, and wrangler
  applies them. So every App spec that opened one of those pages tested the
  production site, not the build, and the chat specs waited for a socket the
  production page never opened through the test's mocks. Production never
  applies `_redirects` (the Next host and its Caddy routes skip it,
  [docs/host/README.md](../host/README.md)). In full-suite mode the launcher
  now drops every rule that leaves the test server before `pywrangler dev`
  starts. The browser-test wrangler config's `run_worker_first = "/*/*"`
  also sent `/fmm/fmm-local.js` to the Worker, which answered with a
  repository page, so `chat.js` failed to load as a module. `fmm`,
  `dashboard-plugins` and `instance-themes` are now excluded, like the other
  static folders. The full App suite takes about 10 minutes, so the job limit
  is now 40 minutes.
- **The App specs meet the real build.** The chat specs counted retained
  frames from before the relay hardening of 2026-10-22. The transport also
  retains the sealed key advertisement it sends with a room's first message,
  because a reloaded page needs it to read the retained messages. The specs
  now count it as `RETAINED_KEY_ADVERTISEMENTS`.

With the browser on the local build, 39 App specs still fail (local run,
2026-10-29). These were hidden while the suite tested production, and each
needs a decision: is the spec stale, or did the page regress? Until they are
fixed, World browser E2E stays red at its last step:

- `dashboard-agent-session-detail` (256, 375, 410): no "waived" text and no
  `[data-org-agent-compare]` link in the session detail.
- `dashboard-agents-page` (100): the count reads "3 sessions · 1 working",
  the spec expects "1 working · 3 total".
- `dashboard-calendar-activity` (115, 160, 185): no repository history
  requests, no "more" day cell, an empty grid.
- `dashboard-desktop-agents` (213): the first confirm dialog is "Delete this
  agent.", not the merge.
- `dashboard-development-links` (119, 146): the issue's Development section
  has no pull link; the posted links differ.
- `dashboard-global-search-page-filter` (418): no `[data-org-agent-session]`
  rows.
- `dashboard-home` (119): `[data-composer-slot]` stays visible when the
  composer does not mount.
- `dashboard-insights-contributors` (14): the profile link has no title.
- `dashboard-issue-avatar-image-markup` (114) and
  `dashboard-issue-detail-description` (149, 212): the author appears twice in
  the issue conversation.
- `dashboard-issue-filters` (214, 287, 313, 495): sort options differ, the
  Code tab and Pulls tab clicks time out, no issue skeleton.
- `dashboard-new-repository` (76, 114, 247): the "New repository" button
  click times out; the modal stays hidden.
- `dashboard-org-repo` (28): no `forkmesh/forkmesh` heading.
- `dashboard-pull-files-deep-link` (232): no inline thread toggle.
- `dashboard-record-list-links` (130): middle-click opens a blank tab.
- `dashboard-repo-activity-graph` (103, 134, 175): no
  `[data-repo-activity-graph]`.
- `dashboard-repo-branches` (536): the bulk update sends a different branch
  list.
- `dashboard-repo-forks` (12): no `[data-repo-fork-count]` in the about
  actions.
- `direct-messages` (70, 119): the sign-up helper finds no email field.
- `network-relay-stats` (4): "No managed mirror hosts are reachable right
  now." instead of "No desktop hosts …".
- `node-origin-docs-blog-session` (24): one session read, not two.
- `private-chat-channels` (6, 209, 464): the invited user's replay shows "No
  messages"; `#chat-channel-manage` stays visible for a non-manager; the room
  list reads "Worldshipping".
- `site-footer-world-frame` (25): no World frame found.

Run one spec against the local Worker with
`CI=1 FORKMESH_BROWSER_FULL=1 npx playwright test tests/<spec> --retries=0`
in `app/browser_tests`. On this box `pywrangler dev` started (2026-10-29).

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
