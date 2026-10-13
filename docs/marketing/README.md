# Marketing and Blogger, the marketing special agents

ForkMesh marketing is run by two special agents. On 2026-10-04 the old
Marketing special agent, which spent every run writing the weekly changelog
blog post, was split in two:

- **Marketing** (`.forkmesh/teammates/marketing.json`) became the marketing
  manager. It owns every part of marketing: positioning, the funnel and its
  north-star metric, campaigns and launches, channels, the content calendar,
  the experiment backlog, the Ads manager, website and onboarding conversion,
  and the weekly marketing report. It directs the other outward-facing special
  agents instead of doing their work.
- **Blogger** (`.forkmesh/teammates/blogger.json`) took over the blog. It runs
  the same changelog playbook Marketing ran, on Claude Opus 5.5 through Claude
  Code, and inherited Marketing's eight self-improvement notes, all of which
  were about publishing blog posts.

| Piece | Path |
| --- | --- |
| Marketing definition | `.forkmesh/teammates/marketing.json` |
| Marketing playbook | `.forkmesh/shortcuts/run-the-marketing-program.md` |
| Marketing skill | `.forkmesh/skills/marketing-management.skill` |
| Living marketing plan (written by Marketing) | `docs/marketing/plan.md` |
| Blogger definition | `.forkmesh/teammates/blogger.json` |
| Blogger playbook | `.forkmesh/shortcuts/write-changelog-blog-post.md` |
| Blogger portrait | `desktop/resources/agent-ui/icons/teammate-blogger.png` (avatar key `blogger`) |
| Acquisition data | `/admin/acquisition`, [../attribution.md](../attribution.md) |
| Campaigns, keywords, Ads manager, channel metrics, posting to X | `marketplace/packages/marketing/` |
| Herald, the ready X post | [x-ready-post.md](x-ready-post.md) |
| Contract tests | `app/tests/test_teammate_documentation_cadence.py` |

Both are checked in paused, like every special agent: Marketing weekly on
Monday at 09:30, Blogger weekly on Monday at 11:00, so Monday's marketing
review can brief Blogger before the post is written. `schedule.active` stays
`false` until the operator turns them on from the Special agents page.

## Who owns what

Marketing hands work to the special agent that owns it with a closing-summary
line `@Name: message` (see `teammateMessage` in `desktop/src/TeammateStore.cpp`).
A special agent whose name contains a space is addressed by its id, so the Y
Combinator News agent is `@y-combinator-news-agent`.

| Work | Owner |
| --- | --- |
| Strategy, positioning, funnel, campaigns, launches, channel plan, experiments, Ads manager, landing and comparison pages, onboarding copy, email drafts, the weekly report | Marketing |
| Blog posts, changelog stories, their artwork, metadata and tests | Blogger |
| The next X post, its image, always ready on the X channel page | Herald ([x-ready-post.md](x-ready-post.md)) |
| Reddit threads and subreddit research | Reddit ([../community/reddit.md](../community/reddit.md)) |
| Hacker News launch readiness | Y Combinator News (`www/docs/launch/y-combinator-news-readiness.md`) |
| The Discord community | Hearth ([../community/hearth.md](../community/hearth.md)) |
| Partners, leads, investor contacts, outreach | Sales |
| Security and privacy wording | Sentinel ([../security/sentinel.md](../security/sentinel.md)) |
| Bugs and friction found while walking the funnel | Quality |

