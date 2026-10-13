# Organization task text is Markdown (2026-09-28)

Task `details`, the QA `howToTest` steps, completion notes (the current
`completionNote` and every `completionHistory[].note`) and replies are Markdown.
They keep their line breaks end to end and are rendered on the web Tasks page and
in the desktop Tasks view. Titles stay one line.

## The bug

A task created through the MCP tool `create_task` with a Markdown body
(`## Why`, a table, a numbered list) came back with every newline turned into a
space: `"## Why The cores run … | Route | Queries | … |---|---:|"`. The MCP
server (`tools/forkmesh_mcp_server.py`) and the desktop send the text
unchanged. The collapse happened in the tasks API itself:
`world/src/world_office_tasks.py` bounded every task string with `_text()`,
which ran `" ".join(value.split())` and also replaced `<` and `>` with spaces.
That is right for titles, names and routing values, and wrong for prose. It
ran on write (create, update, complete, reply, QA request) and again on every
read projection, so stored text lost its structure the moment it was saved.

Tasks saved before the fix cannot be recovered: the newlines were never stored.
Re-save them with their original text (the coordinator re-saves task
`f4a18f54852b0d30d90fa5cd162c5727` with `update_task` once this is deployed).

## Server: `_multiline()`

Multi-line fields now go through `_multiline(value, maximum, fallback)` instead
of `_text()`:

- `\r\n` and lone `\r` become `\n`; a tab becomes four spaces (code-block
  indentation survives).
- Any other non-printable character (NUL, ESC, BEL, U+2028 and friends) becomes
  a space, as before.
- `<` and `>` are kept. Markdown needs them for blockquotes (`> quote`) and code
  (`a < b`, `Vec<T>`). Every renderer escapes them (see below), and the plain
  consumers (Pings copy, Discord, mentions, event summaries) still go through
  the single-line `_text()`, which strips them.
- Trailing spaces on each line are dropped, leading and trailing blank lines are
  dropped, and more than two consecutive blank lines are squeezed to two.
  Leading indentation is kept.
- The length caps are unchanged: details 4000, howToTest 720, completion note
  4000, reply 2000. A whitespace-only value counts as empty.

Fields that use it: `details`, `howToTest`, `completionNote`,
`completionHistory[].note`, reply `body`. Everything else (title, assignee,
department, team, repository, check-in and reminder notes, event summaries)
still uses `_text()`.

The signed PATCH digest (`org_task_update_digest` in `app/src/entry.py`,
`task_update_digest` in the MCP server) hashes the parsed JSON body, so a
newline is `\n` inside the canonical JSON on both sides; nothing changed there.

## Web: the Tasks page

`app/public/dashboard/js/09-tasks.js` `taskMarkdownHtml()` renders with the
dashboard's existing renderer, `renderMarkdown()` in `06-repo-content.js` (the
one issues, pulls, READMEs and Notes use). It is used for the Details card, the
How to test block, the Completion note card, completion-history notes and
comment bodies in the timeline. The one-line row preview stays escaped plain
text.

Safety comes from `renderMarkdown()`: it escapes the whole text before it adds
markup, flattens raw HTML to Markdown instead of passing it through, and
`mdSafeUrl()` turns every link scheme other than `http:`, `https:` and
`mailto:` into `#`. External links open with
`rel="noopener noreferrer nofollow ugc"`. `@mentions` are highlighted only in
text outside tags, `<code>`, `<pre>` and links, so a mention in a URL or a code
span is left alone.

`.task-markdown` in the Tasks view (`marketplace/packages/tasks/content/web/view.html`,
mirrored in `app/public/dashboard/partials/views/tasks.html`) resets
`white-space` to `normal`, because the card and comment containers use
`pre-wrap` for plain text and would otherwise double every block gap.

The page chunk is built by `app/tools/build_dashboard_assets.py`
(`dashboard-page-tasks.js` and the `?v=` in `dashboard.js`).

## MCP

`create_task`, `update_task` and `complete_task` already sent the strings
unchanged; their schemas now say the fields are Markdown with line breaks kept.
`update_task` with `details` uses the `forkmesh-org-task-update-v1` digest proof.

## Tests

- `world/tests/test_world_office_tasks.py`:
  `test_multiline_text_keeps_line_breaks_and_strips_other_controls` and
  `test_markdown_details_notes_and_replies_keep_their_line_breaks` (create,
  list, PATCH, reply, complete). Both fail on the old `_text()`.
- `app/tests/test_task_markdown_frontend.py`: runs the real renderer and
  `taskMarkdownHtml()` in node: headings, bold and italic, code, tables, lists,
  blockquotes, safe links, `javascript:`/`data:`/`vbscript:` links, raw HTML
  and mention highlighting.
- `tools/test_forkmesh_mcp_server.py`: `create_task`, `update_task` and
  `complete_task` put the newlines in the signed request body.
- Desktop: `forkmesh-task-markdown-tests` ([desktop tasks page](../desktop/tasks-page.md)).
