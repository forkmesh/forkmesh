# ForkMesh Apps

ForkMesh Apps lets anyone build a third-party app and install it into
ForkMesh, the way Slack apps work. A developer registers the app. A user or an
organization installs it through OAuth 2.1 and a consent screen. From then on
the app acts only through short-lived tokens bound to the scopes that were
granted. When the app has a bot user, it posts as **"&lt;App&gt; (app)"**. It
never posts as a person.

The code:

| Piece | Where |
|---|---|
| Policy: scopes, redirect matching, PKCE, token shapes, signing, block kit | `app/src/apps_core.py` (pure Python) |
| Routes: console, OAuth, installs, Web API, events, commands, directory | `app/src/apps.py` (over a narrow runtime adapter) |
| Worker adapter, event hooks and cron drain | `app/src/apps_runtime.py` (lazy module) |
| Tables | `app/migrations/0203_apps.sql` |
| Consent screen | `app/public/oauth-authorize.html`, `oauth-authorize.js` |
| Dashboard | `/dashboard/apps` (installed apps, org approvals, directory, review queue) and `/dashboard/apps/build` (developer console): `app/public/dashboard/js/09-apps.js`, `partials/views/apps*.html` |
| Chat surfaces | `app/public/chat-apps.js` (slash commands, block kit, button callbacks) |
| Example app | `examples/apps/hello-app/server.py` |
| Tests | `app/tests/test_apps_*.py`, `app/tests/host/test_apps_e2e.py` |

## What was reused

Nothing here is a parallel system where one already existed.

- **Outgoing requests** use the repository-webhook machinery in
  `repository_webhooks.py`:
  - the `forkmesh-webhook-v1` HMAC scheme and its `ForkMesh-Webhook-*` headers;
  - the retry classification (`classify`) and the capped exponential backoff;
  - the public-https destination rule (`clean_url`).

  The host's egress guard (`app/host/forkmesh_host/egress.py`) resolves each
  name, refuses private, loopback, link-local and metadata addresses, and
  connects to the IP it checked. So the SSRF defence holds at connect time,
  not only at registration.
- **Bot posts to chat** go through the incoming-webhook path of the chat
  channels (`_ChatChannelsRuntime.deliver_webhook`). That path encrypts the
  frame with the room key and hands it to the room's Durable Object. The frame
  carries `senderId: "app:<slug>"` and `sender: "<App> (app)"`.
- **Bot issue and PR posts** use the ForkBot relay path. The post is an
  `issue_inbox` or `pull_inbox` row that the owner's node materializes, with
  author `app:<slug>` and the name `<App> (app)`. `:` is not in the
  account-name alphabet, so no person can hold that author id.
- **Pings** use `enqueue_notification` with the existing `plugin_activity`
  kind. It already means "a package or app wrote this title and body", so no
  client needed a new ping kind.
- **Admin review** follows the Ads network flow in `ad_network.py`:
  - approval is pinned to a content hash;
  - an edit sends a listed app back to the queue;
  - self-review is recorded;
  - every decision is audited.
- **Star ratings** are the Market's account ratings
  (`marketplace_reviews.py`) under a third market, `forkmesh-apps`.
- **Org roles** come from `organization_rbac.py` through `_org_role`. Owners
  and admins manage apps.
- **Audit** goes to `_audit_sensitive_action`. App actions are logged with
  the actor `app:<slug>`.

These existing credentials stay as they are and are not app credentials:
- org bot tokens (`fmbot_…`), used by the remote MCP server and the Tasks
  API;
- browser-extension and mobile session bearers.

## Registration (developer console)

`/dashboard/apps/build`, or `POST /api/apps`:

| Field | Rule |
|---|---|
| `name` | 2–40 characters. The slug (`hello-app`) is derived from it and must be unique. |
| `description` | Up to 400 characters. |
| `icon` | `{glyph, color}` or `{data: "data:image/png;base64,…"}`, 24 KB or less. Remote icon URLs are refused: fetching one would let the developer log every viewer's address. |
| `redirectUris` | 1–10 entries. `https` only, no fragment, no userinfo. They are matched exactly, character for character. |
| `scopes` | The most the app may ever ask for. See the table below. |
| `events`, `eventUrl` | The event types to receive and a public `https` URL. Each event needs its scope. |
| `commands` | Up to 10 entries of `{command, url, description, usageHint}`. The name may not shadow a built-in command (`/issue`, `/topic`, …). Needs the `commands` scope. |
| `interactivityUrl` | Receives button and select callbacks. |
| `bot` | `{enabled: true}` gives the app a bot user. |
| `homepageUrl` | Optional `https` page where people install the app. The App Directory links to it. |
| `organization` | Optional. The app then belongs to that organization, and its owners and admins can manage it. |

