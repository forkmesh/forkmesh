# Notes in the phone apps

The web Notes page is the reference (`docs/app/notes-sharing.md`). Each phone
app has its own section below.

## Android

Code: `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/notes/`
(`NotesLogic.kt`, `NotePresence.kt`, `NoteComments.kt`, `NoteRows.kt`, tested
by `NotesLogicTest.kt`, `NoteCommentsTest.kt` and `NoteRowsTest.kt`) holds
everything that is not a view; `feature/tools/NotesScreens.kt` in the app module holds the list, the
editor and its sheets, and `feature/tools/NoteCommentsViews.kt` holds the
comments sheet, the faces and the "Here now" list.

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
- **Rows** follow "Unread notes and the last edit" in
  `docs/app/notes-activity.md`; `NoteRows.info` reads `unread`,
  `lastEditedBy`, `myLastViewedAt`, `unreadComments`, `openComments` and the
  shares from each note. A row shows:
  - an accent dot before the title, which is bold, when the note is `unread`
    (TalkBack reads "Unread");
  - a **Private** / **Shared** / **Public** badge (`NoteRows.reach`: Public
    when published, Shared when it has a share, else Private); Public is
    accent blue, Shared and Private muted grey, as on the web;
  - the last edit (`NoteTimes.lastEdit`): "Today 3:14 PM", "Yesterday
    9:02 AM", "Oct 29, 3:14 PM" this year or "Oct 29, 2025" before, in the
    phone's time zone, with US month names and a 12-hour clock as on iOS,
    then "· by <name>" when `lastEditedBy` is not you (`NoteRows.lastEdit`);
  - a speech bubble with the `openComments` count when it is above 0;
  - on the right, initials of up to three people or organizations the note
    is shared with (organizations in a rounded square), then "+N".
  The row no longer repeats "Can edit" / "Read only"; the long-press sheet
  and the editor's meta line show the role.
