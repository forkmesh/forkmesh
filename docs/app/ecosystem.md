# ForkMesh Ecosystem (curated project directory)

Part of [the App docs](README.md). Code: `app/src/ecosystem.py`, table
`ecosystem_projects` (migration `0213_ecosystem_projects.sql`), tests
`app/tests/test_ecosystem.py`. Website: `www/public/ecosystem.html`, the
`#ecosystem` section of `www/public/index.html`, tests
`www/tests/test_ecosystem_page.py` and `www/tests/worker_routing.test.mjs`.

The ForkMesh Ecosystem is a public map of the projects building around,
alongside and on top of Git-native collaboration: decentralized Git, forges,
issue trackers, code review, chat, AI coding agents and the tools that run
them. Each listing says what the project does, why ForkMesh follows it and how
it relates to ForkMesh today. Behind it sits a private strategy layer
(overlap, threat, opportunity, recommended action, notes) that only
administrators see. Administrators manage both as JSON in the admin console;
the website reads a public API.

## Why it is shaped this way

- **Data in the App database, not a file in the repository.** An
  administrator can publish or correct a listing without a deploy, and the
  private assessment never enters the public repository.
- **Public and private are stored apart.** Public fields are in the `data`
  column and private ones in `internal`. The public query never selects
  `internal`, and the public payload is built from an explicit field list
  (`public_project`), so a serialization slip cannot leak it.
- **Drafts by default.** Listings describe real third parties, so a project
  stays a draft unless its JSON says `"published": true` or someone clicks
  Publish.
- **Descriptive, not evaluative.** Public fields describe; opinions belong
  under `internal`. There are no scores or rankings on the public pages.
- **Honest relationship states.** `compatibility` defaults to `unknown`
  ("Not yet assessed") and `integration` to `watching`; `integrations` lists
  only ForkMesh integrations that exist today.
- **What ForkMesh runs on is listed too.** Besides projects in our space, the
  directory lists every third party our code depends on, calls, ships or
  names: cloud hosts, payment and email services, AI model APIs, coding agent
  CLIs, libraries, fonts, icon sets, app stores. They carry
  `integration: "uses"` ("Used by ForkMesh"), `watching` says how ForkMesh
  uses them, and `integrations` says where, as `"<surface>: <purpose>"`
  (`"Desktop app: SSH to hosts"`). The categories from `cloud-hosting` to
  `apps` exist for them. Listing them publicly is the same disclosure a
  dependency manifest or a privacy page makes, in one searchable place.
- **Two write modes.** An import merges by slug: fields present replace the
  stored ones, fields left out are kept, `null` clears a field (inside
  `internal`, `null` removes that key). That lets a short JSON such as
  `{"slug": "radicle", "published": true}` update one flag. Editing one
  project replaces it whole, so the editor behaves like an editor: a field
  you delete returns to its default.
- **All or nothing.** If any project in an import fails validation, nothing is
  written and the page lists every error next to the pasted text.

## Routes

| Method and path | Caller | Auth |
| --- | --- | --- |
| `GET /api/ecosystem` | www landing section and `/ecosystem` page | public, `cache-control: public, max-age=60`, in-process edge cache |
| `GET /<ADMIN_PATH>?view=ecosystem` (`&status=published\|drafts`) | administrator | console session (`is_admin`) |
| `GET /<ADMIN_PATH>?view=ecosystem&project=<slug>` | administrator | edit one project as JSON |
| `GET /<ADMIN_PATH>?view=ecosystem&format=json` | administrator | export every project, drafts and `internal` included, as an attachment |
| `POST /<ADMIN_PATH>?view=ecosystem` | administrator | console CSRF token; form `action` is `import`, `save`, `publish`, `unpublish`, `feature`, `unfeature` or `delete` |
| `POST /api/ecosystem/import` | scripts such as `tools/ecosystem_seed.py` | Ed25519 signature by an administrator account's node key (`_admin_authorized`) |

The console dispatches `view=ecosystem` to `admin_handler` before its own POST
branch (`app_runtime._admin`), in the same way as the Support sections. Forms
are capped at 1 MB and imports at 500 projects; the table holds at most 1000.
Every write is audited as `admin.ecosystem_<action>` and purges the API cache
key `https://forkmesh.internal/api/ecosystem`. Delete needs its confirmation
box ticked.

