# Notes: inline comments, chat and who is here

Since 2026-10-31 everyone a note is shared with can leave comments on a
passage of the note, reply, mention collaborators and resolve threads, and
every client shows who else has the note open and whether they are viewing or
editing it. The web page, the desktop, iOS and Android offer the same
features. Server: `app/src/notes.py` and `ForkMeshNoteRoom` in
`app/src/entry.py`.

## Comments

A comment belongs to a note. A **thread** is a comment with no parent; a
**reply** points at a thread in the same note. A thread is **inline** when it
quotes a passage of the note, or about the whole note when it quotes nothing.

`note_comments` (migration `0296_note_comments.sql`) holds one row per
comment: ids, the parent, the author's account index (`author_bi`), the
anchor offsets, `created_at`, `updated_at` (any change, resolving included),
`edited_at` (body edits only), `resolved_at` and `resolved_bi`, and `data`,
the sealed `{body, quote, prefix, suffix, mentions}`. The quote and its
context are note text, so like the note they are only stored encrypted. Rows
go with the note (`ON DELETE CASCADE`); a note holds at most 1000 comments
(`MAX_COMMENTS`), a body at most 4000 characters, a quote at most 1000.

Anyone who can open the note in the app (owner, editor or viewer, including
organization insiders) can read and write comments. Anonymous readers of a
published page cannot see them.

### The API

All four routes answer `comments` (every comment, oldest first; clients
group replies under their thread by `parentId`), `people` (the account names
offered for @mentions: the owner, people the note is shared with directly and
anyone who has commented) and `canResolve` (owner or editor).

- `GET /api/notes/<id>/comments`
- `POST /api/notes/<id>/comments` with `{body, parentId?, quote?, prefix?,
  suffix?, start?, end?}` answers 201 and the new `comment`. A reply's anchor
  fields are ignored.
- `PATCH /api/notes/<id>/comments/<commentId>` with `{body}` edits your own
  comment (`comment_author_required` otherwise), or with `{resolved: true |
  false}` resolves or reopens a thread (`resolve_not_allowed` unless you are
  the owner, an editor or the thread's author; `comment_not_found` for a
  reply).
- `DELETE /api/notes/<id>/comments/<commentId>`: the author or the note's
  owner; deleting a thread deletes its replies.

A comment is `{id, parentId, author, body, quote, prefix, suffix, start, end,
mentions, createdAt, updatedAt, editedAt, resolvedAt, resolvedBy}`. Errors:
`invalid_comment` (empty or too long), `comment_not_found`,
`too_many_comments`.

### Mentions and pings

`@name` in a body mentions that account when it can open the note; the kept
names come back in `mentions`. Each mentioned person gets a `mention` ping
linking to `/dashboard/notes?note=<id>&comment=<commentId>`. A reply also
pings the people already in the thread. Nobody is pinged for their own
comment, and a name that cannot open the note is left as plain text.

### Live updates

Every change sends `{"type": "note-comments"}` to the note's room, so open
clients reload the comments within a second. Changes are also logged in
the note's activity as `comment`, `resolve` and `reopen`
(`docs/app/notes.md`).

### Finding the quoted passage

Offsets are UTF-16 code units of the Markdown, which is what JavaScript, Qt,
Swift's `NSString` and Kotlin count. When a thread is written the client sends
the selected text (`quote`), up to 32 characters before it (`prefix`), up to
32 after it (`suffix`), and its `start` and `end`.

Text moves as people edit, so every client finds the passage again the same
way: look for every occurrence of `quote` in the current Markdown, score 2 when
the text before it ends with `prefix` and 2 when the text after it starts with
`suffix`, keep the highest score and break ties by the distance from `start`.
When the quote is no longer in the note the thread is **detached**: it stays in
the list with its quote shown as removed text, and nothing is highlighted.

### What every client shows

