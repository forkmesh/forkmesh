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
- **Dependencies**: the last sidebar area. One page lists every open-source
  dependency, grouped by ecosystem (desktop C++, Python, Go, npm, bundled web
  libraries, Android Gradle, Swift packages), with its version, the manifests
  that use it (linked to the hosted source tree), its GitHub repository and
  its top three authors by commits: name and login linked to the GitHub
  profile, commit count, a link to the author's projects
  (`github.com/<login>?tab=repositories`) and their website when they list
  one. Search covers it too, so an author's name finds the page.

## Dependencies

The page reads `data/dependencies.json` (embedded as
`:/forkmesh-market/packages/docs/data/dependencies.json`), not the network:
GitHub allows 60 unauthenticated API calls an hour, too few to ask on every
open, and the page must work offline.

`marketplace/tools/docs_dependencies.py` writes the snapshot. It reads
`desktop/CMakeLists.txt` (`find_package`, `pkg_check_modules`), the pinned
`app/host/requirements.txt`, `tools/requirements-ci.txt` and
`app/pyproject.toml`, every `package.json` and `go.mod` outside
`node_modules` and test folders, the Android `*.gradle.kts` files,
`Package.swift`, and versioned files under the web `vendor/` folders. The
GitHub repository comes from PyPI's project URLs, the npm registry's
`repository` field, the Go module path, or the `KNOWN` / `MAVEN_GROUPS`
tables in the tool for C++, vendored and Android dependencies. Authors are
GitHub's contributor ranking with bots dropped; names come from
`/users/<login>` and fall back to the login.

```
python3 marketplace/tools/docs_dependencies.py            # fill in what is missing or older than 30 days
python3 marketplace/tools/docs_dependencies.py --wait     # sleep through rate limits instead of stopping
python3 marketplace/tools/docs_dependencies.py --offline  # re-read manifests only, keep GitHub data
python3 marketplace/tools/docs_dependencies.py --check    # exit 1 if the snapshot lags the manifests
```

Set `GITHUB_TOKEN` for 5,000 calls an hour. A run stopped by the rate limit
keeps what it fetched and exits 3; run it again after the reset.
`marketplace/tests/test_docs_dependencies.py` fails when a manifest gains,
drops or re-pins a dependency without the snapshot being refreshed. A
repository GitHub will not list contributors for (its history is too large)
shows a note instead of authors; so does a dependency with no GitHub home,
such as the Android Gradle Plugin.

The loader keeps only `https://github.com/` repository and profile links,
`http(s)` websites, valid GitHub logins, repository-relative manifest paths,
and at most three authors, because every link on the page opens in the
browser.

## How the documents get in

`content/desktop/build.json` names `www/docs` under `repositoryResources`, so
the desktop build embeds that folder as
`:/forkmesh-market/packages/docs/repository/www/docs/`. The viewer works
offline and always shows the docs of the build you are running. Adding a file
to `www/docs` re-runs package discovery on the next build.

## Code

- `content/desktop/DocsLibrary.{h,cpp}`: catalog, areas, ordering, titles and
  summaries, link resolution, backlinks, search, web URLs and the
  Dependencies page built from the snapshot. Qt Core only.
- `data/dependencies.json` and `marketplace/tools/docs_dependencies.py`: the
  dependency snapshot and its generator (tests:
  `marketplace/tests/test_docs_dependencies.py`).
- `content/desktop/DocsFeature.{h,cpp}`: the rail page and its `builtin:docs`
  registration.
- Tests: `forkmesh-docs-tests` (`content/desktop/tests/test_docs_library.cpp`),
  run against a temporary library and the repository's `www/docs`.
  `www/tests/test_docs_library.py` checks that the guide order, areas and
  reserved slugs match `app/tools/docs_site.py`.
