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
pass. Exit codes: `0` converged, `1` conflicts, `2` configuration error, `75`
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

## Rate limits and status

A rate limit (`429`, or `403` with an exhausted quota) waits for
`Retry-After`; other failures back off exponentially, and a stopped pass keeps
its progress. Reports (`forkmesh-forge-sync-report-v1`, see
[synchronization status](sync-status.md)) give lag against the SLO (default 15
minutes), convergence and conflict rates, and retries. They never contain
tokens.
