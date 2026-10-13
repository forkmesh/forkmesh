# Reddit, the Reddit scout special agent

The Reddit special agent (`.forkmesh/teammates/reddit-agent.json`) finds the
Reddit threads where ForkMesh can honestly help someone and hands the operator
a short brief. The operator writes and posts every word. It was added on
2026-10-02 to bring in users from the subreddits where developers ask for
what ForkMesh does: losing code to an outage or takedown, leaving a hosted
forge, mirroring repositories, and running several coding agents on one
repository.

| Piece | Path |
| --- | --- |
| Special agent definition | `.forkmesh/teammates/reddit-agent.json` |
| Run playbook | `.forkmesh/shortcuts/scout-reddit-for-users.md` |
| Skill | `.forkmesh/skills/reddit-engagement.skill` |
| Reddit tool (read-only) | `tools/reddit_scout.py` |
| Watched subreddits, topics and searches | `tools/reddit_watchlist.json` |
| Profile tool (the one writer) | `tools/reddit_profile.py` |
| u/forkmesh display name and About text | `tools/reddit_profile.json` |
| Portrait | `desktop/resources/agent-ui/icons/teammate-reddit.png` (avatar key `reddit`) |
| Contract tests | `app/tests/test_reddit_scout_special_agent.py`, `app/tests/test_reddit_profile.py` |

Like every special agent it is checked in paused: daily at 09:30, with
`schedule.active` false until the operator turns it on from the Special
agents page.

## Why it never posts

Unlike Hearth on Discord, this agent has no way to post. That is on purpose:

- Reddit's Content Policy treats posting "primarily for the purpose of
  driving traffic" to your own product as spam, and expects you to disclose
  affiliation. A bot account that comments about its own product is the
  pattern Reddit's spam filters and moderators remove fastest, and a
  shadowban takes the account's whole history with it.
- Since November 2025 the Responsible Builder Policy puts every new Data API
  app behind manual approval, and bots must say they are bots. A bot that
  answers questions about its owner's product would not pass that review.
- Many subreddits ban AI-written posts and comments, and readers spot them.
  The account that mentions ForkMesh is the operator's own reputation.

The one exception is the agent's own account page (below): keeping the
u/forkmesh profile up to date is not a post, comment or message, and puts no
words in anyone's thread.

