# Public-index search-to-clone experiment

**Related issue:** [#478 — Launch the searchable public repository index](https://github.com/forkmesh/forkmesh/issues/478)  
**Run date:** 2026-09-12  
**Status:** stopped after the bounded verification run; continue engineering, do not scale traffic

## Audience and hypothesis

The audience is maintainers of small open-source CLI and library projects who
want a second, durable public place where a new contributor can inspect and
clone their project.

Hypothesis: a public index entry that explains freshness and clone health will
turn more high-intent discovery sessions into a repository result and an
anonymous clone than a homepage-only path. The experiment would continue only
if the seven-day cohort reaches both 30% search-to-result and 15%
result-to-clone, with no signup wall and no increase in moderation failures.

## Bounded protocol

The intended cohort is 100 eligible sessions per source over seven days, with
the first 100 sessions retained for the decision. A session is eligible only
when it is not classified as a bot by the existing attribution rules. A
qualified signup means `signup_completed` followed by
`first_mirror_created` in the same attribution session; a signup alone is not
treated as maintainer value.

These are the tagged entry links. They use the allowlisted attribution values
for campaign `search_to_clone_478`:

| Source | Tagged entry link |
| --- | --- |
| Organic search | [`forkmesh.com/?utm_source=search&utm_medium=organic&utm_campaign=search_to_clone_478&utm_term=python-cli`](https://forkmesh.com/?utm_source=search&utm_medium=organic&utm_campaign=search_to_clone_478&utm_term=python-cli) |
| Hacker News referral | [`forkmesh.com/?utm_source=hn&utm_medium=referral&utm_campaign=search_to_clone_478&utm_term=maintainer-preservation`](https://forkmesh.com/?utm_source=hn&utm_medium=referral&utm_campaign=search_to_clone_478&utm_term=maintainer-preservation) |
| Reddit referral | [`forkmesh.com/?utm_source=reddit&utm_medium=referral&utm_campaign=search_to_clone_478&utm_term=oss-maintainer`](https://forkmesh.com/?utm_source=reddit&utm_medium=referral&utm_campaign=search_to_clone_478&utm_term=oss-maintainer) |

The visitor may read the catalog, open a result, copy its HTTPS clone URL, and
run `git clone` without an account. Signup is presented only as an optional
follow-up for maintainers who want to publish or collaborate.

## Ranking and spam controls shown to the visitor

The index is not a popularity leaderboard. The current catalog order is the
deterministic `updatedAt` descending order; search is a local filter over the
published owner, name, description, channel, source, and commit fields. That
limitation is part of this experiment: it does not claim query relevance.

Before a record reaches the anonymous catalog it must be a signed public
record. Active Terms flags are withheld, private records are excluded, and
clone availability is derived from a fresh, healthy, integrity-verified,
non-abuse-blocked HTTPS endpoint. Mirror rows are grouped with their canonical
repository rather than counted as independent discoveries. These controls
remain visible in the result status and are not replaced by signup or payment
gates.

## Baseline and outcome

The baseline was taken from the repository contracts and a production probe
attempt on 2026-09-12. The probe could not resolve either public hostname from
the execution environment (`forkmesh.com` and `app.forkmesh.com` both returned
curl HTTP 000 / DNS failure), so no production visitor was exposed and no
conversion denominator was manufactured.

| Measure | Pre: existing public path | Post: tagged index run | Result |
| --- | ---:| ---:| --- |
| Search → repository result | `N/A (0/0)` | `N/A (0/0)` | Not measurable: search is a client-side filter and emits no search event |
| Repository result → anonymous clone | `N/A (0/0)` | `N/A (0/0)` | Not measurable: result-open is not emitted and Git cannot carry the browser session |
| Qualified signup impact | `N/A (0 observed)` | `N/A (0 observed)` | No production cohort; no qualified signup can be attributed |

These are intentionally `N/A`, not 0% conversion claims. The existing
attribution contract records `landing_view`, `clone_url_copied`, signup, and
first-mirror events, but a copied clone URL is only an intent signal and an
anonymous clone is deliberately not joined to a browser session. The existing
public daily clone counter can report aggregate anonymous clones after its
snapshot boundary, not source-level conversion.

## Failure modes found

1. The public host was not resolvable from the runner, so the live search and
   clone legs could not be exercised.
2. `/dashboard/repos` is served as a static asset path and bypasses the
   server-side attribution hook. A UTM-tagged direct link there would look
   tagged to a human but would not create the intended attribution session.
3. Search and result-open have no events, so search-to-result cannot be
   calculated from the acquisition report.
4. Git clone has no browser cookie by design. Joining it with a campaign would
   require putting an identifier in the clone URL, which would persist in
   `.git/config` and violate the privacy boundary.
5. The current ordering is recency, not maintainer-intent relevance; a new or
   quiet project can be buried even when it matches the query.

## Stop / continue decision

**Stop traffic expansion. Continue the product work.** Issue #478 should not
be closed on this run: there is no production cohort and the required rates
are not yet observable. Before repeating the experiment, add privacy-safe
`index_search` and `index_result_open` events, route tagged entries through a
captured top-level landing page, and expose the existing aggregate clone
counter beside (never combined with) the attributed intent signal. Then rerun
the same seven-day, 100-session-per-source protocol.

The next go decision is: at least 100 eligible sessions per source, at least
30% search-to-result, at least 15% result-to-clone using the documented
aggregate/intent limitation, and zero unexplained moderation or private-data
leaks. Until those conditions are met, keep public reading and cloning open
and keep ranking and filtering rules documented rather than optimizing for
signups.

