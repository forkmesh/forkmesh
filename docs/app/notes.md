# Notes on the web

Part of [the App docs](README.md). The web Notes page
(`app/public/dashboard/js/09-notes.js`, markup and styles in
`app/public/dashboard/partials/views/notes.html`, mirrored byte for byte in
`marketplace/packages/notes/content/web/view.html`) offers the same Notes
features as the desktop ([desktop notes](../desktop/notes.md)) and the phone
apps ([native notes](../native/notes.md)). This page covers sharing and the
access link, Live mode, the activity log, turning a note into a task or an issue, body
search, and the differences between platforms.

## Sharing with people

The owner shares a note with a user or an organization from the **Share**
panel.

### Picking who to share with

The name field suggests matches as you type:

- **Users** come from `GET /api/chat/direct-messages/users?query=…`, the same
  bounded search of active accounts the organization member picker uses. An
  empty query never asks the server, so the account list is never enumerated.
- **Organizations** come from `GET /api/orgs`, the organizations you belong to,
  fetched once per page and filtered locally.

Requests are debounced (150 ms) and a stale answer is dropped when a newer
query has started. Your own account and every principal the note is already
shared with are left out, and names that start with the query sort first.

Arrow keys move the highlight, Enter or Tab or a click picks it, Escape closes
the list without closing the panel. Picking a suggestion fills the name and
switches the User/Org selector to match; Enter again (or **Invite**) shares
it. When the highlighted suggestion is exactly what is typed, the first Enter
shares straight away. A typed name that matches nothing still works: the
server resolves it and answers `principal_not_found` if it does not exist.

### The notes-scoped suggestion endpoint

The desktop, iOS and Android apps ask one endpoint instead:
`GET /api/notes/<id>/shares?query=…` (owner only, `app/src/notes.py`
`_suggestions`). It answers the usual `shares` plus `suggestions`, a list of
`{type, name}` built by the same rules as above: active accounts from the
shared `search_accounts` (on `_WorldCommunityRuntime` in `entry.py`), the
caller's own organizations containing the text, the caller and principals
already shared left out, prefix matches first, at most 8. An empty query
answers no suggestions without searching; a query over 63 characters is
`invalid_query`.

It lives under `/api/notes` because the desktop without a password session
signs each request with the account key, and that signature is scoped to
`/api/notes` ([desktop notes](../desktop/notes.md)); the chat user search
would refuse it.
Filtering on the server also means every client leaves out the same people.

### Who an organization share reaches

A share with an organization reaches its insiders only: owners, admins,
maintainers, members and auditors (`ORG_INSIDER_ROLE_SQL` in
`app/src/organization_rbac.py`). Guests are outside collaborators and get
nothing from it, neither in the list nor by id. An auditor is read-only, so an
`editor` share opens the note for an auditor as `viewer`. `_role` and `_list`
in `app/src/notes.py` apply both rules, so every note route (read, edit,
versions, activity, the live socket) agrees.

### The access link

A shared note is private: the published `/notes/<id>` page only reads public
notes and sends no session, so it cannot open a note shared with you. Once a
note is shared with at least one person, the Share panel shows **Link for
people with access**, `/dashboard/notes?note=<id>`, with **Copy link**. The
**⋯** menu has **Copy link** too, so anyone the note is shared with (not only
the owner) can pass the same link on.

Opening that link selects the named note after the list loads. A note you
cannot open shows the error above the editor and the page falls back to your
first note. Selecting a note rewrites the
address bar's `?note=` (with `history.replaceState`, so Back is not flooded),
so the address bar is always a link to the open note.

Publishing to the web is separate: its own link and Copy link appear under
**Publish to the web**. When a note is published the **⋯** menu also shows
**Open published page**, linking to `/notes/<id>` in a new tab; it is hidden
for private notes.

## Activity: views, history and actions

Every note keeps a log of who opened it, how it changed and what was done to
it. The web Notes page shows the log in an **Activity** column on the right
and the counts on every row of the note list.

### What is recorded

`note_events` (migration `0271_note_events.sql`, `app/src/schema.py`) holds one
row per event: the note, the kind, the actor's account index (`actor_bi`, never
a name), when it happened and a small JSON `detail`. Rows go when the note is
deleted (`ON DELETE CASCADE`), and each note keeps its latest 500
(`MAX_EVENTS`).

