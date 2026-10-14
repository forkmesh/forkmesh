# Organization task text is Markdown

Task `details`, QA `howToTest`, completion notes (`completionNote`,
`completionHistory[].note`) and reply bodies are Markdown. They keep line breaks
end to end and are rendered on the web Tasks page and the desktop Tasks view.
Titles and other routing values stay single-line.

## Server

`world/src/world_office_tasks.py` bounds multi-line fields with
`_multiline(value, maximum, fallback)`: CRLF to `\n`, tab to four spaces, other
control characters to a space, `<` and `>` kept, trailing spaces and extra blank
lines trimmed. Caps: details 4000, howToTest 720, completion note 4000, reply
2000. Every other field uses single-line `_text()`, which collapses whitespace
and strips `<`/`>`; plain consumers (Pings, mentions, event summaries) use it too.

## Web rendering

`taskMarkdownHtml()` in `app/public/dashboard/js/09-tasks.js` uses the shared
`renderMarkdown()` (`06-repo-content.js`): it escapes text before adding markup,
flattens raw HTML, allows only `http:`, `https:` and `mailto:` links
(`mdSafeUrl()`), and opens external links with
`rel="noopener noreferrer nofollow ugc"`. `.task-markdown` resets `white-space`
to `normal`. Rebuild with `app/tools/build_dashboard_assets.py`.

## Tests

`world/tests/test_world_office_tasks.py`, `app/tests/test_task_markdown_frontend.py`,
`tools/test_forkmesh_mcp_server.py`, and the desktop
`forkmesh-task-markdown-tests` ([desktop tasks page](../desktop/tasks-page.md)).
