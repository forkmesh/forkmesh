# Repository activity (Pulse, leaderboard, achievements)

Part of [the App docs](README.md). The repository **Insights** tab (linked as
**Activity** from the About box) is a GitHub/Forgejo-style Pulse with a
contribution leaderboard and repository milestones on top.

## Views

The tab has a left sub-navigation (a wrapped row of pills below 768px):

| View | Route | Data |
| --- | --- | --- |
| Pulse (default) | `/<owner>/<repo>/insights?period=1w` | `pulse` |
| Leaderboard | `?view=leaderboard`, all time with `&scope=all` | `pulse`, `pulse?period=all` |
| Contributors | `?view=contributors` | `stats`, `history`, `traffic` (the previous Insights page, restyled) |
| Code frequency | `?view=frequency` | `stats` (52 weekly commit counts) and the period's `pulse.series` |
| Recent commits | `?view=commits` | `history` (latest 60 commits of the selected branch) |

`period` is `24h`, `3d`, `1w` (default) or `1m` and is kept in the address
bar with `history.replaceState`, as are `view` and `scope`. The repository
page's own URL rewrite keeps these three parameters
(`repoInsightsRouteQuery`).

Pulse shows: an overview (active pull requests as merged versus still open,
active issues as closed versus new, and the four counts, each jumping to its
list), the commit sentence ("Excluding merges, N authors have pushed ..."),
a top-committers bar chart with avatars and profile links, the signed-in
reader's progress card, the period's top five, newcomers, and the merged and
opened pull request and closed and opened issue lists. Lists show ten rows and
a "Show all" toggle (`aria-expanded`). Merged and closed states use the
purple `--contribution-pulls` token from the profile contribution graph, open
states `--dashboard-open`; every state is also a text label and an icon.

## The `pulse` read

`GET /api/repo/<owner>/<repo>/pulse?period=<24h|3d|1w|1m|all>[&person=<name>]`

Served by the mirror gateway (`tools/mirror_gateway.py`,
`GitRepository.pulse`). It is not a separately advertised gateway operation:
a repository serves `pulse` exactly when it serves `stats`
(`PULSE_CARRIER_OPERATION`, the same pattern as `branch-trash`), so no
repository configuration, signed operation list or node binary changes. The
app routes it like the other reads: the host fast path
(`repo_fastpath.READ_OPERATIONS`), `REPO_HOST_READ_ACTIONS`,
`HOSTED_API_READ_OPERATIONS` for private core-hosted repositories (a reader
check, then a capability-signed core read; anyone else gets the same
`{"error": "not_found"}` 404 as `stats`), and `urls.REPO_HOST_RE`. A
restricted repository answers it only to a router capability; its public
spellings answer like an unknown repository. A public repository routed
through other mirrors gets `pulse` only from mirrors running this gateway;
anywhere else the page shows "This repository's host does not serve activity
summaries yet" and the other views keep working.

Without `person` the answer holds `since`/`until` (ISO), `summary`
(`authors`, `main`, `all`, `filesChanged`, `additions`, `deletions`,
`measuredCommits`, `partial`, `truncated`), `committers` (top 20 on the
default branch), `series` (hourly for 24h, six-hourly for 3d, daily
otherwise), `pulls.merged|opened` and `issues.closed|opened` (`count`,
distinct `authors`, newest 200 `items`), `leaderboard` (top 50 with
`breakdown`, `previousRank`, `streak`, `achievements`), `newcomers` and
`scoring`. `period=all` returns only the leaderboard. With `person` it returns
that person's rank, score, `totals` now and `before` (at the window start, or
a week ago for all time).

### Where the data comes from

- Commits: `git log --no-merges` of the default branch (author name, email,
  committer time), indexed once and extended incrementally when the new tip
  descends from the indexed one. "All branches" is
  `--exclude=forkmesh/* --branches`, so the ledger branches never count.
- Pull requests: `.forkmesh/pulls/<n>/pull.md` and its `NNNN-<type>.md`
  event files from `forkmesh/pulls` and the live `forkmesh/pull/<n>` branches
  (the same precedence as the badge counts). Merged time is `mergedAt`, closed
  time the `status: closed` event; records without one fall back to the
  newest event. `changes.patch` and `commits.mbox` are never read.
- Issues: `.forkmesh/issues/{open,closed}/<n>/issue-<n>.json` (and legacy
  folders) on the default branch. The closer is the author of the last
  `status: closed` event. An issue is a bug when a label contains bug,
  defect, security or vulnerab.
- Avatars: `.forkmesh/info.json` contributors, else a GitHub noreply email.

### Score

| Activity | Points |
| --- | --- |
| Merged pull request (author, at merge time) | 10 |
| Review of someone else's pull request (once per reviewer and pull request) | 4 |
| Issue closed (the person who closed it) | 3 |
| Commit to the default branch, merges excluded (at most 5 a day) | 1 |
| Comment on an issue or pull request (at most 3 per thread, slash commands excluded) | 1 |

Ties share a rank (1, 1, 3). Rank movement compares with the previous window
of the same length; for all time, with the ranking a week earlier. People are
matched by their case-folded name, so a git author name and a ledger account
only merge when they are spelled the same.

### Humans only

