# Notes in the phone apps

The web Notes page is the reference (`docs/app/notes-sharing.md`). Each phone
app has its own section below.

## Android

Code: `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/notes/`
(`NotesLogic.kt`, `NotePresence.kt`, tested by `NotesLogicTest.kt`) holds
everything that is not a view; `feature/tools/NotesScreens.kt` in the app
module holds the list, the editor and its sheets.

### List

- Filters **All / Mine / Shared / Public** with a count on each tab
  (`NotesLogic.counts`). Mine is `role == owner`, Shared is any other role,
  Public is `visibility == public`.
- Search matches the title and the body through the server:
  `GET /api/notes?query=<text>` (`NotesApi.search`, see
  `docs/app/notes-convert.md`), asked 250 ms after the last keystroke
  (`NotesLogic.SEARCH_MS`). The request runs in a `LaunchedEffect` keyed on the
  query, so a newer keystroke cancels it, and an answer is used only while the
  box still holds the query it was for (`NoteSearch`). Until it answers the
  list shows local title matches, or "Searching notes…" when there are none.
  The filters and their counts apply to the server's answer
  (`NotesLogic.searchResults`). A query over 200 characters is not sent and
  matches titles only.

### Editor

- **Autosave.** Typing schedules a save 1.5 s after the last keystroke
  (`NotesLogic.AUTOSAVE_MS`). The save-state label in the bar reads Saved,
  Unsaved changes, Saving…, the error, or "Not saved — resolve the conflict";
  tapping it saves now. Saves run one at a time behind a mutex on
  `AppLifecycle.scope`, not the screen's scope, so leaving the screen never
  cancels a PATCH whose new version the next save needs. After a save the
  "saved" text is what was sent, not what came back, because the server trims
  the title and comparing against its copy would autosave forever. Back
  flushes the pending save before closing and offers Discard only if that
  save fails; a screen removed any other way saves on its way out.
- **Conflicts.** `ApiService.accountApiCall` keeps only the error code, so
  `NotesApi.save` turns `version_conflict` into `NoteConflictException`
  carrying a fresh `GET` of the note. A banner offers **Keep mine** (re-save
  the local text on the new version) or **Take theirs**.
- **Write / Preview** toggle. Preview reuses the docs reader's parser and
  views (`DocsMarkdown`, `DocsBlockView`). The format toolbar (heading, bold,
  italic, list, task, link, code) applies `NotesLogic.format` to the current
  selection, the same edits as the web toolbar.
- **Links.** The ⋯ menu has **Copy link** (`<origin>/dashboard/notes?note=<id>`)
  for every role. On a published note it adds **Copy public link** and
  **Open published page**, and the public link shows under the title (tap to
  copy).
- **Linked issues and PRs** (owners and editors): a sheet listing the links
  (tap opens them in the app), removing them, and adding one from a pasted
  URL or `owner/repo#123` with an Issue / PR / Discussion choice for the
  shorthand. Parsing is `NoteLink.parse`.
- **Export Markdown** opens the system Save dialog (`CreateDocument`,
  `text/markdown`) with `<slug>.md` and writes `# <title>` plus the body, as
  the web download does.
- History shows author names; restore, publish / make private, delete with
  confirmation and Turn into a task / issue are unchanged.

### Sharing

- The name field asks `GET /api/notes/<id>/shares?query=` 150 ms after the
  last keystroke. The request runs in a `LaunchedEffect` keyed on the text, so
  a newer keystroke cancels it and its answer is dropped; the answer is also
  ignored unless the field still holds the same query. Picking a suggestion
  fills the name, switches User / Org, and stops the list reopening for that
  name.
- Each person has a role menu (Can edit / Can view), which re-POSTs the same
  principal with the new role, and a remove button.
- Once the note has one share the sheet shows **Link for people with access**
  with **Copy link** and **Send** (the Android share sheet). Sharing shows
  "Shared with <name>. Copy the link below to send it."

### Opening a note from a link

`RouteParser` maps both `/dashboard/notes/<id>` and
`/dashboard/notes?note=<id>` to the Note route (`NavigationTest`).

### Presence

The editor joins `GET /api/notes/<id>/ws` with the core `WebSocketClient`,
sending the session as an `Authorization: Bearer` header (the browser uses
`?sessionToken=` only because it cannot set headers). It sends presence every
15 s and drafts 180 ms after typing, shows up to four faces and a line such as
"Live · ana is here" or "bo is editing…", applies a collaborator's draft only
when there are no unsaved local edits and the draft is based on the version on
screen, and reloads on `note-updated` under the same condition. After
`access-changed` it reloads and reconnects at once; after any other drop it
waits 15 s. When the socket cannot open, the line stays hidden: saving does
not depend on it.

## iOS

