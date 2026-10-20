# Mirroring a repository to GitHub or GitLab

Part of [the App docs](README.md). A repository owner can mirror a repository
to GitHub or GitLab with a personal access token, choosing a direction per
feature. Available from repository Settings, the Code page **Mirror** button,
and the iOS and Android apps (which render the server's `catalog`).

| Part | File |
| --- | --- |
| Settings, provider API, import, outbox, cron | `app/src/repository_provider_mirror.py` |
| Route | `REPO_PROVIDER_MIRROR_RE` in `app/src/urls.py` |
| Tables | `repo_provider_mirrors`, `repo_provider_mirror_links`, `repo_provider_mirror_outbox` |
| Code push | `server/hosted_push_mirror.go`, fed by `hosted_repos.replicas` ([../HOSTED_REPOS.md](../HOSTED_REPOS.md)) |
| Tests | `app/tests/test_repository_provider_mirror.py`, `server/hosted_push_mirror_test.go` |

## Features

Each feature is Off, ForkMesh → provider, provider → ForkMesh or Both ways.

- **Code**: push only, from the primary core, for repositories hosted on the
  cores. No import: hosted refs move only with the owner's signed pin.
  **Branches** (`refs`): all branches (`all`, the default) or only the
  default branch (`default`); tags are always pushed. With "delete branches
  and tags there" (`prune`) and `default`, every other branch on the target
  is deleted.
- **Issues**, **pull requests** (imported as issues labelled `pull-request`),
  **discussions** (GitHub): both ways, imported as unsigned inbox events with
  `source: "provider-mirror"`.
- **Releases**: push only, retried until the tag exists.

Everything written or imported carries `<!-- forkmesh-mirror -->`, and
`repo_provider_mirror_links` maps remote to local numbers, so nothing loops or
imports twice.

## Storage and API

`repo_provider_mirrors` is keyed by `blind_index("<owner>/<repo>")` with
encrypted `data` (token, settings, cursors, status); `revision` changes only on
saved settings. The outbox retries with backoff from 1 minute to 1 hour, 6
attempts. All tables are purged with the repository and covered by
`tools/next_rekey.py`.

`POST /api/repo/<owner>/<repo>/provider-mirror` (owner only) with `action`:
`get`, `save`, `toggle`, `verify`, `disconnect`, `sync`, `candidates`,
`import`. `save` takes `provider`, `remote`, `token` (blank keeps the saved
one), `resources`, `prune` (absent means off) and `refs` (`all`/`default`;
absent keeps the saved value, anything else is `invalid_refs`). The
`provider-mirror` cron phase drains the outbox and syncs due
mirrors every 10 minutes.

## Security

- The token is stored only encrypted, never returned (last four characters
  only), and passed to git through `GIT_CONFIG_*` environment variables, never
  the command line or URL; push errors are redacted.
- Provider calls use `redirect: "manual"` and only `api.github.com` and
  `gitlab.com/api/v4`; remote names are `owner/name` with `[A-Za-z0-9_.-]`.
- The stored token is listed in `www/docs/relay-data-retention.md`.
