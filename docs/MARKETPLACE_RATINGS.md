# Market ratings and improvement reviews

Signed-in accounts rate Market packages from 1 to 5 stars. **A rating below five
stars must come with a review** that says what did not work and how the package
could earn five stars. The server enforces this, not only the page. A 5-star
rating can be left without a review.

- Server: `app/src/marketplace_reviews.py` (the route and its rules),
  `app/src/marketplace_runtime.py` (`_MarketplaceReviewsRuntime`, the Worker
  adapter), and migration `0198_marketplace_reviews.sql`.
- Web: `/market-reviews?market=<market>&package=<id>` on the app origin
  (`app/public/market-reviews.{html,js}`, `app/public/market-review-form.js`).
  The `/market` listing (`www/public/market.html`) shows the average stars and
  links to that page.
- Tests: `app/tests/test_marketplace_reviews.py`,
  `app/tests/test_market_review_form_frontend.py`, and
  `app/tests/host/test_market_reviews_e2e.py`.
- Native: the WS19 workstream (row 11.22 in `docs/native/DESKTOP_PARITY.md`)
  uses the endpoints below. Nothing on the phone needs its own rules: send the
  fields, show the `fields` errors the server returns, and use `rules` for the
  counters.

## Who can rate

| Rule | Error (status) |
|---|---|
| Signed in (session cookie or `Authorization: Bearer <sessionToken>`) | `sign_in_required` (401) |
| Email address confirmed, and account active | `email_not_verified` (403, with `verifyUrl`) |
| Not a maintainer of the package | `maintainer_cannot_rate` (403) |
| Has an install/use record, **if the package has any** | `package_use_required` (403) |
| At most 10 rating writes an hour and 30 a day | `rating_rate_limited` (429) |

**One rating per account per package.** A second write updates the same rating.
Every write that changes something adds a row to
`marketplace_review_history`, so earlier versions are kept.

**Install/use records.** A signed-in client reports "this account installed or
used this package" with `POST /api/market/reviews/uses`. An organization
install (`org_plugin_installs`) counts for the person who installed it and for
the organization's members. When a package has at least one record, only
accounts with a record can rate it. This stops drive-by ratings from accounts
that never used the package. A package with no records yet accepts ratings,
marked `verifiedUse: false`. Native clients should call `/uses` after an
install while signed in.

**Maintainers.** First-party packages (market `forkmesh`) are maintained by the
owners, admins and maintainers of the `forkmesh` organization. A community
package (market `forkmesh-community`) is maintained by the accounts that
submitted its published versions. Platform administrators can moderate every
package.

## Review fields and validation

| Field | Below 5 stars | 5 stars |
|---|---|---|
| `stars` | integer 1-5 (`"1"`-`"5"` accepted) | 5 |
| `problem`: "What didn't work or was missing" | **required** | optional |
| `improvement`: "How we can improve / what would make it 5 stars" | **required** | optional |
| `context`: steps to reproduce / context | optional | optional |
| `comment`: anything else | optional | optional |
| `category` | optional: `bug`, `missing-feature`, `docs`, `performance`, `ux`, `other` | same |
| `version` | optional: `1.4`, `v1.4.2`, `1.4.0-beta.1` (stored as `v1.4`) | same |

Text is trimmed. Spaces are collapsed within each line, line breaks are kept,
and each field is capped at 4000 characters. A required field is rejected, with
this code in `fields.<name>`, when it is:

| Code | Rule |
|---|---|
| `required` | empty |
| `too_short` | under 40 characters |
| `too_few_words` | under 8 words (words of 2 or more letters; CJK text counts one word per two characters) |
| `repeated_characters` | 5 or more of the same letter in a row, or 10 or more of any character |
| `repetitive` | under 5 different words, or one word is more than half of all the words |
| `same_as_problem` | `improvement` repeats `problem` |
| `too_long` | over 4000 characters (any field) |

When `problem` and `improvement` are both missing on a rating under 5 stars, the
error is `review_required`. Any other field problem gives `review_invalid`. Both
return status 400 with the same body:

```json
{"error": "review_invalid",
 "fields": {"problem": "too_few_words", "category": "invalid"},
 "rules": {"reviewRequiredBelow": 5, "minChars": 40, "minWords": 8,
           "maxChars": 4000, "categories": [{"id": "bug", "label": "Bug"}]}}
```

## Endpoints

All paths start with `/api/market/reviews`. Writes (`POST`/`PUT`/`DELETE`) must
come from an allowed origin, like every other account write. The JSON body can
be at most 32 KiB.

