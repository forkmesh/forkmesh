# Public App pages: ledger, network, status, mirror rewards

These standalone pages in `app/public/` are public, need no sign-in and sit
outside the dashboard. Since September 2026 they share the page kit from
`www/public/styles.css` (`<body class="kit-page">`, `.kit-hero`, `.kit-badge`,
`.kit-title`, `.kit-lede`, `.kit-btn`, `.kit-card`, `.kit-tile`), so they open
like the website: a small badge, a left-aligned ABC Favorit title, a grey lede
and a dark-plus-white textured button pair. [docs/www/README.md](../www/README.md)
describes the kit.

| Page | Script | Data |
| --- | --- | --- |
| `ledger.html` (`/ledger`) | `ledger.js` | `GET /api/ledger`, `GET /api/rewards/estimates`, the reader's own wallet through `/api/accounts/<name>` and `/api/world/wallet` |
| `network.html` (`/network`) | `www/public/static-page.js` plus two inline scripts | `GET /api/network/stats?payouts=1`, `/api/network/online-history`, `/api/network/leaderboards` |
| `status.html` (`/status`) | inline script | `GET /api/status` and the `/api/status/ws` stream |
| `mirror-payouts.html` (`/mirror-payouts`) | `mirror-payouts.js` | `GET /api/network/stats`, `/api/rewards/pool` |
| `leaderboards.html` (`/leaderboards`) | `leaderboards.js` | the leaderboard endpoints |

## Ledger

The page reads top to bottom as: hero, wallet cards, the Expenses / Income /
Debt totals, the records list, then "How to read this ledger" (the four
integrity rules and the notes on what the numbers mean). The wallet strip
stays above the totals because the first thing a reader should see is what
the wallets hold, not a total of whatever page of records happens to be
loaded; `test_ledger.py` pins that order. The strip uses `auto-fit` columns
because it carries the reader's own wallet only when they have one, and a
host may configure a single address for both system roles.

Each record is one compact row: date; title, category, the people involved
(sender to recipient, or "Paid by") and the linked issue or pull request;
status; amount with its USD value; and a shortened receipt link to Solana
Explorer. Everything else sits in a details panel the chevron (or a click on
the row) opens: the full transaction signature, lamports and the USD basis,
verification, the source record, wallet links, Discord identities, other
linked records, the issue summary and the row's note. The old table printed
all of that inline, which made a single reward several hundred pixels tall.

- A signature or reward id is one unbroken 88-character base58 word. The row
  shows it abbreviated (first and last six characters); only the details
  panel prints it whole, where `overflow-wrap: anywhere` lets it wrap.
- On phones the record cell becomes `display: contents`, so its title shares
  the first line with the amount while the people and subject lines use the
  full width. Date, status and receipt are grouped in `.ledger-row-meta`,
  which is also `display: contents` on desktop (each lands in its own grid
  column) and a wrapping flex row on phones, with the toggle overlapping its
  right end in the same grid area.
- Rows that name people show each account's picture, falling back to the
  initial; `ledger.js` caches portraits per name for the life of the page.

## Network

Hero with the live relay counts (hosts, cataloged repositories, relay role
and the storage boundary), "How a clone travels" as three step cards, then
full-width cards for host activity, user wallets and leaderboards. The cards
are stacked rather than side by side because the wallet list grows with the
number of accounts and used to leave the activity card mostly empty.

- The activity strip is one row per host with 24 hourly cells, shaded by
  minutes online (none, some, 20+, 45+). Green here is data, like a status
  bar; the page has no decorative green (no LED dot, no green glow, and the
  relay step is marked with the dark kit tile).
- `renderPayoutNodes` in `static-page.js` sorts eligible nodes first, then
  nodes with a published wallet, then online nodes, and shows six rows; a
  "Show all N users" button reveals the rest. A node with a wallet that does
  not mirror a repository for another node reads "Not mirroring" (its
  `eligibilityReason` is `no_mirrors`); it used to fall through to "No
  wallet".
- Each leaderboard shows its top five with a "See all" link to
  `/leaderboards`.
- The activity graph and leaderboards poll every five minutes only while the
  tab is visible and back off exponentially (up to 30 minutes) while the
  endpoint fails, for example with 429s when the request budget is spent; a
  success resets the cadence.

## Status

