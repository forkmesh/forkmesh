# Chat member activity line

The web chat's member list (`/dashboard/chat`, [web-chat-layout.md](web-chat-layout.md))
shows one muted line under each person's name with up to five figures for the
past year: pull requests opened, commits, issues opened, reviews, and how long
ago their last contribution was. Hovering the line shows the full sentence
("Past year, public repositories: 3 pull requests opened, 41 commits, ...,
active on 12 days, last contribution 2 days ago"); screen readers get the same
sentence instead of the bare numbers. A figure that is zero is left out, and a
person with no public activity in the window gets no line at all, so their row
stays one line high.

## API

`GET /api/chat/member-stats?names=alice,bob`

```json
{
  "ok": true,
  "window": {"from": "2026-01-16", "to": "2027-01-15", "days": 365},
  "members": [
    {"name": "alice", "pulls": 2, "commits": 6, "issues": 3, "reviews": 1,
     "activeDays": 2, "lastActive": "2027-01-14"}
  ]
}
```

- At most 50 names per request (`400 too_many_names`); names are lowercased,
  de-duplicated, and anything that is not a valid account name is dropped.
  Any other query parameter is `400 invalid_query`.
- Only members of the room are answered. The dashboard chat is the public
  room every active account belongs to, so a member is an account that passes
  `_is_public_roster_member` (a user account, active, profile not private), the
  same rule as the member count and `/api/accounts/users`. Unknown names,
  private profiles, suspended accounts and node identities are left out of
  `members`, not reported as zero.
- Humans only: bots, AI agents, platform service names and mirror nodes are
  never looked up. The pattern follows the repository Pulse rule set
  ([repository-activity.md](repository-activity.md)) adapted to account names
  (for example `*-bot`, `forkbot`, `agent`, `claude-code`, `codex`, `gpt-*`,
  `copilot*`, `fmnext-*`, `*-mirror`); first names such as Jules or Devin are
  kept. `ACTIVITY_EXCLUDE_AUTHORS`, the per-instance glob list Pulse already
  reads, is honoured here too.

## Where the numbers come from

The figures are the same rows the public profile contribution graph draws
from: `profile_contribution_days`, joined to `profile_contribution_projects`
and `repositories`. Only a project marked public on a repository that is not
private counts, and a day counts only once the project's history is verified
back to it (`verified_from`), exactly as the profile graph requires. A project
mirrored by several nodes is counted once, from its most recent capture. So a
viewer only ever sees totals for activity they could already read on that
person's public profile, and private-repository work never shows. Pull
requests are counted when opened; the contribution snapshot does not carry
merges, so the line does not claim them. Issues reported through a canonical
tracker that is not itself a contribution snapshot are not included.

## Cost and caching

- One request covers every row the reader can see: rows register with an
  `IntersectionObserver` on the member pane (160px margin) and are flushed
  after 120 ms as one sorted batch of up to 50 names; another batch follows
  only if more rows scrolled into view meanwhile. Results are kept in the page
  for 5 minutes (1 minute after a failed request), so the list's re-render
  every minute and every presence change reuse them without a request.
- On the server a batch costs at most two D1 statements, whatever its size:
  one `users` read by blind index (`user_bi IN (...)`) and one grouped read of
  the contribution rows (`subject_user_bi IN (...)`, indexed by
  `idx_profile_contribution_days_subject_day`) for the 365-day window. There is
  no per-row query.
- Each person's answer, including "not a member", is memoised in the isolate
  for 5 minutes, at most 2,000 entries, and dropped early when this isolate
  writes contribution or repository rows (the `contributions` watch generation
  the profile view uses) or when the UTC day changes. The whole response is
  also stored in the edge cache under the sorted name list and the day, and
  sent with `Cache-Control: public, max-age=300`, because it is the same for
  every reader.

## Layout

The name and the activity line sit in a column next to the avatar; the line
is 11px muted text with 11px stroke icons (lucide `git-pull-request`,
`git-commit-horizontal`, `circle-dot`, `message-square`, `clock`), drawn inline
because World also loads `dashboard-chat.js` without the icon library. The line
is one 14px-high wrapping flex row with its overflow hidden, so a figure that
does not fit whole drops out of sight instead of being cut in half (the
tooltip still lists it), and a row is never more than two lines.
Rows grew from a fixed 40px to a 40px minimum with 4px padding. Colours are the
`--dc-muted` token, so the line follows the light and dark themes.

| Path | Responsibility |
| --- | --- |
| `app/src/chat_member_stats.py` | Route, membership filter, batched reads, memo |
| `app/src/app_runtime.py` | Routes `/api/chat/member-stats` |
| `app/public/dashboard-chat.js` | Batching, page cache, row rendering, phone drawer |
| `app/public/dashboard/partials/views/chat.html` | Row and drawer styles |
| `app/tests/test_chat_member_stats.py` | API shape, privacy, caching, frontend contract |