`POST /api/ecosystem/import` is the same import as the console form for
callers without a browser session. The body is
`{"node", "ts", "requestId", "payload", "sig"}`: `payload` is the projects
JSON as a string, `ts` is milliseconds within five minutes of the server
clock, `requestId` is 16 to 64 URL-safe characters, and `sig` is the
base64url Ed25519 signature, by the `node` account's key, over

```
forkmesh-ecosystem-import-v1\n<node>\n<ts>\n<requestId>\n<sha256 hex of payload>
```

the same shape as the desktop Database tab's signed writes. The request id is
spent in the sensitive audit log (`admin.ecosystem_api_import`, target
`ecosystem_request`) before anything is written, so a replay answers 409. The
answer is `{"ok": true, "added", "updated", "unchanged"}`, or 400 with
`errors` and nothing written. The generic Database tab cannot seed this table:
on an empty table it seals the `data` column, and this module reads it as
plain JSON.

`GET /api/ecosystem` answers:

```json
{
  "version": 1,
  "updatedAt": 1789560000000,
  "categories": [{"id": "decentralized-git", "label": "Decentralized Git", "count": 3}],
  "projects": [{
    "slug": "radicle", "name": "Radicle", "tagline": "...", "summary": "...",
    "watching": "...", "url": "https://radicle.xyz", "repo": "...",
    "category": "decentralized-git", "categoryLabel": "Decentralized Git",
    "tags": ["peer-to-peer"], "openSource": true, "license": "MIT OR Apache-2.0",
    "gitNative": true, "maker": "Radicle",
    "compatibility": "unknown", "compatibilityLabel": "Not yet assessed",
    "integration": "watching", "integrationLabel": "Watching",
    "integrations": [], "reviewed": "2026-09", "reviewedLabel": "Sep 2026",
    "featured": true
  }]
}
```

Only published projects appear, ordered by `position` then slug. Categories
with no published project are left out. Labels come from the server so the
vocabulary lives in one place (`CATEGORIES`, `COMPATIBILITY`, `INTEGRATION`).
A database failure answers 503 with `no-store` so it is never cached.

## JSON format

The admin page carries the full reference under "JSON format". In short:

| Field | Notes |
| --- | --- |
| `slug` | Lowercase letters, digits and inner hyphens, at most 64. Derived from `name` when left out. |
| `name`, `tagline`, `url`, `category` | Required for a new project. |
| `summary`, `watching` | "What it does" and "Why we're watching" on the public card. |
| `repo`, `url` | http(s) only, no credentials, at most 400 characters. |
| `category` | One of `CATEGORIES` in `ecosystem.py`. |
| `tags` | Up to 8 short words, normalized to lowercase with hyphens. |
| `openSource` | `true`, `false` or `null` (unknown, no badge). |
| `gitNative` | `true` when the project hosts, replicates or stores its collaboration data in Git. |
| `compatibility` | `unknown`, `none`, `partial`, `full`. |
| `integration` | `watching`, `exploring`, `planned`, `building`, `available`, `uses` (ForkMesh depends on, calls or ships it). |
| `integrations` | Up to 8 phrases for integrations that exist, or for `uses`, where ForkMesh uses it. |
| `reviewed` | `YYYY-MM` or `YYYY-MM-DD`; defaults to the current month. |
| `published`, `featured`, `position` | Visibility, landing-page priority, sort order (new projects go last). |
| `internal` | Private object. Known keys are validated: `overlap`, `threat`, `opportunity`, `confidence` (`low`, `medium`, `high`), `action` (`investigate`, `adopt`, `interoperate`, `absorb`, `partner`, `ignore`), `notes`, `sources` (URLs). Any other key may hold text, a number, a boolean or a list of short texts. |

Unknown top-level fields are rejected rather than dropped, so a typo such as
`open_source` is reported instead of silently losing data. `NaN` and
`Infinity` are rejected at parse time.

## Website

- `/ecosystem` (`www/public/ecosystem.html`) groups published projects by
  category, with search, Open source, Git-native and Used by ForkMesh
  filters and a row of category pills. Filter state lives in the query
  string (`category`, `q`, `open`, `git`, `uses`), so a link can open a
  filtered view; the category pill on a project page links to one.