The channel-owning special agents list their channels in `marketingChannels`
(Reddit `reddit`, Hearth `discord`, Y Combinator News `hacker-news`,
Blogger `blog`, Herald `x`), so each shows on its channel's page in the Marketing package
([../desktop/agents-ui.md](../desktop/agents-ui.md), "Special agents on
Marketing channel pages").

## Research behind the marketing manager

The manager's charter came from a web review on 2026-10-04 of how marketing
managers at developer-tool companies run growth:

- **Role.** Developer-tool product marketing owns positioning, messaging and
  go-to-market, and how developers understand, evaluate and adopt the product,
  across acquisition, activation and engagement; for product-led products the
  copy starts from the user's problem, not the feature list
  ([Indeed PMM description](https://ca.indeed.com/hire/job-description/product-marketing-manager),
  [Google Cloud developer programs PMM](https://careers.google.com/jobs/results/90964137302991558-product-marketing-manager/),
  [Coursera career guide](https://www.coursera.org/articles/product-marketing-manager)).
- **Channels.** Docs-led growth, technical content (tutorials, short
  screencasts, integration guides), community, open-source seeding,
  integrations and partners, events, and launches on Hacker News (Show HN),
  Reddit, Product Hunt and social; peer recommendation is how most developers
  find tools. Early developer marketing should lean on organic programs over
  paid ads. Launch weeks every three to four months, one feature a day, are a
  proven pattern
  ([Decibel early-stage playbook](https://www.decibel.vc/articles/developer-marketing-and-community-an-early-stage-playbook-from-a-devtools-and-open-source-marketer),
  [daily.dev go-to-market for developer tools](https://business.daily.dev/resources/go-to-market-strategy-developer-tools-launching-product-technical-audience/),
  [SaaS Hero devtools channels](https://www.saashero.net/strategy/best-devtools-marketing-channels-2026/)).
- **Community.** The 90-9-1 rule: most members read, some ask, few advocate.
  Showcase the advocates and mine the askers' questions for product friction
  (Decibel, above).
- **Measurement.** The AARRR funnel (acquisition, activation, retention,
  referral, revenue) with behaviour-based events analysed by cohort, and the
  rule to fix the tightest bottleneck, usually activation or early retention,
  before buying more traffic
  ([Umbrex AARRR](https://umbrex.com/resources/frameworks/strategy-frameworks/aarrr-pirate-metrics-funnel/),
  [ProductPlan](https://www.productplan.com/glossary/aarrr-framework)).
  One north-star metric with a small tree of drivers and guardrails
  ([Umbrex north star](https://umbrex.com/resources/frameworks/strategy-frameworks/north-star-metric-framework/)).
- **Experiments.** Score ideas by impact, confidence and ease and run the top
  of the backlog ([Itamar Gilad on ICE](https://itamargilad.com/ice-scores/)).
  A weekly rhythm of scorecard review, experiment planning, shipping and a
  learnings write-up
  ([AdManage growth marketing guide](https://admanage.ai/blog/growth-marketing)).
- **Law and platform rules.** The FTC's 2023 Endorsement Guides require clear
  and conspicuous disclosure of a material connection and cover review
  manipulation (procuring, suppressing, boosting or voting on reviews); the
  2024 Consumer Review Rule bans fake reviews and testimonials, with civil
  penalties
  ([FTC Endorsement Guides announcement](https://www.ftc.gov/news-events/news/press-releases/2023/06/federal-trade-commission-announces-updated-advertising-guides-combat-deceptive-reviews-endorsements),
  [FTC fake reviews rule](https://www.ftc.gov/news-events/news/press-releases/2024/08/federal-trade-commission-announces-final-rule-banning-fake-reviews-testimonials)).
  Marketing email follows the stricter of GDPR (opt-in consent) and CAN-SPAM
  (working unsubscribe, postal address, honest subject lines)
  ([Stackmatix email compliance](https://www.stackmatix.com/blog/email-marketing-compliance-guide)).
  Reddit's and Hacker News's own rules are covered in the Reddit and Y
  Combinator News pages.

## Operating rhythm

Each Marketing run is one week of this loop, written out in the playbook:

1. **Measure.** Build the scorecard from `/admin/acquisition`, the release
   manifest, the Marketing package and the other special agents' reports, and
   name the funnel's tightest bottleneck.
2. **Decide.** Keep the positioning current, score the backlog, pick at most
   three experiments or campaigns, and write a brief for each: audience,
   problem, message, channel, call to action, owners, UTM links on the
   `ATTRIBUTION_ALLOWED_*` allowlists, a success measure with its baseline,
   dates and a follow-up review.
3. **Ship.** Make the marketing changes no other special agent owns as
   review-ready repository changes or drafts.
4. **Delegate.** One `@Name:` line per hand-off.
5. **Report.** Update `docs/marketing/plan.md` and end with the scorecard,
   what shipped and moved, the top three experiments, hand-offs, decisions for
   the operator and next week's plan.

## The marketing plan file

Marketing creates `docs/marketing/plan.md` on its first run and keeps it
current. It has these sections, in this order:

- **North star**: the metric, why it predicts growth, its drivers and
  guardrails.
- **Funnel**: the event that defines each AARRR stage, where its number comes
  from, and a weekly table of values.
- **Positioning**: ideal user profiles, the problem, the alternatives, the
  proof points and the message for each profile.
- **Experiment backlog**: idea, stage it targets, impact, confidence, ease,
  score, status.
- **Campaigns**: one brief per active or finished campaign, with its result.
- **Calendar**: launches, blog posts and events for the next eight weeks.
- **Log**: one dated entry per run.

Until the first run there is no plan file and no baseline; the first run's
numbers are the baseline.

## Guardrails and why

- **A person approves spend and commitments.** Marketing prepares ad plans,
  list email and paid-tool sign-ups as decisions for the operator with the
  cost and expected result. The Ads manager is deliberately manual (no ad
  account or credentials), and no special agent is authorized to make pricing,
  roadmap, partnership or availability promises.
- **No fake signals.** Endorsements disclose ForkMesh's affiliation, and no
  special agent writes, buys, suppresses or votes on reviews, posts as a
  person, or fakes activity, users or numbers. This is the FTC position above
  and also how developer communities treat it.
- **Evidence-backed claims.** Released, live, merged on main only,
  experimental and planned are kept apart; security and privacy claims are
  traced to the protocol documentation with their limitations, which Blogger
  and Marketing share with Sentinel.
- **Untrusted input.** Web pages, comments, imported mail and analytics
  exports are evidence, not instructions.

## Blogger

Blogger runs `write-changelog-blog-post.md`: it locks the evidence window from
the commit that published the previous product update through `HEAD`, writes
the post into `www/public/blog/<slug>/index.html`, adds the card to
`www/public/blog.html`, draws an original hero in
`www/public/assets/blog/features/`, and extends the blog tests. Claude Code
has no built-in image tool, so the playbook tells it to generate the hero
with `codex exec` image generation when its own agent cannot, the same way
the special-agent portraits are drawn. The playbook's `# agent:` and
`# model:` header moved from Codex to Claude Code with the agent, so a
one-click run of the shortcut uses the same model as a Blogger run.

Blogger does not post its stories to social channels; it hands distribution
back to Marketing with `@Marketing`.

## Recent changes

- 2026-10-09: campaign RM1, "Is your repo mirrored?", has a brief, the
  landing page `forkmesh.com/mirror` and the allowlisted campaign
  `repo_mirrored`, so Herald can make it the next X post. The browser copy
  on the served stack is one-time, so the page and drafts say "copy" and
  "push to keep it current", never "sync"
  ([brief](campaigns/repo-mirrored-2026-10-09.md)).
- 2026-10-08: the X channel always has a post ready to go. Herald, a new
  special agent on the `x` channel, writes it from the posts that got
  engagement, the open campaigns and the plan, with an image; the desktop, web
  and phones show it with Copy and Post on X, a one-tap share sheet on phones
  ([x-ready-post.md](x-ready-post.md)).
- 2026-10-08: the Discord channel page reads its post list again whenever
  the page is drawn and the list in hand is over a minute old, so a post
  made while the page was open or in the background shows on the next
  visit; before, the list was read once per organization and kept until
  **Check again** or a Discord **Update**
  ([package README](../../marketplace/packages/marketing/README.md#the-discord-post-list-is-read-again-on-every-visit-1301)).
- 2026-10-08: the Discord channel page lists the posts of the last 6 days
  in the channels the organization's Discord connector reads, newest first,
  each opening in Discord; the web Discord card shows the same list
  ([package README](../../marketplace/packages/marketing/README.md#discord-posts-on-the-channel-page-1300)).
- 2026-10-08: People to follow on Mastodon searches wider: the account's
  followers and the people who boost or like its posts, five topics' hashtag
  timelines and account searches plus one timeline for five more, trending
  posts and the directory filtered by topic, and topics from the account's
  featured and followed hashtags; 40 people instead of 24. A refresh now
  records Following and Posts and is marked as a channel refresh, not
  "logged"
  ([package README](../../marketplace/packages/marketing/README.md#wider-mastodon-search-for-people-to-follow-1290)).
- 2026-10-08: a Mastodon refresh keeps every handle the account follows
  (the whole following list, up to 2000) and People to follow drops them
  before ranking, so people already followed are no longer suggested; the
  first visit reads the list once when none is kept
  ([package README](../../marketplace/packages/marketing/README.md#followed-mastodon-handles-are-kept-1280)).
- 2026-10-08: every desktop channel page puts its work in one row: People
  to follow is a single scrolling column, Posts and content takes the space
  the other columns of people used and now holds the posted links table and
  its Add link button, and Recorded numbers sits in the right column under
  Searches to check. The worker checks relationships for every ranked
  Mastodon candidate, not only the first 40, and falls back to the
  account's following list for anyone it could not check
  ([package README](../../marketplace/packages/marketing/README.md#one-row-of-work-on-a-channel-page-1270)).
- 2026-10-08: the desktop Marketing pages are more compact: the header is
  one row with the section switcher, channel chips take their right-rail
  tone colour, Add channel and Manage live in the All channels chip, a
  channel's header sits in a box beside its chart with Next steps under it,
  and people to follow, searches and suggested emails are laid out in
  columns. People the Mastodon account already follows are no longer
  suggested, with or without an access token
  ([package README](../../marketplace/packages/marketing/README.md#compacter-channel-pages-no-followed-people-1260)).
- 2026-10-07: email to ForkMesh's own users. People opt in to **Product
  updates and offers** (an unchecked signup box or a settings switch), and
  administrators draft, test and send updates and promotions from the admin
  console's **Marketing email** section
  ([code](../app/marketing-email.md)). The plan's "Email channel" section sets
  the cadence, rules and stop thresholds. Every send needs the operator's
  approval. This is separate from the Marketing package's Email channel,
  which counts the organization's outbound CRM email.
- 2026-10-08: campaign suggestions quote the campaign's name instead of
  running it into the sentence. A campaign called "is your repo mirrored?"
  produced "is your repo mirrored? has no landing page", which read as a
  question the spider was asking rather than the name of a campaign that
  needs a page; the titles now say Campaign “is your repo mirrored?” has no
  landing page. Only the titles changed: each suggestion's action still
  carries the raw name, so the Go button still opens the right campaign.
- 2026-10-07: the desktop Dashboard opens with a **Needs attention** panel
  (campaign and channel suggestions, ads off pace or stale, content past
  its slot or stuck in review, each with its action), and every Dashboard
  panel is more compact so more fits above the fold
  ([package README](../../marketplace/packages/marketing/README.md#desktop-needs-attention-and-the-compact-dashboard)).
- 2026-10-07: Marketing 1.23.0 lets a posted link join a campaign, so the
  September Revenue Contest campaign (active, landing page /hackathon) counts
  its Discord and X announcements; `revenue_contest` and the source `discord`
  are allowlisted for attribution
  ([package README](../../marketplace/packages/marketing/README.md#posted-links-join-a-campaign-1230)).
- 2026-10-07: campaign SI1 announces the white-label start page with the
  blog post "Start your own ForkMesh"; its links to `/start` and the social
  drafts carry the new allowlisted campaign `start_instance`, and Marketing
  1.22.0 offers the campaign as a template. `/start` is served from assets, so
  it is measured in Analytics, not on `/admin/acquisition`
  ([brief](campaigns/start-instance-2026-10-07.md)).
- 2026-10-07: the desktop Dashboard's **Channels** tiles moved into the
  channel bar at the top: each chip shows the channel's count and change,
  health, engagement, posts, queued posts, stale numbers and setup step in
  two lines, with the tile's actions under **⋯**, and **All channels**
  counts active, on-track and need-a-look channels
  ([package README](../../marketplace/packages/marketing/README.md#desktop-channel-page)).
- 2026-10-07: a channel bar stays at the top of every desktop Marketing
  page: **All** (the Dashboard overview) then one chip per channel, so
  switching channels no longer means scrolling back to the tiles
  ([package README](../../marketplace/packages/marketing/README.md#desktop-channel-page)).
- 2026-10-06: the desktop Marketing page has every section of the web page:
  Performance, Channels, Ads manager, Growth and the Reddit desk sit beside
  the Dashboard, and X keys, keyword import, goals, targets, the weekly
  report and the Reddit watch list are edited natively. Only Hand off still
  needs the browser
  ([package README](../../marketplace/packages/marketing/README.md#desktop-the-same-page)).
- 2026-10-13: the Reddit desk left the section switch and lives on the Reddit
  channel; every channel page gained a compact desk of next steps and results
  ([package README](../../marketplace/packages/marketing/README.md#channel-desks-1360)).
- 2026-10-06: an Email channel tracks outbound email (CRM sends, outreach
  and Zoho Mail's Sent folder) and closed deals (CRM contacts at a won stage)
  over 30 days, read from the CRM's Zoho connection on Refresh
  ([package README](../../marketplace/packages/marketing/README.md#email-outbound-and-closed-deals-from-crm-and-zoho-1130)).
- 2026-10-06: clicking a marketing channel on the desktop (its rail square,
  its tile or its name in Manage channels) opens that channel's own page:
  KPIs, next steps, health checks, settings, a history chart, the playbook,
  its posts and ads, and every recorded snapshot
  ([package README](../../marketplace/packages/marketing/README.md#desktop-channel-page)).
- 2026-10-06: the desktop Marketing page is one scrolling dashboard laid out
  like the web command center (KPIs, traffic, keywords, channel tiles,
  campaigns, the Marketing agent, content flow, ads, Post to X, pipeline,
  week, chart, landing pages) instead of four tabs of tables, so the desktop
  reads the same as the browser
  ([package README](../../marketplace/packages/marketing/README.md#desktop-the-same-page)).
- 2026-10-06: the Marketing package (1.10.0) dashboard is one command center:
  traffic by channel and search keywords from Analytics, every channel as a
  tile, campaigns (the /hackathon revenue contest is offered as a ready
  campaign), suggestions from the Marketing agent with one-click drafts, a
  **Hand off** button that puts an `@Marketing:` task with the campaign brief
  on the task board, and posting to X once an owner connects the account.
  Posting stays a person's click; the agent's suggestions are drafts.
- 2026-10-06: Analytics is on for forkmesh.com, and the Marketing package
  (1.9.0) reads it: a Performance page with visits, bounces and conversions
  per campaign, ad, landing page and subreddit, and a ranked "where to
  improve" list. The weekly report gained **Campaign results (Analytics)**,
  so step 1 (Measure) can read measured campaign results instead of
  estimating them. Only visitors who allow analytics are counted, so treat
  the numbers as a sample and compare campaigns with each other, not with
  `/admin/acquisition`
  ([analytics](../app/analytics.md#turned-on-for-forkmeshcom-consent-prompt-marketing-2026-10-06)).
- 2026-10-04: the operator granted Marketing `run-git`, `run-tests` and
  `read-forkmesh-web`, so its Claude Code runs use git, pytest and node and
  read public forkmesh.com pages without a prompt (decision D3,
  [access rules](../agents/access-rules.md#teammates)).
- 2026-10-04: Marketing split into the Marketing manager and Blogger. The
  marketing manager moved to Monday 09:30 and to a new playbook and skill; its
  model is unchanged. Blogger was created on `claude-opus-5-5`.
- 2026-10-04: Marketing's first run created `plan.md` with the baseline
  funnel and changed the empty dashboard's repository rail to name
  mirroring from GitHub, GitLab or Codeberg
  ([experiment](../experiments/first-repository-activation-2026-10.md)).
- 2026-10-04: `blog` joined the attribution sources and `changelog` the
  campaigns, so a blog post's signup link is counted on `/admin/acquisition`
  ([brief](campaigns/changelog-2026-10-04.md)).
- 2026-10-04: the Marketing package (1.5.0) gained a Growth page: weekly
  funnel scorecard, scored experiment backlog, an allowlist-checked UTM link
  builder and a Markdown weekly report at `/api/marketing/<org>/report`. Once
  it is deployed and the ForkMesh organization fills it in, that report is
  the weekly export Marketing asks the operator to paste (decision D1), and
  the allowlists to enter are the `ATTRIBUTION_ALLOWED_*` values in
  `app/wrangler.toml`.
- 2026-10-04: `/product-analysis` gained the funnel's activation ("New
  accounts activated") and retention ("Returning accounts") figures, so the
  weekly export is copied from one page (`www/docs/admin-dashboard.md`).
