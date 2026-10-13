# Pull request review threads across patchsets

Status: shipped (desktop, web, mobile signing, Worker, SSH gateway, Go mirror)
· Issue: #483 (closure gate "review conversations remain attached to the right
lines or expose an explicit outdated state across patchsets")

## Problem

A review thread is pinned to `path`, `side` (`old`/`new`) and a line range of
the diff the reviewer was looking at. When the author pushes again, the diff
is re-cut: lines shift, the commented code may move or disappear. Before this
change a thread was either painted on whatever line now had its number, or
dropped from the diff view entirely when that number no longer existed. The
conversation itself survived in the ledger, but the reviewer's claim ("this
code, at this revision") was neither recorded nor verifiable.

## The anchor

Every line-anchored review event (`line-comment`, `thread-comment`) may carry
two more fields:

| Field | Meaning |
| --- | --- |
| `patchset` | The head commit the reviewer's diff was cut against. The desktop uses the head it derived the patch from (falling back to the pull's signed `creationHeadOid`); the web uses `creationHeadOid`, because it renders the signed creation diff. Empty when no client can name one (a portable patch-backed pull). |
| `anchorLines` | The exact text of the commented lines on that side, diff prefix stripped, newline-joined, capped at 12 lines. Empty when the range is not in the diff. |

Both are **signed**. When either is present, the event's signed content
appends `NUL patchset NUL anchorLines` to the existing per-type content
(`forkmesh-pull-comment-v1` is unchanged as the canonical label). An event
without an anchor signs exactly the bytes it signed before, so every existing
signature stays valid and every pre-anchor verifier keeps accepting
pre-anchor events. Verifiers that predate this change reject *anchored*
events with `bad_signature`; the relay, the SSH gateway, the Go mirror
intake, the desktop and the mobile client all carry the rule.

On disk (`.forkmesh/pulls/<n>/NNNN-<type>.md`) the fields are front-matter
keys `patchset: <oid>` and `anchorLinesB64: <base64url>`, the latter using
the same blob encoding as `suggestionPatchB64`. The Go mirror's intake and
the desktop's inbox drain both write them.

Cross-language vectors for the anchored content live in
`desktop/tests/test_crypto.cpp` and `app/tests/test_crypto.py` (same hashes
in both).

## Placement

When a client shows a pull's diff it places every thread against that diff.
The rule is the same on the desktop (`PullReviewModel.cpp`) and the web
(`06-repo-content.js`):

1. **Own patchset.** `patchset` equals the patchset on screen: the thread is
   *current* at the lines it names. No text search is needed.
2. **Text search.** Otherwise, if `anchorLines` is present, look for that
   exact run of consecutive lines on the thread's side of the file, nearest
   to the original line. Found at the same line: *current*. Found elsewhere:
   *moved* to the new line range. Not found: *outdated*.
3. **Patchset only.** A different patchset with no anchored text is
   *outdated*: nothing proves the line still holds the same code. If the diff
   on screen has no named patchset at all (an immutable patch-backed pull) and
   the line exists, the thread is *current*.
4. **Legacy.** A thread with no anchor is painted where it says while that
   line exists and is *outdated* once it does not.

What each state looks like:

- **Current** — the thread sits on its line, as before.
- **Moved** — the thread sits on the line its text now occupies, with a
  `Moved` badge and "Written on new lines 10–12 of revision `abc1234…`; the
  same code now sits here."
- **Outdated** — the thread is never painted on a line. On the desktop it is
  shown at the top of its file's diff table (it cannot fold away, since there
  is no gutter marker to reopen it from); on the web it appears in an
  "outdated review threads" block at the top of the file. Both carry an
  `Outdated` badge and say which revision and lines it was written on. The
  conversation timeline marks the opening event "(outdated)" or "(moved from
  line N)". Per-file counts in the desktop file list include outdated
  threads.

