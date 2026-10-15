# Hackathon

How a ForkMesh hackathon campaign works. Anything worth money is gated on the
referral ledger, every reward is a fixed threshold, there are no random draws,
and sharing earns nothing.

| Area | Where |
| --- | --- |
| Policy (points, missions, rewards, judging, config validation) | `app/src/hackathon.py` |
| Routes, storage, admin page | `app/src/hackathon_runtime.py` |
| Share cards | `app/src/og_card.py` (`render_hackathon_card`) |
| Participant HQ | `app/public/dashboard/partials/views/hackathon.html` |
| Public pages | `www/public/hackathon/leaderboard.html`, `www/public/hackathon/projects.html` |
| Schema | `app/migrations/0182_hackathon.sql` |

## What /hackathon shows

`/hackathon` and `/hackathon/rules` are the current campaign's pages; they are
static templates in `www/templates/pages/`. When a campaign ends, its pages
move under `/hackathon/archive/<slug>` with an "Archived" banner, and
`/hackathon/archive` lists every finished one.

| Campaign | Dates (UTC) | Pages |
| --- | --- | --- |
| Plugin Race | 19 Oct – 18 Dec 2026 | `/hackathon`, `/hackathon/rules`; engine in [docs/app/plugin-race.md](../../docs/app/plugin-race.md) |
| September Revenue Contest | 15 Sep – 15 Oct 2026 | `/hackathon/archive/revenue-contest-2026`, `/hackathon/archive/revenue-contest-2026-rules`; continued as the [affiliate program](../../docs/app/affiliates.md) |

The archive pages are routed to the website by `WWW_HACKATHON_ARCHIVE_PAGES` in
`app/src/static_routes.py`; add a new archive slug there. The Plugin Race
does not use the points engine below, because its finish line is marketplace
ratings.

## Lifecycle

A campaign is a `hackathons` row keyed by slug with status `draft`,
`registration`, `live`, `judging`, or `closed`. A `draft` campaign 404s on
every participant page and API, so unfinished campaigns can ship safely. Only an
administrator advances status.

## Points

`hackathon_points` is append-only with primary key
`(hackathon_slug, account_bi, kind, ref)`, which prevents double payment. Kinds:
`joined`, `verified`, `referral_joined`, `referral_qualified`, `first_mirror`,
`pull_accepted`, `submission`, `referral_post`. No kind is client-reportable.

A referral is *joined* when the invitee verifies email and *qualified* when the
referral ledger credits it. Anything of value reads qualified.

## Rewards

Missions and milestones award `pro_months`, `supporter_months`, a `badge`, or
nothing; cash follows judged tracks only. Grants are claimed once in
`hackathon_reward_grants` and projected into `role_grants` with
`scope_type='promotion'`, stacking months.

## Teams, submissions, and judging

A squad is an invite tree; a team (up to five) submits one project per track.
Submissions must name a public repository first hosted in the campaign window;
doubtful ones are flagged for review, not rejected. Only `valid` projects are
public. Judges score 1 to 5 on four criteria; finalizing ranks each track and
grants the prize table once.

Challenge links fix their target when sent, need a strictly higher score, and
pay no points. Share presses are measured, never paid, and nothing is sent on
anyone's behalf.

## Administration and privacy

`/admin/hackathon` and `/api/admin/hackathon` require a live `is_admin`
session. Public reads are edge-cached and bounded. No contacts are collected;
public pages show only public usernames.
