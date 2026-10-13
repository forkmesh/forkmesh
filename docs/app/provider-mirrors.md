# Mirroring a repository to GitHub or GitLab

Part of [the App docs](README.md). Added 2026-10-13: the user asked for a
repository setting to mirror a ForkMesh repository to GitHub (then GitLab as
well) with a personal access token, with a switch per feature (code, issues,
pull requests, discussions and so on) and a direction for each, plus a
manual mode for picking what to bring into ForkMesh. It is reached from the
repository's Settings tab and from a **Mirror** button at the top of the Code
page.

## Code map

| Part | File |
| --- | --- |
| Settings, provider API calls, import, outbox, cron sync | `app/src/repository_provider_mirror.py` (lazy module) |
| Route | `REPO_PROVIDER_MIRROR_RE` in `app/src/urls.py`, dispatched in `app/src/app_runtime.py` after `/webhooks` |
| Tables | `repo_provider_mirrors`, `repo_provider_mirror_links`, `repo_provider_mirror_outbox` (`app/migrations/0257_repo_provider_mirrors.sql`, `app/src/schema.py`) |
| Cron phase | `provider-mirror` (every 5 minutes, offset 4) in `entry.py` and `cron_activity.py` |
| Event hooks | `apps_runtime.emit_repo_event` (issue and pull opens and comments), `discussions_handler` in `entry.py` (new discussions), `fediverse_routes.py` (release published) |
| Code push | `server/hosted_push_mirror.go` on the primary core, fed by `hosted_repos.replicas` and `_ReplicaRuntime.push_mirror` in `hosted_git.py`; contract in [../HOSTED_REPOS.md](../HOSTED_REPOS.md#push-mirrors-to-github-and-gitlab) |
| Settings card, picker, Code-page button | `renderRepoForgeMirror*`, `handleRepoForgeMirror*`, `loadRepoProviderMirror` in `app/public/dashboard/js/06-repo-content.js`; `data-repo-forge-mirror-card` and `mirrorButton` in `08-repo-detail-network.js` |
| Tests | `app/tests/test_repository_provider_mirror.py`, `server/hosted_push_mirror_test.go`, `app/browser_tests/tests/dashboard-forge-mirror.spec.js` |
| iOS and Android | see [Mobile apps](#mobile-apps) |

## Mobile apps

The same settings are available in both native apps, against the same
endpoint. The apps render the server's `catalog` (providers, features,
directions), so a new feature or provider needs no app change.

| App | Screen | Model and client | Entry points |
| --- | --- | --- | --- |
| iOS (`native/ios`) | `ForkMesh/Features/Code/RepoSettings/ProviderMirrorSections.swift`, a `mirror` case of `RepoSettingsSection` | `ForkMeshCore/Sources/FMCode/CI/ProviderMirrorModel.swift` (`ProviderMirrorClient` on `ApiService`, Bearer session) | Settings › Mirror to GitHub or GitLab; the ⇄ button in the repository's top bar (owners only); the repository actions menu; deep link `/<owner>/<repo>/settings/mirror` |
| Android (`native/android`) | `app/.../feature/code/review/ProviderMirrorScreen.kt` | `core/.../ci/ProviderMirror.kt` (`ApiProviderMirrorClient` on `ApiService.jsonExchange`, Bearer session) | the ⇄ (`sync_alt`) button in the repository's top bar (owners only); Settings › More › Mirror to GitHub or GitLab; `CodeFeature.providerMirrorScreen` |

Both show the connect form (provider, `owner/name`, token, "create it if it
does not exist", a direction per feature with "Automatically" or "I pick what
to import" and a Comments switch) and, once connected, the account, status
of the last code push and import, Sync now, Check connection, Pause, Change
settings, Disconnect, recent activity, and a pick list for each imported
kind. Tests: `ForkMeshCore/Tests/FMCodeTests/ProviderMirrorTests.swift`,
`core/src/test/.../ci/ProviderMirrorTest.kt`.

## What is mirrored, and which way

Each feature has a direction: **Off**, **ForkMesh → provider**, **provider →
ForkMesh** or **Both ways**. Features that can be imported also have an
import mode, **Import automatically** or **I pick what to import**, and a
**Comments** switch.

| Feature | Directions | How |
| --- | --- | --- |
| Code | Off, push | The primary core runs `git push` of every branch and tag with the token (optional prune). There is no import: ForkMesh's history is signed, and fetching commits from the provider into a hosted repository would move refs without the owner's signed pin. To start from a GitHub or GitLab repository, import it first. |
| Issues | all four | Imported issues enter the repository's `issue_inbox` as unsigned events with `source: "provider-mirror"`, author `github` or `gitlab` and author name `@login on GitHub`. Labels come along, a closed issue gets a `status: closed` event, and comments follow. New ForkMesh issues and comments become issues and comments on the provider. |
| Pull requests (GitLab: merge requests) | all four | An imported pull request becomes an issue labelled `pull-request` that links to the original and names its branches: its commits live on the provider. A new ForkMesh pull request is posted to the provider as an issue titled "Pull request: …", because the provider has no branch to open a real one from. Comments on either follow the link. |
| Discussions | all four (GitHub only) | Imported through GraphQL into `discussion_inbox`, with up to 20 replies quoted into the opening post. A new ForkMesh discussion is created in the "General" (or "Ideas", or first) category with `createDiscussion`. Later replies are not synced: a discussion's ForkMesh number is only assigned when the host materializes it. |
| Releases | Off, push | A release announced through `ap-publish` (`release.published`) is created on the provider for the same tag. If the tag has not been pushed yet the provider answers 422 and the outbox retries. |

Wikis, projects and Actions have no ForkMesh counterpart and are not offered.

## Loop prevention

Everything ForkMesh writes to a provider ends with the hidden marker
`<!-- forkmesh-mirror -->`, and everything imported carries it as well.
`note_event` drops an event whose body has the marker, and the importer skips
(and links) a provider item whose body has it. `repo_provider_mirror_links`
records which remote number belongs to which local number, in both
directions, so an item is never imported twice and comments find their
issue. Imports go straight into the inboxes and never call
`emit_repo_event`, so an imported issue is not sent back out.

## Storage

- `repo_provider_mirrors`: one row per repository, keyed by
  `blind_index("<owner>/<repo>")` (the same key as `repositories.key_bi` and
  `hosted_repos.key_bi`). `data` is encrypted and holds the provider, the
  remote `{namespace, name, path}`, the token, the per-feature settings, the
  import cursors (newest `updatedAt` seen per kind), a status block
  (verification, last code push, last import) and the last 20 activity
  entries. `revision` changes only when the owner saves settings, because
  the daemon re-pushes on a new revision; using `updated_at` would make every
  status write trigger a push.
- `repo_provider_mirror_links`: `(repo_bi, kind, remote_key)` to `local_key`
  and `origin` (`forkmesh` or `provider`). Comments use kind `comments` and
  key `<remote number>#<comment id>`.
- `repo_provider_mirror_outbox`: encrypted queued sends, retried with
  exponential backoff from 1 minute up to 1 hour, 6 attempts. A comment
  whose issue has no counterpart yet (404 from the link lookup) and a
  release whose tag is not there yet (422) are retried as well.

All three move with a namespace rename, are purged with the repository
(`_REPO_SCOPED_TABLES`), and are registered in `tools/next_rekey.py`.

## API

`POST /api/repo/<owner>/<repo>/provider-mirror`, owner session or owner
signature (`_authorize_repo_owner_web`), with `action`:

- `get`: the mirror (without the token), the catalog of providers and
  features, `codeHosted`, `private`, the number of queued sends and link
  counts.
- `save`: `{provider, remote, token?, resources, prune, createRemote,
  description}`. The token is checked with `GET /user` and the repository
  lookup; a missing repository is created (`POST /user/repos` or
  `/orgs/<org>/repos`; GitLab `POST /projects` in the namespace) only when
  `createRemote` is set, private when the ForkMesh repository is private. A
  blank token keeps the stored one. Changing the remote clears links and the
  outbox.
- `toggle {active}`, `verify`, `disconnect` (deletes the row, links and
  outbox; nothing already copied is removed on either side), `sync` (drain
  the outbox and run an import now).
- `candidates {kind, page}`: up to 50 provider items, newest first, each
  flagged `imported` or `mirrored` (came from ForkMesh).
- `import {kind, page, keys}`: import the picked items (up to 25).

## Automatic import

The `provider-mirror` cron phase drains up to 10 queued sends, then syncs up
to 3 mirrors whose `next_sync_at` is due (every 10 minutes per mirror). Each
sync lists the 30 most recently updated items per enabled kind, imports up
to 10 new ones oldest first, and for items already linked fetches comments
newer than the cursor. Older history than those 30 items is reached with the
picker.

## Code push

The Python side only supplies the configuration. `hosted_repos.replicas`
reads the set of active mirror keys once per exchange (one query), and for
the primary of a repository whose `blind_index(owner/name)` (or an alias's)
is in that set it adds `pushMirror` with the decrypted token. The daemon's
report comes back in `pushMirrors` and is stored by `record_push`, which
writes only when the result changed. Code pushes therefore run only for
repositories hosted on the core nodes (`hosted_repos`); the settings card
says so for a repository served from its owner's node.

## Security

- The token is stored only inside the encrypted `data` blob, is never
  returned to the browser (only `…` and its last four characters), and is
  sent to the provider's API host and, for code, to the primary core over the
  signed replica exchange. Git receives it as an `http.extraHeader` through
  `GIT_CONFIG_*` environment variables, never on the command line or in the
  URL, and push errors are redacted.
- Provider calls use `redirect: "manual"` so the token never follows a
  redirect, and only the fixed API origins (`api.github.com`,
  `gitlab.com/api/v4`) are contacted. Remote names are limited to
  `owner/name` with `[A-Za-z0-9_.-]` segments; self-hosted GitLab and
  subgroups are not accepted, to match `hosted.pushHosts`.
- The external-dependency manifest (`forge-apis`) and
  `www/docs/relay-data-retention.md` record the stored token.

## Traps

- `setRepoMirrorStatus` and `renderRepoMirrorLists` already exist for
  ForkMesh's own mirror nodes (`07-repo-compose-branch.js`). The dashboard is
  one closure, so a second function with the same name silently replaces the
  first. That is why everything here is named `ForgeMirror` and uses
  `data-repo-forge-mirror-*` attributes.
- The Code toolbar grid has a fixed column count; the Mirror button shares
  the last column with the Code button in a flex wrapper rather than adding
  a column.
