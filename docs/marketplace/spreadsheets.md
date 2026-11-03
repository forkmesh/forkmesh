# Spreadsheets

Part of [Marketplace](README.md). Package: `marketplace/packages/spreadsheets`
([README](../../marketplace/packages/spreadsheets/README.md)). An account
installs it for itself; it works on the web dashboard (`/dashboard/spreadsheets`)
and on the desktop, where the rail button **Spreadsheets** opens the same editor
inside the window ([On the desktop](#on-the-desktop)).

## Why it is built this way

- **One engine, in the browser.** The formula engine, number formats, file
  readers and writers and the editor are one script
  (`content/web/page.js`). Files are opened and written on the user's device,
  so an `.xls` someone uploads never reaches the server as a file and no
  server-side parser handles untrusted Office documents. The engine half
  exports itself to Node when there is no dashboard, which is how
  `tests/engine_checks.js` runs it.
- **No third-party spreadsheet library.** `.xls` (BIFF8 inside a compound
  file), `.xlsx`, `.ods`, Excel XML 2003, HTML tables and CSV are read with
  our own code, and ZIP uses the browser's `CompressionStream`. The dashboard
  stages a single package script, and a vendored library would be a large,
  unaudited dependency in the page that holds people's data
  ([security](../protocol/threat-model.md)).
- **The server stores sealed blobs.** A workbook is saved as gzip-compressed
  JSON, base64, sealed under a key minted for the account (the same pattern
  as Water and Booking, [encryption](../encryption.md)). The title is sealed
  separately so the list does not open every workbook. The server checks only
  shape and size: 3 MB of packed data per workbook (roughly 20–40 MB of cells
  before compression), 500 workbooks and 200 MB per account.
- **Saves cannot overwrite another device.** Every save sends the revision it
  started from; a stale one gets `409 workbook_changed` and the page offers
  "Load their version" or "Keep mine" (`force`).
- **AI edits are operations, not code.** `POST /api/spreadsheets/ai` sends the
  request and a compact text view of the active sheet (at most 12,000
  characters, selection first) to the host model through `ai_json`, which
  owns the model list and the per-account rate limit
  ([package system](package-system.md#server-module-runtime)). The model
  answers with a reply and a list of operations from a fixed set (write,
  format, insert or delete rows and columns, sort, conditional formatting,
  chart, freeze, validation, notes, sheets). The server drops unknown
  operations and fields and caps the cells written; the page applies the rest
  as one undoable step. Nothing the model returns is run as code.

## What the editor does

- Several sheets per workbook (add, rename, duplicate, colour, hide, reorder),
  up to Excel's 1,048,576 rows and 16,384 columns; the grid is drawn on a
  canvas, so large sheets scroll smoothly.
- 235 functions, including dynamic arrays that spill (`FILTER`, `SORT`,
  `UNIQUE`, `SEQUENCE`), `XLOOKUP`, `LET`, `SUMIFS`, financial and statistics
  functions, cross-sheet references, named ranges, circular-reference and
  `#SPILL!` errors. A formula with a function the engine does not know keeps
  the value the file was saved with.
- Formatting: fonts, sizes, bold, italic, underline, strikethrough, text and
  fill colour, borders, alignment, wrap, rotation, indent, merged cells, every
  Excel number format code (sections, colours, conditions, dates, durations,
  fractions), format painter, alternating colours.
- Editing: formula bar and name box, function suggestions and hints,
  click-to-reference while typing a formula, F4 for `$`, fill handle with
  series (numbers, dates, months, weekdays, "Item 1"), copy and paste with
  Excel and Google Sheets (HTML and tab-separated), paste special, find and
  replace, undo and redo for every change, the usual keyboard shortcuts.
- Data: sort, filter views per column, data validation (dropdown lists from
  items or a range, checkboxes, number, date and length rules), conditional
  formatting (comparisons, text, blanks, duplicates, custom formulas, colour
  scales), remove duplicates, trim whitespace, split text to columns, pivot
  tables (written to a new sheet), charts (column, bar, line, area, pie,
  scatter), notes and links, frozen rows and columns, hidden rows and columns.

## Files

| Format | Open | Download |
| --- | --- | --- |
| Excel `.xlsx`, `.xlsm` | values, formulas (shared and array), styles, theme colours, merges, widths, heights, frozen panes, tab colours, hidden sheets, conditional formatting, data validation, links, comments, defined names | the same, plus notes as comments |
| Excel 97–2003 `.xls` (BIFF8) | values, formulas decompiled from tokens (3-D references, add-in functions such as `IFERROR`, shared and array formulas), fonts, number formats, fills, borders, alignment, merges, widths, heights, frozen panes, tab colours, defined names; password-protected files are refused with a message | values, formulas the format can hold, styles, merges, sizes, frozen panes; cells past 65,536 rows or 256 columns are left out |
| OpenDocument `.ods` | values, formulas, styles, number formats, merges, sizes, notes, links | the same |
| Excel XML 2003, HTML tables | values, R1C1 formulas, styles, merges | HTML (every visible sheet) |
| CSV, TSV | delimiter detected; numbers, dates and percentages recognised | the active sheet, shown values |
| JSON, Markdown, PDF | | JSON of every sheet; a Markdown table; PDF through the browser's print dialog |

Charts are not written into Excel or OpenDocument files, and `.xls` notes,
conditional formatting and validation are not read.

## Linked CRM lists

A workbook can be linked to one [CRM](../../marketplace/packages/crm/README.md)
list, so the list can be edited as a sheet. **Open in spreadsheet** on a CRM
list (its tab menu, or **More > Open list in spreadsheet**) goes to
`/dashboard/spreadsheets?crmOrg=<organization>&crmList=<list id>`, where the
list id is a list's 32-character id, `pipeline:<flow>` for a pipeline tab that
has no list of its own, or `no-pipeline`. The page opens the workbook already
linked to that list, or creates one, and pulls the list again. If Spreadsheets
is not installed it asks for the install first and then carries on.

- **Where the link lives.** The workbook's sealed data holds the link: the
  organization, the list, its columns (field and header label), its stages and
  members for the dropdowns, the snapshot of every contact as last pulled or
  synced, the ids of rows being created, and when it last pulled and synced
  (`wb.crm`, cleaned by `crmCleanLink`). The sealed title record also carries
  `{kind, org, list}` so the workbook list can find the linked workbook without
  opening every workbook; it is sealed like the title, and a copy of a
  workbook is never linked.
- **The sheet.** Row 1 holds the headers, frozen; hidden column A, **CRM ID**,
  ties each row to its contact. Then name and email, the list's shown columns,
  every other field the CRM lets a member edit (company, title, phone, the
  social handles, website, location, stage, deal value, next step, owner,
  source, tags, notes, last contacted) and the list's custom fields. Columns
  are found by their header text, so they can be moved, and columns of your own
  (formulas, notes) are left alone. Stage and owner get dropdowns.
- **Sync to CRM** reads the rows (`crmReadRows`), compares each with the
  snapshot (`crmDiff`) and sends only the changed cells, each with the value it
  had in the snapshot, to `POST /api/crm/<org>/contacts/sync` in batches of at
  most 500 rows and 1.5 MB, so a 1,000-row list is two requests. A row without
  an id becomes a new contact on the list; it gets its id written into column A
  and saved before it is sent, so a retry after a failure replays the same
  contact instead of adding a second one. Rows are held back, with a reason,
  when they repeat another row's id, carry an id that is not on the list, or
  changed both name and email (a sign the id belongs to another row, for
  example after sorting only part of the sheet).
- **Conflicts.** The worker refuses a row whose CRM value no longer matches the
  value the sheet started from, and answers with the contact as it is now
  (`crmApply`, `crmThreeWay`). Cells the CRM changed and you did not are pulled
  in; cells both sides changed to different values open a dialog showing both,
  per cell **Keep mine** or **Use the CRM's**, or all at once. Keeping yours
  re-bases on the CRM's value and syncs again; cells left unpicked stay as
  they are and are asked about again. Nothing in the CRM is overwritten
  without that choice.
- **Refresh from CRM** pulls the list again (`crmMerge`): CRM changes to
  cells you have not touched come in, contacts new on the list are added at the
  bottom, rows whose contact left the list are removed unless you edited them,
  and conflicting cells open the same dialog. Columns the list gained are
  added at the right.
- **Deleting a row never deletes a contact.** The CRM's contact delete cannot
  be undone, so the sheet does not use it; a deleted row's contact comes back
  on the next refresh. Delete contacts in the CRM.
- **The bar** above the menus shows the list, when it last pulled and synced,
  how many rows are not synced, rows that need attention (click for the list,
  each row opens) and the last error. Its menu opens the list in the CRM or
  unlinks the workbook.
- **Access** is the CRM's own: the requests carry the dashboard session, so a
  member sees only lists they can open, and a read-only role can refresh but
  not sync.

The diff and conflict rules are pure functions in `FMSheets.crm` and are
checked by `tests/crm_link_checks.js`; the worker side is
`marketplace/packages/crm/tests/test_spreadsheet_sync.py`.

## On the desktop

The rail button opens a desktop page (`SpreadsheetsPage`) that runs the web
editor in an embedded [Qt WebEngine view](../desktop/browser.md), signed in
as the desktop's account. It used to open the dashboard in the system
browser; people wanted the editor to stay in the app.

- **One editor.** The page loads `/dashboard/spreadsheets` from the active
  host, so the desktop gets every feature and fix of the web editor without a
  second implementation. A document script hides the dashboard's header and
  sidebar (`[data-app-header]`, `[data-dashboard-sidebar]`), so only the
  editor shows.
- **It stays.** The view is built the first time the page is shown and kept
  while you switch to other pages, so an open workbook, its selection and
  undo history are still there when you come back. It can also be popped out
  into its own window like other package pages.
- **Signed in as the desktop.** The page gets the desktop's account session
  through `accountSessionToken`, which is why the contribution declares the
  `/api/spreadsheets` boundary. The token goes into a memory-only profile as
  the `forkmesh_account` cookie and the `forkmesh.session` marker
  ([sessions](../app/auth-sessions.md)), both limited to the host's origin;
  nothing is written to disk. When the account, the token or the host
  changes, the view and its profile are thrown away and built again on the
  next show; signed out, the page says so.
- **Only the editor loads in the view.** Main-frame navigation is allowed
  only to `/dashboard/spreadsheets` on the host (`SpreadsheetsWeb.cpp`,
  `navigation`). Clicked links and `window.open` go to the system browser;
  other dashboard pages go through `openWebPage`; a redirect away (sign-out,
  `/login`) shows a message with **Reload**. `file:` URLs are refused.
- **Desktop file handling.** Downloads (`.xlsx`, `.xls`, `.ods`, CSV, JSON,
  Markdown, HTML) ask where to save with the system dialog, starting in the
  last folder used (package setting `saveFolder`); PDF export (the page's
  print) saves a PDF the same way. Opening a file uses the system file
  chooser. Copy and paste reach the system clipboard, including the menu's
  Paste.
- **Other packages open it here.** `openWebPage` first offers a path to the
  installed package pages (`webPage.open` host command); Spreadsheets takes
  `/dashboard/spreadsheets` paths, so the CRM's **Open in spreadsheet** opens
  the linked list in the desktop page.
- **Without Qt WebEngine** (a build without it) the page explains that and
  offers **Open in your browser**; `webPage.open` then declines, so links fall
  back to the browser.

Unsaved edits are saved by the editor 1.5 seconds after the last change;
quitting the desktop within that time can lose the last edit.

## Code map

| Path | What |
| --- | --- |
| `content/web/page.js` | Engine (`FMSheets`: formats, parser, `Calc`, functions, model, readers, writers, AI helpers, `crm` link rules) and the editor page |
| `content/web/view.html` | Page markup and styles, using the [dashboard theme roles](../app/dashboard-colors.md) |
| `content/worker/spreadsheets.py` | `/api/spreadsheets`: install, list, create, open, save, delete, export, uninstall, AI |
| `content/desktop/SpreadsheetsPage.*` | Desktop page: embedded editor, session, downloads, PDF, links |
| `content/desktop/SpreadsheetsWeb.*` | Navigation rules and the session and theme scripts for the desktop page |
| `content/desktop/tests/test_spreadsheets_desktop.cpp` | `forkmesh-spreadsheets-tests`: navigation and script checks |
| `tests/engine_checks.js`, `tests/fixtures/` | Formula, format and round-trip checks, including `.xls` and `.xlsx` files written by LibreOffice |
| `tests/crm_link_checks.js` | Linked CRM list checks: reading rows, diff, conflicts, refresh, sync results |
| `tests/test_spreadsheets.py` | Server tests; runs the engine and CRM link checks with Node |

## Not yet

A linked workbook syncs when you press **Sync to CRM**, not on every save,
and does not follow the CRM live; removing contacts from a list from the sheet
is not built.

Phones: the package is offered on desktop and the web only (`install.places`),
so neither phone app shows it; adding it means one screen in each app
([phone parity](../native/ANDROID.md)). Real-time co-editing, version history,
macros and sharing with other accounts are not built.