- A Cards / List switch sits above the results. List (`view=list`) is a
  spreadsheet of the filtered projects: one row each, columns for name,
  description, category, maker, license, open source, Git-native,
  relationship, where ForkMesh uses it, compatibility, review date and
  website. Every header sorts (`sort=<column>`, `-` prefix for descending;
  empty cells stay last either way), the header row and name column stay
  pinned while the sheet scrolls inside its own box, and the sheet widens
  past the page column to up to 1400 px because twelve columns do not fit in
  1048. Download CSV saves the same filtered, sorted rows plus slug, source,
  tags and summary; cells starting with `=`, `+`, `-` or `@` get a leading
  apostrophe so a spreadsheet does not run them as formulas. The list is the
  better view once the directory holds hundreds of entries, most of them
  dependencies, where cards grouped by category take many screens.
- On a project page with `integration: "uses"`, the sections read "How
  ForkMesh uses it" and "Where ForkMesh uses it" instead of "Why we're
  watching" and "ForkMesh integrations".
- `/ecosystem/<slug>` is the same document showing one project: a centred
  header (emblem, category pill, name, tagline, website and source links), a
  panel with what it does, why we're watching, the ForkMesh relationship,
  ForkMesh integrations and last reviewed, a facts sidebar, and up to three
  more projects from the same category. The www Worker
  serves `ecosystem.html` for these paths (`ECOSYSTEM_PROJECT` in
  `www/worker.js`, `run_worker_first` entry `/ecosystem/*`) instead of
  treating a two-segment path as a repository shortcut. On the App,
  `ecosystem` is a reserved route prefix, `external_site_route` sends
  `/ecosystem` and `/ecosystem/<slug>` to the marketing origin, and a
  self-hosted relay that serves the site from its own bundle maps the project
  path to the `ecosystem` document (`site_document_asset_path`).
- The landing section sits between "Your mesh, wherever you work" and the
  support block. Its heading uses the face and scale of the heading above it,
  with no coloured kicker. It shows eight tiles, featured projects first; when
  more projects are published than fit, the eighth tile counts the rest
  ("+22", drawn as a small stack) and links to `/ecosystem`. Each tile is a
  screw plate holding the project's monogram, with the name and category
  under it. On hover or keyboard focus the plate grows to the card's edges,
  its corner dots travel with it, the monogram fades, and the tagline takes
  the category's place, following the Cal.com feature cards the design was
  taken from. The reveal sits behind `(hover: hover)`, so touch devices keep
  the resting tile (iOS would otherwise spend the first tap on the hover
  state), and `prefers-reduced-motion` turns the transitions off. The grid is
  not a live region: it fills in while the visitor is still reading the hero,
  and announcing eight cards then would be noise; `aria-busy` marks the
  loading state. The section hides itself when the API fails or has nothing
  published, so deploying the website before the App, or before the first
  import, leaves no broken section.
- Both pages build every node with `textContent`, and links to projects must
  be http(s) on the client as well as the server.
- Both fetches pass `credentials: "omit"`: on the canonical hosts
  `api-client.js` sends same-origin `/api/*` calls to `app.forkmesh.com`, and
  a public read has no reason to carry the session cookie.
- Both pages use the visual language of `/features`: a `#f4f4f4` canvas in
  light mode, white cards with soft shadows, pill badges, Geist for text and
  ABCFavorit for headings, and "screw plate" tiles (a bordered tile with a dot
  in each corner). The shared pieces (`fx-badge`, `fx-title`, `fx-btn`,
  `fx-tag`, `fx-tile` and the `--fx-*` tokens) are copied from
  `features.html` with the same names and values, so a later shared
  stylesheet can take both pages. A directory card carries what identifies a
  project: tile, name, tagline, maker or domain, and Open source and
  Git-native tags. Compatibility, integration and the review date are on the
  project page only, because every listing currently shares "Not yet
  assessed" and "Watching", and repeating them on each card was noise.
- Projects have no logo field, so every page draws a monogram, the first
  letter of the name, on a fixed square tile. Real logos differ in size and
  aspect ratio; the way to add them is a `logo` field pointing at self-hosted
  files, drawn inside the same fixed tile with `object-fit: contain`, rather
  than loading images from each project's site.

