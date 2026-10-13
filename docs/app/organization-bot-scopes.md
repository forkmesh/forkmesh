# Organization bot scopes

An organization bot token (`fmbot_<id>_<secret>`) is a revocable bearer
credential that owners, admins and (task scopes only) members mint on
`POST /api/orgs/<org>/bot-tokens` (`org_bot_tokens_handler` in
`app/src/entry.py`). The desktop's Genie credential
(`genie_credential_handler`) mints the same kind of token with the two task
scopes. Each token stores the scopes it was granted; `_org_bot_token_context`
authenticates the bearer, logs one metadata-only usage row and returns the
token's scopes.

## Scopes that exist

`ORG_BOT_TOKEN_SCOPES` is the grantable set. `GET /api/orgs/<org>/bot-tokens`
returns it as `permissions`, and the Organization Admin page
(`app/public/dashboard/js/04-account.js`) draws one checkbox per entry, so the
page offers exactly what the server accepts.

| Scope | Checked by |
| --- | --- |
| `organization.tasks.read` | `organization_tasks_handler` (`/api/tasks`, GET) and the `list_org_tasks` / `get_org_task` tools of `/mcp` (`_remote_mcp_call_tool` in `app/src/remote_mcp.py`) |
| `organization.tasks.write` | `organization_tasks_handler` (`/api/tasks`, every non-GET method) and the `complete_org_task` tool of `/mcp` |
| `local.connect` | `bot_session_handler` (`POST /api/bot/session`, linking the token to a local computer) |

A bot token without the scope a route needs gets 403 `permission_denied`
with `requiredScope`. These three routes are the only callers of
`_org_bot_token_context`; no other route accepts an `fmbot_` bearer
(`_account_session_record` does not resolve one).

## Retired scopes (2026-10-02)

Until 2026-10-02 the grantable set also held seven scopes that no route
checked, because no route a bot token can call does what they name:

- `world.presence`, `world.chat.write`, `world.interact`: the World presence
  socket and World chat authenticate account sessions and chat room tickets,
  never a bot token.
- `repository.contents.read`, `repository.issues.write`,
  `repository.pulls.review`: repository content, issue and pull routes
  authenticate account sessions, signed desktop requests or App credentials
  (`fmat_`, [APPS.md](../APPS.md)), never a bot token.
- `agents.sessions.run`: the agent-job routes
  (`/api/orgs/<org>/repos/<repo>/agent-bots`, `org_agent_bots_handler`)
  authenticate an account session or a signed desktop triple and check the
  member's organization role, never a bot token.

Users could grant them and they did nothing, which made a token look more
powerful (or more dangerous) than it was. Adding bot-token support to those
routes would be new API surface, so the scopes were retired instead. They are
kept in `ORG_BOT_RETIRED_SCOPES`, and `_org_bot_scopes` drops them:

- a grant request that names a retired scope gets the remaining live scopes;
  a request naming only retired scopes is refused with 400
  `invalid_bot_permissions`; an unknown scope is still refused;
- a stored token that still carries retired scopes keeps working with its
  live scopes, and the token list and `/api/bot/session` report only those.

Adding a scope back means adding the route that checks it in the same change.
`app/tests/test_org_bot_retired_scopes.py` pins that every grantable scope is
checked by a bot-token route and that only the three routes above accept a
bot token.