- A **Comments** button in the note's toolbar with the number of open threads.
- Selecting text in the editor offers **Comment**; the composer shows the quote.
- The comments panel lists open threads in document order (whole-note
  threads first, detached ones last) and resolved threads folded under
  "Resolved (n)". A thread shows its quote, author, time, body with
  @mentions highlighted, its replies, a reply box, Resolve or Reopen, Edit
  on your own comments, and Delete on your own comments (the note's owner can
  delete anyone's, to moderate).
- Picking a thread selects its passage in the editor and highlights it.
- Typing `@` in a comment box suggests names from `people`.
- Opening `/dashboard/notes?note=<id>&comment=<commentId>` opens the note
  with that thread shown.

## Reactions

Since 2026-11-04 anyone who can open a note can react with an emoji to a
passage of it, and everyone else with the note open sees the reaction appear
beside that passage. A reaction is anchored like an inline comment (quote,
prefix, suffix, start, end, found again the same way, see
[above](#finding-the-quoted-passage)); one with no quote is about the whole
note.

`note_reactions` (migration `0301_note_reactions.sql`) holds one row per
reaction: `reaction_id`, `note_id`, `author_bi`, the anchor offsets,
`created_at` and `data`, the sealed `{emoji, quote, prefix, suffix}`. Rows go
with the note (`ON DELETE CASCADE`); a note holds at most 2000 reactions
(`MAX_REACTIONS`). The table is in the rekey lists of `tools/next_rekey.py`.

Reactions travel with the comments, so no client needed a new live path:

- every comments answer also carries `reactions`, oldest first, each `{id,
  author, emoji, quote, prefix, suffix, start, end, createdAt}`;
- `POST /api/notes/<id>/reactions` with `{emoji, quote?, prefix?, suffix?,
  start?, end?}` adds yours and answers 201 with the comments payload and the
  new `reaction`. The same emoji from you on the same quote, prefix and suffix
  answers 200 with the one already there instead of a copy;
- `DELETE /api/notes/<id>/reactions/<reactionId>` removes it (its author or
  the note's owner, `reaction_author_required` otherwise);
- the emoji follows the note emoji rule (`_emoji` in `app/src/notes.py`: at
  most 16 characters, no letters, spaces or plain digits), else
  `invalid_reaction`; a full note is `too_many_reactions`, an unknown id
  `reaction_not_found`;
- every change sends `note-comments` to the room, and `commentsKey` gains the
  reaction count and newest time when a note has reactions, so polling
  clients reload too. Reactions are not logged in the activity and ping no
  one.

### What every client shows

- **React** beside **Comment** wherever selecting text offers Comment, with
  the quick set 👍 ❤️ 😂 🎉 😮 👀.
- Reactions whose passages resolve to the same place form a group: one chip
  per emoji with its count, in the accent colour when you are among the
  people, naming them on hover or long press, and a "+" to add another emoji
  there. A chip toggles your reaction; adding to someone else's chip reuses
  their anchor so it joins the group. Reactions whose quote is no longer in
  the note are hidden.
- The web shows each group in a row under the block holding the passage, in
  Live, Preview and Split's preview, with the passage quoted at its start;
  whole-note reactions sit under the title. The desktop and the phones list the
  groups at the top of the comments panel or sheet and mark the passages in
  the editor ([desktop notes](../desktop/notes.md#reactions),
  [phone notes](../native/notes.md)).

## Chat

Since 2026-11-03 the side panel next to a note has a Chat tab beside
Comments, for talking with everyone who has the note open or can open it.
Chat messages are not anchored to the text; select text and comment for that.

`note_chat` (migration `0300_note_chat.sql`) holds one row per message:
`message_id`, `note_id`, `author_bi`, `created_at` and `data`, the sealed
`{body}`. Rows go with the note (`ON DELETE CASCADE`); a note keeps its newest
500 messages (`MAX_CHAT`), older ones are dropped as new ones arrive, and a
body is at most 2000 characters. The table is in the rekey lists of
`tools/next_rekey.py` ([rekey](../operations/rekey.md)).

The owner, editors and viewers can read and write the chat:

- `GET /api/notes/<id>/chat` answers `messages` (oldest first, each `{id,
  author, body, createdAt}`) and `canModerate` (you own the note).
- `POST /api/notes/<id>/chat` with `{body}` answers 201, the list and the new
  `message`; an empty or too long body is `invalid_message`.
- `DELETE /api/notes/<id>/chat/<messageId>`: the author or the note's owner
  (`message_author_required` otherwise, `message_not_found` for an unknown id).

Every change sends `{"type": "note-chat"}` to the note's room, and the
presence routes answer `chatKey` (message count and newest time) so polling
clients reload when it moves. Clients count messages from others newer than
the last one you saw in that note as unread, on the Chat button and tab.

## The side column

Since 2026-11-04 the note editor on the web and the desktop has one right
column with four tabs, so the people working on a note see what is happening
without leaving it:

- **Comments**, the threads above.
- **Chat**, the thread above.
- **Activity**, the timeline from [notes](notes.md#on-the-page): views,
  edits, shares, publishing, links and restores, newest first.
- **History**, the saved versions (`GET /api/notes/<id>/versions`), newest
  first, each with who saved it and when. **View** opens one read-only with
  its title and formatted Markdown (`GET /api/notes/<id>/versions?version=<n>`
  answers `version: {version, createdAt, author, title, markdown, emoji}` for
  the owner, editors and viewers; `version_not_found`, `invalid_version`);
  **Restore** (owner and editors) saves it as a new version.

Every tab is live. On the socket, `note-activity` and `note-updated` reload
Activity, `note-updated` reloads History, and `note-chat` reloads Chat. The
desktop, which polls, reloads Activity and History when the presence answer's
`activityKey` moves (the note's version, the number of events, their summed
`hits` and the latest `last_at`, so a folded repeat view counts too). The
comments and chat buttons, the Activity button and **Version history** in the
menu all open the column on their tab. Phones have no room for a column:
iOS and Android show History, Activity and Chat as tabs of one sheet
([phone notes](../native/notes.md)).

## Following someone

Every client sends where its caret is: the web page sends `{"type":
"cursor", clientId, line, column}` on the socket when the caret moves (and
`line`/`column` with each draft), the desktop sends `line` and `column` with
its presence poll. `line` is 1-based, `column` counts UTF-16 units, and 0
means unknown. The room keeps each socket's last cursor and polled client's
last line, passes them on in presence and cursor frames, and includes them in
the `peers` the presence routes answer.

Choosing Follow next to someone in the "Here now" list (the faces menu on the
desktop) keeps their place in view: each time their line moves, the editor
scrolls to it and marks it with their name and colour. Typing, Stop following,
or the person leaving ends it.

## Who is here

### On the live socket

Presence frames on `/api/notes/<id>/ws` carry a `state`: `viewing` or
`editing`. A client sends `editing` while the person has typed in the last 10
seconds and `viewing` otherwise, sending a frame straight away whenever the
state changes and every 15 seconds anyway. The room passes the state on
(`viewing` for a viewer, who cannot edit), keeps each socket's `clientId`,
and when a socket closes tells the others `{"type": "leave", "clientId",
"name"}` so the person disappears at once instead of after 37.5 seconds.

### Without a socket

The desktop does not link QtWebSockets, so it reaches the same room over
HTTP:

- `POST /api/notes/<id>/presence` with `{clientId, state, line?, column?}`,
  where `state` is `viewing`, `editing` or `left`, registers the caller and
  answers `peers`, everyone else in the room: `{clientId, name, state, line,
  column, via}`, `via` being `live` (socket) or `poll`. The room passes the poll on to socket clients as
  an ordinary presence frame (a `leave` frame for `left`), so the web and
  phones see desktop users too.
- `GET /api/notes/<id>/presence[?clientId=<yours>]` answers `peers` without
  registering, leaving out the given `clientId`.
- A malformed `clientId` (1–64 letters, digits, `-` or `_`) or an unknown
  `state` is `invalid_presence`; a viewer's `editing` is stored as `viewing`.

Both also answer `commentsKey`, the number of comments and the time of the
latest change, `chatKey`, the same for chat messages, and `activityKey` (see
[the side column](#the-side-column)), so a polling client reloads comments,
chat, activity or history only when they move. A
polled peer is forgotten 20 seconds after its last poll; the desktop polls
every 5 seconds while a note is open and sends `left` when it closes the note.
Presence lives in the room's memory only: nothing about who opened a note is
written by these routes (views are counted separately, see
`docs/app/notes.md`).

### What every client shows

Faces for up to four people (then "+N"), each labelled "<name> · viewing" or
"<name> · editing", with a pencil badge on those editing; a line such as
"ana is editing…", "Live · ana is viewing" or "Live · 3 others here"; and
picking the faces lists everyone with their state.

## Tests

`app/tests/test_notes_reactions.py` covers reactions: sealed anchors,
listing with the comments, no duplicates, the emoji rule, outsiders refused,
delete permissions, removal with the note, the cap and the `commentsKey`.
`app/tests/test_notes_comments.py` covers the API: creating threads and
replies, quotes kept sealed, mentions kept only for collaborators and pinged,
replies pinging the thread, edit and resolve permissions, deleting a thread
with its replies, the cap, anonymous readers refused, removal with the note,
and the presence routes. `app/tests/test_notes_chat.py` covers the chat:
sealed bodies, listing, the length rule, outsiders refused, deleting your own
or (as owner) anyone's, the 500-message window, removal with the note, the
`chatKey`, the `activityKey`, and the room remembering cursor lines for polling
followers; `app/tests/test_notes.py` covers viewing one version.
`app/browser_tests/tests/dashboard-notes-live.spec.js` drives the web page:
Live as the default layout, Live mode block editing, reacting to a passage
and seeing others' reactions arrive, the marks on blocks others are editing,
commenting on a selection in a live block, following
a collaborator, the chat tab with its unread count, and the column's Activity
and History tabs updating from socket frames, viewing a version and opening
History from the menu.
