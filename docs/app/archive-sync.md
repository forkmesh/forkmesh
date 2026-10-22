# Backing up a repository to the Internet Archive

Part of [the App docs](README.md). A repository owner can upload a copy of a
public repository to an [archive.org](https://archive.org) item every day,
week or month, with their own archive.org S3 keys. It lives in repository
Settings, in the **Back up to the Internet Archive** card (`#archive`), under
the GitHub/GitLab mirror card.

| Part | File |
| --- | --- |
| Settings, API, daemon config and reports | `app/src/repository_archive_sync.py` |
| Route | `REPO_ARCHIVE_SYNC_RE` in `app/src/urls.py` |
| Table | `repo_archive_syncs` (`app/migrations/0288_repo_archive_syncs.sql`) |
| Upload | `server/hosted_archive_sync.go`, fed by `hosted_repos.replicas` ([../HOSTED_REPOS.md](../HOSTED_REPOS.md#internet-archive-backups)) |
| Settings card | `app/public/dashboard/js/06-repo-content.js` (`renderRepoArchiveSync`), mounted in `08-repo-detail-network.js` |
| Tests | `app/tests/test_repository_archive_sync.py`, `server/hosted_archive_sync_test.go` |

## Why it is separate from provider mirrors

archive.org is not a git host: it stores files. A
[provider mirror](provider-mirrors.md) pushes refs to GitHub or GitLab with a
personal access token and can import issues; an archive backup uploads one
file, a git bundle, to an item. Keeping it a separate setting also lets a
repository have a GitHub mirror and an archive backup at the same time
(`repo_provider_mirrors` holds one provider per repository).

## What is uploaded

- One file, `<repo>.bundle`, in the chosen item: a git bundle of `HEAD`,
  every branch and every tag. Anyone can restore it with
  `git clone <repo>.bundle`. Each upload replaces the previous file; older
  history stays inside the bundle.
- The item is created on the first upload if it does not exist, as mediatype
  `software` in the `open_source_software` (Community Software) collection,
  titled `<owner>/<repo>`, with the repository's web address as `source`.
- Only public repositories. Everything on archive.org is public, so `save` and
  **Back up now** refuse a private repository (`private_repository`), the
  exchange never sends `archiveSync` for one, and the daemon checks the
  visibility again. A repository made private keeps its settings but stops
  uploading.
- Only repositories hosted on the cores: the primary core holds the git data.
  A repository served from its owner's node shows "Waiting for this
  repository to be hosted".

## Schedule

The owner picks **Every day**, **Every week** (the default) or **Every
month**. An upload happens when the interval has passed since the last one
**and** a branch or tag changed, so a quiet repository is not uploaded again.
Saving the settings or pressing **Back up now** uploads at the next exchange
whatever changed; **Back up now** is limited to once per 10 minutes
(`sync_too_soon`). A failed upload is retried after an hour.

## Storage and API

`repo_archive_syncs` is keyed by `blind_index("<owner>/<repo>")` with `active`
and encrypted `data` (item identifier, access key, secret, interval,
`revision`, status and the last 20 history rows). It is purged with the
repository, follows a rename, and is covered by `tools/next_rekey.py`.

`POST /api/repo/<owner>/<repo>/archive-sync` (owner only) with `action`:

- `get`: `archive` (or `null`), `catalog` (`intervals`, `defaultInterval`,
  `keysUrl`), `codeHosted`, `private`.
- `save`: `identifier` (or an `archive.org/details/...` address),
  `accessKey`, `secret` (blank keeps the saved one while the access key is
  unchanged) and `interval` (`daily`/`weekly`/`monthly`; absent keeps the
  saved value). The keys are checked with
  `GET https://s3.us.archive.org/?check_auth=1` before anything is stored, and
  `https://archive.org/metadata/<item>` tells whether the item exists yet.
- `toggle` (`active`), `verify`, `disconnect`, and `sync` (Back up now).

Errors: `invalid_identifier`, `invalid_access_key`, `invalid_secret`,
`secret_required`, `invalid_interval`, `private_repository`,
`verification_failed` (with the archive's `message`), `not_configured`,
`sync_too_soon`, `not_authorized`.

## Security

- The secret key is stored only encrypted, never returned (last four
  characters only), sent to the primary core only inside the signed replicas
  exchange, and used only in the upload's `Authorization` header. Errors are
  redacted before they are reported or logged.
- The daemon uploads only to `https://s3.us.archive.org/<item>/<file>.bundle`
  and does not follow redirects; the app only calls `s3.us.archive.org` and
  `archive.org/metadata`, with `redirect: "manual"`.
- **Stop backing up** deletes the stored keys. It does not delete anything
  already on archive.org; the owner removes an item there.
- The stored keys are listed in `www/docs/relay-data-retention.md`.

## Not covered

- The phone apps do not show the card yet; the web dashboard does.
- Issues, pull requests, discussions and releases are not uploaded, only git
  data.
