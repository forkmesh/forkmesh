# Spreadsheets

Part of [Marketplace](README.md). Package: `marketplace/packages/spreadsheets`
([README](../../marketplace/packages/spreadsheets/README.md)). An account
installs it for itself; it works on the web dashboard (`/dashboard/spreadsheets`)
and the desktop rail button **Spreadsheets** opens that page on the active host.

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

## Code map

| Path | What |
| --- | --- |
| `content/web/page.js` | Engine (`FMSheets`: formats, parser, `Calc`, functions, model, readers, writers, AI helpers) and the editor page |
| `content/web/view.html` | Page markup and styles, using the [dashboard theme roles](../app/dashboard-colors.md) |
| `content/worker/spreadsheets.py` | `/api/spreadsheets`: install, list, create, open, save, delete, export, uninstall, AI |
| `content/desktop/SpreadsheetsLink.cpp` | Desktop rail link to `/dashboard/spreadsheets` |
| `tests/engine_checks.js`, `tests/fixtures/` | Formula, format and round-trip checks, including `.xls` and `.xlsx` files written by LibreOffice |
| `tests/test_spreadsheets.py` | Server tests; runs the engine checks with Node |

## Not yet

Phones: the package is offered on desktop and the web only (`install.places`),
so neither phone app shows it; adding it means one screen in each app
([phone parity](../native/ANDROID.md)). Real-time co-editing, version history,
macros and sharing with other accounts are not built.
