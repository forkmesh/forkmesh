# Market ratings and improvement reviews

Signed-in accounts rate Market packages from 1 to 5 stars. **A rating below five
stars must come with a review** saying what did not work and what would earn
five stars; the server enforces this. A 5-star rating needs no review.

- Server: `app/src/marketplace_reviews.py`, `app/src/marketplace_runtime.py`,
  migration `0198_marketplace_reviews.sql`.
- Web: `/market-reviews?market=<market>&package=<id>`
  (`app/public/market-reviews.{html,js}`, `market-review-form.js`); the
  `/market` listing shows averages.
- Tests: `app/tests/test_marketplace_reviews.py`,
  `test_market_review_form_frontend.py`, `app/tests/host/test_market_reviews_e2e.py`.
- Clients send the fields, show the returned `fields` errors and use `rules`
  for counters; no client-side rules are needed.

## Who can rate

| Rule | Error (status) |
|---|---|
| signed in (cookie or `Authorization: Bearer <sessionToken>`) | `sign_in_required` (401) |
| verified email, active account | `email_not_verified` (403, with `verifyUrl`) |
| not a maintainer of the package | `maintainer_cannot_rate` (403) |
| has an install/use record, if the package has any | `package_use_required` (403) |
| at most 10 rating writes an hour and 30 a day | `rating_rate_limited` (429) |

One rating per account per package; later writes update it, and every change
is kept in `marketplace_review_history`. Clients report installs or uses with
`POST /api/market/reviews/uses`; organization installs count for members. A
package with no records accepts ratings marked `verifiedUse: false`.

Maintainers: first-party packages are maintained by the publishing
organization's owners, admins and maintainers; community packages by the
accounts that submitted published versions. Platform admins moderate all.

## Fields and validation

| Field | Below 5 stars | 5 stars |
|---|---|---|
| `stars` | integer 1–5 | 5 |
| `problem` | required | optional |
| `improvement` | required | optional |
| `context`, `comment` | optional | optional |
| `category` | `bug`, `missing-feature`, `docs`, `performance`, `ux`, `other` | same |
| `version` | e.g. `1.4`, `v1.4.2` (stored as `v1.4`) | same |

Text is trimmed and capped at 4000 characters. Required fields are rejected as
`required`, `too_short` (under 40 characters), `too_few_words` (under 8),
`repeated_characters`, `repetitive`, `same_as_problem` or `too_long`. Errors
answer 400 `review_required` or `review_invalid`:

```json
{"error": "review_invalid",
 "fields": {"problem": "too_few_words", "category": "invalid"},
 "rules": {"reviewRequiredBelow": 5, "minChars": 40, "minWords": 8,
           "maxChars": 4000, "categories": [{"id": "bug", "label": "Bug"}]}}
```

## Endpoints

Under `/api/market/reviews`; writes need an allowed origin; bodies at most
32 KiB.

| Method and path | Who | What |
|---|---|---|
| `GET /summary` | public, cached 60 s | per-package summaries and usage totals |
| `GET /{market}/{package}?sort=&page=&limit=&stars=` | public | package, `summary`, a page of `reviews`, `viewer`, `activity` |
| `POST`/`PUT /{market}/{package}` | raters | create (201) or update (200) |
| `GET /{market}/{package}/feedback` | maintainers, admins | improvement feedback with filters |
| `GET /maintained` | signed in | packages you maintain with open feedback counts |
| `POST /uses` | signed in | record an install or use |
| `GET /items/{id}/history` | reviewer, maintainers, admins | revisions |
| `POST`/`DELETE /items/{id}/reply` | maintainers, admins | public reply (2000 characters), optionally marking it addressed |
| `POST`/`DELETE /items/{id}/addressed` | maintainers, admins | mark "addressed in vX.Y" |
| `POST`/`DELETE /items/{id}/helpful` | signed in, not the author | helpful votes (200 a day) |
| `POST /items/{id}/report` | signed in, not the author | abuse report (20 a day) |
| `POST /items/{id}/moderation` | admins | hide (with reason) or unhide |
| `GET /reports` | admins | reported reviews |
| `POST /activity` | signed in | count a `view` or `open` |
| `GET /{market}/{package}/activity` | maintainers, admins | per-person views and opens |

`Summary` is `{"average", "count", "histogram"}`, cached in
`marketplace_package_rating_stats` and recomputed on every relevant write;
hidden reviews do not count. The `viewer` object reports `canRate` and, when
false, a `reason` from the table above.

`marketplace_package_activity` keeps per-account `views` (Market page) and
`opens` (the package's own screen). An open also writes the use record; a view
does not.

## Clients and notifications

Phones show quick stars: a 5-star tap saves at once, a lower tap opens the
review form (or saves at once if a valid review exists). Every rating below 5
stars pings up to 25 maintainers with kind `package_review` (inbox, socket,
push, email per Pings settings); edits ping again, and maintainer replies ping
the reviewer.

The earlier anonymous `POST /api/market/ratings` answers
`410 account_rating_required`; its `GET` still reads old totals.
