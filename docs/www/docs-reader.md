# Docs reader: phone layout, figures and suggestions

How the rendered documentation library (`/docs/<path>/`) reads on a phone,
turns code-like blocks into figures, draws diagrams and suggests what to read
next. Part of [the website docs](README.md); the library itself is described
in the [code map](README.md#code-map).

| Piece | Where |
| --- | --- |
| Data views, diagram frames, `flow` diagrams | `app/tools/docs_figures.py`, called by `docs_markdown.render(..., rich=True)` |
| Page map, reader index | `page_map_html` and `reader_index` in `app/tools/docs_site.py` |
| Dock, sheets, link previews, wires, viewer, suggestions | `www/public/docs-explore.js` |
| Styles | the end of `www/public/docs-page.css` |

`rich=True` is only passed by the docs library. Marketplace READMEs and other
callers of `docs_markdown.render` keep plain code blocks and tables.

## Full width on a phone

Below 641 px the page gutter drops to 0.85 rem and code blocks, tables and
figures run edge to edge. Tables with three or more columns become one card
per row: the first cell is the card title and every other cell is labelled
with its column header (`data-label` on each `td`). Two-column tables stay
tables.

## Figures from code blocks

| Fence | Becomes |
| --- | --- |
| `json` that parses (a bare `"key": {…}` fragment is wrapped in braces first) | Key and value rows; nested objects indent under a gradient rule; lists of scalars become chips; lists of objects become a table; `<placeholders>` are dashed pills |
| `toml`, `ini`, `dotenv` | One block per `[section]`, a row per setting; inline and whole-line `#` comments are kept as notes |
| `text` or no language, at least three lines with box-drawing characters or arrows | A diagram frame on a grid background that scrolls sideways and says so when it overflows |
| `flow` | A drawn diagram (below) |

A block that does not parse stays a plain code block. Every data view keeps
the original under "Show … source" and has Copy and Expand buttons.

### Writing a `flow` diagram

One chain per line, steps joined by `->`; an optional `title:` line names it:

```text
title: How a request reaches a core
Browser, desktop or git -> DNS round robin -> Any healthy core -> Caddy
Health loop -> DNS round robin
```

Steps are placed in rows by the longest path from a starting step; an edge
that closes a loop is drawn back up the side. The edges are also listed as
text for screen readers. Without JavaScript the rows show with arrows between
them. [Architecture](../architecture.md#no-single-point-of-failure) has one.

## Page map

Every page with links in or out ends with a map: the pages that link to it,
the page itself, and the pages it links to (six shown, the rest behind "Show
more"). When the map is at least 720 px wide (a container query) it is three
columns wired into the centre; narrower, it is a single column on a spine.
`docs-explore.js` draws the wires as SVG and redraws them on resize.

## Phone navigation

Below 1024 px a glass dock sits at the bottom: Library, Sections, Search, For
you, and Back to top inside a ring that fills as you scroll. Library and
Sections open bottom sheets (drag the handle down or press Escape to close);
the Library sheet moves the sidebar into the sheet and adds a filter. The
toolbar slides away while scrolling down and returns on the way up. A small
pill above the dock names the current section and how far through the page
you are. The dock reaches the bottom edge so the
[Genie spider](genie-spider.md) stands on it.

Links in the text are marked by kind: other docs pages (a ring once visited,
filled once read), sections of the same page (`#`) and other sites (`↗`).
With a mouse or keyboard focus, a docs link shows a preview card: area,
reading time, summary and how much of it you have read.

## Suggestions

"Suggested for you" (end of every page, and the For you sheet) ranks pages in
the browser. Nothing is sent to a server and no language model is involved.

- The build writes `/docs/reader-index.json`: every page's title, summary,
  area, reading time, outgoing links and up to 12 keywords (the title's words,
  then the highest TF-IDF words that appear on at least three pages).
- `localStorage` key `forkmesh.docs.reader.v1` keeps, per page: visits,
  active reading time (counted every 5 s while the tab is visible and used in
  the last 45 s), how far through the text you got, seconds per section, and
  code copies; plus words from searches and which docs links you followed.
  It keeps the 160 most recent pages.
- Score: cosine similarity between the page's keywords and a profile built
  from what you read (weighted by time, depth and recency, half-life 14 days)
  and searched; links from the section you spent longest in, from this page,
  and back to it; matches with recent searches; pages you started but did not
  finish; your most-read area. Pages you finished drop to the bottom.
- Each card says why it was picked and shows the score relative to the best.
- "Forget my reading" clears the key.

The index is fetched once, in idle time or on the first preview.
