# Campaign: "Is your repo mirrored?" (X, 2026-10-09)

**Written:** 2026-10-09, on Herald's request for a brief, a landing page and an
allowlisted campaign name so the campaign can be the next X post
**Status:** prepared. Posting waits on the gates below and a person pressing
Post
**Plan:** [../plan.md](../plan.md), campaign RM1
**Campaign tag:** `repo_mirrored`
**Landing page:** `https://forkmesh.com/mirror` (`www/templates/pages/mirror.html`)

## What the product does today

Checked in code on 2026-10-09 against main `cabbb8e3f5`, which is what the
served stack runs (NodeInfo `software.version` `cabbb8e3f599`):

| Copy may say | Evidence | Copy must not say |
| --- | --- | --- |
| Paste the `https://` URL of a public repository on github.com, gitlab.com or codeberg.org into **Remote clone URL** and press **Create & mirror** | `HOSTED_IMPORT_HOSTS` in `app/wrangler.toml`; `app/public/dashboard/partials/views/new-repo.html` | That the **GitHub / GitLab / Codeberg** tab copies code: it imports metadata only ("no Git contents") |
| ForkMesh copies every branch and tag | `server/hosted.go` fetches `+refs/heads/*` and `+refs/tags/*` | That issues, pull requests, releases, wikis or LFS files come along |
| The copy is kept on three servers | `HOSTED_REPOS.md`, core nodes `fmnext-n1` to `n3` | "An independent network" or "independently operated": one operator runs all three (`www/docs/launch/y-combinator-news-readiness.md`) |
| Anyone can clone a public copy without an account: `git clone https://app.forkmesh.com/OWNER/REPOSITORY` | [HOSTED_REPOS.md](../../HOSTED_REPOS.md) | Uptime, speed or "never down" |
| You push new commits to it with plain Git over HTTPS | The credential shown after **Create & mirror** | That it follows the original. The browser path copies **once**, on the primary core while the copy is empty; nothing fetches from GitHub afterwards |
| There is no payment step | No plan check in `app/src/hosted_git.py`; production cannot collect payments ([CFO review](../../finance/cfo.md)) | "Free forever" or any pricing promise |
| Up to 200 MiB per repository and 10 per account | `app/wrangler.toml`, `server/hosted.go` (256 MiB or 10 minutes on the node) | |
| Set it up before an outage | Copying from a host that is down fails | That you can copy a repository while its host is down |
| Public sources only | `HOSTED_REPOS.md`: private sources need a provider token and are not supported | That private repositories can be copied from the browser |

The word "mirrored" suggests a copy that keeps itself up to date, and the
browser path does not do that. The hook stays a question, and every answer
(page, post, image text) says "copy" and "push to keep it current", never
"sync", "automatically" or "stays up to date". The desktop app re-syncs its
own mirrors, but that is a different path and is not promised here.

There is no public way yet to check whether a given GitHub repository already
has a ForkMesh copy: `/api/repositories/search` matches names and
descriptions, not source URLs. So the page answers the question with "here is
how to make one", not with a checker.

## Brief

| Field | Value |
| --- | --- |
| Audience | Maintainers of small open-source projects whose only public home is one host (ideal user profile 1), reached on X |
| Problem | An outage, a policy change or a locked account takes the project's only public copy away from everyone who clones it |
| Message | Is your repo mirrored? Copy a public GitHub, GitLab or Codeberg repository to ForkMesh from your browser, and anyone can clone the copy without an account |
| Channel | X, from the ForkMesh account, as Herald's next ready post; a person presses Post. Mastodon draft below for the same day. Not Reddit or Hacker News: a product pitch is self-promotion there |
| Call to action | `https://forkmesh.com/mirror`, tagged (below); the page sends people to `/signup`, and signed-in people to `/dashboard/repos/new?source=remote` |
| Deliverables and owners | Landing page, allowlist entry and test (this change, Marketing); merge and deploy (operator review); path check (Quality, with the D2 test account); the ready post and its 16:9 image (Herald); pressing Post (a person); the campaign's slug and landing URL in the ForkMesh organization's Marketing page (a signed-in member) |
| Success measure | Sessions with first touch `x` / `repo_mirrored` on `/admin/acquisition`, then signups and `first_mirror_created` from those sessions; views, replies, reposts and likes on the post at 7 days |
| Baseline | 0: the tag is new. Activation (non-operator accounts with a public repository) was 0 on 2026-10-06 |
| Start | The day every gate passes |
| End | 14 days after the start |
| Follow-up review | The next Marketing run after the end date |

