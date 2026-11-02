# Docs design: one shell, pillars and illustrations

How every page under `/docs/` looks and links together. Part of
[the website docs](README.md); the phone layout is in
[docs reader](docs-reader.md).

| Piece | Where |
| --- | --- |
| Shell, toolbar tabs, sidebar, hero, rail | `PAGE_TEMPLATE`, `DOCS_TABS`, `hero_html`, `core_path_html`, `related_rail_html` in `app/tools/docs_site.py` |
| The `/docs/` landing | `www/templates/pages/docs/index.html` |
| Illustrations and icons | `app/tools/docs_art.py`; the landing's copies in `www/templates/partials/docs-art/` |
| Styles | "unified design" at the end of `www/public/docs-page.css` |

## One shell

The landing and the rendered library share one shell: the same toolbar tabs
(`DOCS_TABS`), sidebar (platform switch, entry links, one icon per area, the
"Run ForkMesh" card), outline rail and feedback form, all styled by
`docs-page.css` and driven by `docs-page.js` and `docs-explore.js`. Before
November 2026 the landing carried its own Tailwind chrome and script, so the
two looked and behaved differently. The landing's reference sections still use
the static `tailwind.css` for their inner layout; that file is precompiled, so
new landing markup uses `dh-*` classes from `docs-page.css`, never new Tailwind
utilities. The build fills the landing's sidebar through
`<!-- forkmesh-docs-sidebar -->`.

## Pillars

The landing opens with who ForkMesh is for and six pillars, the questions a
maintainer of a circumvention, privacy or security tool asks before trusting it
with a project: mirroring and distribution, the threat model, cryptography,
self-hosting, testing under blocking, and fitting existing workflows (the
priorities in the Open Technology Fund concept-note feedback). Each card states
only what its linked pages say; keep them in step when those pages change.
Below them the six [core pages](../README.md) form a numbered reading path.

## Interwoven navigation

- A core page's "Part of the core documentation" paragraph becomes a row of
  chips in its hero, the current page filled.
- Every page's rail shows the outline, the six core pages as numbered steps,
  and up to five related pages: the pages it links to, then its neighbours in
  the same area.
- A "Who it is for" section becomes a callout card.
- Section headings carry a small icon tile that cycles through six icons.

## Illustrations follow the theme

Each page's hero carries an isometric drawing: a core page has its own
(network, lock, mirror, server, shield, radar), every other page its area's
(`AREA_STYLE`). The drawings are inline SVG coloured only through CSS classes
and the `--art-*` variables, which mix the dashboard blue
(`app/public/dashboard/theme.json` primary) with `--docs-accent-2`, so they
switch with the reader's light or dark choice. Bitmaps could not. Gradient ids
carry a per-drawing prefix so several drawings can share a page.

After changing `docs_art.py`, run `python3 app/tools/docs_art.py` to rewrite
the landing partials, then `python3 app/tools/site_templates.py`;
`www/tests/test_docs_library.py` fails while the partials are stale.
`python3 app/tools/docs_art.py --preview <file>` writes a sheet of every
drawing.
