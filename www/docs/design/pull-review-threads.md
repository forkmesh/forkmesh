# Pull request review threads across patchsets

A review thread is pinned to a path, a side (`old`/`new`) and a line range of
the diff the reviewer saw. When the pull is updated, lines shift or disappear.
Threads therefore carry a signed anchor so every client can show them on the
right lines or mark them explicitly as moved or outdated.

## The anchor

Line-anchored events (`line-comment`, `thread-comment`) may carry:

| Field | Meaning |
| --- | --- |
| `patchset` | The head commit the reviewer's diff was cut against (empty for a portable patch-backed pull) |
| `anchorLines` | The exact text of the commented lines on that side, prefix stripped, newline-joined, at most 12 lines |

Both are signed: when present, the signed content appends
`NUL patchset NUL anchorLines` to the existing per-type content under the
unchanged `forkmesh-pull-comment-v1` label. Events without an anchor sign the
same bytes as before. On disk (`.forkmesh/pulls/<n>/NNNN-<type>.md`) they are
front-matter keys `patchset` and `anchorLinesB64` (base64url). Cross-language
vectors live in `desktop/tests/test_crypto.cpp` and `app/tests/test_crypto.py`.

## Placement

Desktop (`PullReviewModel.cpp`) and web (`06-repo-content.js`) apply the same
rule:

1. **Same patchset**: the thread is *current* at its lines.
2. **Text search**: otherwise, find `anchorLines` as consecutive lines on the
   same side, nearest the original line. Same line: *current*; elsewhere:
   *moved*; not found: *outdated*.
3. **Patchset only**: a different patchset with no anchor text is *outdated*.
   A diff with no named patchset keeps the thread *current* while the line
   exists.
4. **Legacy** (no anchor): painted where it says while the line exists,
   otherwise *outdated*.

Moved threads show a `Moved` badge and the original lines. Outdated threads are
never painted on a line; they are listed at the top of their file with an
`Outdated` badge. Applying a suggested change uses the current line and refuses
when the thread is outdated. Matching is exact on purpose: a thread is never
shown on code it did not discuss.

## Branch review threads

A branch without a pull request can be reviewed the same way, from the desktop
compare view or the phone apps. Branch threads use the same event types and
anchors and are stored in the `forkmesh/pulls` ledger branch under
`.forkmesh/branch-reviews/<first 24 hex of sha256(branch)>/` with a
`branch.md` naming the branch, so pull loaders never treat them as pulls.
Placement uses anchor text only, because a branch view can include
uncommitted changes.

Signature:
`forkmesh-branch-comment-v2\n<owner/name>\n<type>\n<branch>\n<author>\n<ts>\n<sha256 hex of contentForSigning>`.

Rendering only reads an existing ledger checkout; only posting a comment
creates it.

A phone cannot write Git, so it sends the signed event to
`POST /api/repo/<owner>/<name>/pulls` with `{"branch": "<name>", "event": {...}}`
and no `number`. The relay verifies the v2 signature and queues it in
`pull_inbox` with `number: 0`. The owner's desktop
(`PullStore::applyRemoteBranchEvent`) or a Go mirror (`writeBranchReviewItem`)
drains it into the ledger, skipping event ids already stored. Shared vectors
are pinned in `app/tests/test_branch_review_intake.py`,
`server/intake_test.go` and the iOS and Android test suites.

## Limitations

- The web compare page does not show branch review threads.
- The web places threads against the signed creation diff, so threads written
  against a later desktop head appear moved or outdated there.
- Whitespace-only edits and renames make a thread outdated.
