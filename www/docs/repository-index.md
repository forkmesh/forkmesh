# Searchable public repository index

The public index finds active projects that anyone can clone without an
account. `app/src/repository_index.py` implements exactly this methodology.

## Endpoint

`GET /api/repositories/search` (anonymous; public catalog data only; cacheable
30 s). Parameters: `q` (up to 8 words, all must match), `view`, `limit` (1–50,
default 20), `cursor`, `language`, `topic`, `owner`, `cloneable=1`. Errors are
`400` (`unknown_view`, `relevance_requires_query`, `invalid_limit`,
`invalid_cloneable`, `invalid_cursor`, `cursor_query_mismatch`) or
`503 catalog_unavailable`.

Each result has `identity` (the root-commit repository URN, see
[dependency workflows](dependency-workflows.md)), `clone` (a stock `git clone`
command), `maintainer` (key-signed catalog record), `availability`, and the
`ranking` inputs. Responses also give `total`, `facets`, `withheld`,
`freshness` and `budgets`.

## Budgets

Freshness 6 minutes (catalog snapshot age, reported per response); latency
250 ms for ranking a full catalog; page size 50 (larger limits are rejected).

## Determinism and pagination

The sort key always ends in the unique `owner/name`. `nextCursor` is a keyset
cursor bound to the query; walking all pages returns each match once, and it
is `null` on the last page.

## Views

- **relevance**: per-word integer scores for name, owner, topic, language and description.
- **updated**: latest head-commit time.
- **trending**: commits and merges over 4 weeks, each week capped at 50, plus
  5 points per independent mirror (at most 8 mirrors count).
- **new**: signed hosted-since time.
- **discussions**: open discussions, pull requests and issues, each capped at 500.

Trending and discussions skip records older than 14 days and zero scores.

## Anti-manipulation rules

Only signed public records appear; flagged, blocked or private ones are
withheld. Each repository is one result; only mirrors run by other accounts
count; identical heads collapse into `duplicates`; old records cannot be
replayed; heartbeats and self-reported traffic never rank; timestamps more
than 5 minutes in the future are ignored.
