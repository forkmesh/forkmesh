# Notes: diagrams, page layout and PDF export

Since 2026-11-01 a note can be a full document: it can carry diagrams and
illustrations, a page layout (paper, orientation, margins, cover page,
contents, page numbers) and page breaks, and every client exports it as a PDF.
Everything lives in the note's Markdown, so it travels with the note through
sync, versions, sharing and Markdown export, and the server stores nothing
new. The web page, the desktop, iOS and Android follow this page; it is the
contract.

Why in the Markdown and not in note fields: a note is sealed content, and a
new field would need a migration, a new payload key on every client and an
encrypted column. Two HTML comment lines and a fenced block cost nothing on
the server, are invisible in other Markdown viewers (GitHub, editors) and
survive a Markdown export and re-import unchanged.

## Diagrams and illustrations

A fenced code block whose info string starts with the word `svg` (any case,
fence ```` ``` ```` or `~~~`) is a figure:

````markdown
```svg Figure 1. How a push reaches every mirror
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 200">…</svg>
```
````

- The rest of the info string, trimmed, is the caption, shown centred under
  the figure in small muted text. No caption is fine.
- The body is SVG markup. It is a figure only when, after leading whitespace,
  an optional `<?xml …?>` prolog and comments, it starts with `<svg`, and it
  is at most 262144 UTF-8 bytes. Otherwise, and whenever a client cannot draw
  it, the block renders as an ordinary code block, so nothing is lost.
- The SVG is always drawn as an image, never inserted as live markup: the web
  uses `<img src="data:image/svg+xml;base64,…">`, which runs no script and
  loads no external resource; the desktop draws it with `QSvgRenderer`, iOS
  and Android with their image renderers. A note shared with you cannot run
  code in your browser or app through a diagram.
- The figure is centred, at its natural size, scaled down to the column
  width. A diagram with only a `viewBox` fills the column.
- A diagram should not set its own background or rely on a dark page: the
  preview shows figures on a white card so a diagram drawn for paper reads the
  same in the dark theme and in the PDF.

The figure syntax is only for notes. READMEs, issues and pull requests keep
GitHub's meaning, where ```` ```svg ```` is code.

Ordinary images (`![alt](url)`, `data:image/png;base64,…`) work as before
and also print in the PDF.

## Page breaks

A line, outside a code block, whose text is `<!-- pagebreak -->` (any case,
spaces inside the comment optional) ends the page: in the PDF what follows
starts on a new page. In the preview it shows as a dashed rule labelled
**Page break**. Other Markdown viewers hide it, being an HTML comment.

## Page layout

A line, outside a code block, of the form

```markdown
<!-- page: size=letter; orientation=portrait; margins=normal; cover=yes; contents=yes; numbers=yes -->
```

is the note's page layout. It is never shown in the preview. Only the first
such line counts. Keys and values are lower case, separated by `;`, with
spaces allowed around `=` and `;`. A missing key, an unknown key and an
unknown value use the default:

| Key | Values | Default |
| --- | --- | --- |
| `size` | `a4` (210 × 297 mm), `letter` (8.5 × 11 in), `legal` (8.5 × 14 in) | `a4` |
| `orientation` | `portrait`, `landscape` | `portrait` |
| `margins` | `narrow` (12.7 mm), `normal` (25.4 mm), `wide` (38.1 mm) | `normal` |
| `cover` | `yes`, `no` | `no` |
| `contents` | `yes`, `no` | `no` |
| `numbers` | `yes`, `no` | `yes` |

Writing the layout (the Export PDF sheet with **Save as this note's page
layout** ticked) always writes all six keys in that order, as the first line
of the note, replacing the first existing layout line wherever it was and
leaving a blank line after it. A note without one gets it added at the top.

## The PDF

Every client lays the PDF out the same way:

- White paper, dark text, whatever the app's theme. Body in the platform's
  sans-serif at 11 pt with 1.45 line height, headings scaled from it, code in
  monospace at 9 pt on a light grey panel, links in the accent blue, tables
  with hairline borders.
- **Cover page** (`cover=yes`): the note's emoji (when it has one) large, the
  title, and the export date ("November 1, 2026") under it, centred in the
  upper third; then a new page. The cover has no page number.
- **Contents** (`contents=yes`): a page titled "Contents" listing the body's
  level 1 and level 2 headings in order, level 2 indented, then a new page.
  Entries carry no page numbers (the web and phone print engines cannot
  resolve them).
- **Body**: when there is no cover, the title as a level 1 heading first,
  as in the Markdown export. Then the note, with figures, images and the
  layout and page break lines applied.
- **Page numbers** (`numbers=yes`): "N / M" centred in the bottom margin of
  every page except the cover, counting physical pages.
- A heading is not left alone at the bottom of a page, and figures, images,
  code blocks and table rows are not split across pages where the engine
  allows it.
- File name: the Markdown export's slug with `.pdf` (`project-proposal.pdf` for
  "Project Proposal"; `note.pdf` for an empty title).