Applying a suggested change uses the thread's *current* line, and refuses
when the thread is outdated ("the lines it changes are no longer in the
diff").

## Branch review threads (desktop and phones)

A branch with no pull request can be reviewed the same way. In the desktop's
branch compare view, click a line number to comment on that line, or press
on one and drag down the gutter or the code to select a range. The inline
composer opens under the selection. The file header's comment pill starts
a thread about the whole file. Replies, Resolve and Reopen work the same as
on a pull request. Without this, the only way to leave feedback on an agent's
branch was to open a pull request first.

Branch threads use the same event shapes as pull threads (`thread-comment`,
`thread-reply`, `thread-state`, with the same anchor fields). They are stored
in the `forkmesh/pulls` ledger branch at
`.forkmesh/branch-reviews/<first 24 hex of sha256(branch)>/`, next to a
`branch.md` that names the branch. That ledger is pushed with every other
head, so anyone viewing the branch from a mirror sees the threads. Keeping
them out of `.forkmesh/pulls/` means pull loaders, catalog counts and the
strict mirror reader (which rejects any non-numeric directory there) never
treat them as pull requests.

Signatures use the `branch-comment` prefix, with the branch name in place of
the pull number:
`forkmesh-branch-comment-v2\n<owner/name>\n<type>\n<branch>\n<author>\n<ts>\n<sha256 hex of contentForSigning>`.
Rendering a branch diff only reads these threads from a ledger checkout that
already exists. It never creates the `forkmesh/pulls` branch or its
checkout: that is a write, and it ran git (which pumps the event loop) in
the middle of a render. Only posting a comment creates the ledger. Because
every plain branch diff now carries the per-line comment links that PR
diffs already had, a small diff's HTML is larger and can stream in over
more than one page.

Placement uses the anchor text only (the current patchset is left empty),
because a branch view can include uncommitted work-tree changes that no
commit names.

File-level threads (line 0), on a branch or on a pull request, are shown at
the top of their file in the diff, in the same place as outdated threads.

### From a phone or any node that cannot write the ledger

The iOS and Android apps offer the same thing in their branch diff screen:
tap a line to select it, tap a second line on the same side to extend the
range, then comment. They read threads with the ordinary repository reads
(`tree` and `blob` on `ref=forkmesh/pulls`, path
`.forkmesh/branch-reviews/<key>`), the same way they read pull threads.

A phone cannot write git, so it sends the signed event through the pull
inbox:
`POST /api/repo/<owner>/<name>/pulls` with `{"branch": "<name>", "event": {...}}`
and no `number`. The app server checks the branch name and verifies the
`branch-comment` signature against this repository. Only the v2 form is
accepted, because the kind is new and has no legacy signatures. The server
then queues the event in `pull_inbox` with `number: 0` and the branch, and
rings the host and mirror doorbells. Whichever node drains the inbox stores
it in `.forkmesh/branch-reviews/<key>/`, creating `branch.md` the first
time. That is either the owner's desktop (`PullStore::applyRemoteBranchEvent`)
or a Go mirror (`writeBranchReviewItem` in `server/intake.go`). Both skip an
event id that is already stored, so a replayed inbox row is harmless. Until
it lands, the phone shows its own comment as pending, the same as a pull
comment.

Shared test vectors (the event `thread-comment-ios-1` on `src/app.txt`,
lines 3–5, branch `feature/review`, repository `alice/demo`) are pinned in
`app/tests/test_branch_review_intake.py`, `server/intake_test.go`, the iOS
`BranchReviewTests` and the Android `ReviewToolsTest`.

## What is not covered

- The web compare page does not show or take branch review threads yet.

- Threads on the web are placed against the signed creation diff. A pull
  updated on the desktop (re-push, apply-fix, agent edit) therefore shows its
  desktop-written threads as *moved* or *outdated* on the web until the web
  renders the current head; the anchor makes that explicit rather than wrong.
- Whitespace-only or renamed-file changes are treated like any other edit to
  the anchored text: the thread becomes outdated rather than following a
  fuzzy match. Exact matching is deliberate; a thread should never be shown
  on code it did not talk about.
- Comparing two patchsets side by side is a separate epic item (#483, "compare
  between patchsets") and is not part of this change.

## Verification

- Desktop: `forkmesh-tests` — anchored canonical vectors, front-matter
  round-trip of a multi-line anchor, and placement across a re-cut diff
  (current / moved / outdated / legacy, block moves, per-file counts,
  same-patchset shortcut, patchset-only rules).
- Worker: `app/tests/test_crypto.py` (vectors), `test_pull_review_frontend.py`
  (web placement and authoring contract), `test_ssh_independent_writes.py`,
  `test_pull_submission_inbox.py`, `test_org_pull_inbox_auth.py`.
- Mirror: `server/intake_test.go` — anchored keys persist, unanchored files
  are byte-identical to before.