Code: FMTools (`native/ios/ForkMeshCore/Sources/FMTools/NotesModels.swift`:
`NotesListModel`, `NoteEditorModel`, `NoteHistoryModel`, `NoteShareModel`) holds
the state and is tested on Linux (`Tests/FMToolsTests/NotesTasksTests.swift`,
`NotesParityTests.swift`). `native/ios/ForkMesh/Features/Tools/NotesScreen.swift`
holds the views. Every request goes through the `ToolsApiCall` closure over
`ApiService.notesApi`. Web links come from `ApiService.webLink` through a
`ToolsWebLink` closure, so FMTools needs neither FMChat nor a fixed origin.

### List

- Filters **All / Mine / Shared / Public** with a count on each tab. The counts
  follow the search box, so they show where the matches are.
- Search matches the title and the body through the server:
  `GET /api/notes?query=<text>` 250 ms after the last keystroke
  (`NotesListModel.queryChanged`). A newer keystroke makes an older answer stale
  and it is dropped. Until the answer arrives the list shows local title
  matches, or "Searching notes…" when there are none. Filters and counts then
  apply to the server's answer. A query over 200 characters is not sent. The
  text is percent-encoded so `&` or `#` stay part of the query.

### Editor

- **Autosave.** `edited()` restarts a 1.5 s timer. Saves run one at a time; a
  save asked for during another one is queued. Text typed while a save is in
  flight stays on screen and is saved next on the new version. As on Android,
  the "saved" text is what was sent. The save state appears in the line under
  the title, and the manual Save button stays. Leaving saves pending text. The
  Discard question appears only when the last save failed or a conflict is open.
- **Conflicts.** A 409 `version_conflict` fetches the note again, because
  `OrgApiException` keeps only the code. **Keep mine** re-saves on the new
  version; **Take theirs** replaces the text. Autosave waits until one is chosen.
- **Write / Preview.** A segment switch; viewers only get Preview. Preview uses
  the Docs reader's parser (`DocsMarkdown.blocks`, task boxes included) with
  SwiftUI inline markdown and the Docs code block view. The format bar (heading,
  bold, italic, list, task, link, code) inserts at the end of the text. The app
  targets iOS 17, and `TextEditor` has no selection binding before iOS 18.
  `NoteFormatting.apply` already takes a selection.
- **Links.** **Copy link** (`/dashboard/notes?note=<id>`) is in the ⋯ menu for
  every role. A published note shows its `/notes/<id>` link with Copy under the
  title and adds **Open published page** to the menu.
- **Linked issues / PRs / discussions** (owners and editors): chips under the
  editor (tap opens the web page) and remove buttons. Links are added from a
  URL or `owner/repo#123`, with a kind picker for the short form. Parsing is
  `NoteLinkRef.parse`.
- **Export Markdown** is a `ShareLink` whose item writes `<slug>.md` (`# title`
  then the body) to the temporary folder when shared.

### Sharing

- Suggestions come from `GET /api/notes/<id>/shares?query=` 150 ms after the
  last keystroke. Each keystroke or pick bumps a generation counter, and an
  answer for an older generation is dropped. Picking fills the name, switches
  User / Org, and does not reopen the list for that name.
- Each person has a Can edit / Can view menu (a re-POST with the new role) and
  a remove button.
- Once the note has a share, **Link for people with access** shows the link
  with **Copy link** and **Send** (the system share sheet). Sharing shows
  "Shared with <name>. Copy the link below to send it."

### Opening a note from a link

`RouteParser` maps both `/dashboard/notes/<id>` and `/dashboard/notes?note=<id>`
to `.note(id:)` (`RouteParserTests`).

### Presence

`ApiService.noteSocket(_:connector:)` opens `GET /api/notes/<id>/ws` with the
app's `WebSocketConnector`. It sends the session as an
`Authorization: Bearer` header, as Android does, and uses `noteSocketURL` for
the address. FMTools' `NotePresence` matches Android's frame handling: presence
every 15 s, a reply to each newcomer, drafts 180 ms after typing, peers
forgotten after 37.5 s, and "… is editing…" for 5 s after a draft.
`NoteRoomModel` runs the socket and shows up to four initials and the label
("Live", "Live · ana is here", "Live · 2 others here", "bo is editing…",
"Offline — edits still save").
`NoteEditorModel` joins the room after a note loads or is first saved, and
leaves it when the screen goes away or the note is deleted. It reacts to events
as follows:

- A collaborator's draft replaces the text only when there are no unsaved local
  edits, no save is running and the draft's base version is the one on screen.
- `note-updated` reloads the note under the same rules; with unsaved edits the
  next autosave meets the 409 instead.
- `note-deleted` shows "This note was deleted." and stops autosave.
- `access-changed` closes the socket, reloads the note, and reconnects after
  400 ms. Any other drop waits 15 s.

The label stays hidden until the socket has opened once, so a note whose
socket cannot open shows nothing and still saves.
