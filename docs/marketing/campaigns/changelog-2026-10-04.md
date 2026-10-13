# Distribution: "The work keeps its place" (changelog, 2026-10-04)

**Owner:** Marketing
**Written:** 2026-10-04
**Status:** prepared. Gates 1 and 2 passed on 2026-10-06 (the post and its
tagged signup link answer 200); gate 3 and the operator's approval to post
are open. On 2026-10-07 the Mastodon and X drafts below were superseded by
CH2 ([changelog-2026-10-07.md](changelog-2026-10-07.md)); the Discord
announcement there links this post too
**Plan:** [../plan.md](../plan.md), campaign CH1

## The post

Blogger's product update, commit `5ab5eefef5` on branch
`1374-blogger-persistent-blog`, not on main yet. It will live at
`https://forkmesh.com/blog/the-work-keeps-its-place/`. It covers work merged
from 2026-10-02 to 2026-10-04 and labels each item as on the hosted service,
on main, or waiting for a release. The social copy below keeps those labels.

## Brief

| Field | Value |
| --- | --- |
| Audience | Developers who already follow ForkMesh on Mastodon, X or Discord |
| Problem | They keep their code on another host and have not tried ForkMesh with a repository of their own |
| Message | Paste a public GitHub, GitLab or Codeberg URL into the hosted service and get a cloneable copy; the rest of the week's work is merged and waiting for its releases |
| Channels | Mastodon and X from the ForkMesh account; Discord announcements (Hearth). Not Reddit: a changelog post is self-promotion there, and the Reddit agent is read-only. Not Hacker News: readiness is blocked |
| Call to action | The post's closing link, changed to the tagged signup link below |
| Deliverables and owners | Tagged CTA in the post (Blogger); allowlist entries `blog` and `changelog` (Marketing, this branch); Mastodon and X posts (operator posts the drafts below); Discord announcement (Hearth); post URLs written back into the post's `data-blog-social` block (Blogger) |
| Success measure | Signups with first-touch `utm_source=blog`, `utm_campaign=changelog` on `/admin/acquisition`, and how many of them reach `first_mirror_created`. Secondary: engagements and link clicks on the Mastodon and X posts |
| Baseline | 0: the campaign value is new. Blog visits are not captured (`docs/attribution.md`), so the share links themselves are not measured; only the post's signup link is |
| Start | The day every gate below passes |
| End | 7 days after the start |
| Follow-up review | The next Marketing run after the end date; planned 2026-10-12 |

With fewer than 10 tagged sessions, the result is reported as too small to
mean anything.

## Tagged link

The post's "Try ForkMesh in the browser" link points at `/desktop`, the
desktop install page. It should point at the browser signup, tagged:

```
https://app.forkmesh.com/signup?utm_source=blog&utm_medium=referral&utm_campaign=changelog&utm_content=the-work-keeps-its-place
```

`/signup` is a Worker-routed page, so the visit is captured. `blog` and
`changelog` are added to `ATTRIBUTION_ALLOWED_SOURCES` and
`ATTRIBUTION_ALLOWED_CAMPAIGNS` in `app/wrangler.toml` and to the defaults in
`app/src/attribution.py` on branch `1373-marketing-demand-work`. Until that
deploys, the values are stored as `other`, with the original kept in
`metadata.utm_other`.

## Gates before posting

1. The post is merged and deployed:
   `https://forkmesh.com/blog/the-work-keeps-its-place/` answers 200.
2. The post's CTA uses the tagged link, and the allowlist change is
   deployed.
   The CTA is done: Blogger's commit `805c02f05e`, merged into main at
   `bc466c7b24` and deployed with `cffabd181d88` (2026-10-04 22:25 UTC).
3. A person has mirrored a public GitHub repository from the hosted
   service's New repository page and cloned the copy, so the lead claim is
   checked on the served stack (Quality, once it has a test account, or the
   operator).

## Drafts

Posted from the ForkMesh account, so the affiliation is the account itself.
If anyone shares them from a personal account, add "I work on ForkMesh."

**Mastodon** (under 500 characters):

> Product update: The work keeps its place.
>
> On the hosted ForkMesh service, paste a public GitHub, GitLab or Codeberg
> repository URL and get a cloneable copy.
>
> Merged on main and waiting for their releases: web merges that finish on
> the hosted cores, cloud runner sessions you can follow to a reviewed
> change, and an Android copy of each repository that syncs forward.
>
> https://forkmesh.com/blog/the-work-keeps-its-place/

**X** (273 characters with the link counted as 23):

> New ForkMesh update: paste a public GitHub, GitLab or Codeberg repository
> URL into the hosted service and get a cloneable copy. Web merges on our
> cores, followable runner sessions and Android repo sync are merged on main
> and wait for their releases.
> https://forkmesh.com/blog/the-work-keeps-its-place/

**Discord announcement** (for Hearth to adapt):

> This week's product notes are up:
> https://forkmesh.com/blog/the-work-keeps-its-place/. The thing to try
> today is mirroring a public GitHub, GitLab or Codeberg repository from the
> hosted service's New repository page. If it fails or confuses you, tell us
> in #bugs.

## Result

Pending.
