# Projects on the web

Writers create, edit, close, reopen and delete projects on the Projects tab
without the desktop app. A project is the desktop's `ProjectStore` record,
`.forkmesh/projects/<n>/project-<n>.json`: metadata (title, description,
status, start and end dates, milestone, linked issues) derived from the full
signed event log stored beside it, so the desktop and every other reader see
a web-made project unchanged.

## Signing

The browser signs each project event with its own key
(`signWebProjectEvents`), byte-for-byte like `ProjectStore::canonicalString`:
`forkmesh-project-event-v1\n<type>\n<number>\n<author>\n<ts>\n<sha256(content)>`,
where content is `title NUL body` (open), the title, the body (edit), the
status (`open`/`closed`), `start NUL end` in ms (dates), the milestone, the
sorted issue numbers joined by commas (issues), or `self` (delete). A new
project is signed with the next free number; if someone takes it first the
route answers `409 number_taken` with the next one and the browser signs
again.

## Route

`POST /api/repo/<owner>/<repo>/project-edit` (`app/src/repository_projects.py`)
`{branch, number, events}`, for accounts with write access (the `/file-edit`
permission). It checks every event's shape and signature, appends events it
does not already hold (by id), recomputes the metadata like
`ProjectStore::recomputeMetadata`, and commits the file through the same
push path as web file edits (`commit_paths`). Read-only mirrors refuse it.
`delete` with target `self` tombstones the project, as on the desktop; the
list hides tombstoned projects.

`authorName` is not part of the signed string, so the route does not trust
the one the browser sends: every event goes through `_bind_session_author`
with the signed-in account, which sets `authorName` to that account and
refuses (403 `author_mismatch`) a key bound to a different account. A writer
can no longer sign with a fresh key and file events under someone else's
name.

## Interface

**New project** in the tab header (writers): title, description, start and
end dates, milestone and linked issues (`#12, #15`). In the List view each
project has **Edit**, **Close**/**Reopen** and **Delete**. An edit sends only
the fields that changed. Opening `/<owner>/<repo>/projects` directly now
loads the tab (it stayed empty until the tab was clicked).

Tests: `app/tests/test_repository_projects.py` (real Git, signatures,
numbering, permissions), `app/tests/test_web_projects_frontend.py` (browser
signatures verified by the relay's checker), and the web-made project check
in `desktop/tests/test_crypto.cpp`.
