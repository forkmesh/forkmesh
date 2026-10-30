# ForkMesh Ecosystem (curated project directory)

A public directory of projects around Git-native collaboration (decentralized
Git, forges, trackers, review, chat, AI coding agents) and of every third
party ForkMesh depends on. Each listing has public fields and a private
strategy layer visible only to administrators. Code: `app/src/ecosystem.py`,
table `ecosystem_projects`, tests `app/tests/test_ecosystem.py`. Website:
`www/public/ecosystem.html` and the `#ecosystem` section of
`www/public/index.html` (tests `www/tests/test_ecosystem_page.py`,
`www/tests/worker_routing.test.mjs`).

## Design rules

- **Data in the App database**, so listings change without a deploy and
  private notes never enter the repository.
- **Public and private stored apart**: public fields in `data`, private in
  `internal`. The public query never selects `internal`, and the public
  payload is built from an explicit field list (`public_project`).
- **Drafts by default**: a project is public only with `"published": true`.
- **Descriptive, not evaluative**: no scores or rankings in public fields.
  `compatibility` defaults to `unknown`, `integration` to `watching`;
  `integrations` lists only integrations that exist.
- **Comparisons state facts, not verdicts**: `comparison` rows put the
  project and ForkMesh side by side on one topic each (focus, where
  repositories live, agents, platforms, license). For a dependency the rows
  compare roles. Every seeded listing has one; write a new listing's rows
  from its own site and only claim ForkMesh features that exist.
- **Dependencies are listed** with `integration: "uses"` and
  `integrations` entries of the form `"<surface>: <purpose>"`.
- **Two write modes**: an import merges by slug (missing fields kept, `null`
  clears); editing one project replaces it whole.
- **All or nothing**: any validation error in an import writes nothing.

## Routes

| Method and path | Caller | Auth |
| --- | --- | --- |
| `GET /api/ecosystem` | website | public, `cache-control: public, max-age=60`, in-process cache |
| `GET /<ADMIN_PATH>?view=ecosystem` (`&status=`, `&project=<slug>`, `&format=json`) | administrator | console session |
| `POST /<ADMIN_PATH>?view=ecosystem` | administrator | console CSRF token; `action` is `import`, `save`, `publish`, `unpublish`, `feature`, `unfeature` or `delete` |
| `POST /api/ecosystem/import` | scripts (`tools/ecosystem_seed.py`) | Ed25519 signature by an administrator's node key (`_admin_authorized`) |

Forms are capped at 1 MB, imports at 500 projects, the table at 1000. Every
write is audited as `admin.ecosystem_<action>` and purges the API cache.
Exports include drafts and private notes; treat them as private.

The signed import body is `{"node", "ts", "requestId", "payload", "sig"}`;
`sig` signs

```
forkmesh-ecosystem-import-v1\n<node>\n<ts>\n<requestId>\n<sha256 hex of payload>
```

`ts` must be within five minutes; the request id is spent in the audit log
before writing, so a replay answers 409. The answer is
`{"ok": true, "added", "updated", "unchanged"}` or 400 with `errors`.

`GET /api/ecosystem` returns `{version, updatedAt, categories: [{id, label,
count}], projects: [...]}` with published projects only, ordered by
`position` then slug; labels come from the server (`CATEGORIES`,
`COMPATIBILITY`, `INTEGRATION`). A database failure answers 503 `no-store`.

## JSON format

| Field | Notes |
| --- | --- |
| `slug` | Lowercase letters, digits, inner hyphens, ≤ 64; derived from `name` if absent |
| `name`, `tagline`, `url`, `category` | Required for a new project |
| `summary`, `watching` | Public card text |
| `repo`, `url` | http(s) only, no credentials, ≤ 400 characters |
| `tags` | Up to 8, normalized |
| `openSource`, `gitNative` | `true`, `false` or `null` |
| `compatibility` | `unknown`, `none`, `partial`, `full` |
| `integration` | `watching`, `exploring`, `planned`, `building`, `available`, `uses` |
| `integrations` | Up to 8 phrases |
| `comparison` | Up to 6 rows `{"topic", "project", "forkmesh"}`: topic ≤ 40 characters, cells ≤ 200, all three required; `null` clears |
| `reviewed` | `YYYY-MM` or `YYYY-MM-DD` |
| `published`, `featured`, `position` | Visibility, landing priority, sort order |
| `internal` | Private: `overlap`, `threat`, `opportunity`, `confidence`, `action`, `notes`, `sources`, other short values |

Unknown top-level fields, `NaN` and `Infinity` are rejected.

## Website

- `/ecosystem` groups published projects by category with search and
  filters kept in the query string, plus a sortable List view with CSV
  download (cells starting with `=`, `+`, `-` or `@` are prefixed with an
  apostrophe).
- `/ecosystem/<slug>` (lowercase slugs) serves the same document for one
  project; the site router maps it explicitly so it is not taken for a
  repository path. Its "Compared with ForkMesh" table (topic, project,
  ForkMesh) shows when `comparison` has rows; below 640 px each row stacks
  with the column name before each cell.
- The landing section shows eight tiles and hides itself when the API fails
  or nothing is published.
- Pages build nodes with `textContent`, accept only http(s) links, and fetch
  with `credentials: "omit"`.

## Operations

- Deploy the App before the website.
- Load a file: `python3 tools/ecosystem_seed.py <file>.json --node <admin account>`
  (`--key`, `--origin` optional); safe to re-run.
- Seed files in the repository (`tools/ecosystem_third_parties.json`,
  `tools/ecosystem_hn_launches.json`, `tools/ecosystem_requested.json`,
  `tools/ecosystem_client_work.json`) carry no private notes beyond sources,
  levels and a one-line note; `tools/test_ecosystem_seed.py` runs them through the
  server's import checks. An entry removed from a file must be deleted in the
  console.
- `tools/ecosystem_client_work.json` holds the agency client-work tools from
  a 2026-10-27 X thread, in the `work-management` category; the analysis is
  [ecosystem-client-work.md](ecosystem-client-work.md).

## Traps

- The API cache is per core; other nodes may serve the old list for 60 s.
- Removing a category hides its projects from the public API.
- The console is script-free; the only delete safeguard is the confirmation
  checkbox.
