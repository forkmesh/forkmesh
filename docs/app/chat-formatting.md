# Chat message formatting

Chat uses one Slack-style syntax in the web chat, desktop app, notification
emails and desktop notifications. Home page: [app/README.md](README.md).

| Write | Shows as |
|---|---|
| `*bold*`, `_italic_`, `~strike~`, `++underline++` | inline styles |
| `` `code` ``, ` ```block``` ` | code (never formatted inside) |
| `> quote`, `- item`, `1. item` | quote, lists |
| `<https://url\|label>`, `[label](https://url)`, `https://url` | links |
| `@name` | mention |
| `\*` | literal marker |

Only `http`, `https` and `mailto` become links. Markers count only at word
edges and must close within 1000 characters on the same line.

The web sends `richText: {v: 1, source}` plus a plain `text` fallback.
Renderers share the same rules:

| Where | File | Output |
|---|---|---|
| Web | `app/public/chat-rich-text.js` | DOM nodes, never `innerHTML` |
| Desktop | `desktop/src/ChatFormat.h` | rich text, plain and notification text |
| Server | `app/src/chat_format.py` | email HTML and plain text |

Chat text is end-to-end encrypted, so emails never quote messages.

Tests: `app/tests/test_chat_formatting.py`, `testChatFormat` in
`desktop/tests/test_chat_transport.cpp`.