| Kind | Recorded by | Detail |
| --- | --- | --- |
| `view` | the client opening the note | none |
| `share`, `role`, `unshare` | `POST` / `DELETE /api/notes/<id>/shares` | `type`, `name`, `role`, and the old role (`from`) for `role` |
| `publish`, `unpublish` | a `PATCH` that changes the visibility | none |
| `restore` | `POST /api/notes/<id>/versions` | the version restored and the new version it became (`as`) |
| `link`, `unlink` | `POST` / `DELETE /api/notes/<id>/links` | `owner`, `repo`, `kind`, `number` |
| `export`, `copy-link`, `task`, `issue` | the client, after Download Markdown, Copy link, Turn into a task or Turn into an issue | `owner`, `repo`, `number` for `issue` |
| `comment`, `resolve`, `reopen` | `POST` / `PATCH /api/notes/<id>/comments` ([notes comments](notes-comments.md)) | `reply` for a reply |

A share that changes nothing (inviting someone again with the same role,
removing someone who was not there, linking an issue that is already linked)
records nothing. Edits are not events: the history comes from
`note_versions`, which already records each save and its author, and the
note's creation from the note row itself. The detail holds only what is
already stored in plain text next to the note (sharing and link metadata);
titles and text stay encrypted and are never copied into the log.

Views by the same account within 30 minutes (`VIEW_WINDOW_MS`) fold into one
row whose `hits` goes up, so switching notes or reloading does not flood the
log. Anonymous reads of a published page are counted separately in
`note_views` ([desktop notes](../desktop/notes.md)); a signed-in `GET` is
never counted, so the desktop's freshness poll adds no views.

Logging never fails the action it records: an error writing the event is
ignored. Each new or folded event sends `note-activity` to the note's room
(`ForkMeshNoteRoom` accepts it next to `note-updated`), so an open Activity
column updates within about a second.

### Activity API

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

### On the page

- **List rows** show, under the sharing line, when the note last changed and
  three counts with icons: views, versions and actions; hovering a count names
  it ("42 views from 9 readers").
- **The "Shared with …" line** wraps onto a second line instead of being cut
  off after the first name or two, breaks long names, and ends with an
  ellipsis only past two lines; hovering it shows the whole list.
