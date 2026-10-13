# Desktop: the Notes page

Part of [the desktop docs](README.md). Code: `desktop/src/MainWindowNotes.cpp`
(`requestNotes()`, `refreshNotes()`, `renderNotesList()`), the list-row rules
in `desktop/src/NoteListEntry.h`, and the request proof in
`desktop/src/NoteRequestProof.h`. Server: `app/src/notes.py`, with the
`_NotesRuntime` boundary in `app/src/entry.py`.

## Where the list comes from

The page merges two sources. Local notes are read from disk on every refresh.
Cloud notes come from `GET /api/notes`, the same service the web Notes page
uses. A local note that has a `cloudId` hides its cloud twin, so each note is
listed once. Cloud notes are not cached on disk: every page open and every
Refresh reads the relay again, so a note created on the web or another device
shows up on the next refresh, including after a restart.

## History: versions and who edited them (2026-10-07)

Every save of a cloud note is kept as a version (the newest 100), with the
saving account in `note_versions.author_bi`. `GET /api/notes/<id>/versions`
now also returns `author`, that account's name, for each version, so every
client shows who edited the note and when:

- desktop: the Versions button opens "Note history", one line per version
  (`Version N — date — @name`, the current one marked). Picking an older one
  restores it as a new version, based on the note's cloud version.
- web: the version drawer reads "2 hours ago · by @name".
- iOS: the History sheet subtitle reads the same way.

Names come from the requester's own session, then the note's user shares,
then one account lookup per remaining author (`_NotesRuntime.account_name`).
An author that no longer resolves gets an empty name and the client shows the
time alone. Only the owner and collaborators get names: a signed-in reader of
a published note still lists versions and times, but not who wrote them,
for the same reason a public reader never gets the share list.

Local-only notes keep their snapshots on disk with no author; they are all
the user's own.

## Authentication (fixed 2026-09-27)

`requestNotes()` sends one of two credentials:

- the account session token as `Authorization: Bearer …`, when the desktop
  holds a password session for the relay (`accountSessionTokenForUrl()`);
- otherwise a request signed with the account key: `?node=<account>&ts=<ms>&sig=<sig>`,
  where the signature covers
  `forkmesh-notes-request-v1\n<account>\n<METHOD>\n<path>[\n<sha256 of body>]\n<ts>`.
  GET requests sign no body. Every other method signs the SHA-256 of the exact
  JSON body it sends, including the empty body of a DELETE. The desktop builds
  this with `signOrgTaskProof()`, the helper the Tasks page uses.

On the server, `_NotesRuntime.session()` tries the password session first. If
there is none, it checks the signature with `_plugin_scope_signed_session()`,
scoped to `/api/notes`. `_NotesRuntime.json_body()` keeps the digest of the
raw body for that check. A signed request carries no `Origin` header, so
`same_origin()` accepts a request that has a `sig` parameter. A bad signature
still resolves to no account, and the notes API answers 401. Owner, share and
visibility checks in `notes.py` apply as they do for a session.

### The bug this fixed

On 2026-09-27 jett reported that notes visible on the web never appeared in
the desktop. The desktop's silent login proves the account key but, by
design, creates no password session. `requestNotes()` only knew the Bearer
token. With no token it returned "Cloud notes need a password sign-in"
without making a request, so the page showed local notes only. The API also
accepted nothing but a session: a live `GET /api/notes` with key-signature
parameters returned `401 invalid_session`. Calendar and Tasks had already been
given key-signed routes for the same reason. Notes had not.

The first fix was written on branch `491-desktop-notes-exist-online` but was
never merged into main, so jett reported the same symptom again on
2026-09-28 (branch `649-notes-not-showing-desktop`). It was ported onto main
unchanged, plus an explicit `<QUrlQuery>` include.

The fix needs both halves. A desktop build with the signed fallback shows
cloud notes only once the relay runs the `entry.py` change.

## Tests

- `app/tests/test_notes.py`: versions carry each editor's name for a
  collaborator and none for a non-collaborator reading a published note.
- `app/tests/test_notes_signed_session.py`: a key-signed list, read, create,
  update, delete, versions and shares request resolves to the signer. Changing
  the method, path or body, or signing for another API, is refused. A
  disabled account is refused. A password session still takes priority.
- `desktop/tests/test_note_request_proof.cpp` (`forkmesh-note-request-proof-tests`):
  the signed resource string and body digests, and that the proof name matches
  `NOTES_REQUEST_PROOF` in `app/src/entry.py`.
