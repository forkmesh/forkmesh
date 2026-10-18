# Folder About boxes

User guide: [www/docs/folder-about.md](../../www/docs/folder-about.md) (the file
format, limits and fallbacks).

A folder's About box is read from `<folder>/.forkmesh/info.json`, the file the
repository-level About already uses at the root (`MainWindowRepoDetail.cpp`
writes `about` and `website` there; `loadRepoAboutInfo` reads it on the web).
Reusing the name means one convention, the same `about`/`website` keys, and
no new top-level file in every project folder. `name`, `links` and `topics`
are new keys; the desktop editor keeps unknown keys when it rewrites the file.

## API

`app/src/directory_about.py`, routed from `app_runtime.py` for the actions
`dir-about` and `dir-logo` under `/api/repo/<owner>/<repo>/` (like
`file-edit`, before the git routes).

| Endpoint | Answer |
| --- | --- |
| `GET /api/repo/<o>/<r>/dir-about?path=<folder>&ref=<ref>` | `{"ok", "path", "commit", "about"}`; `about` is `null` for a folder without an About |
| `GET /api/repo/<o>/<r>/dir-logo?path=<folder>&ref=<ref>` | the logo bytes, or 404 |

`about` holds `path`, `source` (`file` or `project`), `invalid` (the file
exists but is not usable JSON), `name`, `description`, `descriptionSource`
(`about`, `readme` or empty), `website`, `websiteSource`, `links`, `topics`,
`logo` (`{path, type, url}` or `null`) and `readme` (the README path used).

- **One read.** Both endpoints make a single `blobs` read of the candidate
  paths (`.forkmesh/info.json`, `logo.svg`, `logo.png`, the README names).
  `blobs` resolves the ref once and returns the commit, costs one `ls-tree`
  plus the blobs on the gateway, and is already in every read path: the
  gateway, the host fast path and `hosted_private_read`. A `tree` read would
  have run the gateway's whole-repository analysis again. The logo bytes come
  back in that read (Bolt's PNGs are up to 280 KB) because nothing cheaper
  tells whether a file exists; the result is cached per commit.
- **Same permissions as a tree read.** The sub-read is built from the incoming
  request (same path owner, `authorization`, `cookie` and client address
  headers) and dispatched exactly like `REPO_HOST_RE` reads: public repos go
  through `_https_mirror_proxy`, private ones only through
  `hosted_private_read` (core-hosted, reader session required), anything else
  is 404. Organization aliases keep the organization in the sub-read path, as
  `_https_mirror_proxy` expects.
- **Caching.** The page passes the commit its tree read returned as `ref`.
  For public repositories, answers for a full commit id are kept in an
  in-process LRU (256 About answers, 24 logos) keyed by owner, repo, folder,
  commit and project name, and sent with `public, max-age=300` (not immutable:
  the project-table name can change for the same commit). A branch ref is
  `no-cache`. Private repositories are never cached, in the process or the
  browser (`private, no-store`), so every read re-checks the session.
- **Validation.** Paths drop `.` and empty segments and refuse `..`, `.git`,
  backslashes and control characters (1 KB). Refs use the same rules as the
  metadata cache key. URLs must be `http`/`https` with a host and no
  credentials; text is stripped of control characters and capped; topics are
  slugged. All output is escaped by the page.
- **Logos.** SVG preferred. PNG must carry the PNG signature; SVG must start
  with `<svg>` (after an optional XML declaration, comments, doctype) and may
  not contain `<!ENTITY`. 512 KB cap. Served with the matching
  `content-type`, `nosniff`,
  `content-security-policy: default-src 'none'; style-src 'unsafe-inline'; sandbox`,
  `content-disposition: inline` and `cross-origin-resource-policy: same-origin`,
  and only ever referenced from `<img>`. The gateway's own `raw` answers SVG as
  an attachment, which an `<img>` cannot show, hence the dedicated endpoint.

## Project folders

Folders listed in the instance's project-address table (`repo_projects`,
[server-update.md](server-update.md)) get an About without a committed file:
`source: "project"`, the display name from the table, everything else from
the README and logo. This makes the Bolt monorepo (about 60 folders, each with
`logo.png`/`logo.svg` and a README starting with `**Live Site:**`) work
without committing 60 files, while every other folder keeps today's
behaviour. The table applies only to its repository name and, when short
addresses name an owner, only under that owner (the path owner or the backing
node).

## README fallback

Only used to fill an empty `about`/`website`, and only from the same `blobs`
read, so it costs nothing extra. The first paragraph that is prose: headings,
setext headings, HTML blocks, images, badges, inline-code lines, fenced code,
lists, tables, quotes and paragraphs made only of `**Label:**` lines are
skipped; links keep their text, emphasis markers and tags are removed; at
least 20 characters; cut on a word at 240 with an ellipsis. The website comes
from a `**Live Site:**`, `**Website:**` or `**Homepage:**` line, which is how
Bolt's project READMEs open.

## Web page

`06-repo-content.js` (`loadRepoDirAbout`, `renderRepoDirAbout`) and the
`[data-repo-dir-about]` aside and `[data-repo-dir-logo]` image in
`08-repo-detail-network.js`.

- The folder About **replaces** the repository About inside a folder rather
  than stacking above it. Folder views already hide the repository rail
  (focus mode with the file explorer on the left), so "above" would have meant
  bringing back the whole repository rail (stars, forks, watch, releases) on
  every folder; the repository About stays one click away on the root.
- It is a sibling aside in the same grid column; showing it restores the
  two-column grid that focus mode collapses, hiding it collapses it again.
- The page asks only when the folder's tree has a `.forkmesh` folder or
  `logo.svg`/`logo.png`, so ordinary folders cost no request. Answers are
  cached in the page per repository, folder and commit.
- Not loaded for background tree preloads or when the answer arrives after
  the visitor moved on (other folder, a file, another tab).
- Website and link rows open in a new tab with
  `rel="noopener noreferrer nofollow ugc"`; the page re-checks every URL is
  `http`/`https` and the logo URL is a same-origin `/api/repo/` path.
- The rail ends with the folder's README and its history
  (`/<repo>/commits?path=<folder>`, the history form project addresses use).

## Tests

`app/tests/test_directory_about.py` (parsing, validation, README fallback,
logo checks, project names, permissions, caching, logo headers, organization
owners, routing) and
`app/browser_tests/tests/dashboard-directory-about.spec.js` (rail, header
logo, escaping, no request for ordinary folders).
