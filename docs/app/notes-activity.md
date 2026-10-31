# Notes: views, history and actions

Since 2026-10-17 every note keeps a log of who opened it, how it changed and
what was done to it. The web Notes page shows the log in an **Activity**
column on the right, and the counts on every row of the note list on the left.

## What is recorded

`note_events` (migration `0271_note_events.sql`, `app/src/schema.py`) holds one
row per event: the note, the kind, the actor's account index (`actor_bi`, never
a name), when it happened and a small JSON `detail`. Rows go when the note is
deleted (`ON DELETE CASCADE`), and each note keeps its latest 500
(`MAX_EVENTS`), so a busy note never grows without limit.

| Kind | Recorded by | Detail |
| --- | --- | --- |
| `view` | the client opening the note | none |
| `share`, `role`, `unshare` | `POST` / `DELETE /api/notes/<id>/shares` | `type`, `name`, `role`, and the old role (`from`) for `role` |
| `publish`, `unpublish` | a `PATCH` that changes the visibility | none |
| `restore` | `POST /api/notes/<id>/versions` | the version restored and the new version it became (`as`) |
| `link`, `unlink` | `POST` / `DELETE /api/notes/<id>/links` | `owner`, `repo`, `kind`, `number` |
| `export`, `copy-link`, `task`, `issue` | the client, after Download Markdown, Copy link, Turn into a task or Turn into an issue | `owner`, `repo`, `number` for `issue` |
| `comment`, `resolve`, `reopen` | `POST` / `PATCH /api/notes/<id>/comments` (`docs/app/notes-comments.md`) | `reply` for a reply |

A share that changes nothing (inviting someone again with the same role,
removing someone who was not there, linking an issue that is already linked)
records nothing. Edits are not events: the history comes from
`note_versions`, which already records each save and its author, and the
note's creation from the note row itself. The detail holds only what is
already stored in plain text next to the note (sharing and link metadata);
titles and text stay encrypted and are never copied into the log.

Views by the same account within 30 minutes (`VIEW_WINDOW_MS`) fold into one
row whose `hits` goes up, so switching between notes, or reloading, does not
flood the log. Anonymous reads of a published page are still counted
separately in `note_views` (`docs/desktop/notes.md`); a signed-in `GET` is
never counted, so the desktop's freshness poll does not add views.

Logging never fails the action it records: an error writing the event is
ignored. Each new or folded event sends `note-activity` to the note's room
(`ForkMeshNoteRoom` accepts it next to `note-updated`), so an open Activity
column updates within about a second.

## The API

`GET /api/notes/<id>/activity` answers anyone the note is shared with (owner,
editors and viewers; not anonymous readers of the published page):

- `counts`: `views` (members' views plus anonymous reads), `readers`,
  `versions` (the note's version number), `actions` (every event except
  views), and `webViews`, `webReaders`, `webLastViewedAt` for the published
  page;
- `events`: the latest 200 events, every retained version after the first as
  `edit` with its `version` and author, and `create`, newest first. Each has
  `kind`, `actor` (the account name), `at`, and for events `count` and
  `detail`.

`POST` with `{"kind": "view"}` (or `export`, `copy-link`, `task`, `issue`)
records the event and answers the same payload, so opening a note costs one
request. Any other kind is `invalid_event`. An `issue` detail is kept only
when `owner`, `repo` and `number` are valid.

`GET /api/notes` and `GET /api/notes/<id>` also carry `views`, `readers` and
`actions` for every note (one grouped query per batch of the listing, as with
shares), so the list needs no request per row.

## Unread notes and the last edit

`GET /api/notes` and `GET /api/notes/<id>` also tell each signed-in reader
what changed since they last looked (`_freshness` in `app/src/notes.py`):

- `lastEditedBy`: the account that saved the current version;
- `myLastViewedAt`: your latest `view` event on the note (0 if never);
- `unreadComments`: comments by other people written after that;
- `openComments`: open threads (`docs/app/notes-comments.md`);
- `unread`: someone else saved the note after your last view, or
  `unreadComments` is above 0. A note shared with you that you never opened
  is unread; your own edits never make a note unread.

Opening a note on any client records a `view` (`POST /api/notes/<id>/activity`
with `{"kind": "view"}`), which is what clears the mark; the client also
clears it locally at once. Every client's note list shows a dot on unread
rows, the shared-with avatars, Private / Shared / Public (and on the desktop
Local / Cloud / Local + cloud), and the last edit as "Today 3:14 PM",
"Yesterday 9:02 AM", "Oct 29, 3:14 PM" or "Oct 29, 2025", with "by <name>"
when someone else made it.

## The web page

`app/public/dashboard/js/09-notes.js` (markup and styles in
`app/public/dashboard/partials/views/notes.html`, mirrored in
`marketplace/packages/notes/content/web/view.html`):

- **List rows** show, under the sharing line, when the note last changed and
  three counts with icons: views, versions and actions; hovering a count names
  it ("42 views from 9 readers").
- **The "Shared with …" line** wraps onto a second line instead of being cut
  off after the first name or two, breaks long names, and ends with an
  ellipsis only past two lines; hovering it shows the whole list.
- **The Activity column** (the pulse button next to Share) lists the events
  newest first, each with an icon, the person ("You" for yourself), what they
  did and when. Saves by the same person within 30 minutes are one line
  ("edited it · v12–v14, 3 saves"). Above the list, the three numbers are also
  filters: Views, History (edits, creation and restores, with a link to
  Version history) and Actions; picking the same one again, or **Show all**,
  shows everything.
- On windows of 1280 px and wider the column sits beside the editor and is
  open until you close it; narrower, it slides over the editor and starts
  closed. The choice is remembered (`forkmesh.notes.activity`).
- Opening a note records a view; saving, sharing, publishing, linking and
  restoring refresh the column, as do `note-activity` and `note-updated`
  messages from collaborators.

The desktop, iOS and Android apps read the same listing, so their view
counts now include members' views, but they have no Activity column yet.

## Tests

`app/tests/test_notes.py` covers recording and reading: each event kind, a
role change against an unchanged re-share, views folding within the window,
counts in the activity payload, the listing and a single note, refusing
unknown kinds, anonymous readers and people no longer shared with, the
500-event cap and removal with the note.
`app/tests/test_dashboard_notes_sidebar_frontend.py` pins the row counts, the
wrapping sharing line and the Activity column's wiring.