No signup target is set: X has sent 22 all-time visits (`t.co`), so with
fewer than 10 tagged sessions the result is reported as too small to mean
anything. The thing worth learning is whether anyone who lands on `/mirror`
reaches `first_mirror_created`, the north star's event.

## Tagged links

```
https://forkmesh.com/mirror?utm_source=x&utm_medium=social&utm_campaign=repo_mirrored&utm_content=is-your-repo-mirrored
https://forkmesh.com/mirror?utm_source=mastodon&utm_medium=social&utm_campaign=repo_mirrored&utm_content=is-your-repo-mirrored
```

`x`, `mastodon` and `social` were already on the `ATTRIBUTION_ALLOWED_*`
lists; `repo_mirrored` is added to `ATTRIBUTION_ALLOWED_CAMPAIGNS` in
`app/wrangler.toml` and `DEFAULT_ALLOWED_CAMPAIGNS` in
`app/src/attribution.py`. `/mirror` is a top-level page, so the marketing
Worker captures the visit, and the parent-domain cookie carries the session to
signup on app.forkmesh.com ([../../attribution.md](../../attribution.md)).
The page's own links are untagged so they keep the post's first touch.
EU/EEA visitors and browsers sending Do Not Track or GPC get no session, so
the count is a floor.

## Gates before posting

1. **Live.** The change is merged and deployed: `https://forkmesh.com/mirror`
   answers 200 and NodeInfo reports a version that contains it. The
   allowlist ships in the same deploy.
2. **The path works for a stranger.** A served-stack test account that has
   never paired a desktop (D2) copies a throwaway public GitHub repository
   through **Remote clone URL**, and the copy clones from a machine with no
   ForkMesh credential. This is the same check as CH1 gate 3; one walk passes
   both.
3. **A person posts.** Herald's ready post uses a draft below (or one that
   keeps every limit above), and a person presses Post on the ForkMesh
   account.

## Drafts

Posted from the ForkMesh account, so the affiliation is the account itself.
If anyone shares it from a personal account, add "I work on ForkMesh."

**X** (`www/tests/test_mirror_page.py` checks it fits 280 characters with the
link counted as 23):

<!-- repo-mirrored-x:start -->
```text
Is your repo mirrored?

If its only public copy lives on one host, an outage or a locked account takes it from everyone who clones it.

Paste a public GitHub, GitLab or Codeberg URL into ForkMesh and anyone can clone the copy, no account needed.

https://forkmesh.com/mirror?utm_source=x&utm_medium=social&utm_campaign=repo_mirrored&utm_content=is-your-repo-mirrored
```
<!-- repo-mirrored-x:end -->

Image (Herald, 16:9): two repository cards side by side, one labelled with a
host's name and one labelled ForkMesh, with the caption "One copy is a single
point of failure." No GitHub logo, no status-page screenshot, no outage
staged as real.

**Mastodon** (under 500 characters):

> Is your repo mirrored?
>
> If a project's only public copy lives on one host, an outage, a policy
> change or a locked account takes it away from everyone who clones it.
>
> ForkMesh can copy a public GitHub, GitLab or Codeberg repository from your
> browser, and anyone can clone the copy without an account. It's a copy you
> push to, not a live sync, and code only: issues and PRs stay put.
>
> https://forkmesh.com/mirror?utm_source=mastodon&utm_medium=social&utm_campaign=repo_mirrored&utm_content=is-your-repo-mirrored

Replies: if someone asks whether it stays in sync, the answer is no: push to
it as a second remote (`git remote add forkmesh …`, then `git push forkmesh`).
Tone is helpful, never mocking another host or an incident in progress.

## Result

| Channel | Posted | Where | Tagged sessions, signups, first mirrors | Views, replies, reposts, likes at 7 days |
| --- | --- | --- | --- | --- |
| X | Not posted | | | |
| Mastodon | Not posted | | | |