The owner decided (2026-10-21) that only people are ranked. Bots, AI agents
and mirror nodes are left out of the leaderboard, newcomers, top contributors,
streaks and milestones, and their commits are left out of the commit counts,
the "N authors" sentence, line counts and the committers chart; the "How the
score works" note says so. Pull requests and issues they opened still appear
in the lists and the overview counts, but not in the "by N people" counts. A
human commit with a `Co-Authored-By:` agent trailer is credited to the human.

One rule set in `tools/mirror_gateway.py` (`_pulse_is_automated`) decides:

- names (`PULSE_AUTOMATED_NAMES`, whole name, case-insensitive): bot shapes
  (`[bot]`, `-bot`, `github-actions`, `*-actions`, dependabot, renovate), the
  platform's own service identities (`ForkMesh …`, `… Reconciler`), AI agents
  (`Claude` and `Claude Code/Opus/Sonnet/Haiku …`, Codex, ChatGPT and `GPT-…`,
  Gemini, Jules, Copilot, Llama, Devin, Cursor Agent, Aider, OpenHands, Sweep)
  and mirror nodes (`fmnext-*`, `mirror<digits>`, `*-mirror`). Names are
  matched whole, so "Claude Monet" or "Mirror Lake" are people;
- author emails (`PULSE_AUTOMATED_EMAILS`, globs): `[bot]` addresses, the
  platform's `@forkmesh.local`/`.invalid` and `*-reconciler@` identities, and
  the fixed addresses agents commit with (`noreply@anthropic.com`,
  `llama@cloudflare.com`, OpenAI, Devin, Cursor, Jules and Copilot noreply
  addresses);
- the gateway's own node name (`config.node`), the one account kind the
  gateway knows without a lookup;
- `ACTIVITY_EXCLUDE_AUTHORS`: a comma list of globs (case-insensitive,
  `fnmatch`, so a literal `[` is written `[[]`) matched against the author
  name and email, for instance-specific accounts. Set it in the environment of
  the node service that runs the gateway (`/etc/forkmesh/node.env` or a
  systemd drop-in for `forkmesh-mirror-node.service`); the gateway reads it
  each time it computes an answer, so a change shows from the next refresh
  window.

Ledger records carry only an account name, so a pull request or comment
author is judged by name and the configured globs.

A streak is consecutive UTC days with any scored or ledger activity (commit,
pull request opened or merged, review, comment, issue opened or closed). The
current streak counts if the last active day is today or yesterday. A
newcomer is someone whose first recorded activity in the repository falls in
the window.

### Milestones and badges

The platform's achievement badges (`app/src/badges.py`, `/api/badges`) are
reused for art and thresholds; nothing new is stored or awarded. The page
maps per-repository counts to catalog badges and reads their tiers from the
catalog: first merged pull request (`coder`, threshold 1), merged pull
requests (`web_weaver`), reviewer (`six_eyes`), bug squasher (`bug_catcher`,
bug issues closed) and streak (`daily_spinner`, longest streak). Leaderboard
rows show the highest tier reached as 20px medals. The signed-in reader's card
shows rank, points, streak, the milestone closest to its next tier with a
progress bar, and any tier crossed during the window. A crossed tier plays a
small burst around its medal once per browser (localStorage), never with
`prefers-reduced-motion`.

### Cost and caching

The gateway runs on small nodes (1 CPU, 1 GB), so nothing rescans history per
request:

- Per repository, in memory: the commit index keyed by the default-branch
  tip (a 45k-commit history: one `git log` of about 0.5 s and a few MB, then
  only new commits), issue records keyed by blob id and pull records keyed by
  folder tree id (a change re-reads only that record, through two
  `cat-file --batch` calls), and per-commit line counts (an LRU of 4,000).
- Line counts use `git log --numstat --no-renames` with
  `core.bigFileThreshold=256k` (larger files count as changed files, not
  lines), at most 1,000 commits per window and 3 s of work per request. An
  answer whose counts are still incomplete says `partial`, is sent
  `no-store` and is not cached, so the next request continues from the memo.
- Answers are cached in the gateway's bounded response cache per
  repository generation, query and window end. The window end is the request
  time rounded up to 5 minutes (24h), 15 minutes (3d), 30 minutes (1w) or an
  hour (1m, all), so a window advances at most once per bucket and concurrent
  misses share one computation. A `person` answer is computed from the same
  index (about 10 ms).
- Measured on a desktop: a 45,560-commit repository: first answer 1.1 s, later
  periods 20-30 ms; a 62,808-commit repository with 1,100 commits a
  week: 3.7 s for a week's first answer, then under 0.1 s.

The dashboard code is a lazy chunk (`09-repo-activity.js`,
`dashboard-page-repo-activity.js`) loaded on the first Insights view, so other
pages do not download it. It reserves skeleton heights while loading and
keeps the page free of horizontal scroll at 375px.

Tests: `app/tests/test_mirror_gateway_pulse.py`,
`app/tests/test_dashboard_repo_activity.py`,
`app/tests/test_repo_read_route_operations.py`,
`app/tests/test_hosted_core.py`, `app/tests/host/test_repo_fastpath.py`,
`app/browser_tests/tests/dashboard-insights-contributors.spec.js`.