| Method and path | Who | What |
|---|---|---|
| `GET /summary` | public (cached 60 s) | `{packages: {<id>: Summary}, marketplaces: {<market>: {<id>: Summary}}, usage: {<id>: {views, opens, people}}, usageByMarketplace: {<market>: {<id>: {views, opens, people}}}}` (usage sums `marketplace_package_activity`; the Market lists show it) |
| `GET /{market}/{package}?sort=newest\|helpful&page=1&limit=10&stars=N` | public; the viewer's own data when signed in | the package, its `summary`, a page of `reviews`, `total`, `hasMore`, `viewer`, `rules` |
| `POST` or `PUT /{market}/{package}` | a rater (see above) | create (201) or update (200) your rating; `{review, summary, created, notified}`; an unchanged write returns `{unchanged: true}` |
| `GET /{market}/{package}/feedback?category=&status=open\|addressed\|all&sort=newest\|oldest\|lowest\|highest\|helpful&page&limit≤200` | maintainers, admins | the improvement feedback: every visible review under 5 stars, with `categories` and `statuses` counts |
| `GET /maintained` | signed in | the packages you maintain, with `summary` and `openFeedback` counts |
| `POST /uses` `{market, package, source?}` | signed in | record that your account installed or used a package (`source`: `desktop`, `mobile`, `web` or `client`) |
| `GET /items/{id}/history` | the reviewer, maintainers, admins | every revision of the rating |
| `POST /items/{id}/reply` `{text, addressedIn?}` / `DELETE` | maintainers, admins | public reply of up to 2000 characters, and optionally mark it addressed; the reviewer gets a ping |
| `POST /items/{id}/addressed` `{version}` / `DELETE` | maintainers, admins | mark the review "addressed in vX.Y", or clear it |
| `POST` / `DELETE /items/{id}/helpful` | signed in, not the author | vote or unvote "helpful" (200 votes a day); `{helpful, viewerFoundHelpful}` |
| `POST /items/{id}/report` `{reason}` | signed in, not the author | report abuse (5-500 characters, 20 reports a day) |
| `POST /items/{id}/moderation` `{hidden, reason}` | admins | hide (a reason is required) or unhide; resolves the open reports |
| `GET /reports` | admins | reported reviews, with their open reports |

`Summary` is `{"average": 4.3, "count": 12, "histogram": {"1": 0, "2": 1, "3": 1, "4": 3, "5": 7}}`.
The average has one decimal place. The summary is cached in
`marketplace_package_rating_stats` and recomputed on every write that can
change it. Hidden reviews do not count.

A review in a list:

```json
{"id": "9f2c…32 hex", "market": "forkmesh", "package": "crm", "stars": 2,
 "reviewer": {"name": "alice", "displayName": "Alice"},
 "problem": "…", "improvement": "…", "context": "", "comment": "",
 "category": "bug", "version": "v1.13", "verifiedUse": true,
 "helpful": 3, "viewerFoundHelpful": false,
 "createdAt": 1790000000000, "updatedAt": 1790000500000,
 "edited": true, "revision": 2,
 "reply": {"text": "Fixed in 1.14.", "by": "owner", "at": 1790001000000},
 "editedAfterReply": false, "addressedIn": "v1.14", "addressedAt": 1790001000000}
```

Admins, and the reviewer looking at their own review (`viewer.review`), also get
`hidden`, `hiddenReason` and `reportCount`.

`viewer` on the package read:

```json
{"signedIn": true, "name": "alice", "emailVerified": true,
 "isMaintainer": false, "isAdmin": false, "canRate": true, "reason": "",
 "useRequired": false, "verifiedUse": false, "review": null}
```

When `canRate` is false, `reason` is one of the errors in the "Who can rate"
table. Use it to explain why, rather than showing the form.

## Views and opens

`marketplace_package_activity` (migration `0256`) keeps one row per market,
package and account. Each row holds `views` (Market page openings), `opens`
(openings of the package's own screen), `first_at` and `last_at`. The user
asked on 2026-10-12 to see how many times each person opened or used each
plugin.

| Method and path | Who | What |
|---|---|---|
| `POST /activity` `{market?, package, kind: "view"\|"open", source?}` | signed in | adds one to your `views` or `opens`; returns `{ok, market, package, kind, views, opens}` with your totals. `invalid_kind` (400), `unknown_package` (404) |
| `GET /{market}/{package}/activity?page=1&limit=50` (limit up to 200) | maintainers, admins | `{totals: {views, opens, people}, people: [{name, views, opens, firstAt, lastAt}], page, limit, hasMore}`, most opens first |

The package read (`GET /{market}/{package}`) adds `activity: {views, opens,
people}`, and its `viewer` adds `views` and `opens` (0 when signed out).

An **open** also writes the use record (see "Who can rate"), so a person who
opened the package can rate it. A **view** does not: looking at the Market
page is not using the package. Each person's own counts are theirs to
inflate. The phones report each package and kind at most once a minute, and
the totals are for maintainers, not a ranking.

## Quick stars on the phone

Each package page on the phones shows five stars near the top, filled to the
viewer's own rating. A tap at or above `rules.reviewRequiredBelow` (five)
saves the rating at once. A lower tap opens the review form with those stars
already picked, because the server still requires the review below five
stars. If the viewer already has a valid written review, a lower tap saves at
once and keeps the text.

## Notifications

Every rating write under 5 stars pings each maintainer (up to 25) with the ping
kind `package_review`. The title is "`N`-star review of `<package>`: how to
improve", the body is the improvement text (with the category first), and the
link goes to the review on `/market-reviews`. It goes through the ordinary
Pings path: an inbox row, the account socket, the APNs banner and the
instant or digest email. The ping kind is on by default for email, and people can
switch it off in their Pings settings like any other kind. When the reviewer
edits the rating, the new revision pings again. A maintainer's reply pings the
reviewer with the same kind.

## The earlier anonymous ratings

`POST /api/market/ratings` (one anonymous rating per browser, from migration
0165) now returns `410 account_rating_required`. `GET /api/market/ratings`
still reads the old anonymous totals. The `/market` listing reads
`/api/market/reviews/summary` instead.
