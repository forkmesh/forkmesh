# Distribution: "Runs that listen, branches that let go" (changelog, 2026-10-07)

**Owner:** Marketing
**Written:** 2026-10-07
**Status:** held. Gates 1 to 3 are open; the operator's approval to post is
open
**Plan:** [../plan.md](../plan.md), campaign CH2

## The post

Blogger's product update for work merged on main from 2026-10-04 to
2026-10-07, uncommitted in the main checkout on 2026-10-07. It will live at
`https://forkmesh.com/blog/runs-that-listen-branches-that-let-go/`.

CH2 replaces CH1's Mastodon and X posts, so there is one changelog post a
week on each. The Discord announcement links both posts.

## Brief

| Field | Value |
| --- | --- |
| Audience | Developers who already follow ForkMesh on Mastodon, X or Discord |
| Problem | They keep their code on another host and have not tried ForkMesh with a repository of their own |
| Message | A public repository on the hosted service can be merged, updated and cleaned up from the browser without the owner's desktop online, and merged pull requests stop leaving branches behind |
| Channels | Mastodon and X from the ForkMesh account; Discord announcements (Hearth). Not Reddit: a changelog post is self-promotion there. Not Hacker News: readiness is blocked |
| Call to action | The post's closing link, already tagged (below) |
| Deliverables and owners | Hero image and passing tests (Blogger); commit and merge (operator review); gate 3 check (Quality); Mastodon and X posts (operator posts the drafts below); Discord announcement (Hearth); post URLs written back into the post's `data-blog-social` block (Blogger) |
| Success measure | Signups with first-touch `utm_source=blog`, `utm_campaign=changelog`, `utm_content=runs-that-listen-branches-that-let-go` on `/admin/acquisition`, and how many reach `first_mirror_created` |
| Baseline | 0: the content value is new. Blog visits are not captured (`docs/attribution.md`); only the post's signup link is |
| Start | The day every gate below passes |
| End | 7 days after the start |
| Follow-up review | The next Marketing run after the end date |

With fewer than 10 tagged sessions, the result is reported as too small to
mean anything.

## Tagged link

```
https://app.forkmesh.com/signup?utm_source=blog&utm_medium=referral&utm_campaign=changelog&utm_content=runs-that-listen-branches-that-let-go
```

`blog`, `referral` and `changelog` are on the `ATTRIBUTION_ALLOWED_*` lists in
`app/wrangler.toml`; `/signup` is a Worker-routed page, so the visit is
captured.

## Gates before posting

1. The hero image
   `www/public/assets/blog/features/runs-that-listen-branches-that-let-go.webp`
   exists and the five tests that fail without it pass:
   three in `app/tests/test_blog_frontend.py`, one in
   `www/tests/test_blog_static_routing.py` and one in
   `app/tests/test_split_workers.py`. Done 2026-10-07: the post merged
   without it and broke the website deploy, so the image was generated then
   (`docs/www/README.md`, Recent changes).
2. The post is merged and deployed:
   `https://forkmesh.com/blog/runs-that-listen-branches-that-let-go/` answers
   200, and NodeInfo reports a commit that contains it.
3. Quality, with the decision D2 test account (one that has never paired a
   desktop, so the owner's desktop is offline by construction): mirrors a
   small public GitHub repository through New repository → Remote clone URL
   and clones the copy (this also closes CH1 gate 3); then, in a new public
   hosted repository `qa-ch2-merge-check`, pushes `qa/ch2-merge`, opens a
   pull, merges it from the web, and confirms the merge runs straight away
   and the branch is gone from the branch list and from a fresh clone. Only
   throwaway repositories the test account owns are touched. If D2 is
   declined, the Mastodon and X posts do not go out.

## Drafts

Posted from the ForkMesh account, so the affiliation is the account itself.
If anyone shares them from a personal account, add "I work on ForkMesh."

**Mastodon** (under 500 characters):

> Product update: Runs that listen, branches that let go.
>
> On the hosted ForkMesh service, merging, updating or deleting a branch of
> a public repository now runs on our cores, so your desktop doesn't have to
> be online. Merged pull requests no longer leave branches behind.
>
> Cloud runs can now take your follow-up prompts, and the phones can watch
> them work.
>
> https://forkmesh.com/blog/runs-that-listen-branches-that-let-go/

**X** (under 280 characters with the link counted as 23):

> ForkMesh update: merge, update or delete a branch of a public hosted repo
> from the web without your desktop online, and merged PRs stop leaving
> branches behind. Cloud runs now hear follow-ups.
> https://forkmesh.com/blog/runs-that-listen-branches-that-let-go/

**Discord announcement** (for Hearth to adapt):

> Two product notes are up:
> https://forkmesh.com/blog/runs-that-listen-branches-that-let-go/ and, from
> earlier in the week, https://forkmesh.com/blog/the-work-keeps-its-place/.
> The thing to try today is mirroring a public GitHub, GitLab or Codeberg
> repository from the hosted service's New repository page. If it fails or
> confuses you, tell us in #bugs.

## Result

Pending.
