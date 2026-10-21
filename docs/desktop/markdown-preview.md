# Desktop: rendered Markdown

Part of [the desktop docs](README.md). Code: `desktop/src/MarkdownRender.h`
(source preparation, Qt Core only), `desktop/src/MarkdownPreviewStyle.h`
(document styling and the painted decorations), `forkmesh::ui::styleReadmeDocument`
in `desktop/src/MainWindowRepoDetail.cpp` (picks the colours). Tests: the
README rendering block in `desktop/tests/test_crypto.cpp`.

## One path for every preview

Every rendered Markdown view goes through the same two steps:

1. `MarkdownRender::forPreview(markdown)` prepares the source, and the view
   calls `QTextDocument::setMarkdown` on the result.
2. `forkmesh::ui::styleReadmeDocument(view)` styles the imported document.

Users of the path:

- the repository overview README (`readmeView`);
- the Files page Preview of a Markdown tab, whether the file came from the
  repository, the filesystem tree, a network share or a cove. Repository files
  get their renderer from `configureRepoMarkdownPreview`; every other Markdown
  tab gets a default one in `wireFilesEditor`. Before October 2026 those other
  tabs called a bare `setMarkdown`, so they showed Qt's raw import: no paragraph
  spacing, grey 1 px table grid, the code font as body text;
- the Markdown preview dialog opened from a diff (`diffMarkdownPreviewBrowser`);
- the Write/Preview editor (`MarkdownEditor`: issue, pull request and
  discussion composers, Notes). It styles only while the Preview tab is shown,
  so typing does not pay for the styling on every keystroke;
- the Docs package reader. In the new design its own pass
  (`styleNextDocument`) runs afterwards and keeps its framed code blocks,
  hairline-only tables and heading spacing.

## Source preparation

`forPreview` normalises line endings, runs `withHtmlFlattened` (raw HTML
rewritten as Markdown, see the comment block at the top of `MarkdownRender.h`)
and then `withHardBreaksJoined`:

- Qt's importer turns a hard line break (a trailing `\` or two trailing spaces)
  into a new block, so the second line got paragraph spacing. The two lines are
  joined with U+2028 instead, which `QTextLayout` breaks inside one paragraph,
  as GitHub does. `toPlainText()` still reports `\n`.
- A line that is only `\` (common in exported notes) becomes a paragraph break,
  and a `\` that ends a paragraph is dropped. GitHub would print both literally;
  in a preview they only ever read as a stray character.
- Fenced and indented code, table rows and escaped backslashes (`\\`) are left
  alone. A break before a line that starts a new block (heading, list item,
  quote, fence, rule, table) only drops the marker.

## Styling (GitHub README look)

`styleReadmeDocument` fills a `markdownstyle::Colors` from design tokens with
`Theme::nextOr`, so Next uses `fg`, `fgMuted`, `border`, `borderStrong`,
`surfaceRaised` (code blocks, alternate table rows), `surfaceActive` (inline
code) and `accentFg` (links) in both schemes, and Classic keeps GitHub's own
dark and light values. Code text uses `Theme::monospaceFont`.

`markdownstyle::apply` sets block and character formats:

| Element | Format |
| --- | --- |
| Paragraph | 16 px after, line height 145 % |
| List item | 4 px after when the same list (or a nested one) continues, else 16 px; indent width 28 px |
| H1, H2 | 24 px above, a hairline 8 px under the text, 16 px after it; weight 600 |
| H3–H6 | 24 px above, 16 px after |
| Fenced or indented code | 16 px padding inside a rounded panel, 16 px after; code font at 88 % |
| Inline code | code font at 88 % in a rounded pill; 4 px letter spacing on the character before and the last character reserves the pill's padding |
| Blockquote | text in `fgMuted`, 20 px per level, a 4 px bar per level |
| Table | collapsed 1 px `border` grid, cells padded 6 × 13 px, bold header row, every second body row shaded |
| Link | `accentFg`, no underline |
| Horizontal rule | a 1 px hairline with 24 px above and below |

Qt's import also leaves an empty first block in the first header cell, which
made the header row two lines tall; it is removed. Qt ignores a block's bottom
margin above a table, so the table's top margin repeats it.

`QTextDocument` cannot draw rounded backgrounds, per-side borders or a rule
under a heading, so `markdownstyle::Decorations` (an event filter on the
viewport, object name `markdownDecorations`) paints the code panels, inline
code pills, quote bars, heading rules and horizontal rules before the
`QTextBrowser` paints its text. Their geometry is collected in document
coordinates and cached until the document or its layout changes. Code blocks
inside a frame (the Docs reader's framed code) are left to that frame, and a
heading whose bottom margin another pass shrank gets no rule.

Hovering a code panel shows a Copy button (`markdownCopyButton`) in its top
right corner; it puts the block's text on the clipboard and reads "Copied" for
1.5 s.

The document text is unchanged by the styling: window tests that read
`toPlainText()` or the first heading block keep passing. Colours are applied
when a document is rendered, so a live scheme switch shows on the next render.

## Links

Previews never navigate themselves (`setOpenLinks(false)`). A preview with
its own `markdownLinkHandler` (repository files) decides; every other preview
(Files tab, network files, the Markdown editor) scrolls to a `#anchor` and
opens only `http`, `https` and `mailto` links in the browser. Markdown from
an untrusted repository or a network share can therefore not open `file://`,
`smb://` or a custom URL handler on this machine with one click.
