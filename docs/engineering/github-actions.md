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

With the browser on the local build, 39 App specs still failed (local run,
2026-10-29). They had been hidden while the suite tested production. The
2026-10-30 fixes below sorted each one into a stale spec or a page regression.

## The 2026-10-30 fixes

Page regressions, fixed in the page:

- **Plugin navigation hid core tabs** (`dashboard-issue-filters` 287, 313).
  `renderPluginNavigation` (`dashboard/js/04-account.js`) read a
  `/api/plugins/` reply with no `plugins` list as "nothing installed" and hid
  Code, Issues and Pull requests. Such a reply is now treated as unknown, as
  `www/public/api-client.js` already did.
- **"fixes #N" in a pull comment stopped linking the issue**
  (`dashboard-development-links` 146). When 83bfd4f074 moved the local event
  into `applyRepoPullLocalEvent`, the calls to `renderRepoRecordDevelopment`
  and `linkPullCommentIssues` were dropped. `handlePullReviewSubmit` makes them
  again.
- **The Mirrors badge showed the tree's stale count** (`dashboard-org-repo`
  28). `updateRepoLiveCounts` no longer takes `mirrors` from tree counts once
  the live mirror list has loaded for that repository.
- **The home prompt slot could stay on "Loading prompt..."**
  (`dashboard-home` 119, flaky in CI). See
  [web-composer.md](../app/web-composer.md).
- **World refused to be framed by www.forkmesh.com**
  (`site-footer-world-frame` 25). The cutover rewrite left
  `frame-ancestors 'self' https://forkmesh.com https://forkmesh.com` in
  `www/public/_headers`: `tools/cutover_rehost.py` restores `www.` only in
  quoted, comma-separated lists, not in space-separated policy lists. Both
  `/world` rules name `https://www.forkmesh.com` again.

The test Worker config (`world/browser_tests/wrangler.browser-tests.toml`)
drifted from `app/wrangler.toml`:

- `/signup` was served as the static `signup.html`, without the Worker's
  one-use `contact_…` email field, so `direct-messages` could not sign up.
  `/signup` and `/signup/` now run the Worker first.
- `APP_ORIGIN` was unset. The ingress lets a WebSocket upgrade through only
  when its origin equals `APP_ORIGIN`, so every real socket got a 426 and the
  chat stayed on "Disconnected". It is now `http://127.0.0.1:4179`.
- The `FORKMESH_NOTE_ROOM`, `FORKMESH_CRON_WATCHDOG`, `FORKMESH_CRON_RUNNER`,
  `FORKMESH_DISCORD_GATE` and `FORKMESH_ORG_OFFICE` Durable Object bindings
  match production now (migration tag `browser-tests-v2`).

`chat-view-once` 454 builds its fake camera clip with ffmpeg, which the
runner does not have; the workflow installs it before the World step.

Stale specs, updated to the page's intended behaviour:

- `dashboard-agent-session-detail` (256, 375, 410): the security preflight is
  gone on purpose (`docs/agents/README.md`), and the toolbar's Diff button
  replaced the compare link; the Branch row still links the compare page,
  whose heading is "Comparing".
- `dashboard-agents-page` (100): the count reads "N sessions · M working" and
  the working icon spins (790d31d7b3).
- `dashboard-calendar-activity` (115, 160, 185): the calendar is a package
  page; the spec registers it.
- `dashboard-desktop-agents` (213): merge raises no confirm; only delete does.
- `dashboard-development-links` (119): pull links use `/pull/N`.
- `dashboard-global-search-page-filter` (418): a repository the user owns
  reads `/agent-sessions`, not `/agent-bots`.
- `dashboard-insights-contributors` (14): names go through
  `issuePersonNameHtml`, which marks `data-issue-person` instead of a title.
- `dashboard-issue-avatar-image-markup` (114),
  `dashboard-issue-detail-description` (149, 212): the page renders a wide
  and a phone avatar and shows one (290f3071c2); the specs count visible ones.
- `dashboard-issue-filters` (214, 287, 495): "Newest" sorts first, a re-read
  needs `.forkmesh` in the root tree, and issues load through `/blobs`.
- `dashboard-new-repository` (76, 114, 247): "New repository" opens its own
  page, `/dashboard/repos/new`, not a modal.
- `dashboard-org-repo` (28): the title row holds Fork and Star, and a
  "Source of truth" row leads the mirrors list.
- `dashboard-pull-files-deep-link` (232): comment files load through
  `/blobs`.
- `dashboard-record-list-links` (130): the spec read the new tab's URL before
  its navigation started.
- `dashboard-repo-activity-graph` (103, 134, 175): Insights opens on Pulse;
  the graph is under `?view=contributors`.
- `dashboard-repo-branches` (536): the first update goes alone, the rest as
  one batch ([branch-ops.md](../app/branch-ops.md)).
- `dashboard-repo-forks` (12): the fork count is in the title row.
- `direct-messages` (70, 119): since 22473e237a an account must confirm its
  email before it writes. The spec confirms each account through
  `/api/accounts/verify-email` with a link token built from the test
  `DATA_KEY`; the test Worker sends no mail, so no newer link replaces it.
  Since 3d7fccfeed the direct-message search leaves out private profiles, so
  the outsider, not bob, goes private, and the spec checks that neither the
  directory nor the search lists them. Only message frames notify, so bob's
  unread count is the four messages; the sealed key advertisement is retained
  but not counted.
- `network-relay-stats` (4): the empty state says "No managed mirror hosts"
  (ccfad67575).