- **Long press** on a row (with the list's long-press haptic, or the TalkBack
  custom action "Note details and actions") opens a sheet. Its header, built
  by `NoteAccess.summary`, shows the title; your role as "You own this note",
  "You can edit" or "View only"; the badge; "Shared with" and one line per
  share ("bo · Can edit", "crew (organization) · Can view") or "Not shared";
  and "Last edited Saturday, October 31, 2026 at 3:14 PM by bo" (`by you` for
  your own edit, `NoteTimes.full`). Its actions are **Open**, **Copy link**
  (the editor's `/dashboard/notes?note=<id>` link on the relay origin), and
  for the owner **Share…** (the editor's share sheet; the list reloads when it
  closes) and **Delete** (the editor's confirm, then the row goes and
  "Deleted “<title>”" shows under the filters).
- **Opening a note records a view.** The editor sends
  `POST /api/notes/<id>/activity` with `{"kind": "view"}` once a note it was
  opened on has loaded (not for a note it created), and ignores a failure.
  The list clears the dot at once: a tap, and the editor whenever the note
  loads or changes, store the note's `updatedAt` in `noteViewsSeen`, and
  `NoteRows.applySeen` treats a row as read while its `updatedAt` is no newer
  than that. A list reload that answers before the view lands therefore
  stays clear, and an edit after the view still shows the dot. The server's
  timestamp is used rather than the phone's clock so a skewed clock cannot
  keep or hide the dot.

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
`/dashboard/notes?note=<id>` to the Note route (`NavigationTest`). A
`comment=<commentId>` query (the link in a mention ping) is kept on the route
as `Route.Note.comment` and survives the saved navigation state
(`RouteCodec`); the editor then opens the comments sheet on that thread (a
reply's id opens its thread), unfolds "Resolved" if needed and selects the
passage. A deleted comment shows "That comment was deleted."

### Presence

The editor joins `GET /api/notes/<id>/ws` with the core `WebSocketClient`,
sending the session as an `Authorization: Bearer` header (the browser uses
`?sessionToken=` only because it cannot set headers). The contract is
`docs/app/notes-comments.md`, "Who is here".

- **State.** Every presence frame carries `state`: `editing` while the person
  has typed in the title or body in the last 10 s (`NotePresence.TYPING_MS`,
  `localState`), else `viewing`; viewers always send `viewing`. A frame goes
  out at once when the state changes (`stateChangeFrame`, which remembers the
  last state sent), another 10 s after the last keystroke, and every 15 s
  anyway. Drafts still go out 180 ms after typing.
- **Peers.** `NotePresence.receive` keeps each peer's `state`; a `cursor` or
  `draft` frame without one keeps the last state, and a draft also counts as
  editing for 5 s, so older clients still show. A `leave` frame removes the
  peer at once; otherwise a peer is dropped after 37.5 s of silence. Editors
  sort first.
- **What shows.** Up to four faces then "+N" (`NotePresence.faces`), a pencil
  badge on those editing, each face labelled "<name> · viewing" or
  "<name> · editing" for TalkBack, and a line: "ana is editing…",
  "Live · ana is viewing", "Live · 3 others here", "Live" or
  "Offline — edits still save". Tapping the faces opens **Here now**, everyone
  with Viewing or Editing; tapping a face there opens the profile.
- A collaborator's draft is applied only when there are no unsaved local edits
  and the draft is based on the version on screen, and `note-updated` reloads
  under the same condition. After `access-changed` it reloads and reconnects
  at once; after any other drop it waits 15 s. When the socket cannot open,
  the line stays hidden: saving does not depend on it.

### Comments

The API, the anchoring rule and what every client shows are in
`docs/app/notes-comments.md`. `NotesApi.comments`, `addComment`,
`editComment`, `resolveComment` and `deleteComment` call
`/api/notes/<id>/comments[/<commentId>]`; each answer is parsed whole by
`NoteCommentsPayload` (`comments`, `people`, `canResolve`), so the list is
replaced after every change.

- **Button.** A speech-bubble button in the editor bar, on any saved note,
  shows the number of open threads (`NoteThreads.openCount`) as a badge and
  opens the comments sheet.
- **Commenting on a selection.** Selecting text in Write mode enables
  **Comment** beside Write / Preview (viewers too: their field is read-only
  but selectable). `NoteAnchoring.quote` takes the selection from the
  `TextFieldValue` as UTF-16 offsets (Kotlin `String` indices) with up to 32
  characters before and after, and caps the quote at 1000; a blank selection
  does not count. The same action is a **Comment** item in the selection's
  floating toolbar (`Modifier.appendTextContextMenuComponents`, which needs
  Compose's new text context menu, on by default since foundation 1.12).
  Either opens the sheet with a composer showing the quote. The add-comment
  button in the sheet's header writes a whole-note comment.
- **Order.** `NoteThreads.build` groups replies under their thread (oldest
  first; a reply whose thread is gone is dropped), finds each quote with
  `NoteAnchoring.locate` (every occurrence, +2 for the prefix, +2 for the
  suffix, ties to the one nearest `start`), and lists open threads with
  whole-note threads first, then by position in the note, then detached ones;
  resolved threads use the same order, folded under "Resolved (n)".
- **A thread** shows its quote with an accent bar (struck through and
  labelled "Removed from the note" when detached), author, time ("edited"
  after a body edit), the body with kept `mentions` in accent bold
  (`NoteMentions.ranges`), its replies, a reply box, **Resolve** / **Reopen**
  when `canResolve` is true or you wrote the thread, **Show in note** when the
  passage is found, Edit on your own comments, and Delete on your own
  comments; the note's owner (`role == owner`) gets Delete on everyone's, to
  moderate (`NoteComment.canEdit`, `canDelete`). Deleting asks first; the
  prompt names whose comment it is when it is not yours and, for a thread,
  how many replies go with it ("bo's thread and its 2 replies will be removed
  for everyone.", `NoteCommentText.deletePrompt`). Your name is the session's
  `nodeName`.
- **Picking a thread** (its quote or Show in note) closes the sheet, switches
  to Write, selects the passage and focuses the editor. The passage is also
  shaded through a `VisualTransformation` until you type. The shading follows
  the thread, so it moves with collaborators' edits.
- **Mentions.** Typing `@` in any comment box offers up to six names from
  `people` (names starting with the text first, then names containing it,
  never you) as chips; picking one replaces the `@text` with `@name `
  (`NoteMentions.active`, `suggestions`, `insert`).
- **Live.** A `note-comments` frame bumps a counter and the sheet reloads
  300 ms later (`NoteThreads.RELOAD_MS`); frames in that window restart the
  wait. Reconnecting the socket reloads too, in case a frame was missed.
- Errors show in the sheet in plain words (`NoteCommentText.error`).

### Pages and note emoji

The contract is `docs/app/notes-pages.md`. Core: `NotePages.kt`
(`NoteEmoji`, `NotePage`, `PageEntry`, `PageNote`, `NotePageDetail`,
`PageOutline`, `NotePagesApi`); views: `feature/tools/NotePagesScreens.kt`.

- **Emoji.** `NoteEmoji.isValid` applies the server's rule on the device
  (1–16 code points, at most 64 UTF-8 bytes, no whitespace, control
  characters or ASCII letters, digits, `#` and `*` only as keycaps, with or
  without U+FE0F before U+20E3). The emoji shows before the title in list
  rows, the long-press sheet, the editor header (a button left of the title;
  viewers see it without the button), the page outline and the page's note
  view. The picker is a grid of 40 common emoji (`NoteEmoji.common`), a field
  for any emoji with the error "Use a single emoji, such as 📝 or 🇫🇷.", and
  **Remove**. A saved note changes it with `PATCH {baseVersion, emoji}`; a new
  note sends it with the first save.
- **Pages section.** Above the notes list (hidden while searching, and only
  when `GET /api/notes/pages` answers): each page with its emoji tile in the
  page colour, its title in that colour and "n entries"; **New page** creates
  one and opens it with Customize.
- **Page screen** (`NotePageScreen`): the emoji, title and Markdown
  description in the page colour, then the outline as a list: sections (tap
  to fold when they group entries), notes indented by depth with their emoji
  (when `showEmoji`) and the unread dot, links (open in the app's browser) and
  locked entries ("You don't have access to this note"). In the outline
  layout tapping a note shows it with **Open in editor**, the last edit, and
  previous / next buttons; Back returns to the outline. The continuous layout
  shows every readable note in order under the outline, and tapping an
  outline row scrolls to it. Showing or jumping to a note records a view and
  clears its dot. A note without `markdown` (past the 4 MB budget) is fetched
  on its own. Page colours: blue is the theme accent; the others are the iOS
  system colours, darkened on light backgrounds so text stays readable.
- **Customize** (owners and editors): title, emoji, description, colour,
  layout, "Show note emoji in the outline", and the outline editor: add
  notes (searching your notes by title), sections and `https://` links;
  each row has a drag handle (long press and drag to reorder) and a menu with
  Rename, Indent, Outdent, Move up, Move down and Remove, also offered as
  TalkBack actions. Save sends every field with `baseVersion`; a conflict
  offers **Reload**.
- **Share** (owner) reuses the note share sheet (`NoteShareTarget`) with
  "Also share the notes on this page" ticked by default (`includeNotes`).
  The ⋯ menu has **Copy link** and, for the owner, **Delete page** (the notes
  stay).
- **Links.** `/dashboard/notes?page=<id>` opens `Route.NotePage`, and
  `&note=<id>` picks that note (`NavigationTest`).

### Tests

`NotesLogicTest` covers the list, the editor helpers and the existing frames.
`NoteCommentsTest` covers parsing the comment payload, quote context and
UTF-16 offsets, the anchor scoring and detached threads, thread order and
the resolved fold, resolve permission, owner deletion and the delete
prompt, mention ranges, suggestions and insertion, the request paths and
bodies, presence `state` frames, `leave`, `note-comments`, the 10 s typing
window and the faces. `NoteRowsTest` covers the last-edit formats against
a fixed clock in `America/New_York` (today, yesterday across a year end, this
year, older, and the same instant in another zone), "· by <name>", the row
fields, badges and "+N", clearing the dot locally, the view request, and the
long-press header (roles, share lines, "Not shared", the full date "by").
`NotePagesTest` covers the emoji rules and the common set, parsing pages and
the page detail, outline rows with folding and locked notes, reading order,
outline editing, the saved fields and links, and every page request.
`NavigationTest` covers `?note=<id>&comment=<cid>`, `?page=<id>[&note=<id>]`
and the codec round trip.

## iOS

Code: FMTools (`native/ios/ForkMeshCore/Sources/FMTools/NotesModels.swift`:
`NotesListModel`, `NoteEditorModel`, `NoteHistoryModel`, `NoteShareModel`;
`NotesListDetails.swift`: row details and the access header;
`NotePresence.swift`: the live room; `NoteComments.swift`: comments) holds
the state and is tested on Linux (see Tests below).
`native/ios/ForkMesh/Features/Tools/NotesScreen.swift` holds the list, the
editor and its sheets, and `NoteCommentsViews.swift` the text view, the
presence line and the comments sheet. Every request goes through the `ToolsApiCall` closure over
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
- **Rows** follow "Unread notes and the last edit" in
  `docs/app/notes-activity.md` and look like Android's (`NoteListRow`, built
  from `NotesListModel.row`):
  - an accent dot on the note icon and a bold title when the note is unread
    (VoiceOver reads "Unread");
  - a **Private** / **Shared** / **Public** badge (`NoteReachBadge`: Public
    when published, Shared when it has a share, else Private); Public is
    accent blue, Shared and Private muted grey;
  - the last edit (`NoteEditTime.label`): "Today 3:14 PM", "Yesterday
    9:02 AM", "Oct 29, 3:14 PM" this year or "Oct 29, 2025" before, with US
    month names (`en_US_POSIX`) and a 12-hour clock in the phone's time zone,
    then "· by <name>" when `lastEditedBy` is not you (`NoteEditTime.line`;
    "you" is the signed-in account, `AuthService.session.nodeName`);
  - a speech bubble with `openComments` when it is above 0;
  - on the right, initials of up to three people or organizations the note
    is shared with (`NoteAvatar.of`; organizations in a rounded square),
    then "+N".
  The row no longer shows "Can edit" / "Read only"; the long-press header and
  the editor's meta line still give the role.
- **Long press** opens a context menu whose preview is the title and the
  access header (`NoteAccessHeader.lines`, the desktop's wording): "You own
  this note" / "You can edit" / "View only"; "Shared with bob (can edit), acme
  (organization, can view)" or "Not shared"; Private / Shared / Public; and
  "Last edited Monday, September 14, 2026, 3:14 PM by bob". Its actions are
  **Open**, **Copy link** (`/dashboard/notes?note=<id>` through
  `ApiService.webLink`, the link the editor copies) and, for the owner,
  **Share…** (the editor's share sheet; the list reloads when it closes) and
  **Delete** (destructive, behind the editor's delete confirmation). VoiceOver
  gets Copy link, Share… and Delete as accessibility actions, plus "Access
  details", which shows the header as a notice; activating the row opens it.
- **Unread is cleared at once.** Opening a note records a view: after the
  editor's first successful load, `NoteEditorModel.recordView` sends
  `POST /api/notes/<id>/activity` with `{"kind": "view"}` once and ignores a
  failure (not for a note the editor created). As on Android, the dot clears
  locally through the note's server `updatedAt`: a tap in the list and every
  note the editor loads or saves record it in `NoteViewsSeen` (one per app
  run), and a row is unread only while the server says so and its `updatedAt`
  is newer than what was recorded. A list reload that answers before the view
  lands stays clear, and a later edit shows the dot again.

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
- **Write / Preview.** A segment switch. Preview uses the Docs reader's parser
  (`DocsMarkdown.blocks`, task boxes included) with SwiftUI inline markdown and
  the Docs code block view. Viewers open on Preview and get **Markdown /
  Preview** (`NoteMode.label(canEdit:)`), where Markdown is the read-only,
  selectable text, so they can quote a passage in a comment.
- **The text view** is a `UITextView` (`NoteTextView`), not SwiftUI's
  `TextEditor`. The app targets iOS 17, and `TextEditor` cannot report its
  selection before iOS 18 (and even then cannot draw a highlight), so the
  iOS 18 selection API with a fallback was not enough. The `UITextView`
  reports `selectedRange` in UTF-16 units (the offsets the comments contract
  uses) on every change (`NoteEditorModel.selectionChanged`), adds **Comment**
  to the text selection menu (`editMenuForTextIn`), draws the picked thread's
  passage with an accent background, and moves the selection when the model
  asks (`selectionRequest`). The format bar (heading, bold, italic, list,
  task, link, code) now applies to the selection, and still to the end of the
  text before anything was selected.
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
to `.note(id:)`, and `&comment=<commentId>` (the link in a mention ping) to
`.note(id:comment:)` (`RouteParserTests`). The comment id must be a safe path
segment, otherwise it is ignored. The editor then opens with the comments
sheet showing that thread (a reply's id shows its thread), scrolled to it and
outlined, with its passage highlighted in the text.

### Presence

`ApiService.noteSocket(_:connector:)` opens `GET /api/notes/<id>/ws` with the
app's `WebSocketConnector`. It sends the session as an
`Authorization: Bearer` header, as Android does, and uses `noteSocketURL` for
the address; phones use the socket, not the HTTP presence route
(`docs/app/notes-comments.md`, "Who is here"). FMTools' `NotePresence`:

- Every presence frame carries `state`: `editing` while the person typed in
  the last 10 s (`NotePresence.typingMillis`), else `viewing`. `NoteRoomModel`
  sends one when the socket opens, every 15 s, and at once when the state
  changes: the first keystroke (`typed()`, called from `draftChanged`) sends
  `editing`, and a timer 10 s after the last keystroke sends `viewing`.
  `sentState` keeps a frame from going out twice for the same state.
- A peer's `state` is taken from its frames. A frame without one counts as
  `viewing` for presence and keeps the last state for cursor and draft
  frames, and a draft still makes a peer "editing" for 5 s, so older clients
  show as before.
- A `leave` frame drops that peer at once; otherwise peers are forgotten
  after 37.5 s. Each newcomer still gets a presence reply.
- `note-comments` frames become `NoteRoomEvent.commentsChanged`.

`NoteRoomModel` shows up to four faces (initials) and then "+N"
(`faces`, `overflow`), a pencil badge and an accent ring on anyone editing,
and the label: "ana is editing…", "Live · ana is viewing",
"Live · 3 others here", "Live", or "Offline — edits still save". Each face is
read as "<name> · viewing" or "<name> · editing". Tapping the line opens a
popover listing everyone (`everyone`) with their state.
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

### Comments

The contract is `docs/app/notes-comments.md`. FMTools' `NoteComments.swift`
holds everything that is not a view:

- `NoteComment` is one comment as the API returns it; `NoteCommentsModel`
  loads `GET /api/notes/<id>/comments` and applies the `comments`, `people`
  and `canResolve` that every comments route answers, so a post, edit,
  resolve or delete needs no second request.
- **Anchors** (`NoteAnchor`): `capture` takes the selected UTF-16 range, the
  quote (at most 1000 units, never splitting a surrogate pair, not blank) and
  up to 32 units of context on each side; `locate` finds every occurrence of
  the quote in the current text, scores 2 for a matching prefix and 2 for a
  matching suffix, and keeps the best, breaking ties by the distance from
  `start`. No occurrence means the thread is detached.
- **Threads** (`NoteThreadList.build`): replies grouped under their thread by
  `parentId`, oldest first; open threads in document order, whole-note
  threads first and detached ones last; resolved threads apart, in the same
  order, folded under "Resolved (n)".
- **Mentions** (`NoteMentions`): the server's `@name` rule (a letter, then
  letters, digits and hyphens, not after a word character or `@`); a body is
  split so only names in the comment's `mentions` are highlighted. Typing `@`
  at the end of a comment box suggests up to five names from `people`
  (prefix matches first, never yourself), and picking one completes it.
- Permissions: Edit on your own comments; Delete on your own, or on anyone's
  when you own the note; Resolve / Reopen when `canResolve` or you wrote the
  thread. Deleting asks first, and for a thread with replies says how many
  ("This removes the comment and its 3 replies for everyone.").
- Comment events from the room reload 300 ms after the last one
  (`reloadSoon`).

On screen:

- A **Comments** button in the editor's toolbar (speech bubble) shows the
  number of open threads; it appears once the note exists.
- Selecting text and choosing **Comment** in the selection menu, or the
  **Comment** button in the format bar, opens the comments sheet with a
  composer showing the quote; in Preview, or with nothing selected, the
  comment is about the whole note. The sheet's **Comment** button also
  starts a whole-note comment. Viewers can comment.
- The sheet lists the threads as above. Each shows its quote (accent bar;
  struck through and greyed with "This text is no longer in the note." when
  detached; "On the whole note" when there is none), author, time ("·
  edited" after an edit), the body with mentions in bold accent, replies, a
  reply box, Resolve or Reopen, and a ⋯ menu with Edit and Delete.
- Tapping a quote closes the sheet, switches to the text, selects the
  passage and draws it with an accent background; tapping elsewhere in the
  text removes the highlight.
- Comment boxes offer `@` suggestions as chips under the field.

### Tests

`Tests/FMToolsTests/`: `NotesTasksTests.swift` and `NotesParityTests.swift`
(list, editor, history, sharing), `NotePresenceTests.swift` (frames, states,
`leave`, four faces and "+N", the editing and viewing frames the room
sends), `NoteCommentsTests.swift` (capture and locate with UTF-16 and
surrogate pairs, thread order, mentions and suggestions, the comments model's
requests and permissions, the delete confirmation, debounced reloads,
opening a thread from a link, picking and commenting on a selection, viewers
commenting, the room's `note-comments` frame) and `NotesListDetailsTests.swift`
(the last-edit formatter in fixed UTC, Tokyo and New York calendars, badges,
avatars, unread and the recorded view, the long-press header and actions).
`FMApiTests/RouteParserTests.swift` covers the `comment` link.
