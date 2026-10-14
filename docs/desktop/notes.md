# Desktop: the Notes page

Part of [the desktop docs](README.md). Code: `desktop/src/MainWindowNotes.cpp`
(`requestNotes()`, `refreshNotes()`, `renderNotesList()`), list-row rules in
`desktop/src/NoteListEntry.h`, request proof in `desktop/src/NoteRequestProof.h`.
Server: `app/src/notes.py`, with the `_NotesRuntime` boundary in `app/src/entry.py`.

## Where the list comes from

The page merges local notes (read from disk on every refresh) and cloud notes
from `GET /api/notes`, the service the web Notes page uses. A local note with a
`cloudId` hides its cloud twin. Cloud notes are not cached on disk; every open
and Refresh reads the relay again.

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
- `desktop/tests/test_note_request_proof.cpp` (`forkmesh-note-request-proof-tests`):
  the signed resource string, body digests, and that the proof name matches
  `NOTES_REQUEST_PROOF` in `app/src/entry.py`.
