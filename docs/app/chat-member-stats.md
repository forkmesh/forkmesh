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

Two sources, combined per person.

**The repositories hosted on the instance.** The same ledger the repository Insights
tab's Pulse and leaderboard read ([repository-activity.md](repository-activity.md)):
commits on the default branch (merges excluded), pull requests opened,
issues opened, and reviews of someone else's pull request, over the trailing
365 days. Each repository answers `GET /api/repo/<owner>/<repo>/pulse?view=people`
with every human's figures, an activity-day bitmap and the time of their last
activity (`view=people` takes no `period` or `person`; the gateway caches it
per hour).

Pulse identifies a person by the case-folded name on their commits and ledger
records. The sweep maps those identities to accounts in two ways:

- **Verified email.** For a repository hosted on the instance's core nodes,
  the sweep asks a core for `pulse?view=people&emails=1` with a router
  capability (`hosted_git.hosted_people_with_emails`). That answer adds up to
  five commit author addresses per identity. The gateway refuses `emails` on
  every public spelling of the read (404, before its answer cache), marks the
  answer `private, no-store`, and neither the Worker router
  (`_https_mirror_request_query`, `edge_routing.masked_target_url`) nor the
  host fast path forwards the parameter from a visitor. The sweep blind-indexes
  each address and finds accounts by `users.email_bi`, then decrypts each
  match and accepts it only when that account's email is verified, equal to
  the address, and the account is an active person. Addresses are only
  compared; nothing about them is stored.
- **Account name.** Otherwise the identity counts for the account spelled the
  same (`AliceW` and `alicew` match, `Jane Doe` and `jane` do not).

A verified email wins over the spelling. An identity whose addresses belong
to two different accounts is ambiguous, so only its spelling counts. Several
identities of one person, such as a full-name commit identity and the
account-name pull request identity, describe different work: their figures
add up and their days merge. One `users` query per repository covers both
kinds of match. A repository whose core does not answer is read through the
public `pulse?view=people` instead, and only names are matched.

Pulse's humans-only rules apply first (bots, AI agents, the platform's service
identities, mirror nodes, `ACTIVITY_EXCLUDE_AUTHORS`), and the member line's
own account-name rule applies again to the matched account.

The repository Insights leaderboard keeps matching by name only. It is
computed by the gateway from public data for any reader, and matching
addresses needs the instance's account emails, which only the app holds;
doing it there would mean sending account data to the gateway or publishing
an address-to-account map.

Which repositories count:

- only repositories hosted on this instance (catalog `source` `hosted-web`);
  a catalog entry that mirrors a repository hosted elsewhere never counts;
- only public ones: public, not private, retained; a repository made private
  or deleted stops counting at once, because the read joins `repositories`
  and requires `is_private=0`;
- one repository per fork family: a fork whose parent is also a counted
  repository on the instance is skipped (commits in a fork do not count,
  as on GitHub), and of several repositories with the same root commit only
  one counts (not a fork first, then the longest history). A hosted fork of
  a project hosted elsewhere counts as that project's copy here.

A cron phase, `member-activity` (every 15 minutes), reads the decrypted public
catalog the catalog pages already memoise, drops the rows of repositories that
stopped counting, and refreshes up to six repositories whose figures are older
than three hours, one gateway read each. Only people who are accounts on the
instance are stored, one `member_repo_activity` row per repository and
account; `member_activity_repos` records when each repository was last read.
A repository whose host does not answer keeps its previous figures until the
next attempt. Figures are therefore up to about three hours old, plus the
time the sweep needs to reach every repository.

**The public profile contribution rows** (`profile_contribution_days`, joined
to `profile_contribution_projects` and `repositories`), the rows the profile
contribution graph draws from. Only a project marked public on a repository
that is not private counts, and a day counts only once the project's history
is verified back to it (`verified_from`), exactly as the profile graph
requires. A project mirrored by several nodes is counted once, from its most
recent capture.

**Combining them.** The two sources often describe the same work (a profile
project is usually one of the instance's repositories), so each figure is the
larger of the two, never their sum: pull requests, commits, issues and reviews
each take the maximum, active days the larger day count, and the last
contribution the later date. Repositories are summed among themselves, which
is safe because only one repository of a fork family counts. Pull requests
are counted when opened; merges are not shown. Private-repository work never
counts from either source.

## Cost and caching

- One request covers every row the reader can see: rows register with an
  `IntersectionObserver` on the member pane (160px margin) and are flushed
  after 120 ms as one sorted batch of up to 50 names; another batch follows
  only if more rows scrolled into view meanwhile. Results are kept in the page
  for 5 minutes (1 minute after a failed request), so the list's re-render
  every minute and every presence change reuse them without a request.
- On the server a batch costs at most two D1 statements, whatever its size:
  one `users` read by blind index (`user_bi IN (...)`) and one read of both
  sources (`UNION ALL`): the grouped contribution rows
  (`subject_user_bi IN (...)`, indexed by
  `idx_profile_contribution_days_subject_day`) for the 365-day window, and
  the stored repository rows (`user_bi IN json_each(?)`, indexed by
  `idx_member_repo_activity_user`). The names are bound once as a list and
  once as one JSON value, so 50 names use 53 bound parameters. There is no
  per-row query, and the request path never reads a repository: only the
  cron phase does.
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
| `app/src/chat_member_stats.py` | Route, membership filter, batched reads, memo, repository sweep |
| `tools/mirror_gateway.py` | `pulse?view=people` (`GitRepository.pulse_people`), `emails=1` for a capability |
| `app/src/hosted_git.py` | `hosted_people_with_emails`: the capability-signed core read |
| `app/src/entry.py` | `member-activity` cron phase |
| `app/migrations/0281_member_repo_activity.sql` | `member_repo_activity`, `member_activity_repos` |
| `app/src/app_runtime.py` | Routes `/api/chat/member-stats` |
| `app/public/dashboard-chat.js` | Batching, page cache, row rendering, phone drawer |
| `app/public/dashboard/partials/views/chat.html` | Row and drawer styles |
| `app/tests/test_chat_member_stats.py` | API shape, privacy, sources, sweep, caching, frontend contract |
| `app/tests/test_mirror_gateway_pulse.py` | The people view |
