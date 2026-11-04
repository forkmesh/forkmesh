# Internet Archive

Part of [Marketplace](README.md). The `internet-archive` package backs up a
public repository to an [archive.org](https://archive.org) item with the
owner's own archive.org S3 keys, and uses those backups as an independent
witness: its page compares the source and every mirror with what was archived,
so a reader can tell which copies still serve the real repository when the
source is offline and only [mirrors](../mirroring.md) remain. The user-facing
summary is the [package README](../../marketplace/packages/internet-archive/README.md).

Until 2026-11-04 this was a card in repository Settings backed by
`app/src/repository_archive_sync.py`. It is now a plugin, so a build can leave
it out ([plugins.md](plugins.md)) and an account can remove it. It installs
itself for accounts created before 2026-11-05 (`install.installedBefore`), so
nobody lost a running backup; repository Settings keeps a link card
(`#archive`) that opens the plugin page for that repository.

| Part | File |
| --- | --- |
| Settings API, witness, upload hooks | `marketplace/packages/internet-archive/content/worker/internet_archive.py` |
| Web page and visualization | `content/web/view.html`, `content/web/page.js` (`/dashboard/internet-archive?repo=<owner>/<repo>`) |
| Table | `repo_archive_syncs` (`app/migrations/0288_repo_archive_syncs.sql`), still declared by the core |
| Host calls | `repository_facts`, `repository_administrator`, `repository_mirrors` on `_PluginApiRuntime` (`app/src/entry.py`) |
| Upload | `server/hosted_archive_sync.go`, fed by `hosted_repos.replicas` ([core-hosted repositories](../operations/core-hosted-repositories.md#internet-archive-backups)) |
| Tests | `marketplace/packages/internet-archive/tests/`, `server/hosted_archive_sync_test.go` |

## What stays in the core

The primary core holds the Git data, so the bundle is still made and uploaded
by the Go daemon; only the settings, the record of uploads and the checks
moved. The core asks plugins through three hooks in `app/src/plugin_data.py`,
offered only to packages whose manifest declares `repository.archive.upload`:

- `archive_upload_keys(runtime)`: the blind indexes of active backups;
- `archive_upload_config(runtime, owner, name, aliases, keys)`: the
  `archiveSync` the replicas exchange sends to the primary;
- `record_archive_upload(runtime, owner, name, aliases, report)`: the
  daemon's report.

`hosted_git.py` calls them through `plugin_archive_upload_*` in `entry.py`.
The daemon only uploads to `https://s3.us.archive.org/<item>/<file>.bundle`,
so the capability stays narrow. The table stays a core table because the
repository purge, rename and `tools/next_rekey.py` already cover it.

The plugin's host calls are gated by its manifest permissions:
`repository.archive.upload` for `repository_facts` (private, hosted, exists)
and `repository_administrator` (the signed-in account when it may administer
the repository), `repository.mirrors.read` for `repository_mirrors` (the
public `GET /api/repo/<owner>/<repo>/mirrors` payload, `None` for a private
repository).

## What is uploaded

- One file, `<repo>.bundle`: a Git bundle of `HEAD`, every branch and every
  tag. Anyone can restore it with `git clone <repo>.bundle`. Each upload
  replaces the previous file.
- The item is created on the first upload as mediatype `software` in the
  `open_source_software` collection, titled `<owner>/<repo>`, with the
  repository's web address as `source`.
- Only public repositories hosted on the cores. `save` and **Back up now**
  refuse a private repository (`private_repository`), the exchange never sends
  `archiveSync` for one, and the daemon checks the visibility again.
- After bundling, the daemon reads the refs back from the bundle
  (`git bundle list-heads`) and reports, besides `revision`, `digest`, `ok`,
  `at`, `error` and `bytes`: the bundle's `sha1`, its `routingDigest` (the
  default branch plus tags, the same digest mirrors are pinned by,
  `routingRefsSHA256`), the default branch `head` (`name`, `commit`),
  `refCount` and the first 100 `refs`, default branch first.

## Schedule

**Every day**, **Every week** (the default) or **Every month**. An upload
happens when the interval has passed and a branch or tag changed. Saving or
pressing **Back up now** uploads at the next exchange; **Back up now** is
limited to once per 10 minutes (`sync_too_soon`). A failed upload is retried
after an hour.

## Checking copies against the archive

`GET /api/internet-archive/repo/<owner>/<repo>/witness`, for any signed-in
account and any public repository, answers in two parts.

**The archive.org copy** (`witness.state`). The plugin reads
`https://archive.org/metadata/<item>` (cached two minutes per item) and
compares the bundle's SHA-1, which archive.org computes itself, with the SHA-1
the daemon reported: `verified`, `older` (archive.org still serves an earlier
upload), `pending` (no match yet, within six hours of the upload), `missing`,
`changed`, `unrecorded` (uploaded before checksums were reported),
`unreachable` or `none`.

**Each copy** (`copies[].verdict`), the source first, then the mirrors from
`repository_mirrors`, compared with the last 30 successful uploads:

| Verdict | When | Shown as |
| --- | --- | --- |
| `match` | The mirror's directly probed refs digest (`directRefsSha256`) equals the latest `routingDigest` | success |
| `head` | Its default branch commit equals the latest archived head | success |
| `earlier` | It matches an older upload: genuine but behind | attention, dashed |
| `newer` | It synced after the latest upload and matches nothing archived | attention, dashed |
| `differs` | It matches nothing archived and has not synced since | destructive |
| `unknown` | It publishes neither a refs digest nor a commit | muted, dotted |

`summary` counts the serving mirrors and how many agree (`match` or `head`).
A `newer` mirror is not wrong, only beyond what the archive can vouch for; the
page gives the `git merge-base --is-ancestor` check that settles it. The
archive does not replace the [refs pins](../mirroring.md#how-a-cloner-knows-a-copy-is-authentic):
it is a second record held outside ForkMesh, which a mirror operator and the
cores cannot rewrite.

## The page

The page draws the source on the left, the archive.org item in the middle and
up to ten mirrors on the right, each edge coloured by its verdict with an
icon and a label (never colour alone), offline copies hollow and dashed.
Every mark has a tooltip and is keyboard-focusable; the copies table below
holds the same data. The timeline shows each upload's bundle size, with failed
uploads as red diamonds. **Check it yourself** lists the `curl`, `sha1sum`,
`git bundle verify`, `git ls-remote` and `git merge-base` commands for the
mirror picked. Colours come from the dashboard theme roles (`success`,
`attention`, `destructive`, `muted-foreground`), so dark mode follows the
theme. The owner's settings card (connect, **Back up now**, check keys,
pause, stop) sits under it.

## Storage and API

`repo_archive_syncs` is keyed by `blind_index("<owner>/<repo>")` with `active`
and sealed `data`: item, keys, interval, `revision`, status, `createdBy`, the
last 20 history rows, the last 30 upload reports (`uploads`) and the latest
bundle's `refs`.

`POST /api/internet-archive/repo/<owner>/<repo>` (repository administrators,
plugin installed) with `action`: `get`, `save` (`identifier`, `accessKey`,
`secret`, `interval`), `toggle`, `verify`, `disconnect`, `sync`. The keys are
checked with `GET https://s3.us.archive.org/?check_auth=1` before anything is
stored. `GET /api/internet-archive/backups` lists the backups the caller set
up or owns.

Errors: `invalid_identifier`, `invalid_access_key`, `invalid_secret`,
`secret_required`, `invalid_interval`, `private_repository`,
`verification_failed` (with `message`), `not_configured`, `sync_too_soon`,
`not_authorized`, `internet_archive_not_installed`, `invalid_session`.

## Removal

Removing the package (`/api/plugins/@account/internet-archive/uninstall`)
runs `purge_account_data`, which deletes every backup the account set up or
owns, keys included. Deleting the account or the repository purges the rows
through the core. Nothing on archive.org is deleted; the owner removes an item
there.

## Security

- The secret key is stored only sealed, never returned (last four characters
  only), sent to the primary core only inside the signed replicas exchange,
  and used only in the upload's `Authorization` header. Errors are redacted.
- The plugin reaches only `https://archive.org` and `https://s3.us.archive.org`
  (`worker.outboundOrigins`); the witness call sends no credentials.
- The witness reads only public data: the public mirrors payload and the
  item's public metadata. Private repositories answer `not_found`.
- The stored keys are listed in [relay-data-retention.md](../relay-data-retention.md).

## Not covered

- Phones and the desktop: the package is web only (`install.places: ["web"]`).
- Issues, pull requests and releases are not uploaded, only Git data.
- The witness compares refs, not objects: the ancestry check for a `newer`
  mirror is left to the reader's own Git.
