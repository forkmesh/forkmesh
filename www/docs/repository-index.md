# Searchable public repository index

The public index lets anyone find an active, trustworthy project and clone it
without an account and without already knowing its ForkMesh URL. This page is
the published methodology: it describes exactly what
`app/src/repository_index.py` does, and changes to the ranking must update
this page in the same commit.

Tracking issue: #478.

## Endpoint

```
GET /api/repositories/search?q=<text>&view=<view>&limit=<1-50>&cursor=<token>
                            &language=<name>&topic=<name>&owner=<account>
                            &cloneable=1
```

No authentication. The response is public catalog data only (it never
includes private or shared repositories, even for a signed-in caller) and
may be cached for 30 seconds.

| Parameter | Meaning |
| --- | --- |
| `q` | Up to 8 words. Every word must match the name, owner, a topic, a language, or the description. |
| `view` | `relevance` (default when `q` is set), `updated` (default otherwise), `trending`, `new`, `discussions`. |
| `limit` | Page size, 1–50, default 20. |
| `cursor` | The previous page's `nextCursor`. A cursor is bound to its query; reusing it with different filters answers `400 cursor_query_mismatch`. |
| `language`, `topic` | Exact, case-insensitive facet filters. |
| `owner` | The publishing account or the organization that owns the repository. |
| `cloneable` | `1` keeps only repositories with a healthy clone endpoint right now. |

Malformed parameters answer `400` with one of `unknown_view`,
`relevance_requires_query`, `invalid_limit`, `invalid_cloneable`,
`invalid_cursor`, or `cursor_query_mismatch`. If no catalog can be read, the
endpoint answers `503 catalog_unavailable` rather than an empty result.

Every result carries:

- `identity` — the provider-independent repository identity
  (`urn:forkmesh:repository:sha256:…`, derived from the signed root commit),
  shared by every mirror and unchanged by renames or host moves. It is empty
  when the record has no root commit. See
  [dependency workflows](dependency-workflows.md).
- `clone.https` and `clone.command` — a copy-ready `git clone` of the
  repository's durable public address, with `clone.requiresAccount: false`.
  `clone.ssh` appears only when the SSH gateway is configured.
- `maintainer` — the publishing account, the owning organization when there
  is one, the maintainer's public key, and
  `verification: "account-key-signed-catalog-record"`.
- `availability` — `cloneOnline` (a fresh, healthy, integrity-verified mirror
  endpoint can serve it now), and the number of independent mirrors.
- `activity`, `freshness`, `duplicates`, and `ranking` — the exact inputs and
  sort key that placed it where it is.

The response also reports `total`, `facets` (language and topic counts over
all matches, plus how many are cloneable), `withheld` (how many catalog rows
each rule removed), `freshness`, and `budgets`.

## Budgets

| Budget | Value | How it holds |
| --- | --- | --- |
| Freshness | 6 minutes | Results are ranked from the published catalog snapshot, which is republished when the catalog changes and expires after 6 minutes. `freshness.snapshotAgeMs` and `freshness.withinBudget` report the actual age on every response. An expired snapshot falls back to the live anonymous catalog (at most 30 seconds old). |
| Latency | 250 ms ranking | Ranking over a full 200-repository catalog is covered by an automated budget test. The route reads one D1 row; it never decrypts the repository table while a snapshot is current. |
| Page size | 50 | `limit` above 50 is rejected, not silently truncated. |

## Determinism and pagination

Results are sorted by a full key that always ends in the unique
`owner/name`, so equal scores never reorder between requests. Pagination is
keyset-based: `nextCursor` encodes the last result's sort key, and the next
page starts strictly after it. Walking every cursor over one snapshot returns
each match exactly once. When the snapshot changes between pages, results
that moved behind the cursor are not repeated. `nextCursor` is `null` on the
last page.

## Views

All scores are integers built only from signed repository facts.

- **relevance** — per word: name exact 100, name prefix 40, name contains 20;
  owner exact 30 or contains 10; topic exact 15; language exact 15;
  description contains 5. Ties sort by most recent activity.
- **updated** — most recent head-commit time, or the signed record time when
  the publisher did not report a commit time.
- **trending** — commits plus merges over the last 4 weekly buckets, each
  week capped at 50, plus 5 points per independent mirror (at most 8 mirrors
  count).
- **new** — the publisher's signed hosted-since time, newest first.
  Repositories without one are left out of this view.
- **discussions** — open discussions plus pull requests plus open issues,
  each capped at 500.

Trending and discussions leave out repositories whose signed record is older
than 14 days, and repositories with a zero score. Those repositories stay
searchable.

## Moderation and anti-manipulation rules

Before a row can appear:

1. It must be in the public catalog, which accepts only records signed by the
   account's key. Private records, records shared with a viewer, blocked
   identities, and repositories with an active Terms flag are withheld.
2. It must name an owner, a repository, and a maintainer key.

Deduplication and manipulation resistance:

- **One result per repository.** Mirror rows (a different machine or account
  serving the same repository) are never separate results. They only add to
  the canonical repository's mirror count.
- **Self-mirroring does not count.** Only mirrors run by a different account
  count as independent. Extra devices on the owner's own account, and accounts
  that republished the same content, add nothing.
- **Republished content is collapsed.** Two catalog records with the same
  head commit are byte-identical history. The earliest hosted publisher (then
  alphabetical) is the result, and the others are listed in `duplicates`.
- **Replay and bulk history are bounded.** The catalog rejects a record older
  than the one it already holds (`stale_update`), so an old signed record
  cannot be replayed to restore a past ranking. Weekly activity is capped, so
  generated or rewritten history cannot buy unlimited trending weight.
- **Heartbeats do not rank.** Republishing an unchanged record does not
  change any score, because scores ignore the record's publish timestamp in
  favour of commit time and activity buckets.
- **Self-reported traffic is not a signal.** Clone and page-view counters
  come from the serving node and could be inflated by cloning oneself, so no
  view ranks on them.
- **Clocks are bounded.** A hosted-since or commit time more than 5 minutes
  in the future is ignored.

Reports of spam or abuse go through the existing Terms flag process. An
active flag removes the repository from every view on the next snapshot.

## From result to clone

A result's `clone.command` works with stock Git and no ForkMesh account:

```
curl -s 'https://app.forkmesh.com/api/repositories/search?q=mesh&cloneable=1&limit=1' \
  | python3 -c 'import json,sys; print(json.load(sys.stdin)["results"][0]["clone"]["command"])'
```

The clone URL is the repository's public address, not a particular mirror,
so it keeps working after the machine that answered today goes away.

## Not covered yet

- The dashboard repository list still filters client-side. It does not call
  this endpoint yet.
- Privacy-safe `index_search` and `index_result_open` events are not emitted,
  so search-to-result conversion cannot be measured yet (see
  `docs/experiments/public-index-search-to-clone-2026-09.md`).
- Maintainer verification is key-based. There is no separate human identity
  check.
