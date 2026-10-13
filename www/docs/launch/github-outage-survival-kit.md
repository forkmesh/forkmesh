# GitHub outage survival kit

Status: **drafted, not published**

Drafted: **2026-09-14** (issue #522)

Owner: a human launch owner, with the Marketing teammate
(`.forkmesh/teammates/marketing.json`) for evidence and copy review.

This kit has three parts: a 60-second video script, a post thread, and the
terms for a $5 bounty. Together they get developers to put a repository on
ForkMesh before the next GitHub outage, so they can keep cloning and shipping
while it lasts. Everything here is a draft. Recording the video, posting the
thread, opening the bounty, and paying claims are human actions. Issue #522
closes only when the [publication evidence](#publication-and-closure-evidence)
table has real URLs.

## Campaign at a glance

| | |
| --- | --- |
| Audience | Developers and small teams whose code, CI, and dependency installs all go through `github.com` |
| Goal | Get repositories mirrored on ForkMesh **before** an outage, and prove that cloning keeps working during one |
| Channels | X (thread and video), reposted to the ForkMesh blog and fediverse account |
| Deliverables | The [video](#1-sixty-second-video), the [thread](#2-thread-three-ways-to-never-lose-code), the [bounty](#3-the-5-mirror-bounty) |
| Success measure | Valid bounty claims (a clone from the `forkmesh.com` URL works), not views, likes, or reposts |
| Follow-up | During the next GitHub incident, repost the thread once, then publish a short write-up with the claim count and anything that broke |

## Claim boundaries

Every sentence in the video, thread, and bounty must fit inside these limits.
If a draft needs to say more, change the product first.

| Copy may say | Evidence | Copy must not say |
| --- | --- | --- |
| Creating the ForkMesh repository takes one click (**Create empty repository**); moving the code there takes one `git push` | [Hosted repositories](../hosted-repositories.md) (#676) | "Switch in one click" with no push, or that the switch happens automatically |
| A public GitHub import is cloned by ForkMesh hosts and refreshed about every 30 minutes, then served with no provider in the path | [Ending the provider dependency](../repository-imports.md#ending-the-provider-dependency) (#679) | That private imports are hosted, or that the copy is real-time |
| `git clone https://forkmesh.com/OWNER/REPOSITORY` is served by whichever eligible mirror is healthy | [Product promise](../product-promise.md), `README.md` | Uptime, speed, or "never down" guarantees |
| One `insteadOf` line points pinned Git dependencies at a mirror without changing lock files | [Dependency workflows](../dependency-workflows.md#switching-providers-without-editing-a-lock) | That every GitHub dependency is already on ForkMesh |
| Set up **before** the outage | [External dependency map](../external-dependency-map.md): new imports fail while the provider API is down | That you can import from GitHub while GitHub is down |
| Your local clone can be pushed to an empty ForkMesh repository at any time | [Hosted repositories](../hosted-repositories.md) | That issues and pull requests come along with a plain push |

Tone: helpful, not gloating. Never mock GitHub, its staff, or an incident in
progress, and never use a screenshot of GitHub's status page as a punchline.

## 1. Sixty-second video

Title: **GitHub is down. Keep shipping in one click and one push.**

### Recording setup

- Use a disposable public repository and a disposable ForkMesh account. The
  account holder creates it; do not use seeded or bot accounts.
- The push credential appears in the **Create & mirror a repository** modal.
  Keep it off camera or blur it in the edit, and sign that session out
  (**Settings → Sessions**) after recording, which revokes it.
- 1920×1080, terminal font at least 20 px, browser zoom 125%. Burn in the
  on-screen captions below; ship narration as a soft subtitle track.
- The outage in the opening shot is staged. Say so on screen.

### Timeline

| Time | Picture | Narration | On-screen caption |
| --- | --- | --- | --- |
| 0:00–0:06 | Title card on a dark background | "GitHub is down. Your commits are still on your laptop. Here's how to keep shipping." | Staged demo: origin unreachable |
| 0:06–0:14 | Terminal: `git push origin main` fails with a connection error | "Your push fails, and your teammates can't clone." | `git push` fails |
| 0:14–0:26 | Browser: app.forkmesh.com → **Repositories → New** → the **Empty repository** source is already selected → type a name → click **Create empty repository** | "On ForkMesh, create an empty repository. That's the one click." | One click: **Create empty repository** |
| 0:26–0:40 | Terminal: `git remote add forkmesh https://app.forkmesh.com/you/app` then `git push -u forkmesh main`; username prompt shown, password blurred | "Add it as a remote and push. Every branch and commit you have is now on ForkMesh." | One push |
| 0:40–0:52 | Second terminal, different directory: `git clone https://forkmesh.com/you/app`, then `git rev-parse HEAD` next to the same command in the first terminal; the hashes match | "A teammate clones from forkmesh.com and gets the exact same commit." | Same commit, no GitHub |
| 0:52–1:00 | End card: kit URL and the bounty line | "Do it before the next outage. Mirror one repo, post proof, and we'll send you five dollars." | forkmesh.com/docs/launch/github-outage-survival-kit · $5 bounty, terms apply |

### Video description and alt text

Description for the post: "A 60-second walkthrough: when GitHub is
unreachable, create an empty ForkMesh repository, push your local clone to it,
and clone it back from forkmesh.com. Terms for the $5 mirror bounty:
https://forkmesh.com/docs/launch/github-outage-survival-kit/"

Alt text: "Screen recording. A git push to GitHub fails. In the ForkMesh web
app the presenter creates an empty repository, adds it as a Git remote, and
pushes. A second terminal clones the repository from forkmesh.com, and both
terminals show the same commit hash."

## 2. Thread: three ways to never lose code

Post from the ForkMesh account on X. Replace the video placeholder with the
uploaded video. X counts each link as 23 characters; `www/tests` checks every
post against the 280-character limit. The commands in posts 2 and 4 are real
Git commands and stay copy-pasteable.

<!-- outage-thread:start -->

```text
GitHub will go down again. Your commits are safe on your laptop, but your team, CI, and installs aren't.

3 ways to keep shipping when it happens. Set them up before the outage.
```

```text
1. Push every commit to two places.

Create an empty ForkMesh repo and add your SSH key, then:

git remote set-url --add --push origin git@github.com:you/app.git
git remote set-url --add --push origin ssh://git@ssh.forkmesh.com/you/app.git

Now one git push updates both.
```

```text
2. Import the repo and let a host keep a copy.

app.forkmesh.com: Repositories, New, GitHub, paste the URL, Start resumable import.

For a public repo, ForkMesh hosts clone it and refresh it about every 30 minutes, so it still clones when GitHub can't.
```

```text
3. Keep builds installing. Pin git dependencies to full commit IDs, then point your mirrored org at ForkMesh with one line:

git config --global url."https://forkmesh.com/acme/".insteadOf "https://github.com/acme/"

Lock files don't change. The commit ID is the content.
```

```text
Already down? New imports need GitHub's API, so they wait. But your local clone can go to an empty ForkMesh repo right now: one click, one push.

60 seconds: [video]
```

```text
Try it for $5: mirror one of your public repos on ForkMesh, clone it from forkmesh.com, and post proof. Say it's for the bounty.

Terms: https://forkmesh.com/docs/launch/github-outage-survival-kit/
```

<!-- outage-thread:end -->

Posting notes:

- Post 4 only works for dependencies whose repositories are mirrored on
  ForkMesh under the same owner and name. If someone asks, link
  [dependency workflows](../dependency-workflows.md).
- Do not reply to strangers' outage complaints with the thread. Only quote-post
  the ForkMesh account's own thread.
- Private repositories: an import of a private repository is never hosted.
  Point people at way 1, which pushes over SSH with their own key.

## 3. The $5 mirror bounty

### Offer

> Mirror one of your public GitHub repositories on ForkMesh, clone it from its
> `forkmesh.com` URL, and post proof on X. The first valid claims get $5 each,
> until the campaign budget runs out.

### Eligibility

1. **One claim per person and per ForkMesh account.** Accounts created in bulk,
   seeded, or run by bots are not eligible. ForkMesh staff and contractors are
   not eligible.
2. **The repository is public, and you own or maintain it on GitHub.** It
   existed on GitHub before the campaign opened and has at least one commit.
   Forks of `forkmesh/forkmesh` do not count.
3. **It is on ForkMesh by either path:** an import that reads *Fully hosted by
   ForkMesh* in **Repositories**, or an empty ForkMesh repository you pushed
   the code to.
4. **The post is public on X, still up at review, and discloses the payment.**
   It includes the `https://forkmesh.com/OWNER/REPOSITORY` URL, a screenshot
   or text of the successful `git clone`, and a plain disclosure such as
   "for the ForkMesh $5 bounty" or `#ad`.

We never pay for likes, reposts, follows, or replies, and a claim never has to
ask anyone for them.

### How to claim

Comment on the campaign issue with the post URL and the ForkMesh repository
URL. Never post payout details publicly. The reviewer asks for them privately
once a claim is approved.

### Reviewer checklist

Run these from a clean machine or container, with no ForkMesh credential:

```sh
git clone https://forkmesh.com/OWNER/REPOSITORY bounty-check
git -C bounty-check fetch -q https://github.com/OWNER/REPOSITORY HEAD
git -C bounty-check merge-base --is-ancestor HEAD FETCH_HEAD && echo "commit is on GitHub"
curl -s https://app.forkmesh.com/api/repo/OWNER/REPOSITORY/mirrors
```

Approve only when all of these hold:

- The clone succeeds from the `forkmesh.com` URL.
- The cloned `HEAD` is GitHub's default-branch commit or an ancestor of it, so
  the mirror holds the claimant's real history.
- The mirrors payload lists at least one online mirror.
- The post meets every eligibility rule, including the disclosure.
- The account and repository have no earlier claim.

Record each decision (approved or rejected, with the reason) as a comment on
the campaign issue. Leave payout details out of the comment.

### Payout and limits

- $5 USD per approved claim, capped by the budget the owner sets before
  opening. The campaign closes when the budget is spent or on its end date,
  whichever comes first. Say so in a post when it closes.
- The owner picks the payout method before launch. Claims are paid by a
  person, not by the microjob or reward-pool code: this bounty is not a
  `JOBS` entry in `app/src/microjobs.py`, and the issue-bounty route is frozen.
- The bounty is a small, optional thank-you. It is not required to use ForkMesh
  and has no effect on identity, permissions, ranking, or moderation
  ([incentive independence](../incentive-independence.md)).
- Rejected claims can be fixed and resubmitted once while the campaign is open.

## Outage-day plan

1. Start when GitHub's status page shows an open incident that affects Git
   operations. Do not act on rumors.
2. Before posting, check that a fresh `git clone` of
   `https://forkmesh.com/forkmesh/forkmesh` works and that
   `https://forkmesh.com/status` is healthy. If ForkMesh itself is degraded,
   post nothing.
3. Quote-post the pinned thread once, with a one-line note that the video's
   push-to-an-empty-repository path works during an outage.
4. Watch the campaign issue. Answer questions with links to this kit. Do not
   promise anything outside the [claim boundaries](#claim-boundaries).
5. Within a week of the incident, publish the claim count, how many clones
   were verified, and what failed.

## Publication and closure evidence

| Item | Evidence required | Status |
| --- | --- | --- |
| Video | Public URL of the uploaded 60-second video that matches the timeline above | Missing: not recorded |
| Thread | Public URL of post 1 of the thread | Missing: not posted |
| One-click demonstration | The video's 0:14–0:52 section, plus the disposable repository's `forkmesh.com` URL | Missing |
| Bounty launch | Campaign issue URL with the budget, end date, and payout method filled in | Missing |
| First paid claim | A reviewer comment approving a claim, and a payout confirmation that contains no personal details | Missing |

## Owner decisions before launch

1. **Budget and end date.** Suggested: $100 (20 claims) over 60 days.
2. **Payout method** for $5 claims, and who pays.
3. **Reviewer** who runs the checklist, and a turnaround target (suggested: 3
   business days).
4. **Posting account** on X, and who is available to answer replies on launch
   day.
