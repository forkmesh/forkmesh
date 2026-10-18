# Who may open a repository's Settings tab

A repository linked to an organization (`org_repos`) is stored under the
account that created or publishes it, for example `alice/site`, and shown at
the organization address `/acme/site` (or `/site` on a single-organization
instance, [../INSTANCES.md](../INSTANCES.md)). Until 2026-10-18 the dashboard
showed the Settings tab only when the signed-in account was the repository's
`owner` field (`sessionOwnsRepo`). On an organization address that field is the
organization, so nobody matched, the creator included, and the organization's
owners could not change the repository's settings from the web.

## The rule

An account administers a repository when either:

- it owns the storing node (`_account_owns_node`: it is that node, or the user
  account that node is linked to), or
- the repository is linked to an organization in which the account's
  repository permission is `admin` (`_org_permission`): the `owner` and
  `admin` roles, or a `maintainer`/`member` whom a team grants `admin`.

`maintainer` (`maintain`), `member`, `auditor` and `guest` do not administer
the repository; this follows the role table in
[www/docs/security/organization-roles.md](../../www/docs/security/organization-roles.md),
where only `owner` and `admin` hold `admin` on linked repositories, the same
split as GitHub's Maintain and Admin repository roles. The rule is
`_repo_admin_allowed(env, actor, node, repo)` in `app/src/entry.py`; a lookup
error grants nothing.

## Server

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

## Dashboard

The client cannot work out organization roles from the catalog, so it asks:
`GET /api/repo/<o>/<r>/viewer-permission` answers
`{"viewer", "permission", "viewerCanAdmin"}` for the signed-in session
(`private, no-store`, never edge cached). `permission` is `admin` for the node
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