## Export PDF on each client

The **Export PDF** action sits next to **Export Markdown** wherever that is.
It opens a sheet with the six layout choices prefilled from the note's layout
line (or the defaults), **Save as this note's page layout** (ticked, shown only
to people who can edit the note), and **Export PDF**. Saving the layout changes
the note text, so it saves like any other edit (autosave or Save).

- **Web** (`app/public/dashboard/js/09-notes.js`): builds the print document in
  a hidden frame (sandboxed: no scripts) and opens the browser's print dialog, where **Save as PDF**
  writes the file; the frame's title is the file name without `.pdf`, which
  browsers use as the suggested name. CSS `@page` carries size and margins and
  the page numbers in the bottom-centre margin box.
- **Desktop** (`NotesPage::exportPdf`, rules in `desktop/src/NoteDocument.h`, drawing in
  `desktop/src/NoteDocumentRender.h`):
  asks for the file and writes it with `QPdfWriter`, laying out a
  `QTextDocument` page by page and painting the numbers itself.
- **iOS**: renders the print document in an off-screen web view, paginates
  it with `UIPrintPageRenderer`, draws the numbers itself (WebKit has no
  `@page` margin boxes) and hands the PDF to the share sheet.
- **Android**: hands the print document, loaded in an off-screen `WebView`,
  to the system print service, where **Save as PDF** writes the file; the
  WebView is Chromium, so `@page` sizes, margins and numbers apply as on the
  web. Details for both phones: `docs/native/notes.md`.

## Versions beside the document

The editor's right column ([the side column](notes-comments.md#the-side-column))
has a History tab that opens any saved version read-only (on the web drawn
like the preview, figures and page breaks included), next to Activity and the
collaborators' Chat. A restored version brings back its layout line and
figures with the rest of the Markdown.

## Shared cases

Every client's tests cover at least these:

| Input | Result |
| --- | --- |
| no layout line | a4, portrait, normal, cover no, contents no, numbers yes |
| `<!-- page: size=letter; cover=yes -->` | letter, portrait, normal, cover yes, contents no, numbers yes |
| `<!--page:size = LEGAL;orientation=landscape;margins=wide;numbers=no-->` | legal, landscape, wide, cover no, contents no, numbers no (keys and values compared lower case) |
| `<!-- page: size=a5; margins=huge; colour=red -->` | all defaults |
| two layout lines | the first one counts |
| a layout line inside a code fence | ignored |
| writing letter/landscape/wide/yes/no/yes into `Intro\n<!-- page: size=a4 -->\nBody` | `<!-- page: size=letter; orientation=landscape; margins=wide; cover=yes; contents=no; numbers=yes -->\n\nIntro\nBody` |
| writing defaults into `Body` | `<!-- page: size=a4; orientation=portrait; margins=normal; cover=no; contents=no; numbers=yes -->\n\nBody` |
| ```` ```svg Figure 1 ```` + `<svg …>` | figure, caption "Figure 1" |
| ```` ~~~SVG ```` + `<?xml version="1.0"?>\n<!-- c --><svg>` | figure, no caption |
| ```` ```svg ```` + `<div>` | code block |
| ```` ```svgfoo ```` | code block (the word must be `svg`) |
| a 262145-byte SVG body | code block |
| `<!-- pagebreak -->`, `<!--PAGEBREAK-->`, `  <!--  pagebreak  -->  ` | page break |
| `<!-- pagebreak --> text` | not a page break |
| title "Project Proposal", "", "  ", "Ünïcode / x" | `project-proposal.pdf`, `note.pdf`, `note.pdf`, `n-code-x.pdf` (the Markdown slug rule) |

## Tests

- `app/tests/test_notes_documents.py`: the web page's layout parsing and
  writing, figure detection and size cap, page break detection, the print
  document and file name, the published page's figures, and the right
  column's four tabs.
- `app/browser_tests/tests/dashboard-notes-pdf.spec.js`: the preview's figure
  and page break, the Export PDF sheet saving the layout line, the sandboxed
  print frame, and a viewer exporting without saving.
- `desktop/tests/test_note_document.cpp` (`forkmesh-note-document-tests`): the
  same rules in `NoteDocument.h`, and a PDF written from a note with a cover,
  contents, a figure and a page break.
- `native/android/core` `NoteDocumentTest` and `native/ios/ForkMeshCore`
  `NoteDocumentTests`: the same rules on the phones.
