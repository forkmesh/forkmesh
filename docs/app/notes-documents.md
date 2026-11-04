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

## Image options

Since 2026-11-04 an image can carry Google Docs' image options: size and
rotation, text wrapping, a place anywhere on the page, recolouring,
transparency, brightness and contrast. They live in a line directly above the
image's line, outside a code block:

```markdown
<!-- image: wrap=behind; width=210; height=297; pages=all; recolor=grayscale -->
![Company logo](data:image/png;base64,…)
```

The line applies to the first image on the next line, when that line starts
with the image. Like the layout line it is an HTML comment, so other Markdown
viewers hide it, and every client hides it in its preview and PDF. The web
applies the options; the desktop and the phones hide the line and show the
image as before. Keys are lower case and `;`-separated; a missing or unknown
key or value uses the default, and numbers are clamped and kept to one
decimal. Lengths are millimetres, so they mean the same on screen and on
paper.

| Key | Values | Default |
| --- | --- | --- |
| `wrap` | `inline`, `wrap` (text flows around it), `break` (on its own line), `behind` and `front` (placed on the page, behind or in front of the text) | `inline` |
| `align` | `left`, `center`, `right` (for `wrap` and `break`; `wrap` uses left or right) | `left` |
| `width`, `height` | 0–2000 mm; 0 is the natural size, and a width alone keeps the aspect ratio | 0 |
| `rotate` | −360–360 degrees | 0 |
| `margin` | 0–100 mm between a wrapped image and the text | 3.2 |
| `x`, `y` | −2000–2000 mm from the page's top-left corner (`behind` and `front`) | 0 |
| `pages` | `anchor` (the page the image's line lands on), `all`, `first`, `rest` | `anchor` |
| `recolor` | `none`, `grayscale`, `sepia`, `negative` | `none` |
| `transparency` | 0–100 | 0 |
| `brightness`, `contrast` | −100–100 | 0 |

Writing the options keeps only the keys that differ from the defaults, in
the table's order, and drops the line when none do. A background is just an
image `behind` the text that fills the page: **Background** in Pages inserts
one with `x=0; y=0`, the paper's width and height and `pages=all` at the end
of the note (and writes the layout line first, so it stays the first
`<!-- page: -->`-style line).

### On the web

Clicking an image in Live or Pages selects it (`selectNoteImage` in
`app/public/dashboard/js/09-notes.js`): a frame with a corner handle that
resizes it, and a toolbar under it with **Edit** (the Image options panel), the
text wrapping menu (In line, Wrap text, Break text, Behind text, In front of
text), **Comment** and **React** (anchored to the image's line, like a
selection) and a bin that deletes it. An image behind or in front of the text
can be dragged on the page in Pages. Delete or Backspace removes the selected
image and its options line; Escape lets go of it. An image added with
**Image** or **Background** is selected as soon as it shows.

In Pages the text window sits above a Behind text image, so a click anywhere
over that image (outside a link) picks it through the text. A drag starts
after 3 px, so a click only selects; it moves the image with `translate`
once per frame and writes x and y once, on release. A change that only moves,
sizes or restyles a Behind or In front image restyles the copies already on
the pages instead of laying the note out again, so a drag or a slider never
reloads the page frame.

Ctrl+Z (Cmd+Z) undoes the last image change (insert, move, resize, wrapping,
options, alt text, delete) and Ctrl+Shift+Z or Ctrl+Y redoes it, while focus
is not in a text field. The browser's own undo cannot do this, because these
changes write the Markdown with a script. The 50-step history is kept per
note and is dropped when the Markdown has changed some other way since, such
as typing or a collaborator's edit, so an undo never throws away text.

The Image options panel follows Google Docs: Size & rotation (width and
height in inches, or centimetres on A4, their scale against the image's
natural size, lock aspect ratio, angle and a 90° turn), Text wrapping with
alignment and the margin from the text, Position (x and y from the page's
corner, which pages it shows on, Fill the page, Center on page; only for
Behind and In front of text), Recolor, Adjustments (transparency,
brightness, contrast) and Alt text (the image's title and its Markdown alt
text). Every change writes the options line, which autosaves and reaches
collaborators like any edit.

## Pages

Since 2026-11-04 the web editor's layout switch has a fifth choice, **Pages**:
the note as it prints, page by page on a grey desk, at "Fit width" or a set
zoom, with the page count, **Image**, **Background** and **Export PDF** above
it. It is the same document the PDF prints, so what you see is what you get.

`paginateNoteDocument` lays the print document out in a column per page
(CSS multi-column with the paper's content width and height, the cover,
contents and page breaks ending a column), measures how many columns it
fills, and builds one paper-sized page per column: the images placed
`behind` the text, a window at the margins onto that column of the text, the
images placed `in front`, and the page number. Heading ids stay only on the
page that shows them, so the contents still link. The view runs in a
sandboxed frame without scripts (`allow-same-origin`, so the page can measure
and wire it).

Export PDF on the web now prints these pages: the print frame is paginated
the same way after it loads, with `@page { margin: 0 }`, so a background
reaches the paper's edge and the PDF matches Pages exactly. Clipped text is
not painted, so each line is in the PDF once.

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
  browsers use as the suggested name. The document's `@page` rule carries the
  size and margins; once loaded, the frame is laid out into paper-sized pages
  as in [Pages](#pages), which draw their own numbers and image options.
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
| `<!-- image: wrap=behind -->` + `![a](a.png)` | the first line hidden, the image shown |
| `<!-- image: wrap=front -->` inside a code fence | code |
| title "Project Proposal", "", "  ", "Ünïcode / x" | `project-proposal.pdf`, `note.pdf`, `note.pdf`, `n-code-x.pdf` (the Markdown slug rule) |

## Tests

- `app/tests/test_notes_images.py`: reading and writing image options, adding,
  replacing and removing the line, the renderer hiding it and marking the
  image's paragraph, mapping images to lines, page geometry, and the
  published page hiding it.
- `app/browser_tests/tests/dashboard-notes-live.spec.js`: Pages with page
  numbers, the image toolbar, wrap menu and options panel placing an image
  behind every page, recoloured, with alt text, the PDF frame laid out the
  same way, resizing and wrapping an image in Live, and a Background added in
  Pages being selected, moved, picked through the text, deleted, undone and
  redone.
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
