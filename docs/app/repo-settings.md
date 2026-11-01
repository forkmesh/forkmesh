# Repository Settings: administrators, features and access

Part of [the App docs](README.md). A repository's Settings tab holds its
About fields, feature switches, ActivityPub switches, webhooks, mirrors,
Actions secrets, collaborators and visibility. This page covers who may open
it, the Issues / Pull requests / Discussions switches, and the Access section
(collaborators and visibility). How private repositories are sealed is in
[encryption](../encryption.md); branch rules are in
[branch-protection.md](branch-protection.md).

## Who may open the Settings tab

A repository linked to an organization (`org_repos`) is stored under the
account that created or publishes it, for example `alice/site`, and shown at
the organization address `/acme/site` (or `/site` on a single-organization
instance, [instances](../operations/instances.md)). On an organization
address the repository's `owner` field is the organization, so matching the
session against it (`sessionOwnsRepo`) alone would hide Settings from
everyone, the creator and the organization's owners included.

### The rule

An account administers a repository when either:

- it owns the storing node (`_account_owns_node`: it is that node, or the user
  account that node is linked to), or
- the repository is linked to an organization in which the account's
  repository permission is `admin` (`_org_permission`): the `owner` and
  `admin` roles, or a `maintainer`/`member` whom a team grants `admin`.

`maintainer` (`maintain`), `member`, `auditor` and `guest` do not administer
the repository; this follows the role table in
[docs/security/organization-roles.md](../security/organization-roles.md),
where only `owner` and `admin` hold `admin` on linked repositories, the same
split as GitHub's Maintain and Admin repository roles. The rule is
`_repo_admin_allowed(env, actor, node, repo)` in `app/src/entry.py`; a lookup
error grants nothing.

### Server

Every endpoint behind the Settings tab uses that one rule:

| Surface | Endpoint | Gate |
| --- | --- | --- |
| About, features, ActivityPub switches | `POST /api/repo/<o>/<r>/about` | `repo_about_handler` |
| Digest preview, federated posts | `.../fediverse-digest`, `.../ap-posts` | `_authorize_repo_owner_web` |
| Webhooks | `.../webhooks` | `_authorize_repo_owner_web` |
| Mirror to GitHub or GitLab | `.../provider-mirror` | `_authorize_repo_owner_web` |
| Actions secrets | `.../actions/secrets` | `actions_secrets._repo_allowed` |
| Logo suggestion review | `.../logo-suggestions` | `native_repository_logo_handler` |
| Delete repository | `DELETE /api/repositories?owner=&name=` | `catalog_handler` |

Organization addresses reach these handlers as the storing node
(`org_alias_rewrite`). The repository delete takes its owner from the query
string instead of the path, so it resolves an organization address the same
way (`_org_repo_node`) before checking the rule and writing the tombstone; the
audit entry names the storing node's repository. Pull request rules and the
Actions Run/Cancel controls commit or act through write access, which
`_org_write_allowed` already grants to organization writers. The private
repository collaborator list (`shares_handler`) stays signed by the owner's
key: granting a collaborator re-wraps the repository key, which only the owner
holds.

### Dashboard

The client cannot work out organization roles from the catalog, so it asks:
`GET /api/repo/<o>/<r>/viewer-permission` answers
`{"viewer", "permission", "viewerCanAdmin"}` for the signed-in session
(`private, no-store`). `permission` is `admin` for the node
owner, else the best organization permission (`maintain`, `write`, `read` or
empty). The answer is per page load and per account
(`loadRepoViewerPermission`); a failed request grants nothing and is retried
on the next render.