The long monitoring caveat that used to fill the hero moved to "How this
page measures uptime" under the systems list. The stat row uses its own
`.status-stats` / `.status-stat` classes because the dashboard's repository
view still uses the shared `.stats-grid` / `.stat-card` styles. The
`id="status-title"` heading and the `/api/status/ws` reference stay: the
status-page monitor in `app/src/entry.py` checks for both.

Each system row lists the checks it runs as a wrap-around row of chips under
its title. The names and hover text come from the `checks` field of
`/api/status` (`STATUS_SYSTEM_CHECK_STEPS` in `app/src/status_monitoring.py`,
`STATUS_MIRROR_CHECK_STEPS` for mirror rows), so each chip names a step the
probe really performs. Hovering or focusing a chip shows its detail in one
shared `role="tooltip"` element, positioned with `position: fixed` because
`.status-list` clips overflow; it hides on scroll and on every re-render.
The World status body drops `checks` along with `checkDescription`.

Each row header also carries a small two-slice pie and a countdown to that
row's next check (`nextCheck` in `/api/status`: `at`, `last`, `kind`,
`everyMs`). The server reads the schedule from `_cron_due_phases` itself
(`_status_phase_slots` / `_status_next_check` in `status_monitoring.py`), so
the pie cannot drift from the cron. The flagship repository and
per-platform release rows count down to their next deep probe, or to the next
retry while the row is failing; every other row counts down to the next
minute sample, which the page rolls forward locally (`everyMs`). The page
redraws once a second with a `setTimeout` aligned to the second, since it
never polls `/api/status` on a timer. The filled slice is the time remaining
and the track is a lighter step of the same ink, per the dataviz meter rule;
the countdown is also printed beside the pie so the pie is never the only
carrier. World drops `nextCheck`.

A row that only a probe can turn red goes red after two failing probes in a
row. These are the per-platform release and flagship repository
rows, plus the homepage, World and status-page synthetic checks. The cores
take the cron minute in turn, so the second probe normally runs on a
different core, and a fault on one core or on one of its connections is not
reported as an outage. The first failure is stored in
`repository_monitor_state` with `changed_at` equal to `checked_at`. The
sampler shows it as green, and nothing is Pinged or mailed because alerts are
raised from the minute row (`_status_probe_verdict` in
`status_monitoring.py`). A second failing probe moves `checked_at` past
`changed_at`, and from then on the row is red with that probe's reason. A
passing probe turns the row green at once. The flagship row keeps its own
rule: a failure stays green until it is `FLAGSHIP_FAILURE_CONFIRM_MS`
(two minutes) old. So a real outage shows one minute later than before (two
for the flagship).

A synthetic probe that raises counts as a failing probe too:
`_cached_synthetic_status_probe` stores the error, with its row's prefix
("Mobile World probe failed: ..."), as the first failure. Until 2026-10-01
the error escaped to `record_status_sample`, whose `except` turned the row
red at once. One `Temporary failure in name resolution` for
world.forkmesh.com on one core then Pinged "World mobile startup needs
attention" while World was loading fine on phones.

Release versions have one row per platform: "Linux latest version",
"Android latest version", "Windows latest version" and "macOS latest
version" (`RELEASE_PLATFORM_MONITORS` in `app/src/entry.py`). They replaced
the single "Homepage downloads (latest release)" row on 2026-10-01, which
went red for every platform whenever one was missing from the newest release,
so a platform could not ship on its own. Each row verifies the signed
`latest` manifest and, when `desktop/CMakeLists.txt` on main names a newer
version whose `candidate-<version>` manifest verifies, uses that candidate as
the newest release. The row passes when the newest release carries that
platform's build (Linux AppImage, Android APK, Windows installer, or both
macOS DMGs) with a valid digest and size and the blob downloads from a
reachable mirror. So when Linux v1.2.6 is published into the candidate, the
Linux row turns green while the others stay red until their builds land.
The rows check the release, not the homepage: the homepage keeps offering
`latest` until the candidate is promoted. All four run in one cron phase,
`probe-release-platforms` (one phase so the busiest minute stays within
`CRON_BATCH_MAX_OBJECTS`), but each platform is probed in its own `try` and
stored under its own `repository_monitor_state` row (`release-linux`,
`release-android`, `release-windows`, `release-macos`), so one platform's
failure or crash never changes another's row.