So the agent does the slow part (finding the moments, reading the thread and
the rules, checking evidence and the account's balance) and the human does the
part only a human can do well there.

## Code map

`tools/reddit_scout.py` has five commands. None of them writes to Reddit.

- `scan` reads new posts in every watched subreddit (and, with `--search`, the
  watchlist's Reddit-wide searches), keeps posts inside the window, matches
  them against the watchlist topics and gives each one advice:
  - `showcase`: the title is a sanctioned project slot (a "New Project
    Megathread" or a weekly showcase thread) in an answer or showcase
    subreddit;
  - `skip` (not printed): no topic matched, or the post is locked or stickied;
  - `listen`: the post already mentions ForkMesh, the subreddit's stance is
    listen, or the title is someone else's launch ("I built", "introducing",
    a version number);
  - `late`: older than 48 hours;
  - `answer`: the person is asking (a question mark in the title, or "looking
    for", "alternatives to", "is there a", "how do I" in the opening text);
  - `join`: a discussion on a topic with no direct ask.

  Candidates are ranked by topics matched, asking, freshness, showcase slots,
  and a penalty for threads with 40 or more comments.
- `thread <link>` reads one thread: comment count, other commenters, whether
  the original poster got a reply and followed up, which alternatives were
  already named (Gitea, Forgejo, GitLab, Codeberg, Radicle and others), and
  whether ForkMesh already came up.
- `rules <subreddit>` reads the subreddit's rules and flags self-promotion,
  AI-content, showcase-thread and disclosure rules. Without OAuth the rules
  endpoint answers 403 here (2026-10-02); the tool then says so. A web fetch
  does not help either: old Reddit's rules pages send logged-out readers to a
  login page (2026-10-06). The rules come from the Reddit desk instead (below).
- `account --user <name> --subreddit <name>` reads the operator account's
  public history (up to 100 items) and counts contributions in the last 30
  days and those that mention ForkMesh. A post that mentions ForkMesh counts
  as a showcase and a comment as a mention; a comment in a ForkMesh thread
  that does not itself mention ForkMesh counts as help. It then says whether a
  mention or showcase fits in that subreddit now:
  - the 9:1 habit: one more promotional contribution must keep the share at
    or under 10%, so a fresh account needs nine helpful contributions first;
  - 7 days between mentions (of either kind) in one subreddit;
  - 30 days between showcases in one subreddit.

  `REDDIT_USERNAME` sets the default user.
- `link --subreddit <name> [--path /]` builds a `forkmesh.com` link tagged
  `utm_source=reddit&utm_medium=social&utm_campaign=reddit_scout&utm_content=<subreddit>`.
  It refuses `/docs/` and `/blog/` paths, because attribution never captures
  those static pages ([../attribution.md](../attribution.md)); evidence links
  to docs go out untagged.

`reddit_scout` was added to `ATTRIBUTION_ALLOWED_CAMPAIGNS` in
`app/wrangler.toml` and `DEFAULT_ALLOWED_CAMPAIGNS` in `app/src/attribution.py`
so tagged visits show under their own campaign on `/admin/acquisition` instead
of `other`.

### Reading modes

- **atom** (default): Reddit's public Atom feeds (`/r/<sub>/new/.rss`,
  `/search.rss`, `/comments/<id>/.rss`, `/user/<name>/.rss`). On 2026-10-02
  these answered 200 from this box while the public `.json` endpoints and
  `oauth.reddit.com` answered 403. Feeds carry no score, comment count, lock
  or sticky state, so `scan` cannot see those in this mode. The limit is
  tight: the tool waits 6 s between requests and, on a 429, backs off 10 s,
  then 20 s, at most 3 tries, then reports the source as refused. A socket
  or TLS read timeout is retried the same way; a source that keeps timing
  out is reported as refused instead of ending the whole scan (2026-10-03).
- **oauth**: when `REDDIT_CLIENT_ID` and `REDDIT_CLIENT_SECRET` are in the
  environment or the fleet `secrets.env`, the tool takes an app-only
  (`client_credentials`) token and reads the same listings as JSON from
  `oauth.reddit.com`, 1 s apart. App-only tokens cannot post. A new app needs
  Reddit's approval under the Responsible Builder Policy, so register it as a
  read-only research tool if the operator wants this mode.

The user agent is `linux:com.forkmesh.reddit-scout:v1.0
(+https://forkmesh.com)`, the platform:app-id:version form Reddit asks for.

## Watchlist

`tools/reddit_watchlist.json` holds the subreddits, the topic patterns with
the public page that backs each one, and the Reddit-wide searches. Stances:

- **answer**: people ask for help here; a disclosed mention can fit when the
  rules allow it. `selfhosted`, `git`, `github`, `opensource`, `devops`,
  `homelab`, `DataHoarder` (archiving code fits ForkMesh's preservation
  angle), `ClaudeCode` and `ChatGPTCoding` (running several agents on one
  repository).
- **showcase**: subreddits made for sharing projects: `coolgithubprojects`,
  `SideProject`.
- **listen**: read for pain points and wording, never suggest a mention:
  `gitlab` (a product's own community), `programming`, `ExperiencedDevs` and
  `linux` (large, strict about promotion).

The stances are a starting point. Rules change, so the agent reads each
subreddit's rules before it suggests anything there and reports when a stance
looks wrong. On 2026-10-02 `r/selfhosted` said new projects younger than three
months may only be shared in its weekly "New Project Megathread", which is
why `scan` treats that thread as a showcase slot.

Topics and the page each one points at:

| Topic | Evidence page |
| --- | --- |
| `forge-alternative` | `/docs/category-comparisons/` |
| `outage-preservation` | `/docs/launch/github-outage-survival-kit/` |
| `mirroring` | `/docs/availability-proof/` |
| `decentralized` | `/docs/protocol/` |
| `portable-collaboration` | `/docs/offline-sync/` |
| `agent-orchestration` | `/docs/qt-client/` |
| `forge-sync` | `/docs/github-sync/` |

On 2026-10-04 three patterns were widened after the 2026-10-02 run missed two
real asks in r/git that a manual read of every title found: "Manage git
worktrees better" (worktree sprawl from AI agents) and a GitHub backup tool
storing snapshots in the user's own S3 bucket. `agent-orchestration` now also
matches worktree-management wording, "agents" within a sentence of
"worktree", and "15+ Claude Code sessions" or "sessions in parallel";
`outage-preservation` matches "GitHub backup(s)", "backing up GitHub" and
"lost my code"; `mirroring` matches `clone --mirror`. Against 73 live posts
in r/git, r/github and r/selfhosted only those two changed.
`test_topics_catch_the_asks_the_first_runs_missed` holds both the catches and
titles from that window that must stay unmatched.

## How it decides

- Help first. Most good replies do not mention ForkMesh; they still count, and
  they are what lets the account mention it later without looking like spam.
- Mention ForkMesh only when it fits what the person asked, the rules allow
  it, the thread is not someone else's launch, and `account` says ok. The
  mention is one line inside real help, with "I work on ForkMesh" in it.
- Recommend the alternative that fits better when one does. Being the person
  who says "for that, Forgejo is simpler" is what makes the next ForkMesh
  mention believable.
- No paste-ready replies. The brief has facts, evidence links and a
  three-point outline. A draft is written only when the operator asks for one
  in the run prompt and the subreddit's rules allow AI-written content, and it
  is labelled as a draft to rewrite.
- Never: the same text in several subreddits, a bare link, a mention in a
  listen subreddit, a reply on a locked thread, a second account, asking for
  upvotes, or messaging people.

## The Reddit desk in Marketing

The Marketing package's Reddit desk (`/dashboard/marketing`, 1.6.0) is where
the operator and the team log the threads a brief points at, the subreddit's
rules, the two checks (rules read, affiliation disclosed) and the address of
the reply they posted themselves. A thread is marked replied only with both
checks and the reply's address; the weekly marketing report counts what was
logged, replied and skipped.

Reddit answers ForkMesh's servers with 403 for the data the desk needs, so
the ForkMesh browser extension (2.3.0) reads it from the operator's own
browser instead: the watched subreddit listings, searches for a watched query,
threads in a watched subreddit and watched subreddits' rules pages, only when
the operator opens them. The popup's **Send to Reddit desk** logs those
threads as to review and saves the rules; the extension never ticks the checks
and never posts, votes or messages. The desk's watch list starts from this
page's watchlist.

Monitored threads (Marketing 1.15.0, extension 2.8.0): a desk thread can be
monitored, so the operator sees the views, points and comments of a thread
they posted in, and of their own comment there, over time. Reddit shows view
counts only to the account that posted, so the extension reads them in the
operator's signed-in tab, when they open the thread or press **Check now**
(an optional six-hour check does the same). The web and desktop desks both
show them, on a **Monitored** tab, and both can add a thread to monitor.

Where to post next, what to talk about, and the walk (Marketing 1.32.0,
extension 2.15.0, 2026-10-08): the user asked the desk for clues rather than
only a log. The desk now ranks every watched subreddit by its own data (open
threads waiting, replies that landed, rules on file, how long since anything
was logged there) and names what blocks a post: rules not read, or a reply
there inside the 7 days this page asks between mentions in one subreddit. It
also lists topics -- the watch list's searches, plus the two-word phrases
that come back in two or more threads logged in the last 90 days -- each a
link to Reddit's newest results and the threads behind it. Nothing is
invented: a topic with no threads behind it says so.

**Walk the watch list** has the extension do one round of the watch list
itself in a single tab, because the desk only ever filled when someone
browsed Reddit by hand. `reddit-walk.js` paces each page the way a reader
does -- scrolling in short bursts with pauses, drifting the pointer onto a
few posts -- because Reddit loads listings lazily and renders parts of a
card on hover, and because those pauses keep one walk's reading rate near a
person's. It clicks nothing, types nothing, votes nothing and posts nothing,
and it is not a way past Reddit's bot check: it spoofs no header and runs in
the operator's own signed-in browser, so a page Reddit refuses stays unread.
`reddit.js` still does all the reading, so its promise never to touch a page
is unchanged.

Reddit's own rate limit on the channel refresh (2026-10-08): the Marketing
Reddit **channel** (not the desk) refreshes from the public Atom feed, and
Reddit answers ForkMesh's servers 429 often enough that the channel showed a
bare `channel_http_429`. It is now named `reddit_rate_limited`, retried once
on `old.reddit.com`, and refused for 15 minutes after a limit so pressing
Refresh again cannot make it worse. The feed carries no counts anyway, so
the message points at **Update**, which reads them in the browser.

How the scout gets the rules (decided 2026-10-06): pasted into the run
prompt. The desk's rules are behind a member session (`GET
/api/marketing/<org>/reddit-rules`) and the agent has none; giving it one
would put a person's credential in an agent shell, which the least-privilege
rule rules out, for data that is public anyway. The operator presses **Copy
rules for the scout** on the desk (Marketing 1.6.1) and pastes the Markdown
into the run prompt. The agent treats it as the rules as read on the date each
section shows and says when a section is older than 30 days. A subreddit on
the "Not read yet" line gets no mention until its rules are on the desk: this
box cannot read them itself, because old Reddit's rules pages send logged-out
readers to a login page and the rules API answers 403 (2026-10-06). No mention
is suggested anywhere until the pasted rules cover at least r/selfhosted,
r/ClaudeCode, r/github, r/git, r/ChatGPTCoding and r/devops. Read access to
the desk is worth revisiting only if a read-only, organization-scoped agent token exists.
Details:
[Marketing README](../../marketplace/packages/marketing/README.md#reddit-desk)
and the extension's README, section "Reddit desk for Marketing"
(`marketplace/packages/browser-extension/content/extension/README.md`).

## The u/forkmesh profile

Added 2026-10-06 because the user asked for the Reddit agent to be able to
update https://www.reddit.com/user/forkmesh/. `tools/reddit_profile.json`
holds the profile's display name (`title`, up to 30 characters) and About
text (`public_description`, up to 200); `tools/reddit_profile.py` keeps the
live profile in line with it:

- `show` reads the profile as Reddit shows it. Without credentials it reads
  the profile's Atom feed (`/user/forkmesh/.rss`), whose `subtitle` is the
  About text; the display name is not in the feed, so it reads as unknown.
  `about.json` answers 403 here (2026-10-06).
- `plan` lists what differs between the file and the live profile.
- `apply` lists the changes; `apply --yes` saves them and reads the profile
  back. It first checks that the token signs in as the account in the file.

How the save works, as PRAW's `UserSubredditModeration.update` does it: a
profile is the subreddit `u_forkmesh`. The tool reads its full settings from
`/r/u_forkmesh/about/edit`, renames the fields that come back under other
names (`subreddit_id` to `sr`, `default_set` to `allow_top`,
`header_hover_text` to `header-title`, `language` to `lang`,
`content_options` to `link_type`, `subreddit_type` to `type`), sets the two
fields and posts it all to `/api/site_admin`, because a setting left out of
that call is reset. Avatar, banner and social links are not touched; the
operator sets them on Reddit.

Write access is not set up yet (2026-10-06): there are no Reddit keys in the
fleet `secrets.env`, so `apply` stops with the setup steps. To set it up,
sign in as u/forkmesh, create a "script" app at reddit.com/prefs/apps (new
apps need approval under the Responsible Builder Policy; describe it as
editing the account's own profile), and put `REDDIT_CLIENT_ID` and
`REDDIT_CLIENT_SECRET` in the fleet `secrets.env` with either a
`REDDIT_REFRESH_TOKEN` (scopes `identity`, `read`, `modconfig`; preferred,
revocable) or `REDDIT_USERNAME` and `REDDIT_PASSWORD` (a script app's
password grant, which fails on an account with two-factor sign-in). The
same client id and secret switch the scout to oauth reading. The save path
has only run against recorded responses in the tests, so the first live
`apply --yes` is the real check; it reports what Reddit shows afterwards.

The agent runs `plan` every run, and edits the file and runs `apply --yes`
only when the operator asks for new profile text in the run prompt
(playbook section 4b).

## Measures

- The account's 30-day balance from `account` (contributions, share that
  mention ForkMesh).
- The subreddit's members, weekly visitors and weekly contributions on the
  Marketing Reddit channel, read by the browser extension's **Update**
  (Marketing 1.12.0, which shows the steps until they are done); the
  channel's Refresh only imports posts from the Atom feed.
- Sessions and signups for `utm_campaign=reddit_scout` on `/admin/acquisition`,
  by `utm_content` (subreddit).
- Opportunities briefed, and how many the operator acted on.
- Pain points: things people asked for that ForkMesh does not do yet.

Judge a change after two to four weeks; a single thread proves little.

## Security and policy

- The scout is read-only by construction: the only POST it makes is the
  OAuth token request, and app-only tokens carry no user and cannot write.
  The contract tests fail if a submit, comment, vote or message endpoint
  appears in it.
- `tools/reddit_profile.py` makes one more POST, to `/api/site_admin` for
  `u_forkmesh`, and only after the token proves it is u/forkmesh. Its tests
  fail if it gains any other write endpoint (submit, comment, vote, message,
  uploads, preferences).
- Reddit text is untrusted. The tool labels its excerpts as such, and the
  agent reports prompt-injection attempts instead of acting on them.
- Credentials come from the environment or the fleet `secrets.env` and are
  never printed.
- Permissions: `read-repositories`, `external-tools` and `run-unattended`.
  `run-unattended` lets a scheduled run start in its own Auto mode and run the
  tool without a person approving each read; it is safe because the only
  thing the agent can change on Reddit is the profile text checked in to
  `tools/reddit_profile.json`. No code, issue, merge or payout authority.

## Research sources (2026-10-02)

- Reddit Content Policy, spam rule:
  redditinc.com/policies/content-policy
- Reddit Help, "What constitutes spam?" (support.reddithelp.com)
- Responsible Builder Policy and the end of self-service API keys
  (November 2025): r/redditdev announcement; summaries at
  molehill.io/blog/reddit_killed_self-service_api_keys_your_options_for_automated_reddit_integration
  and redditapis.com/reddit-responsible-builder-policy
- Self-promotion practice, the 9:1 habit and disclosure:
  redship.io/blog/reddit-self-promotion-rules,
  rankcow.com/blog/reddit-self-promotion-rules
- API rules and user agent format: github.com/reddit-archive/reddit/wiki/API
- r/selfhosted "New Project Megathread" rules, read from the thread on
  2026-10-02
