# Copy keeps formatting

Copying text from anywhere in the desktop (an agent transcript, a chat
message, a README preview, an issue, a log pane) pastes with the layout you
saw: line breaks, a blank line between paragraphs, `-` and `1.` list markers
with nested lists indented, code blocks line for line, and table rows with
tab-separated cells.

## Why

Qt's own plain text for a selection in a `QLabel`, `QTextBrowser` or rich
`QTextEdit` joins paragraphs with a single newline and drops list markers and
table columns, so a pasted message turned into one block of lines. Most paste
targets (terminals, editors, the composer) read only `text/plain`.

## How

`src/CopyFormatting.cpp` is installed once from `main.cpp`. When the desktop
puts a Qt rich-text selection on the clipboard (or the X11 primary
selection), it rebuilds the `text/plain` part from the selection's HTML,
keeping the block structure, and leaves the `text/html`, `text/markdown` and
ODF parts as Qt made them, so apps that paste rich text still get the styling.

It changes nothing when:

- the copy is a single line, or larger than 500,000 characters;
- the result would be the same as Qt's (plain text editors, code views);
- the focus is in a `QPlainTextEdit`;
- the clipboard belongs to another application, or the text was set directly
  by a Copy button (those already copy the Markdown or the raw text).

Terminal tabs and the embedded browser copy through their own paths, which
already keep line breaks.

`tests/test_copy_formatting.cpp` (`forkmesh-copy-formatting-tests`) covers
Markdown and rich-text labels, line breaks, plain text staying exact, and the
installed clipboard hook where the platform reports clipboard ownership.

Related: [markdown-preview.md](markdown-preview.md),
[agents-ui.md](agents-ui.md), [../architecture.md](../architecture.md).