`sessionAdministersRepo(repo)` is `sessionOwnsRepo(repo)` or that answer, and
gates the Settings tab and its route, About editing, the Mirror button, the
digest preview, webhooks, the provider mirror, pull rules editing, logo
review, delete, and the Agents tab (with `isAdmin`). `initRepoPage` starts the
request with the catalog load and waits for it before the first render, so a
`/site/settings` deep link stays on Settings. `sessionOwnsRepo` still decides
what is about the publishing node itself: the agent endpoint (`/agent-sessions`
on the node, the organization's `agent-bots` route otherwise), the composer's
"your node" wording and its repository choices.

Tests: `app/tests/test_org_repo_admin.py`,
`app/tests/test_dashboard_org_repo_settings.py`,
`app/tests/test_endpoint_session_auth.py`,
`app/tests/test_actions_secrets_api.py`,
`app/tests/test_repo_delete_tombstone.py`.

## Features: issues, pull requests, discussions

A repository administrator can switch Issues, Pull requests and Discussions
off in the Settings tab (the Features group above the ActivityPub switches).

- **Storage:** `repo_feature_settings` (migration 0270), one row per
  repository, `repo_bi = blind_index("repo-feature-settings:<owner>/<repo>")`
  with both halves lowercased and an organization alias resolved to its node
  (`_repo_features_bi`). `data` is plaintext JSON of booleans, the same trust
  level as `ap_repo_settings`. No row means everything is on.
- **API:** `GET /api/repo/<owner>/<repo>/about` returns `features`;
  `POST .../about` accepts `features` (absent means unchanged) with the same
  owner authorization as the other About fields and answers with the stored
  switches.
- **Enforcement:** while a feature is off, `POST` to `issues_handler`,
  `pulls_handler` or `discussions_handler` answers
  `403 {"error": "feature_disabled", "feature": ...}`, so nothing new is
  opened or commented. Existing records stay readable and come back when the
  feature is switched on. The web page hides the tab
  (`applyRepoFeatures` in `06-repo-content.js`).
- **Repository delete** purges the row with the other settings tables
  (`_delete_repo_named_state`).

On a self-hosted instance ([instances](../operations/instances.md),
[self-hosting](../self-hosting.md)), `instance-bootstrap` writes all three
switches off for the `<owner>/forkmesh` mirror the instance creates
(`ensure_mirror_features`, stored under the node name `forkmesh/forkmesh`). It
uses `INSERT OR IGNORE`, so running the bootstrap again never undoes an owner
who turned a feature back on.

Tests: `app/tests/test_repo_features.py`, `app/tests/test_ap_repo_settings.py`,
`app/host/tests_unit/test_instance_bootstrap.py`.

## Access: collaborators and visibility

Settings > **Access** (owner only, `renderRepoAccessSettings`) covers who can
read a repository without the desktop app.

### Collaborators (private repositories)

`/api/repo/<owner>/<repo>/shares` (`shares_handler`) lists, adds and removes
collaborators. Besides the owner node's signature (`forkmesh-share-v1`, used
by the desktop), it accepts the owner's own account session: the account
that is, or owns, the repository's node (`_shares_session_owner`). A
collaborator reads the repository and takes part in its issues, pull requests
and discussions ([web-discussions.md](web-discussions.md#who-may-do-what)).

- A repository the core nodes host is read through them, so its
  collaborators need no encryption key; `_hosted_read_allowed` counts a
  collaborator as a reader.
- A repository held by a personal node is sealed to each reader's key by
  that node, so a collaborator there still needs an encryption key
  (`grantee_encryption_key_required`), and the node re-seals on its next sync.

### Visibility (core-hosted repositories)

`POST /api/hosted-repositories/visibility {owner, name, visibility}`
(`_change_visibility` in `app/src/hosted_git.py`): the owner, or an admin of
the organization the repository belongs to, makes it public or private. The
catalog record, its plaintext privacy flag and the hosted row change
together; caches are cleared, the public catalog is republished and the cores
serve the new visibility from their next replica sync. Copies other people
already cloned or mirrored are not recalled. A repository held by a personal
node answers `409 node_held`: that node signs its own catalog record.

Renaming and transferring a repository are still desktop-only: see
[web-desktop-parity.md](web-desktop-parity.md).

Tests: `app/tests/test_web_repo_access.py`.
