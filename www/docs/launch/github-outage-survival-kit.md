# GitHub outage survival kit

A launch kit, not yet published: a 60-second video script, a post thread and a
$5 mirror bounty. The goal is to get repositories mirrored on ForkMesh
**before** a GitHub outage, so cloning and shipping continue during one.
Recording, posting, opening the bounty and paying claims are human actions.
Success is measured by valid bounty claims, not views or reposts.

## Claim boundaries

| Copy may say | Copy must not say |
| --- | --- |
| Creating a ForkMesh repository is one click; moving the code is one `git push` ([hosted repositories](../hosted-repositories.md)) | That the switch is automatic |
| A public GitHub import is refreshed about every 30 minutes and served without the provider ([repository imports](../repository-imports.md#ending-the-provider-dependency)) | That private imports are hosted, or that the copy is real-time |
| `git clone https://forkmesh.com/OWNER/REPOSITORY` is served by a healthy eligible mirror ([product promise](../product-promise.md)) | Uptime or speed guarantees |
| One `insteadOf` line repoints pinned Git dependencies ([dependency workflows](../dependency-workflows.md#switching-providers-without-editing-a-lock)) | That every dependency is already on ForkMesh |
| Set up before the outage; new imports need the provider's API | That you can import from GitHub while it is down |

Never mock GitHub or an incident in progress.

## 1. Sixty-second video

Use a disposable repository and account, keep the push credential off camera,
and label the staged outage on screen.

| Time | Picture | Narration | On-screen caption |
| --- | --- | --- | --- |
| 0:00–0:06 | Title card on a dark background | "GitHub is down. Your commits are still on your laptop. Here's how to keep shipping." | Staged demo: origin unreachable |
| 0:06–0:14 | Terminal: `git push origin main` fails with a connection error | "Your push fails, and your teammates can't clone." | `git push` fails |
| 0:14–0:26 | Browser: app.forkmesh.com → **Repositories → New** → the **Empty repository** source is already selected → type a name → click **Create empty repository** | "On ForkMesh, create an empty repository. That's the one click." | One click: **Create empty repository** |
| 0:26–0:40 | Terminal: `git remote add forkmesh https://app.forkmesh.com/you/app` then `git push -u forkmesh main`; username prompt shown, password blurred | "Add it as a remote and push. Every branch and commit you have is now on ForkMesh." | One push |
| 0:40–0:52 | Second terminal, different directory: `git clone https://forkmesh.com/you/app`, then `git rev-parse HEAD` next to the same command in the first terminal; the hashes match | "A teammate clones from forkmesh.com and gets the exact same commit." | Same commit, no GitHub |
| 0:52–1:00 | End card: kit URL and the bounty line | "Do it before the next outage. Mirror one repo, post proof, and we'll send you five dollars." | forkmesh.com/docs/launch/github-outage-survival-kit · $5 bounty, terms apply |

## 2. Thread

X counts each link as 23 characters; `www/tests` checks every post against the
280-character limit.

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

Way 3 only works for dependencies mirrored on ForkMesh under the same owner and
name. A private repository import is never hosted; point people at way 1.

## 3. The $5 mirror bounty

Mirror one of your public GitHub repositories on ForkMesh, clone it from its
`forkmesh.com` URL, and post proof on X. The first valid claims get $5 each
until the budget runs out.

Eligibility:

1. One claim per person and per ForkMesh account; no bulk, seeded or bot
   accounts; staff and contractors excluded.
2. The repository is public, owned or maintained by the claimant on GitHub,
   existed before the campaign, and is not a fork of `forkmesh/forkmesh`.
3. It is on ForkMesh as a fully hosted import or as a pushed empty repository.
4. The post is public, still up at review, includes the
   `https://forkmesh.com/OWNER/REPOSITORY` URL and the clone output, and
   discloses the payment.

We never pay for likes, reposts, follows, or replies, and a claim never has to
ask anyone for them.

Reviewer checklist, from a clean machine with no ForkMesh credential:

```sh
git clone https://forkmesh.com/OWNER/REPOSITORY bounty-check
git -C bounty-check fetch -q https://github.com/OWNER/REPOSITORY HEAD
git -C bounty-check merge-base --is-ancestor HEAD FETCH_HEAD && echo "commit is on GitHub"
curl -s https://app.forkmesh.com/api/repo/OWNER/REPOSITORY/mirrors
```

Approve only when the clone succeeds, `HEAD` is on GitHub's default branch,
the mirrors payload lists an online mirror, and every eligibility rule holds.
Payout details are exchanged privately, never in public comments. The bounty
has no effect on identity, permissions, ranking or moderation
([incentive independence](../incentive-independence.md)).

## Outage-day plan

1. Act only on an open GitHub status incident affecting Git operations.
2. Confirm a fresh clone of `https://forkmesh.com/forkmesh/forkmesh` works and
   ForkMesh status is healthy; otherwise post nothing.
3. Quote-post the pinned thread once.
4. Within a week, publish the claim count, verified clones and what failed.
