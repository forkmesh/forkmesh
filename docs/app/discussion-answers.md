# Discussion answers

A Q&A discussion can have one reply marked as its answer, as on GitHub. The
answer feeds the Oracle badge ([badges.md](badges.md#oracle)).

## Who may mark

- The discussion's opener, proved by signing with the key that signed the
  discussion's `open` event.
- The repository owner: the node holding the working tree (desktop), the
  owner's account session (web relay or the mirror's portable session), or an
  owner of the organization the repository is linked to (relay only).

Only Q&A discussions take an answer. The category matches `Q&A`, `qa`,
`q and a` or `q-and-a`, ignoring case; the desktop and web call it `Q&A`, and
the iOS picker's `q&a` key normalises to it.

## Format

The answer is one file, `.forkmesh/discussions/<n>/answer.md`, next to
`discussion.md` and the `NNNN-comment.md` replies:

```
---
type: answer
id: <event id>
author: <signer key>
authorName: <display name>
ts: <ms on desktop, seconds from the web>
target: <reply event id, empty to clear>
targetAuthor: <key that signed that reply>
sig: <signature>
---
```

It is a signed discussion event (`forkmesh-discussion-event-v2`, type
`answer`); the signed content is `target`, so the canonical string is
`answer\n<number>\n<author>\n<ts>\n<sha256(target)>`. The event needs a real
discussion number. `targetAuthor` is not signed: whoever writes the file
copies it from the reply, and writing refuses a target that names no reply in
the folder. Clearing writes an empty target rather than deleting the file. An
event older than the answer already written (timestamps compared in
milliseconds) changes nothing.

## Write paths

| Client | How |
|---|---|
| Desktop with the working tree | `DiscussionStore::markAnswer` writes and commits `answer.md` |
| Desktop without it | signs the event and posts it to the relay inbox, like a reply |
| Web | `submitWebDiscussionAnswer` posts through the mirror, then the relay, with the session for the owner check |
| iOS | `InboxService.markDiscussionAnswer` posts to the relay with the session token |
| Android | `InboxService.markDiscussionAnswer` (core) posts to the relay with the session token; the event id is `answer-android-<ms>` |

The relay (`discussions_handler`) verifies the signature and, when the
session owns the repository or its organization, stores the item with
`meta.ownerApproved`. The mirror gateway does the same for the repository
owner's own portable session. Whoever materializes the inbox item then writes
`answer.md` only when the signer is the opener or the item is owner-approved:
the owner's desktop (`DiscussionStore::applyRemoteEvent`) or a managed mirror
(`writeDiscussionAnswer` in `server/intake.go`). Answers are not counted as
discussion contributions.

Readers do not re-check who marked the answer: everything in the repository
is written by the owner's desktop or by an intake that enforced the rule
above, and the owner may mark any reply anyway.

## Display

- Desktop: the reply's menu has Mark as answer or Clear answer; the answer's
  header carries a green Answer tag.
- Web: an Answer tag (success colour, status only) and a Mark as answer /
  Clear answer button under each reply for the opener (matched by this
  browser's signing key) or the owner.
- iOS: the answer's timeline tile turns into a green Answer tile; the opener
  (the device key signed the discussion) or the owner (the session's account
  is the repository owner) gets a button under each reply.
- Android: the same rules and strings (`DiscussionDetail.answerable`,
  `isAnswer`, `canMarkAnswer` in `collab/DiscussionThread.kt`); the answer's
  tile in `DiscussionScreens.kt` uses the check-circle icon in the success
  colour, and the parser splits `answer.md` out of the reply list
  (`ApiService.discussionFromMarkdown`).

A browser's signing key is generated per browser, so a discussion opened on
the web can only be answered by that browser or by the owner. Starting,
closing and moderating discussions from the web are described in
[web-discussions.md](web-discussions.md).
