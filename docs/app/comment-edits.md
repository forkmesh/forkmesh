# Editing and deleting your own comments

People asked for a way to fix or remove a comment they had posted. Issue
comments already had signed `edit` and `delete` events, but the relay only
accepted them from repository managers, so a contributor could not change
their own words. Pull request comments had no edit at all, and the pull `delete`
event deletes the whole pull request.

Both logs stay append-only. A change is a new signed event that names the
comment it changes; the original text stays in Git history.

## Pull request comments

Two event types join the `pull-comment` family:

| Type | Signed content | Effect |
| --- | --- | --- |
| `comment-edit` | `target \0 body` | the newest edit's body replaces the target's body |
| `comment-delete` | `target` | the target is hidden |

`target` is the `id` of a `comment`, `review`, `line-comment`,
`thread-comment` or `thread-reply`. A review's text can be edited but a review
cannot be deleted, so approvals the merge gate counts never disappear behind a
comment change.

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
  `renderPullConversationEvent`, shown on your own comments while signed in;
- desktop: `PullStore::editComment`, `deleteComment`, `makeCommentChange`;
  **Edit comment** and **Delete comment** in the card menu
  (`MainWindow::editPullComment`, `deletePullComment`). A repository this node
  cannot write gets the signed event through the relay inbox.

Tests: `app/tests/test_pull_comment_edit_delete.py`.

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

## Not done yet

- The phone apps do not offer comment edit or delete, and their issue
  timelines do not fold `edit`/`delete` events.
- Discussion comments can still only be hard-deleted by moderators on the
  desktop.
