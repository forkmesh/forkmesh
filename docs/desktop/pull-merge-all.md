# Desktop: Merge all with AI

Part of [the desktop docs](README.md). Code: `desktop/src/PullMergeAll.*`
(prompt, name checks, saved runs, refusal messages),
`desktop/src/MainWindowPullMergeAll.cpp` (button, launch, cleanup) and the
`pulls.` family for phones. Test: `desktop/tests/test_pull_merge_all.cpp`.

## What the button does

**Merge all** sits in the Pull requests list toolbar, between the filters and
**Delete closed and merged**. The phones offer the same action
([From a phone](#from-a-phone)). Only the repository's owner can use it, and only
where this node has a working checkout.

1. It takes every open pull request in the list, oldest first, up to 200.
2. After the operator confirms, a worker finds the local branch for each one.
   This is the same lookup **Fix** uses (`localPullAgentBranch`). A pull request
   that arrived as a signed patch gets its `pr/<n>` branch built from the patch
   first (`PullStore::ensurePullHeadRef`). A pull request is left out, with a
   reason in the notice and the agent log, when its code is not on this node,
   its branch or base name fails `safeRefName`, or its base is not a local
   branch.
3. One agent starts through `startAdHocAgentForRepo` with
   `directCheckout = true` and mode `Auto`. It runs in the repository's own
   checkout with no agent branch, no worktree and no permission prompts, so its
   merges land on the real base branch. Only Claude Code and Codex can run
   without a worktree. If the default provider is anything else, the run uses
   Claude Code.
4. The agent reviews each pull request for security issues, bugs and code that
   breaks the repository's rules, fixes what it can in follow-up commits, merges
   with `git merge --no-ff`, and builds and tests before moving on. It skips a
   pull request that is malicious or cannot be fixed without changing what it
   is for.
5. When the run finishes successfully, `notifyAgentDone` calls
   `finishPullMergeAllRun`, and ForkMesh cleans up (next section).

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
