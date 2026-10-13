# ForkMesh Ads

ForkMesh Ads is the first-party ad network built into ForkMesh. Members write
short text ads, a person reviews every one before it runs, and approved ads
appear on a product surface: the web app, forkmesh.com, the desktop app, the
mobile apps, and ForkMesh World.

Three rules shape everything else on this page:

1. **An ad is placed by surface, never by viewer.** The serve call takes a
   surface name and nothing else. No session, account, repository, search,
   location, or history is read, so an ad cannot be aimed at a person and the
   network never learns who saw one.
2. **A person approves the exact words that run.** The automated screen only
   raises flags for the reviewer. It never approves anything.
3. **Only ads we would be comfortable recommending run.** The
   [prohibited list](#what-is-never-accepted) applies whatever category an ad
   claims, and any signed-in member can report an ad.

Approval means a ForkMesh reviewer found the ad within this policy on the day
they read it. It is not an endorsement of the advertiser and not a claim that
a company is ethical; it can be suspended, and it expires after 180 days.

This network is separate from
[community-reviewed placements](../community-ad-governance.md), which are
voted on by the community of an instance and take priority in World's social
circle. It does not change the [business-model contract](../business-model.md):
ForkMesh does not sell personal data or derived profiles and does not use them
to sell advertising, and this network has no way to.

## Who can advertise, and what it costs

Any verified account can write an ad and submit it for review. There is no ad
billing, bidding, or pay-per-click: payment processing for ads does not exist
yet. Until it does, an active membership level funds live ad slots. The levels
are the [Patreon](patreon.md) membership ladder, read from the same active
`role_grants` rows every other member perk uses, so a membership started
through another lane that grants those roles (a [plan trial](plan-trials.md),
a hackathon prize) counts as well.

| Level | Monthly | Ads running at once | Rotation weight per ad |
| --- | --- | --- | --- |
| Free | $0 | 0 | — |
| Supporter | $5 | 1 | 1 |
| Pro | $10 | 2 | 2 |
| Builder | $50 | 5 | 5 |
| Studio | $100 | 10 | 10 |

- *Ads running at once* counts across all surfaces. When an account has more
  approved ads than slots, the ones approved first run and the rest wait as
  "over allowance".
- *Rotation weight* is each running ad's share of the rotation on a surface:
  a Studio ad is picked ten times as often as a Supporter ad.
- An approved ad whose owner has no active membership simply pauses. Nothing
  is deleted, and it resumes when a membership is active again.
- An account can hold 25 ads in total and 5 awaiting review.

These numbers are published in the `advertising` block of
[`business-model.json`](/business-model.json) and pinned to the code by
`app/tests/test_ad_network.py`. Change them in both places or not at all.
Membership levels never affect identity, permissions, ranking, moderation, or
governance; this perk only buys reach for a reviewed ad.

## What an ad is

Text only: a headline (60 characters), an optional body (160), a
call-to-action label (24), the sponsor's name (48), one `https://` destination,
one category, the surfaces it may run on, and an optional schedule.

There are no images, scripts, embeds, or custom HTML. A remote image would let
an advertiser log every viewer's address, which is exactly what rule 1
forbids. Clients render ad text as plain text, open the destination in a new
tab or the system browser with `rel="sponsored noopener noreferrer"` and no
referrer, and label every slot **Sponsored** with a link to this page.

Destinations must be public HTTPS hosts. Link shorteners are refused because
they hide the page a reviewer approved, and cross-site click identifiers
(`gclid`, `fbclid`, and similar) are refused because they exist to follow a
visitor. Ordinary campaign tags such as `utm_source` are allowed.

Allowed categories: developer tools and libraries; open-source projects and
foundations; hosting, infrastructure and self-hosting; defensive security and
privacy tools; education, books and courses; conferences, meetups and
hackathons; hiring and job boards; accessibility products and services;
nonprofits and public-interest causes; climate and sustainability work;
repairable and open hardware; independent creative and indie software;
community projects and member shout-outs.

## What is never accepted

- Gambling, betting, lotteries and loot-box mechanics
- Tobacco, vaping, alcohol and recreational drugs
- Weapons, ammunition and explosives
- Adult or sexually explicit content
- Promised returns: speculative tokens, trading signals, get-rich-quick and
  multi-level marketing
- Payday loans, debt traps and predatory credit
- Miracle cures and unproven health or diet claims
- Behavioral tracking, data brokers, stalkerware and people-search services
- Malware, exploit or credential sales and offensive hacking services
- Election, candidate and ballot persuasion
- Hate, harassment, discrimination or violence
- Misleading claims, fake urgency, impersonation, clickbait and dark patterns
- Illegal goods or services, counterfeits and piracy
- Exploitative labor and unpaid-work schemes
- Anything aimed at children
- Destinations that fingerprint visitors, force a download or redirect through
  a tracker

A reviewer cites these rules by code when rejecting or suspending an ad, so
the owner always sees which rule applied. The machine-readable list, the
categories, the limits, and the level table are served by
`GET /api/ads/policy`.

## Review

| Status | Meaning | Who moves it |
| --- | --- | --- |
| `draft` | Saved, not submitted | Owner submits |
| `pending` | In the review queue | Reviewer approves or rejects |
| `approved` | May run, subject to membership, schedule and slots | Reviewer suspends; owner withdraws, pauses or edits |
| `rejected` | Declined with a reason and cited rules | Owner edits and resubmits |
| `suspended` | A reviewer's hold on a running ad | Reviewer reinstates (back to `pending`) |
| `withdrawn` | Pulled by the owner | Owner resubmits |

- Submitting requires the owner to attest that the ad follows this policy and
  is honest about what it promotes.
- Reviewers are platform administrators. The queue lives on
  `/dashboard/ads` and is only shown to them; the server enforces the role on
  every review route and records denied attempts.
- **Approval is pinned to the creative.** The reviewer's approval names the
  content hash of the words they read. If the owner edits the ad while it is
  in the queue, the approval is refused (`creative_changed`) and the reviewer
  reloads. If the owner edits an approved ad's words, it stops running and
  returns to `pending`. Changing only the schedule, or pausing, needs no
  review, because neither can show anything that was not approved.
- A rejection or suspension must carry a reason or at least one cited rule.
- A suspended ad cannot be edited or resumed by its owner; only a reviewer
  lifts the hold, and reinstating reopens review rather than skipping it.
- An approval lasts 180 days. After that the ad shows as "review expired" and
  the owner resubmits it.
- Every decision is appended to `ad_network_reviews` (action, reviewer, cited
  rules, content hash) and to the sensitive audit log as
  `ad_network.approve|reject|suspend|reinstate`. The trail holds no ad text
  and outlives the ad's deletion.
- A reviewer deciding their own ad is allowed, so a one-administrator instance
  can run house ads, but it is recorded as a self-review in both trails and
  shown as such in the queue. Instances with more than one administrator
  should have someone else decide.

The automated screen adds flags such as `term:gambling`, `style:shouting`,
`claim:price-or-rate` or `url:query-string`. They are prompts to look closer.
"Exploit" is ordinary language in a security tool's ad, which is why a person
decides.

### Reports

Any signed-in member can report a running ad from its slot (one open report
per member per ad). Reports appear in the queue's **Reported** tab. When five
different members have open reports on a running ad, it is suspended
automatically and waits for a reviewer; the reviewer's next decision closes
the reports it answers. Owners cannot report their own ads.

## Serving and measurement

`GET /api/ads/serve?placement=web|www|desktop|mobile|world` returns one ad or
`null`. Among running ads for the surface, the pick is weighted by level and
rotates on a five-minute bucket; it involves no randomness and no viewer
input, so every visitor in the bucket gets the same answer and the response is
edge-cached (`public, max-age=300`). D1 is read about once per surface per
colo per five minutes regardless of traffic. A review decision or owner change
drops the local colo's cached entries; other colos catch up within the TTL, so
a suspension can take up to five minutes to take effect everywhere.

The only measurement is an aggregate click counter per ad, surface and UTC
day (`ad_network_daily_stats`), incremented by a fire-and-forget
`POST /api/ads/{id}/click`. The table has no viewer, address, user-agent or
session column, and a test pins that. Impressions are not measured: counting
them would need a beacon per view, which would cost a request per page and
tell us more about visitors than we want to know. Counters older than 400
days are deleted by the daily `world-media-cleanup` cron phase. Because clicks
are unauthenticated they can be inflated; nothing is billed on them.

Client rules (all five surfaces follow them):

- The serve call sends no credentials and no authorization header.
- No ad, or a failed request, renders nothing. An ad never blocks, delays or
  breaks the surface it sits on, and is fetched after the surface is idle.
- Every slot can be hidden for seven days, locally, without a server call.
  The desktop and mobile apps also have a "Show sponsored messages" setting.
- forkmesh.com keeps legal, security-report and documentation pages ad-free.
- The desktop app asks only a forkmesh.com relay (or loopback) for ads, never
  raises a firewall prompt on an ad's behalf, and stays silent in offline,
  headless and reduced-view modes. Its two calls are catalogued at the lowest
  importance in `endpoint-catalog.json`, so a firewall rule that denies
  cosmetic traffic hides ads.
- In World, a community-reviewed placement always wins; a network ad is only
  requested when there is none, and only once an avatar reaches the social
  circle.

## Operating it

- **Become a reviewer:** reviewers are accounts with `users.is_admin = 1`.
- **Review:** open `/dashboard/ads`, use the queue tabs, read the destination,
  decide. Check the flags and the owner's note; cite rules when declining.
- **Stop an ad now:** suspend it from the **Approved** tab. To stop everything
  from one account, suspend each of its ads; ending the membership grant also
  pauses them all.
- **Account lifecycle:** renaming an account moves its ads and re-seals the
  owner name reviewers see; deleting an account deletes its ads, reports and
  click counters (the decision trail remains, without ad text).
- **Storage:** `ad_network_ads`, `ad_network_reviews`, `ad_network_reports`,
  `ad_network_daily_stats` (migration `0187_ad_network.sql`). Ad text is public
  by design and stored in the clear; the submitting username is sealed with
  the row key and opened only for reviewers.
- **Rollback:** `app/migrations/down/0187_ad_network.sql` drops the four
  tables. No membership, role grant or account is touched. Clients treat a
  missing or failing serve route as "no ad".

## Boundaries

- No payment is taken for ads. When ad billing arrives it must be published in
  `business-model.json` first, with metering tests, like any other allowance.
- No third-party ad code, exchange, or tag runs anywhere in ForkMesh.
- Ads never affect search, ranking, moderation, governance, or what a
  repository page shows about a project.
- Self-hosted instances run their own network with their own reviewers; ads
  and decisions are instance-local and are not protocol data.
