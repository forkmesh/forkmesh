# Desktop: the Notes page

Part of [the desktop docs](README.md). Code: the Notes plugin's
`marketplace/packages/notes/content/desktop/NotesPage.cpp` (`requestNotes()`,
`refreshNotes()`, `renderNotesList()`), built as `notes.so` apart from the
window since 2026-10-25 ([package modules](package-modules.md#plugins-built-apart-from-the-window));
the quick-add "take a note", note links and the note-to-task/issue conversions
reach it through `NativePackageHost`. List-row rules in
`desktop/src/NoteListEntry.h`, request proof in `desktop/src/NoteRequestProof.h`,
link parsing, filters and export naming in `desktop/src/NoteTools.h`, comment
anchoring, threads, mentions and presence wording in `desktop/src/NoteComments.h`.
The comments panel is the plugin's `NoteCommentsPanel.cpp`, the faces
`NotePresenceFaces.cpp`. Note pages: `NotePageView.cpp` (the page view),
`NotePageDialogs.cpp` (emoji picker, Customize, page Share) and the rules in
`desktop/src/NotePages.h`.
Server: `app/src/notes.py`, with the `_NotesRuntime` boundary in `app/src/entry.py`.

## Where the list comes from

The page merges local notes (read from disk on every refresh) and cloud notes
from `GET /api/notes`, the service the web Notes page uses. A local note with a
`cloudId` hides its cloud twin. Cloud notes are not cached on disk; every open
and Refresh reads the relay again.

## List rows

Each row (`noteListItem()`, painted by `NoteRowDelegate`) shows the title, an
accent dot and a bold title when the note is unread, and on the right up to
three avatars of the people it is shared with (initials in the name's colour,
squares for organizations), then "+N". The second line has the storage badge
(Local / Cloud / Local + cloud), the visibility badge (Private / Shared /
Public; local notes are Private), a comment icon with `openComments` when above
0, and the last edit: "Today 3:14 PM", "Yesterday 9:02 AM", "Oct 29, 3:14 PM"
or "Oct 29, 2025", then "· by <name>" when `lastEditedBy` is someone else
(`noteLastEditLabel()` in `NoteTools.h`, English month and AM/PM whatever the
system locale). The fields come from `GET /api/notes`
([notes-activity.md](../app/notes-activity.md#unread-notes-and-the-last-edit));
a Local + cloud note uses its cloud twin's, and a local-only note is never
unread. A save keeps those fields and makes you the last editor until the next
listing.

Access details stay off the row. Its tooltip, and the top of its right-click
menu as disabled lines, carry the same header as the phones' long-press menu
(`noteAccessHeader()`): "You own this note" / "You can edit" / "View only";
"Shared with ana (can edit), acme (organization, can view)" or "Not shared";
the visibility and storage badges; and "Last edited Thursday, October 29,
2026, 3:14 PM by ana". The menu then offers Open, Copy link (cloud notes, the
`/dashboard/notes?note=<id>` link), Share… (owner, cloud) and Delete (owner,
with the usual confirm). Share… and Delete on a row that is not open open it
first and run once it has loaded (`runRowAction()`).

Opening a cloud note (or a Local + cloud note's twin) posts
`POST /api/notes/<id>/activity` with `{"kind": "view"}`, signed like every
other notes request, and clears the dot on the row at once
(`recordNoteView()`). Reloading the open note does not post again.

## Note emoji

The button before the title opens the picker (`pickNoteEmoji()`): a grid of
common emoji, a field for any emoji and Remove. The field is checked with
`isValidNoteEmoji()`, which mirrors the server's rule in
[notes-pages.md](../app/notes-pages.md#note-emoji) (1–16 code points, 64 UTF-8
bytes, no whitespace, control characters or ASCII letters, digits/`#`/`*` only
as keycaps, everything else a symbol, mark or format character). Only owners
and editors can change it. The emoji lives in the note object: local notes
keep it in `notes.json`, cloud saves send `emoji` (autosave too), so a Local +
cloud note syncs it like the title. It shows before the title in the list, the
editor and page outlines.

## Pages

Pages are cloud-only and use the signed `requestNotes()` path, since
`/api/notes/pages` is under `/api/notes`
([notes-pages.md](../app/notes-pages.md)). The **Pages** group above the search
box lists `GET /api/notes/pages` (colour square, emoji and title, number of
entries) with **New page**; it is hidden when the route fails. New page
creates "Untitled page" and opens Customize once it has loaded.

Opening a page replaces the editor with the page view (the editor stays behind
it; presence pauses and the note's heading buttons hide meanwhile). The view
loads `GET /api/notes/pages/<id>?content=1` and shows the title and emoji in
the page's colour, the description (Markdown) and the outline: sections (click
to fold; the saved `collapsed` flag is where they start), indented notes with
their emoji and the unread dot, links that open in the browser, and locked
entries ("You don't have access to this note"). The outline layout shows the
picked note on the right and records a view for it; the continuous layout
shows every note one after another and the outline scrolls to each. Notes the
answer left without `markdown` are fetched one by one. Open in editor (or a
double-click) opens the note in the editor; Notes goes back.

A collapsed section hides the entries after it until the next section at its
level or an entry less indented (`visibleOutlineRows()`). The outline editor in
Customize keeps every entry at most one level deeper than the one above
(`normalizedOutline()`), depth 0–3: Indent needs the entry above to be at least
as deep, Outdent drops one level, Move up and down (and drag) move one entry
and re-check the depths, a new entry goes after the picked one (inside it when
it is a section). Customize also edits the title, emoji, description, colour,
layout and show-emoji; it saves with `baseVersion`, and a 409 reloads the page
and says so while keeping the dialog open. Owners can delete the page there
(its notes are kept).

Share (owner) works like a note's share, with "Also share the notes on this
page" ticked (`includeNotes`). Copy link gives
`/dashboard/notes?page=<id>`, plus `&note=<id>` for the picked note; those
links open the page in the app (`NoteLink::page`; `main.cpp` takes a page link
as a note link through `NoteLink::isValid()`).

## Opening a note

Opening a note shows its rendered Preview; Write switches to the Markdown
source (`showOpenedNoteView()`). An empty note, and a new one, open on Write
instead, since there is nothing to preview. The view only resets when another
note is opened: reloading the open note (a refresh of the list) keeps
whichever tab you were on.

## Filters and search

Above the list sit a search box and the filters All / Mine / Shared / Public,
each with its count, as on the web page. Mine is a note you own (every local
note), Shared is one someone else owns and shared with you, Public is a
published note. The counts honour the search text. Changing the filter or the
search never switches the open note; a list that ends up empty says why.

Search matches titles and, for notes this computer has read (local notes and
the open cloud note), the body. The listing carries no bodies, so after 250 ms
the box also asks the relay `GET /api/notes?query=<text>` (trimmed, lower case,
at most 200 characters), which matches title or body server-side; its ids are
added to the matches. A reply for an older query, or one that arrives after
the text changed, is dropped. Until it answers, local matches show.

## Saving, autosave and conflicts

A Cloud only note you can edit (owner or editor) saves itself 1.5 s after the
last keystroke, like the web page; the label beside Save reads Unsaved
changes, Saving…, Saved or the error. Local and Local + cloud notes still save
with Save: every local save adds an on-disk snapshot, and autosaving would fill
the 100-snapshot history with keystrokes. Only one save is in flight at a
time, because the next PATCH needs the version the previous one returns; a
Save pressed meanwhile runs when it answers. Text typed while a save is in
flight stays on screen and is saved against the new version. An editor's save
leaves `visibility` out, since the server refuses that field from anyone but
the owner. Switching notes with an autosave pending sends it at once instead
of asking to discard.

When the relay answers 409 `version_conflict` it sends the current note. The
editor then shows "Keep mine" (save your text on top of their version) and
"Take theirs" (load their version and drop yours; for Local + cloud the disk
copy is rewritten to match). Autosave pauses until one is chosen. A conflict is
no longer saved as a stray local copy of a Cloud only note; other cloud errors
still fall back to this computer as before.

## Sharing

Share (owner only) opens a people panel: User / Organization, name, Can edit /
Can view. Typing asks `GET /api/notes/<id>/shares?query=<text>` after 150 ms
(empty or over 63 characters is not sent); `requestNotes()` signs only the
path, so the query string needs no change to the proof. Answers for an older
query are dropped. Picking a suggestion fills the name and sets User or
Organization. The list shows each person's name and type with a role picker
(POST the same principal with the new role) and Remove (DELETE). Once anyone
has access, "Link for people with access" shows
`<origin>/dashboard/notes?note=<id>` with Copy link, and a share confirms with
"Shared with <name>. Copy the link below to send it." A non-owner is told to use
Copy link instead.

The ⋯ menu has Copy link (any cloud note you can open), Copy public link and
Open published page (published notes), and Export Markdown…, which writes
`# <title>` plus the body to `<slug>.md` (the web's file name rule). A
published note also shows Copy public link beside "Live on the web".

## Links to a note

`forkmesh://note/<id>`, `<relay>/dashboard/notes?note=<id>` and
`<relay>/dashboard/notes/<id>` open the note in the app, both when clicked
inside it (`openBodyReference()`, `openReferenceLink()`) and when the system
hands the link to the app (`main.cpp`, `isLocalActionLink()`). A web link is
only taken when its host is the relay this app uses; a link to another host
says so instead of opening something else. `forkmesh::notes::parseNoteLink()`
is the single parser.

## Linked issues

Links lists the issues, pull requests and discussions linked to the note,
adds one from a URL, a desktop `forkmesh://issue|pull/...` reference or
`owner/repo#123` (with the kind picked beside it), opens and removes them
(`GET/POST/DELETE /api/notes/<id>/links`). Local-only notes keep their links on
disk. The linked items also show under the editor; issues and pull requests
open in the app, discussions on the relay's site.

## Presence

The app does not link QtWebSockets, so it uses the HTTP presence routes of
[notes-comments.md](../app/notes-comments.md#without-a-socket). While a cloud
note (or the cloud twin of a Local + cloud note) is open and the Notes page is
showing, `pollNotePresence()` posts `{clientId, state}` to
`/api/notes/<id>/presence` every 5 s; `clientId` is random per run, `state` is
`editing` when you can edit and typed in the note or its title in the last
10 s, else `viewing`, and the first keystroke sends at once. Opening another
note, a local note, leaving the page or closing the app sends `left`. A failed
call hides the presence row and retries every 30 s; saving never waits on it.

Beside the title: up to four faces (initials in the name's chat colour, or the
account picture), then "+N", a pencil badge on editors, a tooltip "<name> ·
viewing|editing" on each, and a line such as "ana is editing…", "Live · ana is
viewing" or "Live · 3 others here". Clicking the faces lists everyone with
their state; picking one opens the profile. When the answer's `commentsKey`
moves, the comments reload.

## Comments

Cloud notes only; the Comments button (heading row, with the number of open
threads) and the Comment button (editor toolbar) are hidden for local notes.
Comment, also in the right-click menu of Write and Preview, takes the selected
passage: in Write the selection itself, in Preview the selected text located
in the Markdown (formatted text that differs from the Markdown asks you to
select it in Write). It sends `quote`, up to 32 UTF-16 units of `prefix` and
`suffix` (never splitting a surrogate pair) and `start`/`end`, which are
`QString` offsets and so already UTF-16. Blanks around the selection are
trimmed; more than 1000 characters is refused. New in the panel comments on
the selection, or on the whole note when nothing is selected.

The side panel lists open threads (whole-note first, then document order,
detached last) and folds resolved ones under "Resolved (n)". A thread shows
its quote (struck through with "Removed from the note" when detached), author,
time, body with kept mentions coloured, replies, a reply box, Resolve or Reopen
(when the answer's `canResolve` is true or you wrote the thread), Edit on your
own comments and Delete on your own (on anyone's when you own the note). Delete
asks first, naming the replies that go with a thread. Typing `@` offers names from `people`; Ctrl+Enter
posts. Anchors are re-found 0.5 s after typing stops, and the list is only
rebuilt when a thread's place changes; drafts, focus and scroll survive a
rebuild. Clicking a thread selects its passage in Write and marks it; in
Preview the quote (or its longest line without Markdown marks) is marked
there, and Write opens when the rendered text cannot be found.
`/dashboard/notes?note=<id>&comment=<cid>` opens the note with that thread
shown, the thread of a reply included (`trustedNoteLink()`).

## Versions

Every save of a cloud note is kept as a version (the newest 100), with the
saving account in `note_versions.author_bi`. `GET /api/notes/<id>/versions`
returns each version's `author` name. The desktop's Versions button opens
"Note history"; picking an older version restores it as a new version. Only the
owner and collaborators see author names; a reader of a published note sees
versions and times only. Local-only notes keep author-less snapshots on disk.

## Authentication

`requestNotes()` sends one of two credentials:

- the account session token as `Authorization: Bearer …`, when the desktop
  holds a password session (`accountSessionTokenForUrl()`);
- otherwise a request signed with the account key: `?node=<account>&ts=<ms>&sig=<sig>`,
  where the signature covers
  `forkmesh-notes-request-v1\n<account>\n<METHOD>\n<path>[\n<sha256 of body>]\n<ts>`.
  GET signs no body; every other method signs the SHA-256 of the exact JSON
  body, including an empty DELETE body. Built with `signOrgTaskProof()`.

On the server, a request with `node`, `ts` and `sig` is resolved only by
`_plugin_scope_signed_session()` scoped to `/api/notes`, never by the browser
cookie; `_NotesRuntime.json_body()` keeps the raw body digest for it. Any
other request uses the password session. A signed request carries no
`Origin`, so `same_origin()` accepts the complete `node`/`ts`/`sig` triple or
a real bearer token (not the dashboard's `cookie` marker). Before this, any
`sig` value lifted the origin check while the cookie still signed the request
in. A bad signature resolves to no account (401). Owner, share and visibility
checks apply as for a session.

The live room (`/api/notes/<id>/ws`) is cookie-authenticated in the browser,
and WebSocket handshakes are not covered by CORS, so `_NotesRuntime.realtime()`
refuses a handshake whose `Origin` is not an allowed ForkMesh origin (403). The
dashboard and World open it from their own origin; the desktop does not use it
(see Presence).

## Tests

- `app/tests/test_notes.py`: version author names by role.
- `app/tests/test_notes_signed_session.py`: key-signed requests resolve to the
  signer; altered method, path or body, another API scope, or a disabled
  account are refused; an unsigned request uses the password session; a signed query never
  falls back to the cookie; the live room needs an allowed `Origin`.
- `desktop/tests/test_note_tools.cpp` (`forkmesh-note-tools-tests`): note
  link parsing and host check, comment links, built links, linked-issue
  references, export names, share query rules, filters, search, conflict
  detection, and the list row: last-edit stamps and tooltip, badges, share
  avatars and "+N", unread rules and the local clear, kept fields on save, and
  the row menu's access header.
- `desktop/tests/test_note_pages.cpp` (`forkmesh-note-pages-tests`): the emoji
  rule (skin tones, flags, ZWJ, keycaps, letters, whitespace, the code point
  and byte limits, every picker emoji), outline depth, indent, outdent, move,
  remove and insert rules, the 300-entry cap, link urls, folded sections,
  locked and labelled entries, page field defaults, and `?page=`/`&note=` links.
- `desktop/tests/test_note_comments.cpp` (`forkmesh-note-comments-tests`): the
  anchor scoring (prefix, suffix, distance, overlaps, UTF-16 offsets), quote
  capture, the Preview search text, thread grouping and order, resolve rights,
  mention spans and suggestions, error and time wording, and the presence
  peers, labels, line, "+N", editing state, initials and colour hash.
- `desktop/tests/test_window_resize.cpp` `checkNoteSaveAlert`: finds the title
  and storage controls by object name, since the search box is now the first
  line edit on the page.
- `desktop/tests/test_note_request_proof.cpp` (`forkmesh-note-request-proof-tests`):
  the signed resource string, body digests, and that the proof name matches
  `NOTES_REQUEST_PROOF` in `app/src/entry.py`.
