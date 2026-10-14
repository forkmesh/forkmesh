# ForkMesh Apps

Third-party apps install into ForkMesh the way chat-platform apps do. A
developer registers the app; a user or organization installs it through
OAuth 2.1 with a consent screen; the app then acts only through short-lived
tokens bound to the granted scopes. A bot user posts as **"&lt;App&gt; (app)"**,
never as a person.

| Piece | Where |
|---|---|
| Policy: scopes, redirect matching, PKCE, tokens, signing, block kit | `app/src/apps_core.py` |
| Routes: console, OAuth, installs, Web API, events, commands, directory | `app/src/apps.py` |
| Runtime adapter, event hooks, cron drain | `app/src/apps_runtime.py` |
| Tables | `app/migrations/0203_apps.sql` |
| Consent screen | `app/public/oauth-authorize.html`, `oauth-authorize.js` |
| Dashboard | `/dashboard/apps`, `/dashboard/apps/build` |
| Chat surfaces | `app/public/chat-apps.js` |
| Example app | `examples/apps/hello-app/server.py` |
| Tests | `app/tests/test_apps_*.py`, `app/tests/host/test_apps_e2e.py` |

Reused machinery: repository-webhook signing, retry and URL rules; the host
egress guard; the chat incoming-webhook path for bot posts; the inbox relay
for bot issue and pull posts (author `app:<slug>`, which no account can hold);
`plugin_activity` Pings; the ads review flow; Market ratings
(`market=forkmesh-apps`); organization roles; the sensitive audit log.

## Registration

`/dashboard/apps/build` or `POST /api/apps`: `name` (2–40 characters, unique
slug), `description`, `icon` (glyph or inline image up to 24 KB; no remote
URLs), `redirectUris` (1–10, https, exact match), `scopes`, `events` and
`eventUrl`, `commands` (up to 10, no built-in names), `interactivityUrl`,
`bot`, `homepageUrl`, optional owning `organization`.

The response shows once: client id (`fmapp_…`), client secret (`fmcs_…`,
stored as a keyed HMAC) and signing secret (`fmss_…`, stored sealed). Secrets
cannot be read back; rotate them instead.

## Scopes

| Scope | Grants |
|---|---|
| `repos:read` | repository metadata and `repository.push` events for covered repositories |
| `issues:read` / `issues:write` | issue events; opening issues and commenting as the bot |
| `pulls:read` / `pulls:write` | pull events; commenting as the bot |
| `chat:read` / `chat:write` | public-channel app messages; posting in public channels |
| `pings:read` / `pings:write` | the installer's Pings as events; sending Pings |
| `agents:run` | asking a repository's host to start a coding agent on an issue |
| `commands` | slash commands |

A user install covers the installer's repositories and Pings; an organization
install covers the organization's repositories and pings its owners and
admins. Removing a scope from the app revokes its tokens; a narrower
re-consent revokes older tokens. Private channels and direct messages are
never visible to apps.

## OAuth 2.1

1. The app sends the user to
   `/oauth/authorize?response_type=code&client_id=…&redirect_uri=…&scope=…&state=…&code_challenge=…&code_challenge_method=S256`.
2. Unknown clients or mismatched redirect URIs stop with an error page and
   never redirect. PKCE `S256` is required. Requested scopes must be a subset of
   the registered ones.
3. The consent page shows scopes, developer, redirect host and review status;
   the user picks themselves or an organization.
4. **Allow** issues a single-use code valid 10 minutes, stored as a keyed HMAC.
5. `POST /oauth/token` (`authorization_code` with the verifier and client
   credentials) returns `access_token` (`fmat_…`, 1 hour), `refresh_token`
   (`fmrt_…`, 30 days), `scope`, and the install, app and bot identities.

Refresh tokens rotate on every use; reuse of a rotated token revokes the whole
family, and a replayed code revokes what it issued. `POST /oauth/revoke`
(RFC 7009) revokes tokens. Uninstalling revokes every token, deletes pending
codes, events and response URLs, and sends `app.uninstalled`. Suspending an
app disables all of its tokens.

Organizations: owners and admins install directly; a member's consent creates
an install request that admins approve for exactly those scopes. Owners set a
request policy (`any`, `allowlist`, `none`). Private (unlisted) apps are
available only to their developer and owning organization.

## Web API

`Authorization: Bearer fmat_…`, rate-limited per install (120/min) and per app
(600/min), `429` with `Retry-After` beyond that.

- An explicit allowlist of existing GET routes (`apps.WEB_API_ROUTES`) accepts
  app tokens read-only; everything else answers `403 app_token_not_allowed`.
- App methods, `POST /api/apps/v1/<method>`, attributed to the bot and
  audited: `auth.test`, `chat.postMessage` (public channels),
  `issues.create`, `issues.comment`, `pulls.comment`, `pings.send`,
  `agents.run`.

## Events API

Events are queued per eligible install and delivered asynchronously:
`issue.opened`, `issue.comment`, `pull.opened`, `pull.comment`, `pull.merged`,
`repository.push`, `chat.message`, `app.mention`, `ping.created`,
`app.uninstalled`.

```json
{"type": "event_callback", "eventId": "…", "appId": "…", "installId": "…",
 "target": {"kind": "user", "name": "<account>"}, "ts": 1790000000000,
 "event": {"type": "issue.opened", "repository": "<owner>/<repo>", "number": 7}}
```

Signing (same as repository webhooks):

```
ForkMesh-Webhook-Id:        <unique id; deduplicate on it>
ForkMesh-Webhook-Timestamp: <unix seconds, fresh on every attempt>
ForkMesh-Webhook-Signature: v1=hex(HMAC-SHA256(secret,
                              "forkmesh-webhook-v1\n<ts>\n<id>\n" + body))
```

