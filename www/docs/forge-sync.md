# Two-way forge sync

`tools/forge_sync.py` keeps ForkMesh issues and comments (fields, labels,
assignees, state, edits, comment deletions) in step with GitHub (`github`
adapter) or Gitea, Forgejo and Codeberg (`gitea` adapter). Branches and tags
use the ref sync in [Connecting to GitHub](github-sync.md).

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
pass. `sync --avatars` and `--attachments` also make the provider copies
([Avatars](#avatars), [Attachments](#attachments)). Exit codes: `0` converged, `1` conflicts, `2` configuration error, `75`
retryable provider failure. A paused connection sends no requests.

## Merge rules

- The ledger stores a digest of each field and comment as last agreed. A change
  on one side is copied over; differing changes on both sides become a
  conflict and nothing is written until it is resolved.
- `sourceOfTruth` sets a per-field rule: `manual` (default), `forkmesh` or
  `provider`. Discarded values are kept in ledger `history`.
- Synced records carry a `forkmesh-forge-sync:v1` marker so echoes and lost
  ledgers never create duplicates. Issues are never deleted.

## Mirror mode

`connect --mirror` makes a one-way, read-only copy of a provider's issues, for a
repository that mirrors a project whose home stays on the forge (for example a
community that discusses in GitHub issues). `--two-way` switches back. The
ledger stores `"mode": "mirror"`; a ledger without `mode` is two-way.

- Every issue is imported, open and closed, with every comment, labels,
  assignees and state. Titles, bodies and comments are copied in full, in any
  language and script: there is no length, label or comment cap beyond the
  16 MiB per-issue record limit of the git node (`maxIssueRecordBytes`). Line
  endings are stored as `\n`, leading or trailing blank lines are trimmed and
  titles lose leading or trailing spaces,
  the canonical form every ForkMesh client signs.
- The provider wins every field. A change made in ForkMesh to a mirrored title,
  body, state, label set, assignee list or comment is replaced on the next
  pass by the provider's value, without a conflict. Comments deleted on the
  provider are deleted in ForkMesh (signed `delete` events).
- Nothing is written to the provider: no issue is exported, no ForkMesh comment
  is posted, and no comment is edited or deleted there. The engine wraps the
  adapter so any write method raises before a request is made, so a read-only
  token is enough and a token with write scope is never used to write.
- ForkMesh-only comments and issues stay in ForkMesh.
- Provider records are always imported as new mirrored records: a
  `forkmesh-forge-sync` marker in a provider issue or comment is ignored, so a
  forged marker cannot claim, and then overwrite, a ForkMesh-only issue or
  comment.

## Where the ledger lives

The ledger defaults to `.forkmesh/forge-sync.json` in the checkout. `--ledger
PATH` (or `FORKMESH_FORGE_LEDGER`) keeps it anywhere else, for example on the
sync server, so the repository carries only the records; it is about 3 KB per
issue (field and comment digests), 1.7 MB for a 609-issue forum, and its
timestamps change on every pass. Each pass also writes the small public
`.forkmesh/forge-source.json` (`forkmesh-forge-source-v1`: `provider`,
`repository`, `baseUrl`, `issueUrl` with a `{number}` placeholder, and
`renumbered`, the issues whose ForkMesh number differs from the provider's),
which is all a reader needs to link an issue to its original. A mirror of the
repository that wants its own provider sync builds a fresh ledger on its first
pass.

## Identities

Imported records are signed by the sync key and credited as
`<provider>:<login>`. The web dashboard shows a `github:<login>` author as
`@<login>`, linked to `https://github.com/<login>` in a new tab
(`forgeIdentity` in `app/public/dashboard/js/02-helpers.js`); its avatar is the
picture the sync stored in the repository ([Avatars](#avatars)), else the
initial, and list rows show plain `@<login>` text (`userNameHtml`; the
server renderer `user_name_html` matches), so a page never loads anything from GitHub (readers of a mirror
may be on networks where GitHub is blocked or watched). The ledger `identities` map links provider logins to
ForkMesh accounts for assignees only; ambiguous links are ignored.

## Avatars

With `--avatars`, a `sync` pass also copies each provider author's picture
into the repository, so pages can show it without the reader's browser ever
contacting the provider. Without it the pass leaves `.forkmesh/contributors`
alone and authors show their initials, as before.

- **Who.** Every issue author the pass lists, every comment author it reads,
  and every `<provider>:<login>` author already in the records (comments on
  issues that have not changed since an earlier pass are not listed again, so
  the records fill those in). People are kept in the ledger `avatars` map
  (`login`, the API's `avatar_url`, `fetchedAt`, `fetchedUrl`, `failed`).
- **Where from.** The API's `avatar_url` (`user.avatar_url` on issues and
  comments, `RemoteIssue.author_avatar` / `RemoteComment.author_avatar`), asked
  for at 96 px (`&s=96` on GitHub). Without one, GitHub's
  `https://avatars.githubusercontent.com/<login>?s=96`. Only `https` on the
  provider's picture host is fetched: `avatars.githubusercontent.com` for
  GitHub, the `baseUrl` host for Gitea; a redirect anywhere else is refused.
  At most 1 MiB, 10 s, 100 pictures per pass (a big forum fills in over a
  few passes). The token is never sent.
- **What is stored.** `.forkmesh/contributors/<provider>-<login lowercased>.png`.
  With Pillow installed the picture is decoded and written as a 96x96 PNG.
  Without it, a picture whose bytes start like a PNG or a JPEG is kept as it
  came (browsers draw a JPEG served as `image/png`, since `<img>` decodes by
  content); anything else is dropped and the page keeps the initial.
- **When again.** Only when the file is missing, the provider's URL changed,
  or it is 30 days old (`AVATAR_REFRESH_MS`); a failed fetch waits a day
  (`AVATAR_RETRY_MS`). An unchanged pass downloads and writes nothing.
- **Roster.** `.forkmesh/contributors/contributors.json` gets one row per
  person (`slug`, `name`, `github` or `login`, a `profile` link for GitHub,
  `avatar` when the picture exists), sorted by login, in the schema of
  [the contributor roster](folder-about.md#contributor-roster). Rows whose
  slug does not start with `<provider>-` are kept as they are, before the
  synced ones, with the file's own `count`; a file that is not valid JSON is
  left alone.
- **Mirror mode** is unchanged: pictures are read from the picture host,
  nothing is written to the provider.

The dashboard and the server-rendered pages draw the stored picture for a
`github:<login>` author (`issueActorAvatarHtml` / `avatar_html`) from the
repository's own raw URL, over the initial, when the issue index lists the
person in `forgeAvatars`.

## Attachments

On a `github` connection a `sync --attachments` pass also copies the images and files that
issue titles, descriptions and comments embed from GitHub into the
repository (`tools/forge_attachments.py`), so a reader's browser never asks
GitHub for them. Readers of a mirror may be on networks where GitHub is
watched or blocked; a picture that loads from GitHub would tell the network
which thread they are reading. Without `--attachments`, or with
`FORKMESH_FORGE_ATTACHMENTS=off`, nothing is copied.

- **Which addresses.** `https://github.com/user-attachments/assets/<uuid>`,
  `https://github.com/user-attachments/files/<id>/<name>`,
  `https://github.com/<owner>/<repo>/assets/<id>/<uuid>`,
  `https://github.com/<owner>/<repo>/files/<id>/<name>`,
  `https://user-images.githubusercontent.com/<user>/<file>`,
  `https://private-user-images.githubusercontent.com/<user>/<file>?jwt=…`
  and `https://objects.githubusercontent.com/github-production-repository-file-5c1aeb/…`,
  wherever they appear: `![](…)`, `[](…)`, `<img src="…">`, `<a href="…">` or
  a bare address (`SOURCE_RE`; `&amp;` reads as `&`, trailing `.,:;!?` are not
  part of the address). Every event's `title` and `body` is read, earlier
  versions of an edited text included. Images on any other host (ehgt.org,
  labs.ripe.net, …) stay as they are and are not copied.
- **Where from.** Only `https`, no port or user name, on `github.com` (only
  the `user-attachments/` and `<owner>/<repo>/assets|files/` paths), the three
  `*.githubusercontent.com` hosts above, and the two S3 buckets GitHub
  redirects to (`github-production-user-asset-6210df.s3.amazonaws.com`,
  `github-production-repository-file-5c1aeb.s3.amazonaws.com`). Redirects are
  followed only inside that list; one that leaves it is refused before it is
  requested. A public repository's attachments need no token, and none is
  sent (the token would otherwise follow the redirect to S3).
- **Links that expire.** A `private-user-images` address carries a JWT that
  lives five minutes. While it is still valid it is used; otherwise the same
  picture is fetched from `user-images.githubusercontent.com/<user>/<file>`,
  then `github.com/user-attachments/assets/<uuid>` (the uuid is in the file
  name). An `objects.githubusercontent.com` address with an expired S3
  signature is fetched as `github.com/user-attachments/files/<id>/<name>`,
  the name taken from its `response-content-disposition`. Tested on
  github.com/example/forum on 2026-10-21: all of these answer without a
  token.
- **What is kept.** Each file is checked by its first bytes: PNG, JPEG, GIF,
  WebP, SVG, PDF, ZIP, gzip, 7z, xz, bzip2, MP4/QuickTime, WebM, pcap and
  pcapng, and plain UTF-8 text when the name ends in a text extension
  (`.txt`, `.log`, `.csv`, `.json`, `.c`, …; stored as `.txt`). Anything else
  (an HTML login page, an executable) is not stored. At most 25 MiB a file
  (`MAX_BYTES`, by `Content-Length` and by what is read), 30 s a request.
  Files are stored by content as `.forkmesh/attachments/<sha256>.<ext>`, so
  two addresses of the same picture share one file. SVG is stored but the raw
  route serves it as a download (`raw_spec`); pages draw it as an image
  through `data-repo-svg-src`, never inline.
- **Index.** `.forkmesh/attachments/index.json`:
  `{"schema": "forkmesh-attachments-v1", "files": {"<address as written>":
  {"path", "type", "bytes"}}, "unavailable": {"<address>": {"reason",
  "tries", "nextTry"}}}`, sorted, written only when it changes. Each issue
  with a copied file also gets `.forkmesh/attachments/issues/<n>.json`
  (`{"schema": "forkmesh-attachments-issue-v1", "files": {…}}`, the same rows
  for the addresses in that issue), which is what an issue page reads. The
  signed issue records are never changed. Nothing is ever deleted: a file
  stays when the comment that used it is edited away.
- **Retries.** A file that could not be fetched is listed under
  `unavailable` and asked again after a day, then 2, 4, … days, at most every
  30 days; one that is too large or of a type that is not kept waits 30 days.
  A rate limit (`429`, or `403` with `Retry-After`) waits once if
  `Retry-After` is at most 60 s, otherwise the pass stops copying and leaves
  the rest for the next pass; neither marks the file unavailable.
- **Pace and budget.** One request at a time, 0.5 s apart, newest issues
  first; at most `--attachment-files` (300) downloads,
  `--attachment-megabytes` (256) and 300 s per pass. The rest waits for the
  next pass, so a large forum fills in over a few passes; an unchanged pass
  requests and writes nothing.
- **Size.** github.com/example/forum (611 issues, about 5,900 comments) has
  912 such addresses: 763 pictures and 149 files. A sample of 45 averaged
  54 KB a picture and 500 KB a file, so the copies add roughly 110-120 MB to
  the repository, once; every clone and mirror carries them.

Pages draw a copied file from the repository's own raw URL and a GitHub
attachment that was not copied as a link, never as an `<img>`
([repo-views.md](../../docs/app/repo-views.md#issue-page-first-view)).

## Rate limits and status

A rate limit (`429`, or `403` with an exhausted quota) waits for
`Retry-After`; other failures back off exponentially, and a stopped pass keeps
its progress. Reports (`forkmesh-forge-sync-report-v1`, see
[synchronization status](sync-status.md)) give lag against the SLO (default 15
minutes), convergence and conflict rates, and retries. They never contain
tokens.
