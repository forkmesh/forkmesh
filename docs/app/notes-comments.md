# Notes: inline comments and who is here

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
(`docs/app/notes-activity.md`).

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

- `POST /api/notes/<id>/presence` with `{clientId, state}`, where `state` is
  `viewing`, `editing` or `left`, registers the caller and answers `peers`,
  everyone else in the room: `{clientId, name, state, via}`, `via` being
  `live` (socket) or `poll`. The room passes the poll on to socket clients as
  an ordinary presence frame (a `leave` frame for `left`), so the web and
  phones see desktop users too.
- `GET /api/notes/<id>/presence[?clientId=<yours>]` answers `peers` without
  registering, leaving out the given `clientId`.
- A malformed `clientId` (1–64 letters, digits, `-` or `_`) or an unknown
  `state` is `invalid_presence`; a viewer's `editing` is stored as `viewing`.

Both also answer `commentsKey`, the number of comments and the time of the
latest change, so a polling client reloads comments only when it moves. A
polled peer is forgotten 20 seconds after its last poll; the desktop polls
every 5 seconds while a note is open and sends `left` when it closes the note.
Presence lives in the room's memory only: nothing about who opened a note is
written by these routes (views are counted separately, see
`docs/app/notes-activity.md`).

### What every client shows

Faces for up to four people (then "+N"), each labelled "<name> · viewing" or
"<name> · editing", with a pencil badge on those editing; a line such as
"ana is editing…", "Live · ana is viewing" or "Live · 3 others here"; and
picking the faces lists everyone with their state.

## Tests

`app/tests/test_notes_comments.py` covers the API: creating threads and
replies, quotes kept sealed, mentions kept only for collaborators and pinged,
replies pinging the thread, edit and resolve permissions, deleting a thread
with its replies, the cap, anonymous readers refused, removal with the note,
and the presence routes.
