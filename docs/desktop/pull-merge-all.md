# Desktop: Merge all and Review all with AI

The Pull requests list has two buttons that hand every open pull request to
one agent run: **Merge all** reviews, fixes and merges them, and **Review
all** only reviews them and posts the results. This page covers the prompt
each one drafts, how branches are found, the cleanup and posting ForkMesh does
afterwards, and the phone entry point.

Part of [Desktop](README.md).

## Code

| Button | Code | Test |
| --- | --- | --- |
| Merge all | `desktop/src/PullMergeAll.*` (prompt, name checks, saved runs, refusal messages), `desktop/src/MainWindowPullMergeAll.cpp` (button, launch, cleanup), the `pulls.` local-control family for phones | `desktop/tests/test_pull_merge_all.cpp` |
| Review all | `desktop/src/PullReviewAll.*` (prompt, report format, verdict, summary, saved runs), `desktop/src/MainWindowPullReviewAll.cpp` (button, draft, posting) | `desktop/tests/test_pull_review_all.cpp` |

## Merge all: what the button does

**Merge all** sits in the Pull requests list toolbar, between the filters and
**Delete closed and merged**. The phones offer the same action
([From a phone](#from-a-phone)). Only the repository's owner can use it, and only
where this node has a working checkout.

1. It takes every open pull request in the list, oldest first, up to 200.
2. A worker finds the local branch for each one. This is the same lookup
   **Fix** uses (`localPullAgentBranch`). A pull request that arrived as a
   signed patch gets its `pr/<n>` branch built from the patch first
   (`PullStore::ensurePullHeadRef`). Pull requests still without a branch are
   then looked up in the cloud ([Pulling branches from the
   cloud](#pulling-branches-from-the-cloud)). A pull request is left out, with
   a reason in the notice, when its code is on neither this node nor the
   cloud, its branch or base name fails `safeRefName`, or its base is not a
   local branch.
3. The button does not start the agent itself
   (`draftPullMergeAllPrompt`). It writes the prompt into the composer,
   points the composer at the repository, turns off the composer's
   **Own branch and worktree** toggle, and picks Claude Code when the
   composer is on a provider that cannot run without a worktree (only Claude
   Code and Codex can). A copy of the button flies from the toolbar into the
   composer (`flyToComposer`) so the operator sees where the prompt went. If
   the composer is detached into its own window, the flight is skipped.
4. The operator reviews the prompt, the agent mode and the model, and sends
   it. The agent runs in the repository's own checkout with no agent branch
   and no worktree, so its merges land on the real base branch.
5. When the sent prompt still starts with the drafted heading, the repository
   matches and the branch toggle was off (`pullmergeall::draftMatches`),
   `adoptPullMergeAllDraft` saves the run for that session. A prompt sent with its own branch, or an unrelated prompt,
   is just an ordinary agent.
6. The agent reviews each pull request for security issues, bugs and code that
   breaks the repository's rules, fixes what it can in follow-up commits, merges
   with `git merge --no-ff`, and builds and tests before moving on. It skips a
   pull request that is malicious or cannot be fixed without changing what it
   is for.
7. When the run finishes successfully, `notifyAgentDone` calls
   `finishPullMergeAllRun`, and ForkMesh cleans up (next section).

The button drafts rather than starts so the operator can read and change the
prompt. The phones start it directly ([From a phone](#from-a-phone)), through
`startPullMergeAll`, which shares the preparation step
(`preparePullMergeAll`) with the button.

## Pulling branches from the cloud

The timed checkout sync (`refreshCore` in `UpstreamCheckoutSync.cpp`) copies
the cloud's branches into `refs/remotes/origin/*` and
`refs/remotes/forkmesh-mesh/*` only now and then, so the worker, before its
loop, asks `origin` once with `git ls-remote` for
`refs/heads/pr/<n>` of every open pull request without a local branch, and
fetches only the ones the cloud has (`fetchServedPullBranches`). For each
pull request that local replay cannot build, it creates `pr/<n>` at the
cloud's commit, or failing that at `refs/remotes/origin/pr/<n>` or
`refs/remotes/forkmesh-mesh/pr/<n>` (`adoptTrackedPullBranch`). The branch is
only ever created (`update-ref` with an all-zero old value), never moved, so
a local `pr/<n>` always wins. Local replay still runs first, because a
patch pull's numbered branch follows its current signed patch, which a
branch published earlier may not.

Some pull requests have no commit anywhere: a signed patch with no
`headBaseOid` that no longer applies to the base (a stacked series whose
earlier parts changed, or a patch from a remote fork). Fetching cannot help
there. The skip reason says so (`pullmergeall::missingCodeReason`): "its
signed patch does not apply to `main` and no node has published its branch;
the author needs to rebase it". A branch pull says which branch is missing.
(`materializePullRef` returns without an error in that case, so the reason
has to be worked out here.)

## Stacked patches whose earlier parts merged

A series sent as signed patches carries one commit per part, each cut from
the author's `main` with the earlier parts on top. When earlier parts arrived
as branches rebased onto a newer `main` and were merged, the files a later
part expects (`index <pre>..` in its patch) exist nowhere here: not on the
base tip, not in its history (`patchPreimageBaseCommit`), not as blobs for
`git am --3way`. `replayStackedPullSeries` in `PullStore.cpp` rebuilds such a
pull before it is given up:

1. The patch's first commit gives the author's email and date. The author's
   commits from the three days before it are found on the base branch and on
   every `pr/*` branch, local or tracked (`git log -F --author=<email>`),
   starting from the files the patch touches and widening to the files those
   commits touch. One commit per author date and subject is kept, so a copy
   rebuilt on a `pr/*` branch counts once. More than 30 commits means it is
   not a short stack, and the search stops.
2. The candidate bases are the last 40 first-parent commits of the base branch
   committed before the patch was written. For each one, the author's commits
   it does not contain are the stack. Files that no stacked commit touches
   must already hold the blobs the patch expects.
3. A scratch worktree at that base cherry-picks the stack in author-date
   order. Only when every file then holds exactly the blob the patch names
   does `git am` replay the patch on top. At most three bases are tried.

The resulting `pr/<n>` branch forks from the author's base and carries the
author's exact tree, so it conflicts with the current base tip just as the
pull request does. That merge is what the agent resolves. Later parts find
the rebuilt earlier part on its `pr/*` branch, so a whole series rebuilds in
number order. A patch cut from a fork whose files this repository never had
(for example generated pages of a fork with a different `main`) cannot be
rebuilt and keeps the "the author needs to rebase it" reason.

## Why ForkMesh cleans up, not the agent

The pull request ledger is signed with the node identity key. The agent cannot
sign, and must not edit `.forkmesh/`. So the agent only merges, and ForkMesh
removes what was merged afterwards:

- A pull request counts as merged only when its branch tip is an ancestor of its
  base (`git merge-base --is-ancestor`). This is why the prompt forbids squash,
  rebase and cherry-pick. A pull request merged any other way, or skipped, stays
  open, and nothing is deleted on the agent's word alone.
- Merged records are removed in one batch with `PullStore::deletePulls`, as
  **Delete closed and merged** does, without rewriting history.
- Each merged head branch is removed with `git update-ref -d refs/heads/<b>
  <oid>`, where `<oid>` is the tip that was proven to be an ancestor of the
  base. If the branch moved after the check, git refuses and the branch stays.
  Every removed commit is still reachable from the base, so nothing is lost. A
  branch is kept when it is checked out in any worktree (git would leave that
  worktree on a branch that no longer exists) or when it is also some pull
  request's base.
- If the run fails or is stopped, nothing is cleaned up. Continuing the session
  until it succeeds triggers the cleanup then.

The planned run is saved in the `pulls/mergeAllRuns` setting, keyed by agent
session id. This way a restart in the middle of a run still cleans up when it
finishes. Records for sessions that no longer exist are dropped the next time
the button checks for a run already in progress. A second click while a run is
going opens that run instead of starting another.

## From a phone

The iOS and Android pull request lists have the same action at the top. On
iOS it is a **Merge all** button in the navigation bar of the Pulls tab
(`RepoPullsTab`, `PullMergeAllFlow.swift`). On Android it is a sparkle icon
in the Pulls section's top bar (`RepoSectionScreen`, `PullMergeAllAction.kt`).
The agent has to run where the checkout is, so the phone asks the paired
desktop through the `pulls.` local control family
(`desktop/src/LocalControlPulls.*`, `MainWindowLocalControlPulls.cpp`).

1. `pulls.prepare` with `{owner, name, action: "merge-all-ai"}` checks the
   same blockers as the button (`pullMergeAllBlocker`) and returns a one-use
   token plus the summary the phone shows in its confirmation. The family is
   owner-only, like every family, and the token is bound to the device, the
   action and the repository, and expires after a minute.
2. `pulls.run` with the token loads the repository's pull requests from its
   `PullStore`. The repository does not have to be open on the desktop. The
   run then goes through `startPullMergeAll`, the same dialog-free path the
   button uses after its own confirmation. The desktop does not switch to the
   Agents tab for a phone request.
3. The reply carries `{message, sessionId, pulls, skipped}`. A refusal is one
   of `unknown_repo`, `read_only`, `not_repo_owner`, `busy`, `no_open_pulls`,
   `nothing_mergeable` or `launch_failed`. The phone clients
   (`PullMergeAllRemote` in `FMCode` and in Android `core`) turn each code
   into a sentence.

An organization-owned repository such as `forkmesh/forkmesh` belongs to the
account only through its organization role. The desktop fetches that role
list (`/api/orgs`) lazily: when Settings opens or a pull request is shown,
and a phone request usually arrives before either. So both `pulls.` methods go
through `whenPullOwnershipKnown`. When the
account does not own the repository directly and the role list has not loaded
yet, the request waits for one `/api/orgs` fetch (15 s timeout, inside the
phone's 20 s call timeout) before it checks the blockers. A failed fetch
still answers `not_repo_owner`, and the next phone request tries the fetch
again.

The phone sends the repository's public owner (`displayOwner`), not the
owner of the catalog row it opened. The public catalog lists
`forkmesh/forkmesh` only as copies on serving nodes such as
`fmnext-n1/forkmesh`, and the phone shows those copies as one repository
under `forkmesh`; the row's `owner` (`fmnext-n1`) is unknown to
`repoIndexFor` and would be refused with `unknown_repo`.
`PullMergeAllRules.owner` picks the owner on both phones.

The confirmation happens on the phone, as it does for `mergequeue.`. The
desktop shows no second dialog, because the person is not at it. A desktop
without the family answers `unknown_method`, and the phone asks for an update.
Cleanup after the run is the same as for the desktop button.

## Prompt safety

Pull request titles, author names and code come from other people. A malicious
pull request could hide instructions aimed at an agent that runs with no
permission prompts. The prompt marks all pull request content as untrusted and
asks the agent to report any embedded instructions as a security finding.
Titles are flattened to one line without backticks or double quotes and capped
at 160 characters. Branch names must match `safeRefName`. The prompt forbids
pushing, rewriting history, hard resets, cleaning, stashing and deleting
branches or worktrees, and tells the agent to stop on a dirty checkout instead
of discarding anything.

## Review all: what the button does

**Review all** sits in the Pull requests list toolbar, before **Merge all**,
and works like it:

1. It takes every open pull request in the list, oldest first, up to 200, and
   finds each one's branch with the same step Merge all uses
   (`preparePullMergeAll`). A pull request whose code is not on this node or
   in the cloud is left out, with the reason in the notice.
2. It writes the review prompt into the composer, turns off **Own branch and
   worktree**, picks Claude Code when the composer's provider cannot run
   without a worktree, and flies a copy of the button into the composer
   (`fillComposerForPullDraft`, shared with Merge all).
3. The operator reads the prompt, picks the model and mode, and sends it. The
   agent runs in the repository's own checkout and only reads: the prompt
   forbids editing files and every git command that changes something.
4. It reviews each pull request for security, stability and maintainability
   and writes its results as JSON to a report file ForkMesh named in the
   prompt: `<data>/pull-reviews/<uuid>.json`, outside the repository.
5. When the sent prompt still starts with the drafted heading, still names
   that report file, and was sent with no branch,
   `adoptPullReviewAllDraft` saves the run in the `pulls/reviewAllRuns`
   setting, keyed by agent session id, so a restart in the middle still posts
   the reviews when the run finishes.
6. When the run succeeds, `notifyAgentDone` calls `finishPullReviewAllRun`,
   which reads the report and posts the reviews (next section). If the run
   fails or is stopped, nothing is posted; continuing the session until it
   succeeds posts them then.

Only one Review all runs per repository at a time; a second click opens the
running one.

## Review all: why ForkMesh posts, not the agent

Review threads and review events are signed with the node identity key, which
the agent cannot use. So the agent only writes the report, and ForkMesh posts
from it, off the UI thread, with the same `PullStore` calls a person uses:

- Each finding becomes a review thread (`addThreadComment`) on its file and
  lines, anchored to the reviewed patchset (`pullAnchorFor`). A finding that
  carries both the current lines and their replacement gets a suggestion
  patch, so **Apply fix & commit** applies it in one click. A finding without
  a fix is a plain conversation thread.
- Then one review event per pull request. ForkMesh picks the verdict, not the
  agent (`pullreviewall::reviewState`): no findings is **approved**, any
  finding, nits included, is **changes requested**.
- The review's body is the summary, so it is the last entry of the
  conversation: the verdict, the agent's own summary, one line per finding
  linking to its thread (`forkmesh-pull-thread:<id>`, which opens the
  diff in Changes in Git) with an **Apply fix** link (`applyfix:<id>`) where it
  has one, and a last line naming the reviewer: the provider and the model the
  session ran with (`reviewerLabel`). When the session used the CLI's default
  model, the label falls back to the model id the agent wrote in the report,
  marked "as the agent reported it".
- The agent's summary and comments are escaped (`markdownText`), so a pull
  request that talks its reviewer into writing links or HTML cannot put them
  in the summary.
- At most 25 findings per pull request are posted. A pull request missing
  from the report, or closed in the meantime, gets nothing; the notice and the
  agent log list them. The report file is removed once posted.

The single-PR **Review with AI** (`reviewCurrentPullWithAi`, see
[agents-ui.md](agents-ui.md)) posts its result as "commented"; Review all
posts real verdicts. An approval is signed by this node like the operator's
own, so it counts wherever approvals do.

The web dashboard shows the same threads and review. The two desktop link
schemes in the summary do nothing there.