- **The Activity tab** of the note's right column (the pulse button next to
  Share; see [the side column](notes-comments.md#the-side-column)) lists the events
  newest first, each with an icon, the person ("You" for yourself), what they
  did and when. Saves by the same person within 30 minutes are one line
  ("edited it · v12–v14, 3 saves"). Above the list, the three numbers are also
  filters: Views, History (edits, creation and restores, with a link to the
  History tab) and Actions; picking the same one again, or **Show all**,
  shows everything.
- On windows of 1280 px and wider the column sits beside the editor and is
  open until you close it; narrower, it slides over the editor and starts
  closed. Whether it is open is remembered (`forkmesh.notes.activity`), and
  which tab (`forkmesh.notes.sideTab`).
- Opening a note records a view; saving, sharing, publishing, linking and
  restoring refresh the column, as do `note-activity` and `note-updated`
  messages from collaborators.

The desktop shows the same timeline in its side panel, and iOS and Android
in the note's History · Activity · Chat sheet.

## Unread notes and the last edit

`GET /api/notes` and `GET /api/notes/<id>` also tell each signed-in reader
what changed since they last looked (`_freshness` in `app/src/notes.py`):

- `lastEditedBy`: the account that saved the current version;
- `myLastViewedAt`: your latest `view` event on the note (0 if never);
- `unreadComments`: comments by other people written after that;
- `openComments`: open threads ([notes comments](notes-comments.md));
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

## Turning a note into a task or an issue

The note's **⋯** menu offers the same conversions as the iOS, Android and
desktop apps. Anyone who can open the note sees them; the note itself is
never changed.

### Turn into a task

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
  details limit, [tasks.md](tasks.md#task-text-is-markdown)).

The text is what is on screen, so unsaved edits are included, as on native.
Notices: "Task created from this note", or on failure a known error
(`not_found`, `forbidden` / `org_member_required`, `unauthorized` /
`invalid_session`, worded as in `PlanningErrors`) or "The task was not
created."

### Turn into an issue

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

The note listing (`GET /api/notes`) carries no Markdown, so typing in
**Search notes** also asks `GET /api/notes?query=<text>` (debounced 250 ms,
stale answers dropped). The server already decrypts every listed note to read
its title, so `app/src/notes.py` `_matches` checks the lowercase text against
the title and the Markdown and returns the matching notes (same shape as the
plain listing, without Markdown). A query over 200 characters is
`invalid_query`. Until the answer arrives the list shows title matches, or
"Searching notes…" when there are none. Native clients can use the same
parameter.

## Live mode

Write, Split, Preview and Live sit in the editor's layout switch (remembered
in `forkmesh.notes.mode`; people who can only view always get Preview). Live
is the preview you can edit: the note is cut into blocks at blank lines
(a fenced code block stays whole), each rendered on its own, and clicking a
block turns it into a box holding that block's Markdown, with the caret near
where you clicked. Typing writes the block back into the note, which autosaves
and sends drafts as in Write. Escape, or clicking elsewhere, renders it again;
Up at the start or Down at the end of a block moves to the one before or
after; Ctrl/⌘+B and +I format the selection; clicking below the last block
starts a new paragraph, and an empty block is dropped when you leave it. The
title is editable in place. Unchanged blocks keep their rendered elements, so
images do not reload while you type, and edits from others re-render the
blocks around the one you are in. Selecting text in a rendered block or in the
block you are editing offers Comment as in the other modes
([notes comments](notes-comments.md)).

## The same features on every platform

The web page, the desktop ([desktop notes](../desktop/notes.md)), iOS and
Android ([native notes](../native/notes.md)) offer the same Notes features:
filters with counts and title-and-body search (`GET /api/notes?query=`,
[above](#search-covers-the-body)),
autosave, write, preview and Live with a format bar, Keep mine / Take theirs on a
save conflict, the share panel with suggestions and role changes, the link for
people with access, opening `/dashboard/notes?note=<id>`, publishing with a
copyable public link, version history, linked issues and pull requests,
Markdown export, delete, and turning a note into a task or an issue.

Known differences:

- Live presence and inline comments run everywhere
  ([notes comments](notes-comments.md)). The web and phones use the note's
  socket; the desktop does not link QtWebSockets, so it polls
  `/api/notes/<id>/presence` every 5 seconds instead (every second while
  following someone, every 2 while the chat is open).
- Live mode, following someone and the note chat are on the web and the
  desktop; the phone apps do not have them yet. On the web Live renders the
  note block by block and turns the block you click into its Markdown; on the
  desktop Live styles the Markdown in place and shows the marks only on the
  caret's line ([desktop notes](../desktop/notes.md#live-mode)).
- The desktop editor has Write and Preview tabs but no split view; phones
  have no room for one.
- The desktop autosaves Cloud-only notes. Local and Local + cloud notes save
  with the Save button, because each local save adds an on-disk snapshot.
- On iOS the format bar adds text at the end of the note: the text editor
  cannot report the selection before iOS 18.
- Local (on-disk) notes are a desktop feature.
- Writing with AI on a selection: the web asks the site's Workers AI through
  `POST /api/notes/assist` ([notes assist](notes-assist.md)); the desktop asks
  the default local agent (Claude Code or Codex) through the composer and the
  editor's right-click menu ([desktop notes](../desktop/notes.md#rewriting-a-selection)),
  with the same quick actions. The phone apps do not have it yet.

## Tests

- `app/tests/test_notes.py`: recording and reading activity (each event kind,
  a role change against an unchanged re-share, views folding within the
  window, counts in the activity payload, the listing and a single note,
  refusing unknown kinds, anonymous readers and people no longer shared with,
  the 500-event cap and removal with the note); the suggestion endpoint
  (matching users and organizations, the owner and already-shared people left
  out, prefix order, empty and over-long queries, and non-owners refused); and
  `test_listing_search_matches_title_and_body`.
- `app/tests/test_dashboard_notes_sidebar_frontend.py`: the row counts, the
  wrapping sharing line and the Activity column's wiring; the conversion
  bodies and notices, the menu markup in the partial, the marketplace mirror
  and the built `dashboard/notes/index.html`.
- `app/browser_tests/tests/dashboard-notes-share.spec.js` drives the built
  page with mocked APIs: suggestions for users and organizations, keyboard
  picking, already-shared principals left out, the access link row and copy,
  opening `?note=<id>`, body search, the task body with the published link,
  and the issue dialog (bad repository, then a filed issue).