Receivers must check the HMAC in constant time, reject timestamps more than
300 s off, and refuse replayed ids. A new `eventUrl` must answer
`{"type": "url_verification", "challenge": …}` with the same challenge before
it receives events. Retries: 408, 425, 429, 5xx and timeouts back off from
30 s to 30 minutes, at most 6 attempts; other statuses and redirects are
dropped. After 20 consecutive failures events are switched off until the URL
is verified again. The console shows the last 100 deliveries.

## Chat stays end-to-end encrypted

The server relays only ciphertext for human chat. An app receives only:
messages posted through app surfaces (bot posts and in-channel replies), a
message that mentions it (forwarded by the sender's own client via
`POST /api/apps/mentions`, public channels only), and its own slash commands.

## Slash commands

A composer message starting with `/<command>` matches commands of apps
installed for the sender or their organizations (`/apps` lists them; the
sender's own install wins, then the oldest). `POST /api/apps/commands/run`
sends a signed request with a 5-second timeout; the command text is not posted
to the room. Replies are `ephemeral` (only the sender) or `in_channel` (posted
by the bot; needs `chat:write` and a public channel). A `responseUrl` accepts
up to 5 delayed replies for 30 minutes.

## Interactive messages

Block kit: `section` (text plus up to 10 fields), `context`, `divider`,
`actions` (`button`, `static_select`); at most 20 blocks and 16 KB; text is
rendered as text, never HTML. Server notices from `system:` senders use the
same blocks ([MAIL_LOG.md](MAIL_LOG.md)). Clicks call `POST /api/apps/interact`;
the server accepts only an action id and value the recorded message offered,
then sends a signed `block_actions` request to the app.

## App Directory and review

Submitting moves an app to `pending`. Admins approve, reject, suspend or
reinstate; approval is pinned to a content hash of everything a reviewer saw,
and any edit sends a listed app back to review. Suspension revokes all tokens.
Decisions are stored, audited and sent to the developer. `GET
/api/apps/directory` lists approved apps with ratings and install counts;
installs always start at the app's own page. Only installers can rate.

## Threat model

| Threat | Mitigation |
|---|---|
| Stolen authorization code | PKCE S256 plus client secret; single use for 10 minutes; replay revokes |
| Open redirect | exact-match https redirect URIs; mismatches never redirect |
| Consent clickjacking and CSRF | `frame-ancestors 'none'`, `X-Frame-Options: DENY`, origin checks on cookie writes |
| Leaked tokens | 1-hour access tokens bound to one install; refresh rotation with family revocation |
| Database read | secrets, codes and tokens stored as keyed HMACs; signing secret and URLs sealed |
| Impersonation | bot identities cannot be account names; app tokens run no state-changing user route |
| Over-broad access | least-privilege scopes, consent, organization approval and policies |
| Server-side request forgery | public https only at registration; the egress guard checks addresses at connect time |
| Forged callbacks or clicks | signed requests with timestamp and id; clicks checked against the recorded message |
| Abusive apps | rate limits, endpoint disable after failures, suspension, audit log |
| Private chat leakage | private channels and direct messages never reach apps |

Development-only switches: `APPS_DEV_ENDPOINT_ORIGINS` and the host's
`FORKMESH_EGRESS_ALLOW`; never set in production.

## Developer guide

1. Register at `/dashboard/apps/build` and store the secrets safely.
2. Send users to `/oauth/authorize` with PKCE; exchange the code at
   `/oauth/token`.
3. Verify every incoming request's signature, timestamp and id.
4. Answer `url_verification`, then verify the event URL in the console.
5. Call the Web API and refresh tokens when they expire.

```
FORKMESH_ORIGIN=https://app.forkmesh.com \
FORKMESH_CLIENT_ID=fmapp_… FORKMESH_CLIENT_SECRET=fmcs_… \
FORKMESH_SIGNING_SECRET=fmss_… HELLO_PUBLIC_URL=https://hello.example.org \
python3 examples/apps/hello-app/server.py
```

## Client surfaces

The web ships the dashboard pages, consent page, slash commands, block
rendering, callbacks and mention forwarding. Desktop and native clients use
the same endpoints: installed apps (`GET /api/apps/installed`,
`DELETE /api/apps/installs/<id>`), command suggestions and runs, ephemeral
replies rendered locally, block rendering for `app:` senders, mentions from
public channels only, and consent opened in the system browser (native apps
never hold a client secret).

## The `/dashboard/apps` page

The page is split into tabs (Plugins, Installed apps, Directory, and Review
queue for reviewers only); `#installed`, `#directory` and `#review` open a tab
directly. It used to stack everything on one scroll, with each account plugin
drawn as a bordered card inside the bordered panel and dependents nested as
cards inside cards. With 76 account plugins that read as a wall of boxes.

Plugins now follow the store pattern of Raycast, VS Code's extension view and
Linear's integrations: a category rail on the left (a select under 900px),
search across name, summary and category, an All / Installed / Available
filter, and one hairline-divided list with a row per plugin. A row holds a
neutral letter tile, the name, a one-line summary, the Phone / Desktop / Web
places as pill toggles, and a blue switch for install and remove. Removing
still asks for confirmation, since it deletes the plugin's account data.
Dependents are no longer nested: the row carries a "Needs <parent>" label, and
the parent shows how many add-ons depend on it. Searching clears the category
so it always looks through every plugin.

App installs, organization policies, directory listings and review items use
the same row list. The organization "Save policy" button is a neutral button:
green is reserved for the page's one create button.
