# Docs

The ForkMesh documentation library on a desktop rail page: every Markdown page
and data file under `www/docs/`, the same library forkmesh.com/docs renders.
Free, bundled with the desktop and on by default; remove it from the rail or
the Market like any other package.

## What the page does

- **Library**: the sidebar groups documents by the web library's areas
  (Guides, Design, Engineering, Security, Operations, Mobile, Strategy &
  research, Plans & specs, Schemas & data), in the website's order and with the
  website's titles and summaries.
- **Search**: the box above the sidebar, and the window's top search box, match
  every word against titles, summaries, paths and the full text; title matches
  rank first and each result quotes the line that matched.
- **Reading**: documents render with the same Markdown treatment as a
  repository README. Links to other documents open in the page (with their
  `#section`), links to repository files open the hosted source tree, and web
  links open in the browser. Back and Forward (Alt+Left, Alt+Right) walk the
  pages you opened.
- **Around a page**: an "On this page" outline, "Pages that link here",
  previous/next in library order, and "Open on web" for the forkmesh.com copy.
  The page reopens the last document you read.

## How the documents get in

`content/desktop/build.json` names `www/docs` under `repositoryResources`, so
the desktop build embeds that folder as
`:/forkmesh-market/packages/docs/repository/www/docs/`. The viewer works
offline and always shows the docs of the build you are running. Adding a file
to `www/docs` re-runs package discovery on the next build.

## Code

- `content/desktop/DocsLibrary.{h,cpp}`: catalog, areas, ordering, titles and
  summaries, link resolution, backlinks, search and web URLs. Qt Core only.
- `content/desktop/DocsFeature.{h,cpp}`: the rail page and its `builtin:docs`
  registration.
- Tests: `forkmesh-docs-tests` (`content/desktop/tests/test_docs_library.cpp`),
  run against a temporary library and the repository's `www/docs`.
  `www/tests/test_docs_library.py` checks that the guide order, areas and
  reserved slugs match `app/tools/docs_site.py`.
