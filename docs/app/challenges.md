# Code challenges

A challenge is a race between users over a fixed number of days: whoever does
the most code in the window wins. Code: `app/src/challenges.py` (rules),
`app/src/challenges_runtime.py` (routes and storage), tables
`code_challenges` and `code_challenge_members`, page `/dashboard/challenges`
(`09-challenges.js`), tests `app/tests/test_challenges.py`. Invites are the
link `/dashboard/challenges?join=<id>`.

## Settings

| Preset | Counts | Length | Daily cap | People |
| --- | --- | --- | --- | --- |
| `week` (default) | Code points | 7 days | none | 10 |
| `sprint` | Commits | 3 days | none | 10 |
| `month` | Code points | 30 days | 100 | 20 |

Custom: name (≤ 60 chars); metric `points` (commit 1, pull request 5, review
2, issue 1), `commits`, `pulls`, `reviews`, or `custom` weights 0–100; days
1–31; start today, tomorrow or a UTC date ≤ 30 days ahead; `dailyCap` (0 =
none); `maxMembers` 2–50. Settings are fixed after creation. An account can
host at most 5 open challenges and be in at most 20.

## Scoring

Scores come from `profile_contribution_days` (the profile contribution
graph): only public projects on public repositories count, days are UTC, and
boards update when a member's desktop publishes a new contribution snapshot.
Ranks are competition ranks; nobody wins if the top total is 0.

## API

All answers are `no-store` JSON; writes need a user session.

| Route | What it does |
| --- | --- |
| `GET /api/challenges` | Caller's 30 most recent challenges with boards, plus `options` and `today` |
| `POST /api/challenges` | Create from `{"preset": "week"}` or custom settings |
| `GET /api/challenges/<id>` | Members get the board; anyone else gets a preview without scores |
| `POST /api/challenges/<id>/join` | Join while open and not full (cap checked inside the insert) |
| `POST /api/challenges/<id>/leave` | Leave (not the host) |
| `POST /api/challenges/<id>/delete` | Host only |

Errors are `{"ok": false, "error": "<code>"}`.

## Traps

- The challenge id is the invite secret: 128 random bits (`_ap_uuid`); its
  preview shows no scores.
- A composed dashboard page needs entries in `dashboard_shell.PAGES`,
  `static_routes.DASHBOARD_PAGE_ASSETS`, `BLOCKED_STATIC_HTML_PATHS` and
  `run_worker_first`, and a rebuild with `app/tools/build_dashboard_assets.py`.

## Notices and empty states

Event-only pieces use the shared dashboard components
([dashboard-components.md](dashboard-components.md)) instead of page-local
styles:

- Challenges: the action notice is a success or danger `fm-flash`; an invite
  ("X challenged you to ...") is a blue info flash with a small green **Accept
  challenge** button, like GitHub's update banner. The finished banner (trophy)
  and the upcoming hint are neutral flashes. Signed out, load error and no
  challenges are `fm-blankslate`s. The phase pill is `fm-label` (`Live` is
  `fm-label-success`), card actions are `fm-btn-sm`, and the gold rank-1 colour
  is gone: ranks 1 to 3 are foreground, the rest muted.
- Only **Start in one click** stays green; preset, customize, copy-invite and
  the custom form's submit are neutral, so a page has one green button.
- Hackathon: the "challenge has closed" notice is a warning flash; not joined,
  signed out and load errors are blankslates (**Join the hackathon** is the one
  green button). Card buttons are `fm-btn-sm`; mission, submission and
  milestone pills are `fm-label` with `fm-label-success` for done. Reward and
  trophy icons are muted grey; a beaten challenge's trophy is success green.
