# Discussions on the web

Everything the desktop does with a repository's discussions works in a
browser, with nothing installed: start a discussion, reply, mark a Q&A answer,
and, for the repository owner, close, reopen, archive, delete replies and
delete the whole discussion.

## Who may do what

| Action | Who | Proof the relay checks |
|---|---|---|
| Start a discussion | any signed-in account | the browser key's signature |
| Reply | any signed-in account, while the discussion is open | the signature |
| Mark or clear the answer (Q&A only) | the opener's browser key, or the owner | the signature; the owner's session for `ownerApproved` ([discussion-answers.md](discussion-answers.md)) |
| Close, reopen, archive, unarchive | the repository owner, or an owner of the organization the repository is linked to | the signature and the owner's account session |
| Delete a reply, or the whole discussion | the same | the same |

The browser hides the controls a visitor cannot use: the New discussion button
sends a signed-out visitor to the login page, the reply box is replaced by a
read-only notice on a closed or archived discussion, and the Owner tools row
and each reply's Delete appear only for the owner (`loadRepoDiscussionModeration`).

Private repositories take issues, pull requests and discussions from the
web from accounts that can read them (the owner, or an account the
repository is shared with: `_private_collaboration_allowed`); anyone else
gets the same 404 as for a name that does not exist, and reading the inbox
stays with the owner's node. A private repository's submissions skip every
public side effect: federation (`_ap_publish_repo_event` already skips
private repositories), provider mirrors, public contribution tallies, and
mentions, follower and assignee pings to accounts that cannot read it
(`_private_repo_reader_filter` in `notify_mentions`, `notify_subscribers`
and `notify_issue_assignees`). Issues and pull requests reach the tally
through `_record_public_contributor`, which checks the repository is public
(failing closed) before `_record_contributor`; until 2026-10-21 only
discussions checked, so a private repository's issues and pull requests
counted toward public profiles.

## Events

Every action is one signed discussion event (`forkmesh-discussion-event-v2`,
bound to `owner/repo`), signed with the browser's key (`getWebIssueKey`, the
key web issue events use). The canonical string is
`<type>\n<number>\n<author>\n<ts>\n<sha256(content)>`:

| Type | Number | Content | Extra fields |
|---|---|---|---|
| `open` | `0` | `title NUL body NUL category` | `title`, `body`, `category` (one of Announcements, Ideas, Q&A, Show and tell, Maintainer notes) |
| `comment` | the discussion | `body` | `body` |
| `answer` | the discussion | `target` | `target` |
| `status` | the discussion | `status` | `status`: `open`, `closed` or `archived` |
| `delete` | the discussion | `target` | `target`: the reply's event id, or `#discussion` for the whole discussion |

`status` and `delete` are new with the web flow; the desktop, mirror gateway,
relay and Go mirror all know them. The Go mirror removes a whole discussion's
folder with `removeDiscussionFolder` in `server/intake.go`. It deletes plain
files one by one and refuses a folder holding anything else, so there is no
recursive delete. Event ids carry a random suffix
(`comment-web-<ts>-<rand>`) because materializers skip an id they already hold.
The opening event's `ts` is in milliseconds, like the desktop's; replies keep
seconds, as before.

## Delivery

`fetchRepoMutation(repo, "discussions", ...)` tries the repository's online
mirrors first and the relay second, like issues:

- **Mirror** (`tools/mirror_gateway.py`, `_repo_collaboration`): verifies the
  signature and, for `status` and `delete`, that the portable session is the
  owner's and the request comes from the dashboard origin. The Go daemon writes
  the files and commits at once (`writeDiscussionItem` in `server/intake.go`);
  the answer carries the new discussion's number, and the browser opens it.
- **Relay** (`discussions_handler`): verifies the signature, binds the author
  name to the session, checks owner authority for `status`/`delete` (403
  `not_authorized` otherwise) and stores the item in `discussion_inbox` with
  `meta.ownerApproved` when the owner sent it. A new discussion is answered
  with number 0: it is numbered by whoever drains the inbox (a mirror, or the
  owner's desktop), so the browser says it was accepted instead of linking to
  a number nobody has assigned.

Both materializers write exactly the desktop's format, so the desktop, iOS
and Android read web-made discussions unchanged:

- `open` writes `.forkmesh/discussions/<n>/discussion.md`.
- `comment` writes the next `NNNN-comment.md`; a reply to a closed or archived
  discussion is refused (`discussion_locked`, 409, on a direct mirror
  submission) and acknowledged without writing when it comes from the inbox,
  so it cannot block the queue.
- `status` rewrites `status:` in `discussion.md` (only with `ownerApproved`).
- `delete` removes the reply file whose `id` is the target, or the whole
  discussion folder for `#discussion` (only with `ownerApproved`; a repeat
  is consumed).

On the desktop `DiscussionStore::applyRemoteEvent` applies the same rules
through `setStatus`, `deleteComment` and `deleteDiscussion`.

## Changes waiting on a drain

A status change or a deleted discussion sent through the relay waits in the
owner's inbox, and until it is drained mirrors still serve the old state.
The browser keeps each such change (`rememberPendingDiscussionState`, in
`localStorage` under `forkmesh.repoPendingDiscussionState`, at most 100, for
a week) and lays it over every list and page it reads
(`applyPendingDiscussionStates`, `withPendingDiscussionState`) until a mirror
serves the same status, or no longer serves the deleted discussion; issues
do the same with their status events.

## Abuse limits

The relay keeps the inbox limits issues have: at most
`MAX_PENDING_DISCUSSIONS` (500) waiting items per repository, at most
`MAX_PENDING_PER_AUTHOR` (50) per signing key, 64 KB of signed content per
event, and a replayed signature is answered as accepted without being queued
or announced twice.

## Notifications

A new discussion pings the owner (`notify_pending_inbox`, `opened`), and
mentions in a new discussion or a reply notify the mentioned accounts. A reply
also notifies everyone following the thread and makes the replier a follower
(`notify_subscribers` / `subscribe_thread` with source `discussion`).
Moderation events notify no one but the owner's pending-inbox badge.

## Interface

- List: New discussion button; an empty repository shows Start a discussion;
  "Discussions unavailable" (no mirror reachable) still offers to start one.
- Composer (`renderDiscussionComposeForm`): category cards, title, a Markdown
  body with Write and Preview tabs and @mention suggestions, Ctrl/Cmd+Enter to
  send. Attachments are not offered: discussion events carry no attachment
  list, unlike issue events.
- Thread: replies with Mark as answer (Q&A) and, for the owner, Delete; below
  them the reply box or the closed/archived notice, then Owner tools (Close /
  Reopen / Archive / Unarchive, Delete discussion).
- First view: the node renders a discussion page (`/<owner>/<repo>/discussions/<n>`)
  into the first HTML (`repo_views.py`), with a Closed or Archived pill.

Tests: `app/tests/test_web_discussions.py`, `server/intake_discussion_moderation_test.go`,
the discussion checks in `desktop/tests/test_crypto.cpp`, and
`app/tests/host/test_repo_views.py`.
