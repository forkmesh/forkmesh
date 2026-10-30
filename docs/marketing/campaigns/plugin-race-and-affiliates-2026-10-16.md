# Campaign brief: "Plugin Race and affiliate launch"

**Campaign tag:** `plugin_race`
**Tracking hashtag:** `#ForkMeshPluginRace`
**Landing pages:** [forkmesh.com/hackathon](https://forkmesh.com/hackathon) and
[forkmesh.com/affiliates](https://forkmesh.com/affiliates)
**Plan:** [../plan.md](../plan.md)

One launch for two programs: the Plugin Race (the new hackathon) and the
affiliate program that replaces the September Revenue Contest with the same commission and rules. Both share one
hashtag so every mention can be found with one search on each network.

## Brief

| | |
| --- | --- |
| Audience | Developers who could build a plugin that runs on desktop, iOS and the web; ForkMesh users who would rate one honestly; people who know businesses that should be on ForkMesh |
| Message | Build a useful plugin, get ten honest ratings, win 0.1 SOL. Bring a company onto a paid ForkMesh plan and earn half of its first month, all year. |
| Channels | X, Reddit (r/forkmesh), Mastodon, Discord, email to opted-in members, blog |
| Call to action | Enter the race at /hackathon#enter; join at /affiliates#join |
| Success measure | 25 qualified entrants and 10 accepted plugins by 9 November; all 10 prizes won by 18 December; 15 affiliates and 30 leads in the CRM by 18 December |
| Baseline | 0 for all of the above (both programs are new) |
| Duration | 16 October 2026 to 18 December 2026 (UTC). The race itself opens 19 October. |
| Owners | Launch posts: marketing. Entrant and plugin review: whoever holds the admin queue. Payouts: the operator with the prize wallet. |

## Why one hashtag tracks it

Every race entrant has to post about it on X, Reddit and Mastodon with
`#ForkMeshPluginRace` before they qualify, and the server checks the tag.
So the tag counts both our own reach and entrant reach.

- **The entrant list** is the exact record: `GET /api/admin/plugin-race`
  returns each entrant's three post URLs and whether each was confirmed.
- **The Marketing package** counts a saved post toward this campaign when the
  post's title or note carries `#ForkMeshPluginRace`. Save launch posts and
  any entrant posts worth tracking under Posted links.
- **Searches** to run twice a week:
  - X: `#ForkMeshPluginRace`
  - Mastodon: the `#ForkMeshPluginRace` hashtag timeline on mastodon.social
  - Reddit: `ForkMeshPluginRace` in site search
- **Visits** come from the `utm_campaign=plugin_race` links below, which show
  in Analytics. `plugin_race` is on the attribution allowlist.

## Tagged links

```text
https://forkmesh.com/hackathon?utm_source=x&utm_medium=social&utm_campaign=plugin_race
https://forkmesh.com/hackathon?utm_source=reddit&utm_medium=social&utm_campaign=plugin_race
https://forkmesh.com/hackathon?utm_source=mastodon&utm_medium=social&utm_campaign=plugin_race
https://forkmesh.com/hackathon?utm_source=discord&utm_medium=social&utm_campaign=plugin_race
https://forkmesh.com/hackathon?utm_source=email&utm_medium=email&utm_campaign=plugin_race
https://forkmesh.com/affiliates?utm_source=x&utm_medium=social&utm_campaign=plugin_race
https://forkmesh.com/affiliates?utm_source=reddit&utm_medium=social&utm_campaign=plugin_race
https://forkmesh.com/affiliates?utm_source=mastodon&utm_medium=social&utm_campaign=plugin_race
https://forkmesh.com/affiliates?utm_source=discord&utm_medium=social&utm_campaign=plugin_race
https://forkmesh.com/affiliates?utm_source=email&utm_medium=email&utm_campaign=plugin_race
```

## Gates before posting

| Gate | Status on 2026-10-30 16:15 UTC |
| --- | --- |
| Branch merged and deployed; /hackathon, /hackathon/rules, /hackathon/archive and /affiliates answer 200 | Passed. All four answer 200 (merged in `aa1972db78`). |
| `GET /api/plugin-race` and `GET /api/affiliates` answer 200 | Passed. Both answer 200 with `ok: true`. |
| CRM installed on the `forkmesh` organization, so affiliate leads land in it | Passed. The CRM's organization list shows it installed on `forkmesh`. |
| Race dates, 3-star minimum and affiliate commission confirmed by the owner | Open. The served values match this brief (opens 19 October, closes 18 December 23:59 UTC, 3 stars, ten 0.1 SOL prizes, 50% commission), but no owner confirmation is recorded. |
| Prize wallet holds at least 1 SOL plus fees | Open. The wallet address is not in the repository; the operator checks the balance. |
| Someone named to review entrants and plugins daily | Open. |

The race is already open on the site: `GET /api/plugin-race` says `open: true`,
0 qualified entrants, 0 plugins and 10 prizes left, eleven days in, with no
launch post on any channel. The three open gates are the operator's; once they
pass, the 16 October and 19 October posts go out together as one "the race is
open" post, and the 26 October review moves to a week after it.

## Drafts

### X, race (252 characters with the link)

<!-- plugin-race-x:start -->
```text
We're running a plugin race.

Build a ForkMesh plugin that works on desktop, iOS and the web. The first 10 plugins to get 10 ratings from verified people win 0.1 SOL each.

Opens 19 October. Rules and board:
https://forkmesh.com/hackathon?utm_source=x&utm_medium=social&utm_campaign=plugin_race

#ForkMeshPluginRace
```
<!-- plugin-race-x:end -->

### X, affiliates (225 characters with the link)

<!-- affiliates-x:start -->
```text
The revenue contest is over. The commission isn't.

Bring a company onto a paid ForkMesh plan and half of their first month is yours. Same rules as the contest, now open all year.

https://forkmesh.com/affiliates?utm_source=x&utm_medium=social&utm_campaign=plugin_race

#ForkMeshPluginRace
```
<!-- affiliates-x:end -->

### Mastodon (443 of 500 characters)

<!-- plugin-race-mastodon:start -->
```text
We're running a plugin race, and you can win SOL.

Build a ForkMesh plugin that works in the desktop app, on iPhone and on the web. The first 10 plugins to reach 10 ratings from verified people win 0.1 SOL each. Raters qualify the same way builders do, so the ratings are real.

Opens 19 October, runs to 18 December.

https://forkmesh.com/hackathon?utm_source=mastodon&utm_medium=social&utm_campaign=plugin_race

Also new: the revenue contest's 50% commission is now open all year.

#ForkMeshPluginRace #ForkMesh
```
<!-- plugin-race-mastodon:end -->

### Reddit, r/forkmesh

**Title:** Plugin Race: build a ForkMesh plugin, first 10 to reach 10 ratings win 0.1 SOL

```text
We're starting something new for people who build on ForkMesh.

The Plugin Race opens on 19 October and runs until 18 December, or until all
ten prizes are gone.

The idea is simple. Build a plugin that does a real job and works in all three
places ForkMesh runs: the desktop app, the iOS app and the web dashboard.
Publish it in the community marketplace. The first 10 plugins to get 10
ratings of 3 stars or more from verified people win 0.1 SOL each.

"Verified" means something here. Builders and raters both need:

- a ForkMesh account with a verified email
- to pass our member check (account a week old, desktop app proved on two days)
- a public post about the race on X, Reddit and Mastodon with #ForkMeshPluginRace
- a Discord account in our server

That last part is deliberate. Ten ratings should mean ten people, not ten
throwaway accounts.

Everything is on one page, including the live board:
https://forkmesh.com/hackathon?utm_source=reddit&utm_medium=social&utm_campaign=plugin_race

Official rules: https://forkmesh.com/hackathon/rules

One more thing. The September Revenue Contest ended on 15 October. Its
commission (half of a new customer's first month) and its rules carry on as
an affiliate program that is open all year:
https://forkmesh.com/affiliates?utm_source=reddit&utm_medium=social&utm_campaign=plugin_race

Questions welcome here or on Discord.
```

### Discord, #announcements

<!-- plugin-race-discord:start -->
```text
@everyone The Plugin Race starts on 19 October.

Build a plugin that works on desktop, iOS and the web. The first 10 plugins to reach 10 ratings from verified people win **0.1 SOL** each.

To build or to rate, you need four things: a verified ForkMesh account, our member check, a post about the race on X, Reddit and Mastodon with **#ForkMeshPluginRace**, and a Discord account (you have that one already).

Rules, board and sign-up: https://forkmesh.com/hackathon?utm_source=discord&utm_medium=social&utm_campaign=plugin_race

Also new: the **affiliate program**. The revenue contest's deal, all year: claim a company, bring it onto a paid plan, keep half of its first month. https://forkmesh.com/affiliates?utm_source=discord&utm_medium=social&utm_campaign=plugin_race

Ask questions here, and use this server to find people to build with.
```
<!-- plugin-race-discord:end -->

### Email to opted-in members

**Subject:** Build a plugin, win 0.1 SOL

```text
Hi,

Two new things at ForkMesh this week.

The Plugin Race opens on 19 October. Build a plugin that works on desktop, iOS
and the web, publish it in the community marketplace, and get 10 honest
ratings. The first 10 plugins to get there win 0.1 SOL each. You can also take
part just by trying plugins and rating them; raters go through the same checks
as builders, so every rating counts for something.

Details and the live board:
https://forkmesh.com/hackathon?utm_source=email&utm_medium=email&utm_campaign=plugin_race

The affiliate program is open too. It is the revenue contest's deal without
the end date: claim a company, bring it onto a paid ForkMesh plan, and half of
its first month is yours:
https://forkmesh.com/affiliates?utm_source=email&utm_medium=email&utm_campaign=plugin_race

If you post about either one, tag it #ForkMeshPluginRace so we can find it.

The ForkMesh team
```

## Follow-up and review

| Date | What |
| --- | --- |
| 16 October | Launch posts on X, Mastodon, Reddit, Discord; email to opted-in members |
| 19 October | "The race is open" post on every channel, with the board link |
| Every Monday | Board update post; save new #ForkMeshPluginRace posts under Posted links |
| 26 October | First review: entrants, checks stuck on "Waiting", accepted plugins, affiliate leads |
| 9 November | Midpoint review against the success measure |
| Each win | Winner post with the payout transaction signature |
| 18 December or tenth prize | Recap post and the result below |

## Result

To be written at the end of the campaign.
