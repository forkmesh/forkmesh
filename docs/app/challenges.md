# Code challenges

Part of [the App docs](README.md). Code: `app/src/challenges.py` (rules),
`app/src/challenges_runtime.py` (routes and D1), tables `code_challenges` and
`code_challenge_members` (migration `0217_code_challenges.sql`), dashboard
page `/dashboard/challenges` (`app/public/dashboard/partials/views/challenges.html`,
`app/public/dashboard/js/09-challenges.js`, a per-page chunk), tests
`app/tests/test_challenges.py`.

A challenge is a race between ForkMesh users over a fixed number of days, in
the style of Apple Watch activity competitions: whoever does the most code in
the window wins. Anyone signed in with a user account can start one, either in
one click from a preset or with their own rules, and invites people by sending
the link `/dashboard/challenges?join=<id>`.

## Presets and settings

| Preset | Counts | Length | Daily cap | People |
| --- | --- | --- | --- | --- |
| `week` (the one-click default) | Code points | 7 days from today | none | 10 |
| `sprint` | Commits | 3 days from today | none | 10 |
| `month` | Code points | 30 days from today | 100 | 20 |

A custom challenge sets:

- **name**, up to 60 characters;
- **metric**: `points` (commit 1, pull request 5, review 2, issue 1),
  `commits`, `pulls`, `reviews`, or `custom` with a weight of 0 to 100 for each
  of commits, pull requests, reviews and issues (at least one above 0);
- **days**, 1 to 31;
- **start**: today, tomorrow, or a UTC date up to 30 days ahead;
- **dailyCap**: the most points one day can add, 0 for no cap. Like the Apple
  Watch cap, it keeps one huge day from deciding a long challenge;
- **maxMembers**, 2 to 50, the host included.

Settings are fixed once the challenge exists. An account can host at most 5
open challenges (not yet finished) and be in at most 20.

## Scoring

Scores come from `profile_contribution_days`, the rows behind the profile
contribution graph, so nothing new has to be reported and a challenge cannot
be scored from anything the profile graph would not show:

- only public projects on public repositories count, one source repository
  per project (latest capture), the same selection as
  `_contribution_commit_totals` in `entry.py`. Private work never appears on a
  board that other people read;
- days are UTC dates, as in the contribution snapshots;
- scores change when a member's desktop publishes a new contribution
  snapshot, so a board lags real work by the snapshot interval;
- ranks are competition ranks: equal totals share a rank. A finished
  challenge's winners are everyone at rank 1, and nobody wins if the top total
  is 0.

Each board row carries the per-day scores, drawn as bars on the page, and the
raw counts of commits, pull requests, reviews and issues in the window.

## API

All answers are `no-store` JSON. Writes need a user session (node sessions are
refused, as in the hackathon), and cookie sessions pass the usual origin check
in `_account_session_record`.

| Route | What it does |
| --- | --- |
| `GET /api/challenges` | The caller's 30 most recent challenges with boards, plus `options` (presets, metrics, limits) and `today` |
| `POST /api/challenges` | Create: `{"preset": "week"}` or the custom settings above. 201 with the challenge |
| `GET /api/challenges/<id>` | Members get the board. Anyone else, signed in or not, gets a preview for the invite: name, host, dates, rules, member names, no scores |
| `POST /api/challenges/<id>/join` | Join while not finished and not full. The member cap is checked inside the insert, so two joins cannot overfill it |
| `POST /api/challenges/<id>/leave` | Leave. The host cannot leave; the host deletes |
| `POST /api/challenges/<id>/delete` | Host only: removes the challenge and its members |

Errors are `{"ok": false, "error": "<code>"}`; the page maps each code to a
sentence (`CHALLENGE_ERROR_TEXT` in `09-challenges.js`).

## Traps

- The challenge id is also the invite secret. It is 128 random bits
  (`_ap_uuid`), and the preview it unlocks shows no scores.
- `/dashboard/challenges` is a composed dashboard page: it needs its entries
  in `dashboard_shell.PAGES`, `static_routes.DASHBOARD_PAGE_ASSETS` and
  `BLOCKED_STATIC_HTML_PATHS`, the `!/dashboard/challenges` pair in
  `app/wrangler.toml`'s `run_worker_first`, and a rebuild with
  `app/tools/build_dashboard_assets.py`.