The response shows three credentials once:
- the **client id** (`fmapp_…`), which is public;
- the **client secret** (`fmcs_…`). Only a keyed HMAC of it is stored;
- the **signing secret** (`fmss_…`). It is stored AES-GCM sealed, because
  ForkMesh needs it to sign requests.

Neither secret can be read back. **Rotate** replaces it.

## Scopes

| Scope | Grants |
|---|---|
| `repos:read` | Repository metadata for the repositories the install covers, and `repository.push` events. |
| `issues:read` / `issues:write` | Issue events. Opening issues and commenting, as the bot. |
| `pulls:read` / `pulls:write` | Pull request events. Commenting on pull requests, as the bot. |
| `chat:read` / `chat:write` | Public-channel messages that reach apps (see [Chat stays end-to-end encrypted](#chat-stays-end-to-end-encrypted)). Posting in public channels, as the bot. |
| `pings:read` / `pings:write` | The installer's Pings as events. Sending Pings to the installer, or to an organization's admins. |
| `agents:run` | Asking the repository's host to start a coding agent on an issue. The host decides. |
| `commands` | Slash commands. |

A token carries the scopes granted at consent, capped by the install's current
scopes. If the developer removes a scope from the app, every token of the app
is revoked. If a re-consent narrows the scopes, the install's older tokens are
revoked.

**What an install covers.**
- A **user install** covers the repositories the installer owns (their own
  and their linked nodes') and the Pings addressed to them.
- An **organization install** covers the organization's repositories. Its
  Pings go to the organization's owners and admins.

Private channels and direct messages are sealed to their members, so no app
ever reads or posts into them.

## Install and authorization (OAuth 2.1)

1. The app sends the user to
   `/oauth/authorize?response_type=code&client_id=…&redirect_uri=…&scope=…&state=…&code_challenge=…&code_challenge_method=S256`.
2. The consent page calls `GET /api/apps/oauth/authorize` with the signed-in
   session.
   - ForkMesh checks the client and the exact redirect URI **before** it
     redirects anywhere. On an unknown client or a mismatched URI it stops
     with an error page and does not redirect.
   - Every later error returns to the redirect URI with `error=` and
     `state=`.
   - PKCE is required and must be `S256`; `plain` is refused.
   - The requested scopes must be a subset of the app's registered scopes.
3. The page lists the scopes, the developer, the redirect host, and an
   "unreviewed" note for unlisted apps. The user picks **Just me** or one of
   their organizations.
4. **Allow** calls `POST /api/apps/oauth/authorize`. That creates or updates
   the install and issues a code:
   - single use;
   - valid for 10 minutes;
   - stored only as a keyed HMAC.

   The browser is then sent to `redirect_uri?code=…&state=…`.
5. The app calls `POST /oauth/token` with `grant_type=authorization_code`, the
   code, the same `redirect_uri`, the `code_verifier`, and its client
   credentials (HTTP Basic or form fields). The response is:

   ```json
   {"access_token": "fmat_…", "token_type": "Bearer", "expires_in": 3600,
    "refresh_token": "fmrt_…", "refresh_token_expires_in": 2592000,
    "scope": "repos:read commands",
    "install": {"id": "…", "target": "user", "name": "alice"},
    "app": {"id": "…", "slug": "hello-app"},
    "bot": {"id": "app:hello-app", "name": "Hello App (app)"}}
   ```

**Refresh.** `grant_type=refresh_token` rotates the refresh token on every
use. If an already rotated or revoked refresh token is presented again, the
whole token family is revoked. A replayed authorization code also revokes the
tokens that code produced. Both events are audited. A refresh may narrow the
scope with `scope=`.

**Revocation.** An app revokes its own token with `POST /oauth/revoke`
(RFC 7009). Revoking a refresh token revokes its family. People revoke an app
under **Installed apps**:
- on `/dashboard/apps`, which **Settings → Installed apps** links to;
- with `DELETE /api/apps/installs/<id>`.

An uninstall revokes every access and refresh token, deletes pending codes,
queued events, response URLs and interactive-message records, and sends the
app one `app.uninstalled` event. A platform admin can revoke any install.
**Suspending** an app turns off all of its tokens at once.

### Organizations

- An organization install needs an owner or admin:
  - If an owner or admin consents, the install happens at once.
  - If a member consents, an **install request** is created instead. The
    organization's owners and admins get a Ping. The app is sent back with
    `error=access_denied&error_description=admin_approval_pending`.
- Once an admin approves the request (`/dashboard/apps`, or
  `POST /api/apps/orgs/<org>/requests/<id>`), the app is approved for exactly
  those scopes. The member then installs again and gets a code. Asking for
  more scopes needs a new approval.
- Owners and admins decide which apps members may **request**
  (`PUT /api/apps/orgs/<org>/policy`):
  - `any`: any app;
  - `allowlist`: only listed app ids;
  - `none`: no member requests.
- **Private** apps (not listed in the directory) are available only:
  - to their developer;
  - inside the organization that owns the app.

## Web API

Send `Authorization: Bearer fmat_…`. Every call is checked against the
token's scopes and against two budgets:
- per install: 120 calls a minute;
- per app: 600 calls a minute.

Over budget the answer is `429` with `Retry-After`.

**Existing REST reads.** An app token is accepted on an explicit allowlist of
the same GET routes users call, mapped to a scope in
`apps.WEB_API_ROUTES`.
- For a user install, the request runs as the installer, read-only.
- Anything not on the list answers `403 app_token_not_allowed`.
- A method other than GET or HEAD never runs with an app token.

This way an app token can never change state through a route that would record
a human as the author.

**App methods.** `POST /api/apps/v1/<method>`. Each write is attributed to the
bot and audit-logged under `app:<slug>`.

| Method | Scope | Body |
|---|---|---|
| `auth.test` | any | — |
| `chat.postMessage` | `chat:write` + bot | `{channel, text, blocks?}`. Public channels only. |
| `issues.create` | `issues:write` + bot | `{owner, repo, title, body}` |
| `issues.comment` | `issues:write` + bot | `{owner, repo, number, body}` |
| `pulls.comment` | `pulls:write` + bot | `{owner, repo, number, body}` |
| `pings.send` | `pings:write` | `{title, body?, href?}`. Goes to the installer, or to an organization's admins. |
| `agents.run` | `agents:run` + bot | `{owner, repo, number}` |

The repository must be covered by the install.

## Events API

When a subscribed event happens, ForkMesh queues one delivery for each
install that can see it and holds the event's scope. Deliveries are never
sent inline: a slow endpoint must not slow the push, merge or post that caused
the event.

| Event | Emitted by |
|---|---|
| `issue.opened`, `issue.comment` | `issues_handler`, ForkBot and app-created issues |
| `pull.opened`, `pull.comment` | `pulls_handler` |
| `pull.merged`, `repository.push` | the same points that queue repository webhooks |
| `chat.message` | messages apps post in public channels (bot posts and in-channel replies) |
| `app.mention` | `@<slug>` in an issue or pull request the install can see, in an app's chat post, or in a chat message the sender's client forwards (see below) |
| `ping.created` | `enqueue_notification` for an installer who granted `pings:read` |
| `app.uninstalled` | uninstall and revocation |

The request body:

```json
{"type": "event_callback", "eventId": "…", "appId": "…", "installId": "…",
 "target": {"kind": "user", "name": "alice"}, "ts": 1790000000000,
 "event": {"type": "issue.opened", "repository": "alice/demo", "number": 7,
           "title": "…", "author": "bob"}}
```

**Signing.** Each request is signed with the app's signing secret, using the
same scheme as repository webhooks:

```
ForkMesh-Webhook-Id:        <unique id; deduplicate on it>
ForkMesh-Webhook-Timestamp: <unix seconds, fresh on every attempt>
ForkMesh-Webhook-Signature: v1=hex(HMAC-SHA256(secret,
                              "forkmesh-webhook-v1\n<ts>\n<id>\n" + body))
```

The receiver checks the HMAC in constant time and rejects a timestamp more
than 300 seconds away from its own clock. It also remembers recent ids so a
replayed request is refused.

**URL verification.** Setting or changing `eventUrl` marks it unverified.
**Verify event URL** in the console (`POST /api/apps/<id>/verify`) sends:

```json
{"type": "url_verification", "challenge": "<random>"}
```

The endpoint must answer 200 with `{"challenge": "<the same value>"}`. Only a
verified URL receives events.

**Retries.** A delivery is handled like a repository webhook:
- 2xx: delivered;
- 408, 425, 429, 5xx or no response: retried with backoff from 30 s up to
  30 min, at most 6 attempts;
- anything else, and any 3xx: dropped. Redirects are never followed.

After 20 failed deliveries in a row, events for the app are switched off, the
developer gets a Ping, and verifying the URL switches them back on. The
console's **Delivery log** (`GET /api/apps/<id>/deliveries`) keeps the last
100 events, commands, interactions and verifications, with status, attempt
count and outcome.

The cron phase `app-events` drains the outbox every five minutes. A few rows
are also drained right after each enqueue.

## Chat stays end-to-end encrypted

Chat rooms are encrypted, and the server only relays ciphertext. ForkMesh
Apps does not change that. There is no blanket forwarding of human chat to
apps. An app receives exactly three kinds of chat content:

1. **Messages posted through app surfaces**: bot posts
   (`chat.postMessage`) and in-channel replies to commands and button clicks.
   The server writes these itself, so it can hand them to other apps that
   hold `chat:read` as `chat.message` events. The posting app never gets its
   own message back.
2. **Messages that mention the app.** When a person sends a message
   containing `@<slug>`, their own client forwards that one message to that
   one app (`POST /api/apps/mentions`). The app must be installed for the
   sender or one of the sender's organizations, hold `chat:read` and
   subscribe to `app.mention`. The event carries the sender, the channel and
   the text.
3. **Its slash commands.** Typing `/<command> args` sends the command and its
   arguments to that app, and the text is not posted to the room.

Private channels and direct messages never forward anything, mentions
included.

## Slash commands

A web chat composer message that starts with `/<command>` is matched against
the commands of the apps installed:
- for the sender;
- for the organizations the sender belongs to.

`/apps` lists them. When two installed apps register the same command, the
app installed for the sender wins over an organization's, then the oldest
install wins.

The client calls `POST /api/apps/commands/run` with `{text, channelId}`.
The command is not posted to the room.
ForkMesh POSTs a signed request to the command's URL, with a 5-second
timeout:

```json
{"type": "slash_command", "command": "hello", "text": "args",
 "user": {"name": "alice"}, "channel": {"id": "…", "name": "general"},
 "installId": "…", "responseUrl": "https://…/api/apps/respond/fmru_…",
 "triggerId": "…"}
```

The app answers with `{"responseType": "ephemeral" | "in_channel", "text",
"blocks"?}`:
- **Ephemeral** replies are returned only to the person who typed the command.
- **In-channel** replies are posted by the bot. That needs `chat:write` and a
  public channel. Otherwise the reply falls back to ephemeral.

The `responseUrl` accepts up to 5 delayed replies for 30 minutes. Delayed
ephemeral replies reach the user as a Ping.

## Interactive messages

The block kit is kept small:
- `section`: text, plus up to 10 fields;
- `context`;
- `divider`;
- `actions`: `button` and `static_select` elements, each with an `action_id`.

Clients render text with `textContent` and never as HTML. The rules:
- at most 20 blocks, 16 KB in total;
- action ids are unique within a message;
- button styles are `primary` or `danger`.

**Server notices use the same blocks.** A frame from a `system:` sender (the
`#email-log` mail log, [MAIL_LOG.md](MAIL_LOG.md)) carries `section`,
`context` and `divider` blocks next to a plain `text` copy. Write a section
as a headline of ` · `-separated parts (what happened, a one-word result,
the time) with `Label: value` fields. Clients draw a `system:` frame's
blocks in place of its text:
- the web chat (`chat-apps.js`) as bordered cards;
- the desktop (`desktop/src/ChatBlocks.h`) as cards with the first headline
  part in bold, a known result word coloured (green for accepted or
  delivered, amber for suppressed or skipped, red for failed or bounced), and
  the fields as a label and value grid. The desktop draws `app:` blocks the
  same way, but not their buttons or selects: it has no session to call
  `/api/apps/interact` with.

ForkMesh records each message an app posts with blocks (`app_messages`, kept 7
days). When someone clicks, the client calls `POST /api/apps/interact` with
`{appMessageId, actionId, value, channelId}`. ForkMesh checks two things:
- the action id exists in that recorded message;
- the value is one the app offered.

It then sends a signed request to the app's `interactivityUrl`:

```json
{"type": "block_actions", "user": {"name": "bob"},
 "channel": {"id": "…"}, "message": {"id": "…"},
 "action": {"actionId": "wave", "value": "wave"},
 "installId": "…", "responseUrl": "…"}
```

The reply has the same shape as a slash command reply.

## App Directory and review

- **Submit to the App Directory** (`POST /api/apps/<id>/submit`) moves an app
  from `private` (or `rejected`) to `pending`.
- Platform admins see the queue on `/dashboard/apps`
  (`GET /api/apps/review`). They decide with
  `POST /api/apps/<id>/review {action, reason, contentHash}`:
  - `approve` lists the app;
  - `reject` and `suspend` need a reason;
  - `reinstate` returns a suspended app to `pending`.
- The queue shows each app's redirect, event and interactivity URLs, its
  scopes and its last five decisions, so a reviewer sees what the consent
  screen and the endpoints will be.
- Approval is pinned to the content hash of the fields a reviewer saw: name,
  description, icon, scopes, events, commands, the bot switch, and all URLs.
  An approval sent with an older hash is refused (`content_changed`). Editing any of
  them sends a listed app back to `pending`.
- Suspension revokes every token of the app, drops its queued events, and
  refuses new consent.
- Every decision is stored in `app_reviews`, audit-logged, and sent to the
  developer as a Ping with the reason.
- A self-review (an admin reviewing their own app) is allowed, but it is
  recorded as one.

`GET /api/apps/directory` is public. It lists listed apps (never their URLs
or secrets) with their Market rating and install count, best rated first. A
listed app can be installed by anyone; there is no install button in the
directory, because an install always starts at the app (the directory links
to its `homepageUrl`).
- Ratings use the Market's review system with `market=forkmesh-apps` and the
  app slug as the package id.
- The developer is the maintainer, so they get the low-rating Pings and can
  reply.
- Only people who installed the app can rate it.

## Threat model

| Threat | Mitigation |
|---|---|
| Stolen authorization code | The code needs the PKCE verifier (S256 only) and the client secret. It is single use for 10 minutes, and a replay revokes what it issued. |
| Open redirect or code leak through the redirect | Exact-match redirect URIs, https only, no fragments. A mismatch never redirects. |
| Consent clickjacking | The consent page is served with `frame-ancestors 'none'` and `X-Frame-Options: DENY`. The approval POST uses the same-origin cookie or a bearer token. |
| CSRF on the approval POST | Cookie-authenticated writes pass `_request_origin_allowed`. |
| Leaked access token | It expires after 1 hour and is bound to one install and its scopes. Revocation is immediate. |
| Leaked refresh token | Rotated on use. Reuse revokes the family. |
| Database read | Client secrets, codes, access, refresh and response-URL tokens are stored as keyed HMACs. The signing secret, URLs and targets are AES-GCM sealed with `DATA_KEY`. |
| App impersonating a person | Bot posts carry `app:<slug>` and `<App> (app)`, and neither can be an account name. App tokens run no state-changing user route. |
| Over-broad access | Least-privilege scopes, a consent screen, org admin approval, and org request policies. Removing a scope revokes tokens. |
| SSRF from event, command or interactivity URLs | Checked at registration: public https only, no private or link-local literals, no ForkMesh names. At connect time the egress guard resolves the name and connects to the checked IP. Redirects are not followed. |
| Forged callbacks to the app | Every request is signed with HMAC plus a timestamp and a unique id. The app checks all three. |
| Forged button clicks | A click is accepted only for an action id and value that the recorded app message offered. |
| A noisy or abusive app | Per-install and per-app rate limits, a disabled endpoint after 20 failures, admin suspension, and the audit log. |
| Leaking private chat | Private channels and direct messages are sealed to their members and never reach an app. |

Operator switches, for development only:
- `APPS_DEV_ENDPOINT_ORIGINS` names exact `http://host:port` origins that
  redirect, event and command URLs may use.
- The host's `FORKMESH_EGRESS_ALLOW` must also name them.

Neither is set in production.

## Developer guide

1. Register the app at `/dashboard/apps/build`. Keep the secrets in your
   server's secret store.
2. Build an install link that sends the browser to `/oauth/authorize` with
   PKCE, then exchange the code at `/oauth/token`.
3. Verify every request ForkMesh sends you: signature, timestamp and id.
4. Answer `url_verification`, then click **Verify event URL**.
5. Call the Web API with the access token. Refresh it when it expires.

`examples/apps/hello-app/server.py` does all of this in about 300 lines of
standard-library Python:

```
FORKMESH_ORIGIN=https://app.forkmesh.com \
FORKMESH_CLIENT_ID=fmapp_… FORKMESH_CLIENT_SECRET=fmcs_… \
FORKMESH_SIGNING_SECRET=fmss_… HELLO_PUBLIC_URL=https://hello.example.org \
python3 examples/apps/hello-app/server.py
```

Then open `https://hello.example.org/install`.

## Client surfaces (web, desktop, native)

**Web.** The web surfaces ship with this change:
- `/dashboard/apps` and `/dashboard/apps/build`;
- the consent page;
- slash commands, block rendering, button and select callbacks, and mention
  forwarding in `/chat` (`chat-apps.js`, hooked from `chat.js`);
- slash commands with ephemeral replies in the dashboard chat
  (`dashboard-chat.js` loads `chat-apps.js` on the first `/command`). The
  dashboard chat is World #general, which is not a channel an app can post
  in, so in-channel replies fall back to ephemeral there.

**Desktop and native apps** need the following. Each is tracked in
`docs/native/PROGRESS.md`.

- **Installed apps**: `GET /api/apps/installed` and
  `DELETE /api/apps/installs/<id>`. Show the scopes, "last used" and a Revoke
  button. Org admins also get the pending requests
  (`POST /api/apps/orgs/<org>/requests/<id>`).
- **Slash commands**:
  - `GET /api/apps/commands` for suggestions;
  - `POST /api/apps/commands/run` when the composer text starts with `/`;
  - render an ephemeral reply locally, never into the room.
- **Interactive messages**: render `blocks` on chat frames whose `senderId`
  starts with `app:`. On a click call `POST /api/apps/interact` with the
  frame's `appMessageId`.
- **Mentions**: after sending a public-channel message that names an app from
  the `mentionable` list of `GET /api/apps/commands`, call
  `POST /api/apps/mentions`. Never forward any other message.
- **Consent**: open `/oauth/authorize` in the system browser. The native apps
  never handle an app's client secret.

**Native iOS (WS22).** Settings › Installed apps (also `/dashboard/apps`)
lists the installs with their scopes and "last used", revokes them, and lets
organization admins approve or deny requests; "Browse the App Directory"
opens the web directory. In chat, app commands appear in the composer's
suggestions, `/apps` lists them, a command never reaches the room, and an
ephemeral reply is shown only on that phone and kept in memory. Bot messages
in public office channels render their blocks; a button or select calls
`/api/apps/interact` with the office channel id. A message that names a
`mentionable` app is forwarded only from a public office channel. `oauth` is a
reserved path for the link parser, so a consent link always opens in Safari.
Status and what is left: `docs/native/PROGRESS.md` (WS22).

**Native Android (AW22)** has the same surfaces (Settings › Installed apps and
`/dashboard/apps`; commands, ephemeral replies, blocks and mentions in chat).
Its verified App Link claims every path on app.forkmesh.com, so the router
sends `/oauth/...` and `/dashboard/apps/<page>` to a browser explicitly
rather than back to the app.
