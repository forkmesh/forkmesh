# Notes: turning a note into a task or an issue, and search

The web Notes page (`app/public/dashboard/js/09-notes.js`, markup in
`app/public/dashboard/partials/views/notes.html`, mirrored byte for byte in
`marketplace/packages/notes/content/web/view.html`) offers the same
conversions as the iOS, Android and desktop apps, from the note's **⋯** menu.
Anyone who can open the note sees them; the note itself is never changed.

## Turn into a task

`POST /api/tasks` with

```json
{"title": "…", "details": "…", "department": "general",
 "destination": "department", "assigneeKind": "unassigned"}
```

the same body as iOS `NoteConversions.toTask` (`FMPlanning/TaskOperations.swift`)
and Android `NoteConversions.toTask` (`planning/TaskOperations.kt`).

- **Title**: the note title trimmed, or else the first non-empty Markdown line
  with its leading `#`s removed, or else "Untitled note"; at most 160
  characters.
- **Details**: the note's Markdown followed by
  `\n\n---\nConverted from the note "<title>"`, plus
  ` (published at <origin>/notes/<id>)` when the note is published. The
  Markdown is cut with `…` so the whole text fits 4,000 characters (the task
  details limit).

The text is what is on screen, so unsaved edits are included, as on native.
Notices: "Task created from this note", or on failure a known error
(`not_found`, `forbidden` / `org_member_required`, `unauthorized` /
`invalid_session`, worded as in `PlanningErrors`) or "The task was not
created."

## Turn into an issue

A small dialog asks "Issue in which repository?" with an `owner/repository`
field (the page uses its own `<dialog>`, as for delete, never
`window.prompt`). A value that is not exactly two non-empty parts keeps the
dialog open with "Choose a repository as owner/name." Otherwise the issue is
filed with `submitWebIssue` (`03-issues-profile-io.js`), the browser-signed
open event the repository's issue composer uses (`POST
/api/repo/<owner>/<repo>/issues`), the counterpart of the native
`IssueFiler`. Title and description are built as for a task, but the
description may run to 60,000 characters.

Notices: "Issue filed from this note", "The issue was not filed.", and
"Sign in on this device to file issues." without a session. Both items are
disabled while a conversion is running so a double click cannot file twice.

## Search covers the body

The note listing (`GET /api/notes`) carries no Markdown, so the sidebar
search used to match titles only. Typing in **Search notes** now also asks
`GET /api/notes?query=<text>` (debounced 250 ms, stale answers dropped).
The server already decrypts every listed note to read its title, so
`app/src/notes.py` `_matches` checks the lowercase text against the title and
the Markdown and returns only the matching notes (same shape as the plain
listing, still without Markdown). A query over 200 characters is
`invalid_query`. Until the answer arrives the list shows title matches, or
"Searching notes…" when there are none. Native clients can use the same
parameter.

## Open published page

When a note is published the **⋯** menu shows **Open published page**,
linking to `/notes/<id>` in a new tab; it is hidden for private notes.

## Tests

- `app/tests/test_notes.py` `test_listing_search_matches_title_and_body`.
- `app/tests/test_dashboard_notes_sidebar_frontend.py`: the conversion bodies
  and notices, the menu markup in the partial, the marketplace mirror and the
  built `dashboard/notes/index.html`.
- `app/browser_tests/tests/dashboard-notes-share.spec.js`: body search, the
  task body with the published link, and the issue dialog (bad repository,
  then a filed issue) against mocked APIs.
