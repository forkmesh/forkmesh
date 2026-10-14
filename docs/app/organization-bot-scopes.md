# Organization bot scopes

An organization bot token (`fmbot_<id>_<secret>`) is a revocable bearer
credential minted on `POST /api/orgs/<org>/bot-tokens` (`org_bot_tokens_handler`
in `app/src/entry.py`) by owners, admins and (task scopes only) members. The
desktop's Genie credential mints the same kind of token with the task scopes.
`_org_bot_token_context` authenticates the bearer, logs a metadata-only usage
row and returns the token's scopes. A token dies with its creator's membership
and acts with the creator's current role.

## Grantable scopes

`ORG_BOT_TOKEN_SCOPES` is returned as `permissions` by
`GET /api/orgs/<org>/bot-tokens`; the Organization Admin page draws one checkbox
per entry.

| Scope | Checked by |
| --- | --- |
| `organization.tasks.read` | `organization_tasks_handler` (`/api/tasks`, GET) and the `/mcp` task read tools |
| `organization.tasks.write` | `organization_tasks_handler` (non-GET) and the `/mcp` `complete_org_task` tool |
| `local.connect` | `bot_session_handler` (`POST /api/bot/session`) |

A missing scope answers 403 `permission_denied` with `requiredScope`. These
three routes are the only callers of `_org_bot_token_context`; no other route
accepts an `fmbot_` bearer.

## Retired scopes

`ORG_BOT_RETIRED_SCOPES` holds scopes no bot-token route checks:
`world.presence`, `world.chat.write`, `world.interact`,
`repository.contents.read`, `repository.issues.write`,
`repository.pulls.review`, `agents.sessions.run`. `_org_bot_scopes` drops them:
a grant naming them keeps only the live scopes (only retired ones answers 400
`invalid_bot_permissions`), and stored tokens carrying them work with their live
scopes only.

Rule: adding a scope requires adding the route that checks it in the same
change. `app/tests/test_org_bot_retired_scopes.py` enforces this.
