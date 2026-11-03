# Editing and deleting your own comments

Part of [the App docs](README.md). Authors can edit and delete their own issue
and pull request comments (the pull `delete` event deletes the whole pull
request, so comments have their own events). Both logs stay append-only. A change is a new signed event that names the
comment it changes; the original text stays in Git history.

## Pull request comments

Two event types join the `pull-comment` family:

| Type | Signed content | Effect |
| --- | --- | --- |
| `comment-edit` | `target \0 body` | the newest edit's body replaces the target's body |
| `comment-delete` | `target` | the target is hidden |

`target` is the `id` of a `comment`, `review`, `line-comment`,
`thread-comment` or `thread-reply`.

Reviews can be deleted too. A
`comment-delete` on a plain review (`commented`) hides it like a comment. On
an approval or a change request it removes the text only, and wins over any
edit, before or after it: the verdict stays, so the merge gate and
`buildPullReviewSnapshot` count the same approvals and change requests as
before, and nobody can make an approval disappear with a comment change. The
verdict's row ("approved these changes") stays in the conversation, with no
edit or delete buttons, since there is no text left. The web asks "Delete
this review's text?" and says the approval or change request stays; the
desktop asks the same.

A change applies only when its signer is the target's author: the same public
key, or the same account name. The relay binds `authorName` to the signed-in
session (`_bind_session_author`), so a browser key and a desktop key of one
account can edit each other's comments, and nobody else's. Every reader folds
with the same rule: `foldPullCommentChanges` (web),
`PullStore::foldCommentChanges` (desktop conversation and
`buildPullReviewSnapshot`). Changes that fail the rule are ignored, never
shown.

Code:

- relay: `pull_comment_content` and `verify_pull_comment_event` in
  `app/src/events.py`; `pulls_handler` in `app/src/entry.py` queues the event
  and skips mention, subscriber and ActivityPub fan-out for it;
- gateways: `_dashboard_pull_event_content` (`tools/mirror_gateway.py`),
  `_pull_comment_content` and `PULL_COMMENT_EVENT_TYPES`
  (`tools/ssh_gateway.py`);
- Go intake: `pullEventMarkdown` writes `target` into the event file;
- web: `submitWebPullCommentEdit`, `submitWebPullCommentDelete`,
  `handleWebPullCommentAction`, the pencil and trash buttons in
  `renderPullConversationEvent` and on the inline comment cards of the Files
  changed tab (`renderPullInlineThreadCard`, edited in place in the card),
  shown on your own comments while signed in; `pullReviewKeepsVerdict` decides
  whether a delete hides a review or clears its text, the same test as
  `PullStore::reviewKeepsVerdict` (desktop) and `folded_conversation`
  (`app/host/forkmesh_host/repo_views.py`, the `.txt` view);
- desktop: `PullStore::editComment`, `deleteComment`, `makeCommentChange`;
  **Edit comment** and **Delete comment** in the card menu
  (`MainWindow::editPullComment`, `deletePullComment`). A repository this node
  cannot write gets the signed event through the relay inbox.

Tests: `app/tests/test_pull_comment_edit_delete.py`.

## Pull request descriptions

A pull's description was fixed when it was opened: the pull signature covers
title, base, head, patch and commits but not `description`, and the web
revision drops it, so the author had no way to correct it. A description edit
is now an ordinary `comment-edit` whose `target` is the word `description`
(`PULL_DESCRIPTION_TARGET`). The relay, the gateways and the Go intake already
accept and store a `comment-edit` with any non-empty target, so nothing on the
wire changed.

The same author rule applies, with the pull's `pull.md` record as the target:
the change counts only when its signer is the pull's `author` key or its
`authorName` account (`pullDescriptionEditApplies`). The newest such edit
replaces the description; edits by anyone else are ignored. The description
edits never show as timeline rows, because `foldPullCommentChanges` already
drops every `comment-edit` and only the id-targeted ones change a comment.

- web: `pullDescriptionEdit` picks the edit, `renderRepoRecordDetail` shows it
  with "· edited", and the pull's author (`pullDescriptionEditable`: signed
  in as the author, pull not deleted, merged and closed pulls included) gets a
  pencil in the opening card. It opens the shared `openIssueCommentEditor`;
  `handleWebPullDescriptionEdit` signs the event and `paintPullDescription`
  redraws just that card;
- `.txt` view: `folded_description` in `app/host/forkmesh_host/repo_views.py`
  (the bundle now carries the pull's `authorName`).

Tests: `app/tests/test_pull_description_edit.py`,
`test_pull_text_shows_the_authors_newest_description_edit` in
`app/tests/host/test_repo_views.py`.

## Issue comments

`_accept_issue_submission` still lets managers sign `edit` and `delete`. When
the manager check fails, `_issue_comment_owned_by_submitter` reads the issue
record from the mirror and accepts the event when its `target` is a `comment`
(or, for an edit, the `open` event) signed by the same key or by the
session-verified account. `target: "self"`, which deletes the whole issue,
still needs a manager. The web shows the edit and delete buttons on your own
comments (`webSessionAuthored`).

The issue `edit` signature covers body and attachments but not `target`; the
relay check above is what stops an edit being pointed at someone else's
comment.

## Replayed issue events

The signature on an issue `edit` covers its body and attachments but not its
`id` or `target`, and every stored event is public, so a copy of a signed event
under a new `id` could put back replaced text or move an edit onto another of
the author's comments. Each receiver treats a signature it already holds as
already applied:

- the relay (`_issue_event_signature_stored`) answers a `comment`, `edit` or
  `vote` whose signature is already in the stored issue with
  `{"ok": true, "alreadyApplied": true}` and queues nothing;
- the desktop (`IssueStore::applyRemoteEvent`) and the mirror node
  (`writeIssueItem`, `foldIssueEvents` in `server/intake.go`) skip an event
  whose signature matches one in the issue.

Signing `target` would need a new issue event version on every client; that is
not done yet.

## The web editor and keyboard focus

Issue and pull request comments share one in-place editor,
`openIssueCommentEditor` (`app/public/dashboard/js/06-repo-content.js`).
A line comment is drawn twice on a pull request page, once in the Conversation
tab and once in the Files changed tab, and both copies stay in the page with
the inactive tab hidden, so both carry the same `data-repo-pull-comment-edit`
id. `visibleCommentEditButton` picks the copy that is on screen, not the first
one in the page.

Closing the editor removes the textarea that had focus, so Escape, **Cancel**
and saving return focus to the comment's **Edit** button (otherwise keyboard
and screen reader users land at the top of the page). A save re-renders the
timeline and replaces that button, so `handleWebPullCommentAction` and
`handleWebIssueAction` call `restoreCommentEditFocus` after the re-render. It
leaves focus alone if the person has already moved somewhere else while the
save was in flight. Tests: `app/tests/test_comment_editor_focus_frontend.py`.

## Not done yet

- The phone apps do not offer comment edit or delete, and their issue
  timelines do not fold `edit`/`delete` events.
- The desktop and the phone apps do not offer a pull description edit and
  still show the description from `pull.md`; `PullStore::foldCommentChanges`
  ignores the `description` target.
- Discussion comments can still only be hard-deleted by moderators on the
  desktop.
