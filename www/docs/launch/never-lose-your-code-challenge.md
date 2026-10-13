# Never Lose Your Code Challenge

Status: **draft, not launched** (issue #520)

Prepared: 2026-09-14

This is the campaign packet for a human launch owner: the rules, the bounty
verification flow, the 30-second failover video storyboard, and the first three
posts. Nothing here has been posted, recorded, or paid. Issue #520 stays open
until the posts and video are published and the bounty ledger has its first
verified entry.

## Goal and measure

Bring the first **100 developers** to ForkMesh by asking open-source maintainers
to preserve a public repository they maintain and show it.

The measure is the bounty ledger below, not likes or impressions: one row per
account whose mirror passed verification. The campaign ends at 100 paid rows or
when the owner closes it, whichever comes first. Budget ceiling: 100 × $5 =
**$500**. Report weekly totals through the
[activation-to-revenue scorecard](../activation-revenue-scorecard.md)
(`signup` and `first_mirror` rows).

## Positioning

Campaign line: **Never lose the code that matters.**

It is campaign copy only. The canonical
[product promise](../product-promise.md) sentence stays unchanged on primary
product surfaces, and `Your Code, Your World.` stays the supporting slogan.

Claims every post and the video must stay inside:

- A mirror is a complete copy of a repository on another host that stays in
  sync and can serve clones when the source is offline.
- A clone of the stable URL is served only by a mirror whose integrity is `ok`
  and whose commit matches. With no eligible mirror, cloning fails; it never
  pretends.
- Do not claim "unstoppable", "can never go down", or instant failover. The
  12 September 2026 readiness review measured about two minutes in which every
  serving mirror returned 502 ([readiness packet](y-combinator-news-readiness.md)).

## Audience and outreach

Target: maintainers of public repositories that other people depend on —
libraries, CLIs, and self-hosted tools with a single canonical host.

- Post from the `@forkmesh` account. Reply to a maintainer only in a thread they
  started about hosting risk, outages, account suspensions, or takedowns, or
  when they tag `@forkmesh`.
- No bulk or automated mentions, no unsolicited DMs, no replies to strangers'
  unrelated threads. X treats bulk unsolicited mentions as spam, and the
  campaign should not become the spam machine the [hackathon](../hackathon.md)
  design refuses to be.
- A person writes and posts every reply.

## Participant rules

1. Install ForkMesh or sign in at [app.forkmesh.com](https://app.forkmesh.com).
2. Bring a **public** repository you maintain: on **Repositories**, choose
   **New repository**, then **GitHub / GitLab / Codeberg**, paste the URL, and
   select **Start resumable import** ([repository imports](../repository-imports.md)).
   Or [push to a hosted repository](../hosted-repositories.md).
3. Wait until the repository's **Mirrors** tab shows at least one **verified**
   copy.
4. Post a screenshot of the Mirrors tab on X, tag `@forkmesh`, include the
   ForkMesh repository link, add `#NeverLoseYourCode`, and **disclose the bounty**
   with `#ad` or the words "paid $5 bounty".
5. One bounty per person and one per repository. Mirroring a repository you do
   not maintain, a fork made to farm the bounty, or a private repository does
   not qualify.

The bounty is a thank-you, not employment or an investment. Nothing in ForkMesh
requires it; see [incentive independence](../incentive-independence.md).

## Bounty verification

A screenshot is a lead, not proof. The reviewer re-reads the evidence the
[availability proof](../availability-proof.md) publishes for anyone:

```sh
curl -s https://app.forkmesh.com/api/repo/OWNER/REPOSITORY/mirrors \
  | jq '.availability | {proof, copies: [.copies[] | {node, class}]}'
```

Approve only when all of these hold at review time:

| Check | Pass |
| --- | --- |
| Post | Public, tags `@forkmesh`, links the ForkMesh repository, discloses the bounty |
| Maintainer | The poster's X or forge profile links to the source repository, or they are a listed maintainer/committer there |
| Copy | `proof` is `partial` or `quorum`, and at least one copy has `class` `verified` |
| Uniqueness | Neither the account nor the repository is already in the ledger |

Record every decision, including rejections, in the ledger: post URL, ForkMesh
account, repository, `generatedAt` from the mirrors payload, reviewer, verdict,
payout reference, and date paid.

### Why not microjobs or hackathon missions

The existing incentive rails cannot pay this bounty as written, and should not
be bent to:

- [Paid microjobs](../operations/microjobs.md) pay fixed SOL amounts set in
  `microjobs.JOBS` (`referral_post` is 0.01 SOL). The referral check fetches the
  posted page and requires counted visitors; it explicitly treats a screenshot
  as no proof.
- [Hackathon](../hackathon.md) cash follows judged tracks only. A mission or
  milestone cannot pay cash, and its `first_mirror` event pays points.

**Launch gate:** the owner must choose the payout method (a conventional
payment or a manual, owner-signed transfer) and publish it in the rules before
the first post. Until then, the bounty flow is not verified end to end.

## 30-second failover video

A real, unedited screen recording of the [four-step proof](../product-promise.md#four-step-proof)
against a disposable public repository with two verified mirrors run by two
operators. Never fake the outage or cut away during the clone. Rehearse the
whole run first, and re-record if routing misbehaves.

| Time | Picture | On-screen caption |
| --- | --- | --- |
| 0–3 s | ForkMesh logo over a terminal | Never lose the code that matters. |
| 3–8 s | Repository **Mirrors** tab: source and mirror both verified, same commit | One repository. Two hosts. |
| 8–12 s | `git -C before rev-parse HEAD` prints the commit | Note the commit. |
| 12–17 s | On the source host, `systemctl stop forkmesh-mirror-node` | Take the source offline. |
| 17–21 s | Mirrors tab refresh: source offline, mirror still verified | The mirror is still healthy. |
| 21–27 s | `git clone https://forkmesh.com/OWNER/REPOSITORY after` completes; `rev-parse HEAD` matches | Same URL. Same commit. |
| 27–30 s | `forkmesh.com` and `@forkmesh` | Mirror your repository. #NeverLoseYourCode |

Keep the terminal font large enough to read on a phone, show the clock in the
recording, and keep the unedited take and the source and mirror health snapshots
with the campaign record. After recording, start the source host again.

## Kickoff posts

Post in order, a few hours apart. Post 1 starts a thread; posts 2 and 3 reply
to it. `RULES_URL` is the published rules page and `VIDEO` is the uploaded
failover video. Lengths count each link as 23 characters, as X does.

**Post 1 — the problem (276 / 280)**

> Your project lives on one host. One outage, suspended account, or takedown,
> and every new clone fails.
>
> Never lose the code that matters.
>
> Starting today: the Never Lose Your Code Challenge. The first 100 open-source
> maintainers who mirror a public repo on ForkMesh get $5. 🧵

**Post 2 — how to claim (267 / 280)**

> How to get the $5:
>
> 1. Import a public repo you maintain into ForkMesh
> 2. Wait for its Mirrors tab to show a verified copy
> 3. Post a screenshot, tag @forkmesh, add #NeverLoseYourCode and #ad
>
> One per person and per repo. We verify the mirror.
>
> RULES_URL

**Post 3 — the proof (258 / 280, with `VIDEO` attached)**

> 30 seconds, unedited:
>
> We stop the source host of a public repo. Then we run a fresh git clone of the
> same URL.
>
> It still clones, from a mirror, at the same commit. That's mirror failover.
>
> Try it on your own repo: RULES_URL
>
> #NeverLoseYourCode

## GitHub companion post

For a pinned discussion on the GitHub copy of `forkmesh/forkmesh`, posted by a
person:

> **Never Lose Your Code Challenge: $5 for the first 100 maintainers**
>
> If your project has one canonical host, an outage, a suspended account, or a
> takedown breaks every new clone and every build that depends on it.
>
> Import a public repository you maintain into ForkMesh, wait for its Mirrors tab
> to show a verified copy, and post a screenshot on X tagging @forkmesh with
> `#NeverLoseYourCode` and `#ad`. We verify the mirror from its public
> availability proof, then pay $5. One per person and per repository.
>
> Full rules and the 30-second failover video: RULES_URL

## Launch checklist

- [ ] Payout method chosen and written into the published rules.
- [ ] Rules published at `RULES_URL`.
- [ ] Failover video recorded unedited, reviewed against the storyboard claims.
- [ ] One rehearsal claim verified with the `curl` check above and paid.
- [ ] Posts 1–3 and the GitHub discussion posted by a person.
- [ ] Links to the posts, video, and first ledger entry added to issue #520.
