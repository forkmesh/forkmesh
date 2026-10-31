# Two-way forge sync

`tools/forge_sync.py` keeps ForkMesh issues and comments (fields, labels,
assignees, state, edits, comment deletions) in step with GitHub (`github`
adapter) or Gitea, Forgejo and Codeberg (`gitea` adapter). Branches and tags
use the ref sync in [Connecting to GitHub](github-sync.md); other guides are in
the [docs index](README.md).

## Usage

```bash
python3 tools/forge_sync.py connect --provider gitea --repository octo/app \
  --base-url https://git.example.org --slo-minutes 15
FORKMESH_FORGE_TOKEN=… FORKMESH_BRIDGE_SIGNING_KEY="$(cat sync-key.pem)" \
  python3 tools/forge_sync.py sync --report sync-report.json
python3 tools/forge_sync.py status
python3 tools/forge_sync.py resolve --issue 12 --field body --take forkmesh
python3 tools/forge_sync.py pause --reason "maintenance"; python3 tools/forge_sync.py resume
```

`connect` writes the ledger `.forkmesh/forge-sync.json`; commit it after each
pass. `sync --avatars` and `--attachments` also copy pictures and files into
the repository ([Avatars](#avatars), [Attachments](#attachments)). Exit codes:
`0` converged, `1` conflicts, `2` configuration error, `75` retryable provider
failure. A paused connection sends no requests.

## Merge rules

- The ledger stores a digest of each field and comment as last agreed. A
  change on one side is copied over; differing changes on both sides become a
  conflict, and nothing is written until it is resolved.
- `sourceOfTruth` sets a per-field rule: `manual` (default), `forkmesh` or
  `provider`. Discarded values are kept in ledger `history`.
- Synced records carry a `forkmesh-forge-sync:v1` marker, so echoes and lost
  ledgers never create duplicates. Issues are never deleted.

## Mirror mode

`connect --mirror` makes a one-way, read-only copy of a provider's issues, for
a project whose home stays on the forge. `--two-way` switches back. The ledger
stores `"mode": "mirror"`; a ledger without `mode` is two-way.

- Every issue is imported, open and closed, with every comment, label,
  assignee and state, in full and in any script. The only cap is the git
  node's 16 MiB per-issue record limit (`maxIssueRecordBytes`). Text is stored
  in the canonical form every ForkMesh client signs: `\n` line endings,
  leading and trailing blank lines trimmed, titles trimmed.
- The provider wins every field. A ForkMesh edit to a mirrored title, body,
  state, labels, assignees or comment is replaced on the next pass, without a
  conflict. Comments deleted on the provider are deleted in ForkMesh (signed
  `delete` events).
- Nothing is written to the provider. The engine wraps the adapter so any
  write method raises before a request is made; a read-only token is enough.
- ForkMesh-only comments and issues stay in ForkMesh.
- A `forkmesh-forge-sync` marker in a provider record is ignored, so a forged
  marker cannot claim and overwrite a ForkMesh-only issue or comment.

## Where the ledger lives

The ledger defaults to `.forkmesh/forge-sync.json`. `--ledger PATH` (or
`FORKMESH_FORGE_LEDGER`) keeps it elsewhere, for example on the sync server,
so the repository carries only the records. It is about 3 KB per issue
(1.7 MB for a 609-issue forum) and its timestamps change every pass.

Each pass also writes the small public `.forkmesh/forge-source.json`
(`forkmesh-forge-source-v1`: `provider`, `repository`, `baseUrl`, `issueUrl`
with a `{number}` placeholder, and `renumbered`, the issues whose ForkMesh
number differs from the provider's): all a reader needs to link an issue to
its original. A mirror that wants its own provider sync builds a fresh ledger
on its first pass.

## Identities

Imported records are signed by the sync key and credited as
`<provider>:<login>`. The web dashboard shows a `github:<login>` author as
`@<login>`, linked to `https://github.com/<login>` in a new tab
(`forgeIdentity` in `app/public/dashboard/js/02-helpers.js`), with the picture
stored in the repository ([Avatars](#avatars)) or the initial. List rows show
plain `@<login>` text (`userNameHtml`, server `user_name_html`). A page never
loads anything from GitHub, because readers of a mirror may be on networks
where GitHub is blocked or watched ([Censorship resilience](censorship-resilience.md)).
The ledger `identities` map links provider logins to ForkMesh accounts for
assignees only; ambiguous links are ignored.

## Avatars

With `--avatars`, a pass copies each provider author's picture into the
repository so the reader's browser never contacts the provider. Without it,
`.forkmesh/contributors` is left alone and authors show initials.

- **Who.** Every issue and comment author the pass reads, plus every
  `<provider>:<login>` author already in the records. The ledger `avatars` map
  keeps `login`, the API's `avatar_url`, `fetchedAt`, `fetchedUrl`, `failed`.
- **Where from.** The API's `avatar_url` (`RemoteIssue.author_avatar` /
  `RemoteComment.author_avatar`) at 96 px (`&s=96` on GitHub), else
  `https://avatars.githubusercontent.com/<login>?s=96`. Only `https` on the
  provider's picture host (`avatars.githubusercontent.com`, or the Gitea
  `baseUrl` host); a redirect elsewhere is refused. At most 1 MiB, 10 s and
  100 pictures per pass. The token is never sent.
- **Stored as** `.forkmesh/contributors/<provider>-<login lowercased>.png`.
  With Pillow the picture is re-encoded as a 96x96 PNG; without it, PNG or
  JPEG bytes are kept as they are (`<img>` decodes by content) and anything
  else is dropped.
- **Refresh.** Only when the file is missing, the URL changed, or it is 30
  days old (`AVATAR_REFRESH_MS`); a failed fetch waits a day
  (`AVATAR_RETRY_MS`). An unchanged pass writes nothing.
- **Roster.** `.forkmesh/contributors/contributors.json` gets one row per
  person (`slug`, `name`, `github` or `login`, `profile` for GitHub, `avatar`),
  sorted by login, in the [contributor roster](folder-about.md#contributor-roster)
  schema. Rows whose slug does not start with `<provider>-` are kept first, with
  the file's own `count`; invalid JSON is left alone.

Pages draw the stored picture for a `github:<login>` author
(`issueActorAvatarHtml` / `avatar_html`) from the repository's raw URL when
the issue index lists the person in `forgeAvatars`.

## Attachments

On a `github` connection, `sync --attachments` copies the images and files
that issue titles, bodies and comments embed from GitHub into the repository
(`tools/forge_attachments.py`). A picture loaded from GitHub would tell a
watching network which thread a mirror reader is on. Without
`--attachments`, or with `FORKMESH_FORGE_ATTACHMENTS=off`, nothing is copied.

- **Addresses.** `https://github.com/user-attachments/assets/<uuid>`,
  `https://github.com/user-attachments/files/<id>/<name>`,
  `https://github.com/<owner>/<repo>/assets/<id>/<uuid>`,
  `https://github.com/<owner>/<repo>/files/<id>/<name>`,
  `https://user-images.githubusercontent.com/<user>/<file>`,
  `https://private-user-images.githubusercontent.com/<user>/<file>?jwt=…`
  and `https://objects.githubusercontent.com/github-production-repository-file-5c1aeb/…`,
  found in Markdown images and links, `<img src>`, `<a href>` or as bare
  addresses (`SOURCE_RE`; `&amp;` reads as `&`, trailing `.,:;!?` are not part
  of the address). Every event's `title` and `body` is read, earlier versions
  included. Images on other hosts are not copied.
- **Hosts.** Only `https`, no port or user name, on `github.com` (the paths
  above), the three `*.githubusercontent.com` hosts, and the two S3 buckets
  GitHub redirects to (`github-production-user-asset-6210df.s3.amazonaws.com`,
  `github-production-repository-file-5c1aeb.s3.amazonaws.com`). A redirect
  leaving that list is refused before it is requested. No token is sent (it
  would follow the redirect to S3); public attachments need none.
- **Expired links.** A `private-user-images` JWT lives five minutes. After
  that, the picture is fetched from `user-images.githubusercontent.com/<user>/<file>`,
  then `github.com/user-attachments/assets/<uuid>` (the uuid is in the file
  name). An `objects.githubusercontent.com` address with an expired signature
  is fetched as `github.com/user-attachments/files/<id>/<name>`, the name taken
  from `response-content-disposition`. All of these answer without a token.
- **Kept types.** Checked by first bytes: PNG, JPEG, GIF, WebP, SVG, PDF, ZIP,
  gzip, 7z, xz, bzip2, MP4/QuickTime, WebM, pcap, pcapng, and UTF-8 text with
  a text extension (`.txt`, `.log`, `.csv`, `.json`, `.c`, …; stored as
  `.txt`). Anything else (an HTML login page, an executable) is dropped. At
  most 25 MiB a file (`MAX_BYTES`) and 30 s a request. Files are stored by
  content as `.forkmesh/attachments/<sha256>.<ext>`. SVG is served by the raw
  route as a download (`raw_spec`) and drawn through `data-repo-svg-src`,
  never inline.
- **Index.** `.forkmesh/attachments/index.json`:
  `{"schema": "forkmesh-attachments-v1", "files": {"<address as written>":
  {"path", "type", "bytes"}}, "unavailable": {"<address>": {"reason",
  "tries", "nextTry"}}}`, sorted, written only on change. Each issue with a
  copied file also gets `.forkmesh/attachments/issues/<n>.json`
  (`forkmesh-attachments-issue-v1`), which the issue page reads. Signed issue
  records are never changed, and nothing is deleted.
- **Retries.** An unfetchable file is listed under `unavailable` and retried
  after 1, 2, 4, … days, at most every 30 days; a too-large or unkept file
  waits 30 days. A rate limit (`429`, or `403` with `Retry-After`) waits once
  if `Retry-After` is at most 60 s, otherwise the pass stops copying; neither
  marks the file unavailable.
- **Budget.** One request at a time, 0.5 s apart, newest issues first; at most
  `--attachment-files` (300) downloads, `--attachment-megabytes` (256) and
  300 s per pass. The repository keeps at most 1 GiB of copies
  (`TOTAL_BYTES`, summed from `index.json`); past that a pass stops with
  `stopped: "total"`. Copies stay in Git history for good, so without the cap
  anyone who can comment upstream could grow every clone by 256 MB a pass. A
  611-issue forum with about 5,900 comments needed roughly 110-120 MB.

Pages draw a copied file from the repository's raw URL, and an uncopied
GitHub attachment as a link, never an `<img>`
([repo-views.md](app/repo-views.md#issue-page-first-view)).

## Rate limits and status

A rate limit (`429`, or `403` with an exhausted quota) waits for
`Retry-After`; other failures back off exponentially, and a stopped pass keeps
its progress. Reports (`forkmesh-forge-sync-report-v1`, see
[synchronization status](sync-status.md)) give lag against the SLO (default 15
minutes), convergence and conflict rates, and retries. They never contain
tokens.