## Operations

- Deploy the App (migration 0213 and the module) before the website.
- To load a JSON file without the console, an administrator runs
  `python3 tools/ecosystem_seed.py projects.json --node <admin account>`
  (`--key` for a key other than this machine's ForkMesh identity, `--origin`
  for another relay). It prints the added, updated and unchanged counts, is
  safe to run again, and sends only the file's contents.
- The first seed, 35 projects researched on 2026-09-25, is kept outside the
  repository because its `internal` notes are private; load it with the
  console form or `tools/ecosystem_seed.py`. Treat exports the same
  way: they contain every draft and every private note.
- Six seed projects are drafts because they are not developer tools: the
  Aceternity template, Qisutu, Town, Open Executive, flat.social and OpenClaw.
- `tools/ecosystem_third_parties.json` lists every third party the code
  mentioned on 2026-09-27 (four inventories over `desktop/`; `app/`,
  `server/` and `deploy/`; `mobile/`, `native/`, `tools/`, `conformance/`
  and `examples/`; `www/`, `world/` and `marketplace/`), one entry per
  product, published, sorted by category then name. It has no `internal`
  block, which is why it can live in the repository, and it leaves out the
  hand-maintained slugs above so an import never overwrites them. Load it
  with `python3 tools/ecosystem_seed.py tools/ecosystem_third_parties.json
  --node <admin account>`; it stays under the 500-project import limit.
  Generic standards (HTTP, TLS, SHA-256), basic OS utilities, names that
  only appear in user-agent or GPU detection patterns and fallback-only font
  names were left out. `tools/test_ecosystem_seed.py` runs the file through
  the server's own import checks. When code adds or drops a third party,
  edit the file and import it again: the import merges by slug, and an entry
  removed from the file must be deleted in the console.
- 2026-09-30: the file had been committed on 2026-09-27 but never imported,
  so production still listed only the 29 hand-maintained projects. The
  signed import (`--node jett`) added all 406 listings with no slug overlap;
  `/api/ecosystem` then returned 435 published projects.
- `tools/ecosystem_hn_launches.json` holds 36 projects from recent Show HN
  and Launch HN posts found during the Y Combinator News launch research
  (`www/docs/launch/y-combinator-news-readiness.md`): agent workspaces,
  sandboxes, orchestrators and Git hosts. Each has a working project URL
  checked on 2026-09-30, a license only where the repository states one, and
  `internal.sources` pointing at its HN thread. `rickub` was left out because
  a hand-maintained listing already exists. Imported 2026-09-30 (36 added),
  for 471 published projects.
- `tools/ecosystem_requested.json` holds projects added one at a time on
  request, each with `internal.sources` pointing at where it was found and
  a slug no other seed file uses. Load it with `python3
  tools/ecosystem_seed.py tools/ecosystem_requested.json --node <admin
  account>`; re-importing is safe because the import merges by slug.
  2026-10-01: Magnitude (`magnitudedev/magnitude`, a local inference engine
  for coding agents) under AI models & APIs, `watching`, beside Ollama,
  llama.cpp and vLLM. ForkMesh has no Magnitude integration, so
  `integrations` is empty.
  2026-10-02: rhun (`vshvedov/rhun`, MIT, a code editor written in assembly
  with a terminal, Git view and Claude Code/Codex agents panel) under
  Developer tooling, `watching`, sourced from its Show HN thread
  (item 49926726); the repository link comes from rhun.app.

## Traps

- The API cache is per core node. After a write, the other nodes can serve the
  previous list for up to 60 seconds.
- Removing a category from `CATEGORIES` hides its projects from the public API
  (they remain in the admin). Move them to another category first.
- `/ecosystem/<slug>` matches lowercase slugs only; any other path under
  `/ecosystem/` still goes to the App, as before.
- The admin console is script-free, so the only delete safeguard is the
  required confirmation checkbox on the edit page.

## Possible next steps

- A scheduled research agent that proposes import-ready JSON (as drafts) and a
  weekly report, keeping the human publish step.
- Server-rendered project pages, for search engines that do not run
  JavaScript.
- Linking listings to ForkMesh issues through `internal.issue`.