- `node-origin-docs-blog-session` (24): `site-header.js` keeps the session
  read in `sessionStorage` for 5 minutes, so Docs then Blog read it once.
- `private-chat-channels` (6, 209, 464): each account has its own chat key
  since 2026-10-22, so the invited user connects (and leaves a key) before
  the admin sends; managers may open settings on public channels; the room
  list drops the leading "#".

Run one spec against the local Worker with
`CI=1 FORKMESH_BROWSER_FULL=1 npx playwright test tests/<spec> --retries=0`
in `app/browser_tests`.

## A passing retry that still fails the run

Run 41044496008 (b3fd863c34, 2026-10-30) went red with 190 passed, 2 flaky
and "1 error was not a part of any test". Every test had passed by its retry.
The extra error was `page.evaluate: Target page, context or browser has been
closed` from `chat-view-once.spec.js`. Each recorder spec opens the dialog
with `const framePromise = page.evaluate(() => window.harness.record(...))`
and awaits it only after it sends. When an assertion fails first, Playwright
closes the page, the promise rejects with nothing listening, and the runner
counts that rejection as a worker error, which exits 1 whatever the retry
does. The specs now call `framePromise.catch(() => {})` straight away and
still `await framePromise` later, so a real failure still fails the test. Any
promise a spec starts and awaits later needs the same.

The first attempt of "video messages record the chosen face and background"
failed under load. With the spec pinned to two CPUs (`taskset -c 0,1`) it
failed 2 runs in 6, always with "The face or background could not start here"
about 4 seconds after the background was picked. Picking a face and then a
background starts two face stages at once, and each took 11 to 14 seconds to
become ready on SwiftShader. Logging each stage showed three causes:

- The face also picks its voice ("theo" for Theo), and `openViewOnceCapture`
  started the voice stage only after the face stage was ready. By then the
  face models held the CPU, and the voice worklet's `addModule` ran past its
  6-second limit (`MODULE_TIMEOUT_MS` in `chat-call-voice.js`) with "voice
  module timed out". The recorder then blamed the face, because a face filter
  was active. The voice stage now starts first, since its worklet is small and
  the face stage has a 20-second budget. A failure is tagged with the stage
  that threw, and a voice failure shows the voice message. "the face's voice
  starts before the face, and a voice that cannot start says so" covers both.
  With all three changes the same two-CPU run passed 8 in 8 with retries off.
- The recorder showed "The face or background could not start here" when the
  older stage failed, even though the user had already moved on and the newer
  stage was still loading or already showing. `openCapture` in
  `app/public/chat-view-once-recorder.js` now turns a failure from a stale
  generation into the same `superseded` error that a stale success already
  gave, so only the current attempt can change the status. "a face that fails
  after a newer choice started leaves the newer preview alone" covers it and
  fails without the change.
- The harness kept whichever stage resolved last as `window.harness.faceStage`.
  When the older, already stopped stage finished second, the spec read
  `background: ""`. The harness now notes when the recorder stops a stage and
  reads the newest one still running. Ordering by request does not work: the
  voice stage runs first, so an older capture can reach its face stage after
  a newer one.

`dashboard-settings-agent-profiles.spec.js` (175) read the desktop's
`terminal.start` call right after the modal's title changed, but
`showAgentLoginModal` draws the title before `createAgentLoginTerminal`
starts the terminal. The spec polls for the call with `expect.poll`, as it
does for `terminal.detach`.

## The direct-message frame count

From b783a27f4b (2026-10-30) about every other main run failed
`direct-messages.spec.js` (150) at the retained-frame poll with "Expected: 5,
Received: 6", on both attempts. Alice's page retains six frames: the sealed
key advertisement, the two messages, the thread reply, the delete of that
reply and the attachment. The 2026-10-30 fix counted
`RETAINED_KEY_ADVERTISEMENTS + 4`, a count taken before the attachment frame
went out. `expect.poll` returns as soon as it sees that count once, so the
spec passed when its first samples landed before the attachment was sealed
and sent, and failed when the attachment went first. It now waits for
`RETAINED_KEY_ADVERTISEMENTS + 5`. Since the count only grows, the poll can no
longer pass early, and the plaintext checks after it now cover the attachment
frame as well. Bob's unread count stays 4: the delete is retained but does not
notify.

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

Two ways to start the Worker anyway (2026-10-30):

- Point the host at an address Cloudflare's DNS-over-HTTPS returns, in a
  private mount namespace: `unshare -rm sh -c 'mount --bind <hosts-copy>
  /etc/hosts && exec "$@"' sh <command>`. Even then `workerd`'s own connect to
  that address timed out again and again on this network, while `curl` from the
  same namespace answered in milliseconds.
- Seed the Pyodide bundle cache instead. Set `MINIFLARE_WORKERD_PATH` to a
  wrapper that adds `--pyodide-bundle-disk-cache-dir=<dir>` after `serve`, and
  put the bundles in that folder with `curl`. The files are
  `pyodide_<version>_<revision>_<backport>.capnp.bin` on
  `pyodide-capnp-bin.edgeworker.net` (for example
  `pyodide_314.0.6_2026-08-17_8.capnp.bin`). The versions and revisions are
  in the `workerd` binary's strings next to the `pythonWorkers2026…` flag
  names; probe the backport numbers with a one-byte range request. With the
  cache the Worker starts with no outbound connection at all.

`build_site_assets.py cutover` deletes and rebuilds `app/dist`, and a running
`pywrangler dev` then answers 500 for every static file until it restarts. To
rebuild while the Worker runs, serve a copy of `dist` (a scratch config whose
`[assets] directory` points at it) and `rsync -a --delete` the new build into
that copy.
