# Notes: pages and note emoji

Since 2026-10-31 a note can carry an emoji, and notes can be gathered into a
**page**: one shareable page that lists many notes under a left-hand outline,
with its own title, emoji, description, colour and layout. The web page, the
desktop, iOS and Android show both. Server: `app/src/notes.py`.

## Note emoji

`emoji` is part of the note's sealed data next to the title (it is note
content, so it is only stored encrypted). `POST /api/notes` and
`PATCH /api/notes/<id>` accept `emoji`; `""` clears it. Every note payload,
the listing included, answers `emoji` (`""` when there is none). Only the
owner and editors can change it.

An emoji is 1–16 code points and at most 64 UTF-8 bytes. Every character
must be a symbol, a combining mark or a format character (zero-width joiners,
variation selectors), so no letters, whitespace or punctuation; digits, `#`
and `*` are allowed only in keycaps (the text holds U+20E3, with or without
U+FE0F before it). Anything else is `invalid_emoji`. Any
emoji a keyboard can type works, including skin tones, flags and ZWJ
sequences.

Every client shows the emoji before the title: in the note list, the editor
header, the page outline and anywhere else a note title is shown. Picking one
uses the same picker everywhere: a grid of common emoji, a field to type or
paste any emoji, and **Remove**. The desktop stores the emoji of local notes in
`notes.json` as well, and syncs it for Local + cloud notes.

## Pages

A page belongs to its owner and is shared like a note (users and
organizations, `viewer` or `editor`; organization shares reach insiders only).
`note_pages` and `note_page_shares` (migration `0297_note_pages.sql`) hold
them; the page's content is sealed in `data`:

| Field | Meaning |
| --- | --- |
| `title` | up to 200 characters, "Untitled page" when empty |
| `emoji` | same rules as a note's |
| `description` | up to 2000 characters of Markdown, shown above the content |
| `accent` | `blue`, `purple`, `pink`, `red`, `orange`, `yellow`, `teal` or `gray` |
| `layout` | `outline` (the outline on the left, one note at a time on the right) or `continuous` (every note one after another, with the outline jumping between them) |
| `showEmoji` | show the notes' emoji in the outline (default true) |
| `outline` | up to 300 entries, each `{id, kind, depth, label, noteId, url, collapsed}` |

An outline entry's `kind` is:

- `note`: a note by `noteId`. `label` overrides the note's title when not empty.
- `section`: a heading (`label`), which groups the entries under it and can be `collapsed`.
- `link`: an outside link (`label` and an `https://` `url`).

`depth` is 0–3 (indentation); `id` is a client-made 8–32 character id
(letters, digits, `-`, `_`) so entries can be reordered and edited.

### The API

Pages live under `/api/notes/pages` so the desktop's signed requests (scoped to
`/api/notes`) reach them.

- `GET /api/notes/pages` lists the pages you own or that are shared with you:
  `{id, title, emoji, description, accent, layout, role, version, createdAt,
  updatedAt, shares, entries}`, with `entries` the number of outline entries.
- `POST /api/notes/pages` with any of the fields above creates one (201).
- `GET /api/notes/pages/<id>` answers `page` (every field, `outline` included,
  plus `role` and `shares`) and `notes`, a map from each outlined note id to
  `{id, title, emoji, updatedAt, lastEditedBy, unread, role}` for the notes you
  can open. A note you cannot open is left out, and clients show its entry as
  locked ("You don't have access to this note"). With `?content=1`, each note
  also carries `markdown`, up to 4 MB in total; notes past that limit come
  without it and the client fetches them one by one.
- `PATCH /api/notes/pages/<id>` with `baseVersion` and any fields (owner or
  editor). A stale `baseVersion` is `version_conflict` (409) with the latest
  page. A `note` entry may only add a note the editor can open
  (`note_not_found`), but entries already on the page stay even if the editor
  cannot open them.
- `DELETE /api/notes/pages/<id>` (owner only) removes the page, never the notes.
- `GET`, `POST` and `DELETE /api/notes/pages/<id>/shares` (owner only) work like
  a note's shares. `POST` takes `includeNotes: true` to also share every note on
  the page that you own with the same person or organization. It never
  lowers an existing share: an editor stays an editor.

Errors: `page_not_found`, `invalid_page` (a field fails its rule),
`too_many_entries`, `invalid_emoji`.

### What every client shows

- A **Pages** group above the notes list, each row with the page's emoji,
  title, colour and number of entries; **New page** creates one.
- Opening a page shows its title, emoji and description in the page's colour,
  the outline on the left (sections, indented notes with their emoji, links,
  locked entries), and on the right either the picked note (outline layout) or
  every note in order (continuous layout). Unread notes get the usual dot.
  On phones the outline is a list you tap through, or a sheet.
- **Open in editor** on a note in a page opens it in the normal note editor.
- Owners and editors get **Customize**: title, emoji, description, colour,
  layout, show emoji, and an outline editor to add notes (search your notes),
  sections and links, rename them, indent and outdent, move them up and down
  (drag where the platform supports it), and remove them.
- **Share** on a page works like a note's, with "Also share the notes on this
  page" ticked by default.
- `/dashboard/notes?page=<id>` (and `&note=<noteId>` to pick a note) opens the
  page on every platform.

### On the web dashboard

`app/public/dashboard/js/09-notes.js` and the Notes view partial. The page
view replaces the note editor while a page is open; **Open in editor** leaves
the page and selects the note. A section folds the entries after it that are
indented deeper than it, up to the next entry at its own depth or shallower.
Picking a note in the outline layout records a `view` on that note, as opening
it in the editor does, so its unread dot clears. The emoji field checks the
rules above before sending, and also refuses ASCII punctuation and characters
outside the symbol, mark and format categories, as the server does; a keycap
may carry U+FE0F before U+20E3. **New page** and **Save** are neutral buttons,
leaving the green one for **New note**. `app/browser_tests/tests/dashboard-notes-pages.spec.js`
covers the picker, the outline, both layouts, customizing, a version conflict,
sharing with `includeNotes`, the `?page=&note=` link and the phone outline.

## Tests

`app/tests/test_notes_pages.py` covers note emoji rules, page create, read,
edit, conflicts, access (owner, editor, viewer, organization insiders,
outsiders), locked entries, adding only notes you can open, content limits,
shares with `includeNotes`, and removal.
