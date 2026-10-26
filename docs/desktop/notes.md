# Desktop: the Notes page

Part of [the desktop docs](README.md). Code: the Notes plugin's
`marketplace/packages/notes/content/desktop/NotesPage.cpp` (`requestNotes()`,
`refreshNotes()`, `renderNotesList()`), built as `notes.so` apart from the
window since 2026-10-25 ([package modules](package-modules.md#plugins-built-apart-from-the-window));
the quick-add "take a note", note links and the note-to-task/issue conversions
reach it through `NativePackageHost`. List-row rules in
`desktop/src/NoteListEntry.h`, request proof in `desktop/src/NoteRequestProof.h`,
link parsing, filters and export naming in `desktop/src/NoteTools.h`.
Server: `app/src/notes.py`, with the `_NotesRuntime` boundary in `app/src/entry.py`.

## Where the list comes from

The page merges local notes (read from disk on every refresh) and cloud notes
from `GET /api/notes`, the service the web Notes page uses. A local note with a
`cloudId` hides its cloud twin. Cloud notes are not cached on disk; every open
and Refresh reads the relay again.

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

Not on desktop: the app does not link QtWebSockets, and adding a module for a
"who else is here" row was not worth the dependency. Refresh still reloads a
collaborator's saved version, and the conflict choice covers concurrent saves.

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

On the server, `_NotesRuntime.session()` tries the password session first, then
verifies the signature with `_plugin_scope_signed_session()` scoped to
`/api/notes`; `_NotesRuntime.json_body()` keeps the raw body digest for it. A
signed request carries no `Origin`, so `same_origin()` accepts requests with a
`sig` parameter. A bad signature resolves to no account (401). Owner, share and
visibility checks apply as for a session.

## Tests

- `app/tests/test_notes.py`: version author names by role.
- `app/tests/test_notes_signed_session.py`: key-signed requests resolve to the
  signer; altered method, path or body, another API scope, or a disabled
  account are refused; a password session takes priority.
- `desktop/tests/test_note_tools.cpp` (`forkmesh-note-tools-tests`): note
  link parsing and host check, built links, linked-issue references, export
  names, share query rules, filters, search and conflict detection.
- `desktop/tests/test_window_resize.cpp` `checkNoteSaveAlert`: finds the title
  and storage controls by object name, since the search box is now the first
  line edit on the page.
- `desktop/tests/test_note_request_proof.cpp` (`forkmesh-note-request-proof-tests`):
  the signed resource string, body digests, and that the proof name matches
  `NOTES_REQUEST_PROOF` in `app/src/entry.py`.
