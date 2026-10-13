# Hackathon

How a ForkMesh hackathon campaign works, what earns what, and which
guarantees the code enforces rather than merely documents.

The design goal is the one the growth plan set: taking part in the contest
should itself be the marketing, so that a participant has both a reason and a
one-tap way to bring the next one. The constraint that shapes every decision
below is that this must not become a spam machine or a lottery. So anything
worth money is gated on the referral ledger that already protects the public
referral programme, every reward is a fixed threshold, and pressing a share
button is worth nothing.

Code:

| Area | Where |
| --- | --- |
| Pure policy: points, missions, milestones, share copy, challenges, rewards, submissions, judging, growth funnel, config validation | `app/src/hackathon.py` |
| Routes, D1 access, hooks, personal pages, admin page | `app/src/hackathon_runtime.py` |
| Share cards | `app/src/og_card.py` (`render_hackathon_card`) |
| Participant HQ | `app/public/dashboard/partials/views/hackathon.html`, `app/public/dashboard/js/09-hackathon.js` |
| Landing page (main's) | `www/public/hackathon.html`, `www/public/assets/hackathon/` |
| Leaderboard, gallery, rules | `www/public/hackathon/{leaderboard,projects,rules}.html`, `www/public/hackathon.{css,js}` |
| Schema | `app/migrations/0182_hackathon.sql` (and its `down/` reversal) |
| Tests | `app/tests/test_hackathon*.py` |

## Lifecycle

A campaign is one row in `hackathons`, keyed by slug, with a status:

| Status | Public pages | Joinable | Submissions | Judging |
| --- | --- | --- | --- | --- |
| `draft` | 404 | no | no | no |
| `registration` | yes | yes | no | no |
| `live` | yes | yes | until `ends_at` | after `ends_at` |
| `judging` | yes | no | no | yes |
| `closed` | yes | no | no | no |

**`draft` is what makes this safe to ship.** Every push to this repository
deploys, so the feature has to be able to sit in production unfinished. A
draft campaign 404s on every personal page and every participant API, and
`GET /api/hackathon` returns `campaign: null`. A relay whose migration has not
run yet reports no campaign the same way instead of failing. Only an
administrator can advance a status, never a deploy.

With no slug, `_campaign()` resolves *the current campaign*: the newest one
that is not a draft. Every table is keyed by slug, so a second campaign needs
no migration.

## Points

`hackathon_points` is append-only, with primary key
`(hackathon_slug, account_bi, kind, ref)`. That key **is** the anti-double-pay
rule. A once-per-participant kind collapses its reference to the empty string,
so a second award is rejected by the key itself. A repeatable kind carries the
event's own reference, so one event pays exactly once however often its hook
fires. `hackathon_participants.points` caches the sum and only moves with a
write that landed.

| Kind | Points | Written when |
| --- | --- | --- |
| `joined` | 50 | the participant row is created |
| `verified` | 150 | their email is verified |
| `referral_joined` | 100 | somebody they invited verifies their email |
| `referral_qualified` | 200 | the referral ledger **credits** that referral |
| `first_mirror` | 300 | their first repository is published to the catalog |
| `pull_accepted` | 150 | a pull they authored is accepted |
| `submission` | 500 | their team submits a project (each member, once per submission) |
| `referral_post` | 100 | the microjob verifier confirms a public post |

A submission is worth more than a qualified referral on purpose, so the
Builders board cannot be won by recruiting alone.

**No point kind is client-reportable.** `CLIENT_REPORTABLE_POINT_KINDS` is the
empty set and a test pins it there.

## Joined versus qualified

Two different numbers, never merged.

* **Joined**: the invited person verified their email. This moves progress
  bars, the onboarding nudge, squad membership and `referral_joined` points,
  immediately.
* **Qualified**: `referral_attributions.decision` became `credited`, which
  takes email verification, seven days of membership, sustained use from a
  proved desktop device, and the burst, network and risk rules the public
  referral board enforces.

Anything of value reads *qualified*. The hackathon never re-decides a
referral: `on_referral_credited` is called from inside
`_finalize_referral_signup`, so its gate is that function's gate.

## Rewards

### What a reward can be

Missions and milestones award `pro_months`, `supporter_months`, a `badge`, or
nothing. Two kinds are deliberately impossible:

* **Cash** follows the judged tracks only. Nothing reachable from a mission,
  a milestone or recruiting can pay it.
* **Points** come only from the events in the table above. A mission is
  derived from those same events, so a points reward on it would pay each
  event twice. The validator refuses one.

Every reward is a fixed threshold. There are no entries, tickets or random
draws anywhere, which is what keeps this a contest of skill rather than a
sweepstakes. `validate_config` refuses, rather than clamps, an unknown reward
kind, an unknown metric or an out-of-range threshold or month count.

### How memberships are granted

Every reward is first claimed in `hackathon_reward_grants`, keyed by
`(slug, account, reward_key, kind)`, with one
`INSERT … ON CONFLICT DO NOTHING … RETURNING`. A reward whose event is observed
twice is granted exactly once.

Months are then projected into `role_grants` under
**`scope_type='promotion'`**, never the platform scope. Patreon revokes
platform-scope membership rows on every webhook, without even filtering by
scope, so a prize written there would vanish at the next subscription
renewal. A test replays a provider revoke against a granted prize.

* **Months stack.** One upsert computes `MAX(live expiry, now) + months`
  inside the database, so rewards landing together both count, and a lapsed
  promotion restarts from now. A month is thirty days.
* **Pro includes Supporter**, as every other tier table here does, which is
  what makes the World outfit perk follow a Pro prize.
* The plan surface (`/api/plans/usage`) reports an earned membership in its
  own `promotion` field, beside the paid `membership`. It is never folded
  into the paid one; otherwise the pricing page would stop offering
  membership to somebody whose free months are about to end.

### When rewards happen

| Source | Granted when |
| --- | --- |
| Missions | the moment a counter they read reaches the threshold: a friend verifying, a referral credited, a first mirror, a landed pull, a squad growing |
| Squad goals | every member of the squad that reached the goal, once; later arrivals are evaluated on joining |
| Prize table | when an administrator finalizes results |

The default missions pay memberships only for recruiting (3 qualified: 3
months; 10 qualified: 6 months and the Connector badge) and for a squad of
25 (3 months each). "Submit your project" is progress only. The Pro months
and Builder badge for a valid submission are the published prize, paid at
finalization and only once a project is confirmed valid. Paying the mission
too would reward the same entry twice, and before anyone had checked it.

## Squads and teams

A **squad** is an invite tree, not a chosen team. It is created the first time
a participant's link brings somebody in, and everyone who later arrives
through them joins it. `/hackathon/squad/<code>` is that participant's own
microsite, with their name, their meter and their reward. This is the
"let people run their own mini-contest" idea, with nothing to set up.

A **team** is the submission unit: up to five people, one project per track,
joined with an eight-character invite code. A participant is on one team per
campaign, enforced by the member table's key. The captaincy passes on when
the captain leaves, and the last member cannot leave while the team has a live
submission. The two words are never interchanged.

## Challenge links

"Alex has 1,240 points. Can you beat that?" A participant sends a link from
their HQ; `/hackathon/challenge/<token>` shows who dared you, the number to
pass, and how many people accepted and won.

* **The target is fixed when the link is sent**, as the number the page
  shows, so a challenge never moves under somebody chasing it.
* **Strictly more beats it.** A tie beats nobody. A beat is claimed with one
  `UPDATE … RETURNING` and announced to both people exactly once.
* **Beating one pays no points**, or two friends could mint points by
  challenging each other in a loop.
* **Accepting is not a referral.** It joins the campaign if needed but records
  no referrer and no squad, so the challenger earns nothing the referral
  ledger didn't decide. A newcomer gets a second button through `/r/<name>`.
* One link serves a group chat of up to fifty; a participant can have twenty
  open; a link closes after `challengeHours` (default 72). Pages are noindex.

Accepting happens on the HQ at `/hackathon/me?accept=<token>`, where the
session lives. The log-in link keeps the token, and the HQ removes it from the
address once used.

## Submissions, gallery and judging

**Submitting.** A team names a repository as `owner/name`. Each save checks
the catalog: public, not private, and first hosted inside the campaign window
with a week's grace. Anything unconfirmed becomes a **review flag, not a
rejection**. A false rejection costs a builder their entry, while a false flag
costs a judge one look. Posting again for the same track edits the submission
until the deadline, and an administrator's `invalid` survives an edit.

**Gallery.** Only `valid` projects are public, at `/hackathon/projects` and
`/hackathon/projects/<id>`. A flagged project appears the moment it is
cleared. Everything a submitter wrote is escaped, and outbound links are
`https` only and marked `nofollow ugc`.

**Community Choice.** One vote per verified participant per track, which can
be moved, never for their own team. It opens when the `community_choice`
milestone unlocks and closes after judging. Counts are recomputed from the
votes, so a moved vote moves the count.

**Judging.** Administrators name judges. A judge scores 1–5 on four criteria
(fits the track, working software, portable and self-hostable, craft), can
revise a score, and cannot score their own team. Scoring opens when
submissions close.

**Results.** Finalizing ranks each track by average judge score, then more
judges, then earlier submission; an unscored project cannot place. It writes
1st and 2nd, closes the campaign and grants the prize table through the
reward ledger. Running it again changes nothing.

## Community milestones

A milestone unlocks where the counter it watches moves: a join, a
verification, a new squad, a submission, or an administrator saving new
thresholds. That makes an unlock immediate for two small reads, without a
scheduler slot. The unlock is claimed with `UPDATE … WHERE unlocked_at=0
RETURNING`, so it happens once, and it never re-locks when a withdrawal moves
a counter down. The HQ's milestone panel is the in-product announcement.

## Sharing

The share kit offers copy, X, Mastodon, Bluesky, LinkedIn, Reddit, Hacker
News, Discord, Telegram, WhatsApp, email, SMS on phones, the native share
sheet, a QR code, and a **story image**. Every channel opens the participant's
own client with editable text.

The story image is a 1080×1920 PNG drawn in the browser, carrying the
participant's rank, points and invite QR. On a phone it goes to the share
sheet, elsewhere it downloads, and it costs the Worker nothing.

* **Nothing is sent on anyone's behalf.** No contact list, no recipient
  column, no message to a third party.
* **A share press is measured, not paid.** It is the K-factor numerator and
  worth zero points; a mission on `shares` may award nothing.
* **Presses are deduped per channel per minute.**

## Ambassadors

A verified participant applies with a short pitch; an administrator approves
or rejects, and a rejected applicant may apply again. Approval awards the
Hackathon Ambassador badge. An approved ambassador's HQ shows their own funnel
since the campaign opened, using the referral click audit's accepted rows so
duplicates and crawlers are already excluded. It also offers a printable
poster with their QR code, marked "post only where you have permission".

## Administration

`/admin/hackathon` is gated like `/admin/acquisition` (account session plus
`is_admin`, checked live), has no script and a `default-src 'none'` CSP. It is
the Hackathon section of the unified admin, drawn inside the shared admin
shell, with a campaign switcher and one tab (`?tab=`) per area:

* **Growth**: K-factor, invitations per participant, invitation
  conversion; the viral funnel; share presses by channel; joins per UTC day;
  and recruiters with credited, pending and rejected referrals.
* **Campaign**: slug, title, status, UTC dates and the JSON
  config, validated before anything is written.
* **Judges & results** (adding and removing judges, finalize results),
  **Submissions** (review) and **Ambassadors** (decisions).

Forms post back and redirect to the tab they came from, so a refresh never
repeats an action, and a
cross-origin form is refused. The page and `POST /api/admin/hackathon` call
the same functions.

**The K-factor.** ForkMesh never sees a message being sent, so "invitations"
are share presses and are labelled as intent. K therefore equals referred
joins per participant. The funnel shows a share of the previous stage only
where a stage is a subset of it; presses and "built something" are not.

## Routes

| URL | Served by | Notes |
| --- | --- | --- |
| `/hackathon` | www static asset | Main's landing page; observed by acquisition attribution. |
| `/hackathon/leaderboard` | www static asset | Builders, Recruiters, Squads. |
| `/hackathon/projects` | www static asset | Project gallery. |
| `/hackathon/rules` | www static asset | Official rules. |
| `/hackathon/join/<name>` | App, **proxied** by www | Personalised invitation. |
| `/hackathon/squad/<code>` | App, **proxied** by www | Squad microsite. |
| `/hackathon/challenge/<token>` | App, **proxied** by www | Challenge page; noindex. |
| `/hackathon/projects/<id>` | App, **proxied** by www | One valid project. |
| `/hackathon/me` | composed dashboard page | Participant HQ. |
| `/admin/hackathon` | App, `is_admin` | Administrator page. |
| `/api/hackathon[/…]` | App | Campaign, join, me, share, challenges, teams, submissions, projects, votes, judging, ambassadors, boards, squads, cards. |
| `/api/admin/hackathon` | App, `is_admin` | JSON equivalent of the administrator page. |

Personal pages are **proxied, not redirected**, so a shared link keeps the apex
URL it was shared as. This matters twice: the microjob verifier looks for
`forkmesh.com/r/<name>` in a posted page, and an unfurl should show the link
actually posted. They are matched before the two-segment repository shortcut,
like `/r/<name>`, so they never read as `owner/repo`.

## Read budget

D1 bills rows read, and the Worker was near half its daily free allowance
before this feature. So public reads are edge-cached (60 s) and bounded to
fifty rows (two hundred for the gallery). Campaign counters are denormalised
and moved by the writes that change them, so neither the meter nor a board
runs `COUNT(*)`. A participant's own rank is one indexed count on their own
page.

## Share cards

`render_hackathon_card` draws the same 1200×630 PNG family as the repository
and referral cards, in-process with the standard library and edge-cached per
colo. Kinds: `invite`, `rank`, `squad`, `challenge`, `project`.

## Privacy

* No contact of any kind is collected, stored or transmitted.
* Share events store a channel and a surface only.
* Public pages show public ForkMesh usernames only, the same trust level as
  the referral board and `badge_awards`.
* Notifications go only to the person concerned, about their own standing or
  reward.

## Done by people, not code

* Deciding the dates, the cash amount per track, the tracks, the milestone
  ladder, eligibility and who judges.
* Paying cash, running the guaranteed interview, and making featured
  placements on the homepage and the Marketplace after results.
* Recruiting ambassadors and anybody else. Nothing here contacts anyone.
* Having the official rules reviewed before the cash figure is announced.

## Deliberately not built

* **The physical QR scavenger hunt.** It only works with a venue and
  permission to place codes. Ambassadors' posters already cover the
  permission-based part.
* **Mass notifications on a milestone unlock.** One maintenance phase cannot
  notify every participant within its subrequest budget; the HQ panel and the
  landing meter carry the unlock instead.
* **Generated video for Stories.** The story image covers the channel at no
  Worker cost.
* **Random draws of any kind**, for the reason above.
