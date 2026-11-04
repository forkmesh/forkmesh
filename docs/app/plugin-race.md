# Plugin Race

Part of [the App docs](README.md). The hackathon that runs from 19 October to
18 December 2026 (UTC). Builders publish a plugin in the community marketplace
that works on desktop, iOS and the web. The first 10 accepted plugins to reach
10 counted ratings win 0.1 SOL each. The public rules are
[forkmesh.com/hackathon/rules](https://forkmesh.com/hackathon/rules).

| Area | Where |
| --- | --- |
| Rules, checks, standings and routes | `app/src/plugin_race.py` |
| Worker adapter | `app/src/programs_runtime.py` (`_PluginRaceRuntime`) |
| Pages | `www/templates/pages/hackathon.html`, `hackathon/rules.html`, `www/public/assets/hackathon/race.js` |
| Schema | `app/migrations/0262_affiliates_and_plugin_race.sql` (`plugin_race_entrants`, `plugin_race_posts`, `plugin_race_plugins`) |
| Campaign | [docs/marketing/campaigns/plugin-race-and-affiliates-2026-10-16.md](../marketing/campaigns/plugin-race-and-affiliates-2026-10-16.md) |

The dates, prize, slots, 10-rating finish line and 3-star minimum are
constants at the top of `plugin_race.py`. The pages state the same numbers, so
change both together.

The race is separate from the points-and-squads hackathon engine
(`hackathon.py`, [docs/hackathon.md](../hackathon.md)). Its
finish line is marketplace ratings, which that engine does not model.

## Who counts

Builders and raters pass the same four checks before anything they do counts:

| Check | Source |
| --- | --- |
| `account` | `email_verified` and status `active` on the account record |
| `member` | `account_users._referral_qualification_flags`: account 7 days old, a desktop device that proved its key on two occasions a day apart and was seen in the last 48 hours |
| `x`, `reddit`, `mastodon` | A public post per network carrying `#ForkMeshPluginRace` |
| `discord` | `users.discord_bi` is set, or a username an admin confirmed |

The member check makes a throwaway account expensive; it is the same check
that decides whether a referral counts.

Posts are reduced to one canonical URL per network and checked by one bounded
fetch with redirects refused:

- X uses the public oEmbed endpoint.
- Reddit uses the post's `.json`.
- Mastodon uses `/api/v1/statuses/<id>` on the post's instance. The host must
  be a public name, with no IP literal and no internal suffix.

A page with the tag is `confirmed`, one without it `missing`, and one we could
not read `pending` for an admin. A post URL belongs to the first account that
submits it (`plugin_race_posts`).

When every check is confirmed the entrant becomes `qualified` and
`qualified_at` is stamped once. Re-entering re-runs the checks; a
`disqualified` entrant cannot.

## Plugins and standings

- A plugin entry needs a qualified builder, the race to be open, a
  `marketplace_submissions` row published by that account in
  `forkmesh-community` since the race opened, a summary, and how to try it on
  each of desktop, iOS and web.
- At most 3 entries per builder. Entries start in `review`; an admin accepts
  or rejects them after checking the plugin is useful and runs in all three
  places.
- A rating counts when it is in `marketplace_reviews` for an accepted plugin,
  not hidden, 3 stars or more, from a `qualified` entrant who is not the
  builder.
- A rating counts from `max(created_at, rater's qualified_at, race start)`,
  and only if that is before the end. The tenth such moment is the plugin's
  `reachedAt`.
- The first ten by `reachedAt` (ties by package name) hold ranks 1 to 10.
  `standings()` is a pure function of the rows, so the board is recomputed on
  every read and the same data always gives the same order.

Disqualifying a rater removes their ratings from every count at once.

## Routes

| Route | Who | What |
| --- | --- | --- |
| `GET /api/plugin-race` | anyone | Rules, board, qualified count, prizes left; with a session, the caller's checks and plugins |
| `POST /api/plugin-race/entrant` | verified account | `{x?, reddit?, mastodon?, discord?, acceptRules}` (`acceptRules: true` the first time) |
| `POST /api/plugin-race/plugins` | qualified entrant | `{package, summary, platforms: {desktop, ios, web}}` |
| `GET /api/admin/plugin-race` | admin | Entrants with their posts, every plugin, standings with each winner's payout address |
| `POST /api/admin/plugin-race` | admin | `{action, ...}`, see below |

Admin actions:

| `action` | Body | Effect |
| --- | --- | --- |
| `post` | `account, network, state: confirmed\|rejected` | Settle a post we could not read, then re-check the entrant |
| `discord` | `account, state` | Confirm an unlinked Discord username |
| `entrant` | `account, status: disqualified\|pending` | Disqualify, or reinstate and re-check |
| `plugin` | `package, status: accepted\|rejected, note?` | Review an entry; rejecting an unpaid winner frees its rank, a paid one cannot be rejected |
| `award` | none | Freeze current winners into `award_rank` (never re-ranks a frozen one) |
| `paid` | `package, tx` | Record the payout transaction signature |

## Paying a prize

The server holds no wallet key:

1. Run `award`.
2. Read each winner's Solana address (the `solana` field the settings page
   saves) from the admin listing.
3. Send 0.1 SOL from the prize wallet.
4. Record the signature with `paid`; the board shows it as proof.

A winner with no address after 30 days loses the slot (rules section 3):
reject the plugin with a note, which clears its rank, and the next plugin to
cross the line takes the slot at the next `award`.

Payouts do not reuse `microjob_claims`: its `job` column has a CHECK
constraint, so a new kind would mean rebuilding a table holding live payouts.
