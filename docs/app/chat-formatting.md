# Chat message formatting

Chat messages use one Slack-style formatting syntax everywhere: the web chat,
the desktop app, notification emails and desktop notifications. Home page:
[app/README.md](README.md). Related: [chat-private-rooms.md](chat-private-rooms.md),
[chat-unread-reminders.md](chat-unread-reminders.md),
[../MAIL_LOG.md](../MAIL_LOG.md), [../native/DESKTOP_PINGS.md](../native/DESKTOP_PINGS.md).

## The syntax

| Write | Shows as | Notes |
|---|---|---|
| `*bold*` | **bold** | `**bold**` works too |
| `_italic_` | *italic* | |
| `~strike~` | ~~strike~~ | `~~strike~~` works too |
| `++underline++` | underline | ForkMesh extra; Slack has no underline |
| `` `code` `` | inline code | its contents are never formatted |
| ` ```code``` ` | code block | a line starting with ` ``` ` opens a block, the line ending in ` ``` ` closes it |
| `> quote` | quote | each `>` line joins the same quote |
| `- item`, `* item`, `• item` | bulleted list | |
| `1. item`, `1) item` | numbered list | |
| `<https://url\|label>` | link named "label" | Slack's link form; `<https://url>` shows the address |
| `[label](https://url)` | link named "label" | Markdown's link form |
| `https://url` | link | trailing punctuation stays outside |
| `@name` | mention | |
| `\*` | a literal `*` | a backslash escapes any marker |

Only `http`, `https` and `mailto` targets become links. Any other target
(`javascript:`, `data:`, `file:`) shows as plain text.

Like Slack, a marker only counts at a word edge. The opening marker must start
the line or follow a space or punctuation, and the closing marker must be
followed by a space, punctuation or the end of the line. So `snake_case_name`,
`2*3*4` and `file_name.txt` stay as written. A marker pair closes within 1000
characters on the same line.

## Writing formatted messages

- **Web chat** (`app/public/chat.html`, `chat.js`): the formatting bar inserts
  the markers. Bold writes `*`, italic `_`, strikethrough `~`, and there is a
  code-block button. Shortcuts (Ctrl or Cmd): B bold, I italic, Shift+X
  strike, Shift+C code, Alt+Shift+C code block, Shift+U link, Shift+7
  numbered list, Shift+8 bulleted list, Shift+9 quote. These are Slack's own
  shortcuts.
- **Desktop** (`MainWindowIssues.cpp` composer, `wrapComposerSelection` in
  `MainWindowMessages.cpp`): Ctrl+B, Ctrl+I, Ctrl+Shift+X, Ctrl+Shift+C wrap the
  selection, and Ctrl+Shift+U inserts a link. The input's tooltip lists the
  syntax.
- **Typing it by hand** works on every client. The desktop and iOS send the
  text as typed, and the web and desktop render it.

The web sends `richText: {v: 1, source}` plus a plain `text` fallback with the
markers removed (`plainTextFromRichSource`). Android still shows only the
fallback.

## Renderers

There are three implementations of the same rules. Each one parses a message
into blocks (paragraph, list, quote, code block), and each line into inline
nodes.

| Where | File | Output |
|---|---|---|
| Web | `app/public/chat-rich-text.js` (`parseBlocks`, `parseInline`, `renderRichText`, `plainTextFromRichSource`) | DOM nodes, never `innerHTML` |
| Desktop | `desktop/src/ChatFormat.h` (`forkmesh::chatformat`) | QLabel rich text (`toHtml`), plain text (`toPlainText`), notification text (`notificationText`) |
| Server | `app/src/chat_format.py` (lazy module `_chat_format`) | email HTML with inline styles (`to_html`), plain text (`to_text`) |

`app/tests/test_chat_formatting.py` checks that the web fallback and the
server's `to_text(links=False)` produce the same text. The desktop cases live
in `testChatFormat` in `desktop/tests/test_chat_transport.cpp`.

### Desktop

- `chatBodyText` (`ServerNode.cpp`) and `OfficeChannelMirror` keep a message's
  markup in `ChatMessage::text`. They take the web's rich source when there is
  one (`chatformat::bodySource`). Before 2026-10-07 the desktop removed every
  marker on receipt and showed plain text.
- `MessageRow` renders the body with `chatformat::toHtml`, including mentions
  and links. Copy and Edit work on the markup, so an edit keeps its formatting.
- Server notices (`ChatBlocks.h`) format their section text and field values
  with the same rules.

### Desktop notifications

`MainWindow::onMessage` builds the notification from the formatted message, not
the raw text:

- The body is `chatformat::toPlainText`: no markers, a link shows its label,
  list items get `•`, quotes are wrapped in quotation marks.
  `notificationText` then keeps up to 4 non-empty lines and 180 characters,
  and adds `…` when it cuts.
- A server notice (an `#email-log` card) is summarised from its blocks by
  `chatblocks::plainSummary`: the headline, then each `Label: value` field.
- An attachment shows as `📎 name`, followed by any text.
- Titles: `Alice in #general`, `Alice sent you a message`,
  `Alice mentioned you in #general`.
- Linux notification servers read the body as markup. When `GetCapabilities`
  lists `body-markup` (checked once through `gdbus`), the body is escaped, so
  `&` and `<` in a message show as typed rather than breaking the card.

## Emails

The server never sees end-to-end encrypted message text, so chat emails can't
quote a message. What the server does format:

- **Notification emails** (`_notification_email_item` in `entry.py`, both
  digest and instant mail) render each item's body with `chat_format`. The
  HTML gets bold, italics, code, lists and named links, and the plain-text part
  removes the markers and writes a link as `label (url)`.
- **Unread channel reminders** (`channel_reminders.reminder_email`) list the
  channels in one table, with an unread-count badge on each row.

## Changes

- 2026-10-07: One Slack-style syntax for all clients. The web composer now
  writes `*bold*`, `_italic_` and `~strike~` (it used to write `**`, `*`, `~~`),
  gained a code-block button and Slack's shortcuts, and renders code blocks and
  `<url|label>` links. An old web message written as `*italic*` now shows as
  bold, which is how iOS already showed it. The desktop renders formatting and
  formats its notifications. Notification emails render formatted bodies.
