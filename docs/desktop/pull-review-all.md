# Desktop: Review all with AI

Part of [the desktop docs](README.md). Code: `desktop/src/PullReviewAll.*`
(prompt, report format, verdict, summary, saved runs) and
`desktop/src/MainWindowPullReviewAll.cpp` (button, draft, posting). Test:
`desktop/tests/test_pull_review_all.cpp`.

## What the button does

**Review all** sits in the Pull requests list toolbar, before
[**Merge all**](pull-merge-all.md), and works like it:

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

## Why ForkMesh posts, not the agent

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
[agents-ui.md](agents-ui.md)) still posts its result as "commented". Review
all posts real verdicts because the operator asked for them on 2026-10-31.
An approval is signed by this node like the operator's own, so it counts
wherever approvals do.

The web dashboard shows the same threads and review. The two desktop link
schemes in the summary do nothing there.