The combined "Installers: Linux, Android, Windows & macOS" row, with its
`probe-installer` phase and `installer-delivery` monitor, was removed on
2026-10-01: the four platform rows already report each installer, so the
combined row only repeated their failures.

Visitor evidence is not held back. Two or more failed `forkmesh/forkmesh`
content reads in a minute turn the flagship row red in the next sample, and
Worker errors turn Website, API, Worker errors and Realtime red in the same
minute. If no core completes a deep probe for two minutes, the row reads
"did not complete within its monitoring interval" and is red at once. Core,
runner, mirror, Git hosting, database and email rows come from shared state
rather than a per-core probe, so they need no second probe.

Each core node of the served stack has its own row, `core:<node>` ("Core
node — n1"), listed after the fixed systems and before the mirrors. The
roster is dynamic: every `cluster_members` row with a heartbeat in the last
24 hours gets a row (`_status_core_rows`, capped at `STATUS_CORE_MAX` = 32),
so one core or fifteen each get a monitor with no config change. A new core
appears with its first heartbeat. A core taken out with `next_fleet.py
remove-core` disappears at once: its rqlite step deletes the core's
`cluster_members` row through a healthy peer (`RETIRE_MEMBER_SQL` in
`tools/next_remove_core.py`), so the removed core is neither listed red nor
paged every five minutes for a day. A core that stops any other way (a crash,
a deleted VM) stays red for a day, then drops off. Core ids are checked by
pattern (`_status_batch_system_id`), not against a list, so adding or
removing a core never blanks another row's history, unlike renaming a fixed
system (`STATUS_SYSTEM_SUCCESSORS`). On Cloudflare D1 the table is empty and
no core rows appear.

A core row is green only when all of these hold (`_status_core_verdicts`,
chips from `STATUS_CORE_CHECK_STEPS`):

- its cluster heartbeat is under 30 seconds old, which needs a working rqlite
  quorum and write path;
- it reports `active`, not `sealed`, `quarantined` or `leaving`;
- every `dns_health_votes` probe of it from the last 90 seconds passed, for
  both the shared app hostname and its own node hostname. The raw `detail`
  is used, not the hysteresis `healthy` flag, so a failing core goes red
  within a minute;
- once its IDS has reported, the `ids_heartbeats` row is under three minutes
  old and not in `lockdown`.

The reason names the core and every failing check, including which peers'
probes failed. Core rows page continually by default: while one is red,
every administrator gets a Ping every five minutes
(`_status_monitor_alert_setting` defaults `continual` on for `core:*`; the
admin matrix on the Pings settings page has an "All core nodes" row to turn
it off). Continual Pings are drained apart from transition Pings, up to
`STATUS_ALERT_CONTINUAL_BATCH` (16) per minute. With the old shared
one-alert batch, when several cores were down in the same five-minute
boundary minute, only the first was re-pinged.

Each runner in the served stack's pool has its own row too, `runner:<name>`
("Runner — r1"), listed after the cores and before the mirrors. The roster is
every `runner_nodes` row with `scope='pool'` and `status='active'`
(`_status_runner_rows`, capped at `STATUS_RUNNER_MAX` = 32). That covers r1
and any autoscaled `fmnext-run-N` burst runner while it is registered. The
autoscaler revokes a burst runner when it scales it down, so its row drops
off. Personal runners, which are users' own machines, are never listed on
the public page.

A runner row is green while the runner has claimed or polled
`/api/runners/*` within `STATUS_RUNNER_SEEN_FRESH_MS` (three minutes;
`_status_runner_verdicts`, chips from `STATUS_RUNNER_CHECK_STEPS`). Both
calls stamp `runner_nodes.last_seen_at`. The Go runner polls about every
20 seconds whether it is idle or busy (`runnerIdlePoll` in
`server/runner.go`), and it backs off to at most five minutes on errors,
so three minutes is the same staleness bound the daemon uses
(`runnerStaleAfter`). When a runner goes red, the reason says how long it
has been silent and how many unexpired job leases it still holds. Those
leases are agent jobs stuck behind it. A runner that polls but whose Claude
Code or Codex login has expired (its `runner_availability` row says so, see
[RUNNERS.md](../RUNNERS.md), "Expired logins") is red too, with the reason
"fmnext-r1: Claude login expired — reconnect" and the check "Provider logins".
Runner rows do not page continually
by default; the "All runners" row on the admin Pings matrix (`runner:*`)
turns that on.

### The Nodes section (2026-10-13)

Core and runner rows (`core:*`, `runner:*`) sit in their own **Nodes**
section below **Systems** on `/status` (`app/public/status.html`,
`isNodeSystem`). Owner request: nodes come and go with the fleet and change
more often than services, so a node row shows only its last 60 one-minute
checks and its last 24 hourly checks with the 24-hour uptime; the 30-day
column and percentage are left out. Services and mirrors keep all three
strips. The section is hidden when no node row is reported (a Cloudflare
deployment). The API is unchanged: node rows still carry `days`, because the
24-hour strip is built from `days[].hours`.

The other views of `/api/status` follow the same split: the iOS and Android
Status screens list a "Nodes" section between "Systems" and "Mirrors"
(`StatusReport.nodeSystems`; the iOS detail page leaves out "Uptime, 30
days" for a node), and the World status board draws no 30-day strip or
figure for a node row. The desktop footer shows one dot per row and needs no
change. The flag both apps labelled "Main node" is `current.mainnodeOnline`:
whether any verified mirror serves `forkmesh/forkmesh` lately. It is not a
core, so the apps now call it "Verified mirror".

Tests: `app/browser_tests/tests/status-page-reliability.spec.js` (headless
Chromium; `STATUS_SCREENSHOT=<png>` saves the systems card), the
`StatusModelTests` and `InsightsTest` model tests.

## Leaderboards: My stats (2026-10-02)

The toolbar's "My stats" button shows the signed-in reader where they stand
without searching every board. It reads the account from the
`forkmesh.session` the ledger reads, because `/api/leaderboards` is one cached
public payload and cannot say who is reading. Pressing it opens a panel above
the grid with each board the reader ranks on (rank, board, value), narrows
the grid to those boards and highlights the reader's rows. A row is the
reader's when its name is the account or, on repository boards, starts with
`<account>/`; "Referring websites" ranks hosts, not members, so it never
matches. Boards carry only their top entries, so a reader below them is told
so and pointed at their `/@name` profile. A signed-out press goes to
`/login?next=/leaderboards?mine=1`, and `?mine=1` opens the panel on arrival.

## Mirror rewards

A full page instead of a centred card, with a "Not live yet" pill and a
notice that the program has not been released: no node is selected or paid
yet, and the page shows the policy and the public pool as they will work at
launch. It no longer loads `auth-page-theme.css` (it is not an auth form), so
`test_public_light_theme.py` no longer lists it with the auth pages.

## Balances come from an hourly cache

Public wallet balances on these pages (ledger wallets, network user wallets,
the mirror reward pool) are served from an hourly cache so opening a page
costs no on-chain read. Each page's Refresh button is the one path that asks
the relay to read Solana again (`refresh=1`), and every balance says how old
its reading is.

## Status stream: a failed socket constructor falls back to reconnect (2026-09-28)

On 2026-09-25 `/status` reported three times
`Failed to construct 'WebSocket': Failed to create a WebSocket: the provided URL is invalid.`
from `connectStatusStream` through `RoutedWebSocket` (`api-client.js`). The URL
the page builds, `wss://<host>/api/status/ws`, is valid: Chromium 149 accepts
it and names any URL it rejects in the message (`The URL '…' is invalid.`), so
the wording without a URL comes from a browser path we could not reproduce. The
throw was uncaught, so the live stream stopped for the rest of the visit.

- `connectStatusStream` now wraps the constructor. A throw leaves
  `statusSocket` unset and goes through `scheduleReconnect`, the same backoff
  (1 s doubling to 30 s) a closed socket uses. The page keeps rendering from
  `GET /api/status`.
- `api-client.js` `websocketURL` gave the browser an `https:` URL whenever it
  was handed an `https:` or relative URL, and kept any `#fragment`. Older
  browsers (Chrome before 125, for one) reject a non-`ws(s)` scheme,
  and every browser rejects a fragment. It now always returns `ws:` or `wss:`
  with no fragment, and still routes same-origin `/api/` sockets to the App
  origin.

Tests: `test_websocket_routing_always_hands_the_browser_a_ws_scheme` and
`test_status_stream_survives_a_websocket_constructor_failure` in
`app/tests/test_api_cors_routing.py`.
