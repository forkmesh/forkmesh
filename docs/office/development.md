# Office development

Part of [Office](README.md). How the Office is built and run: the code map,
the static client and the per-organization room object on the cores, the
plug-in contract for features, the HTTP and socket protocol, security, the
TURN relay on each core, configuration, tests, performance budgets,
operations and traps.

## Code map

| Path | What it is |
| --- | --- |
| `office/public/app.js` | Client shell: join, input, click-to-move pathfinding, render loop, proximity mesh, the feature `api` |
| `office/public/map.js` | Tile map, zones and walkability (DOM-free) |
| `office/public/render.js` | Procedural pixel art: the map is pre-rendered once; name tags are cached sprites |
| `office/public/avatar.js`, `sprite.js` | Avatar codes (packing, validation) and cached avatar sprites |
| `office/public/rtc.js` | Proximity WebRTC mesh with three fixed transceiver slots (audio, camera, screen); tracks switch with `replaceTrack`, no renegotiation |
| `office/public/html.js` | `html` tagged templates, `setHtml`, the Trusted Types policy |
| `office/public/notes.js` | Inline status lines: `setNote` (in a panel) and `noteLine` (keyed lines above the dock) |
| `office/public/features/<name>.js` | Client half of each feature, registered in `features/index.js` |
| `app/src/org_office.py` | Python port of the shared rules: map, zones, team gates, status validation, desks, tables, whiteboard ops, pong physics, attendance |
| `app/src/org_office_room.py` | `OfficeCore` (socket logic) and the `ForkMeshOrgOffice` Durable Object adapter (run by the cores' emulation); `SqlOfficeStore` |
| `app/src/org_office_routes.py` | `/api/office/session`, `/api/office/entry`, `/api/office/ws`, tickets, ICE servers |
| `app/src/org_office_chat.py` | Creates and syncs each org's office channels in the site chat |
| `app/migrations/0226_org_office.sql`, `0227_org_office_site_chat.sql` | `org_office_visits`, `org_office_channels` |
| `app/tools/build_site_assets.py` `stage_office` | Bundles and minifies the client into `app/dist/office/` |
| `office/perf-budget.json` | Bundle and frame budgets |
| `office/tools/build_park_map.py` | Generates the water park layout into `office/public/park/layout.js` and `app/src/org_office_park.py` (`--check` fails when stale) |
| `office/public/features/rides.js`, `rides/` | Ride engine: boarding, timelines, vehicles |
| `office/tests/*.test.mjs` | Node tests of the client modules |
| `office/tests/parity_dump.mjs` | Dumps client values that `app/tests/test_org_office_protocol.py` compares with the Python port |

## Architecture

- **Served at** `https://app.forkmesh.com/office` (a path, not a subdomain),
  so the session cookie, org tables and task API are same-origin.
- **Static client.** `stage_office` (run by `build_site_assets.py app`)
  bundles `office/public/app.js` with esbuild into one ES module, minifies
  the stylesheets into `office.css`, stamps both with content hashes (`?v=`,
  served `immutable`), and fails over the budget in `office/perf-budget.json`
  or if the bundle contains `eval(`/`new Function`. Caddy serves `/office`
  and `/office/*`; `office` is a reserved route prefix.
- **Server.** One `ForkMeshOrgOffice` Durable Object per organization, named
  `office:org:<org_bi>:v1`, placed on one core by rendezvous hash
  (`durable_cluster.py`) and reached from other cores over the mesh. It
  keeps live presence in memory, batches movement into one `mv` frame per
  50 ms and diffs `ex` (per-person extras) so unchanged values are never
  rebroadcast.
- **Chat is the site chat.** Each org's office rooms (general, engineering,
  design, coffee, lobby, garden) are private `chat_channels` rows of
  `kind: "office"`, named `office/<org>/<room>`, mapped in
  `org_office_channels`. Membership follows org membership and team gates and
  is resynced on every `/api/office/session` and after member or team
  changes; removal rotates the room key. Deleting the organization runs
  `org_office_chat.dissolve` before the roster goes: it removes the office
  `chat_channels` rows with their members, reads, invites and webhooks, the
  `org_office_channels` mapping, and closes the live rooms, so former members
  keep no channel to read. These channels cannot be managed
  from `/chat` and do not count toward the channel limit. The panel uses the
  site's chat modules (transport, seal, moderation, retention, rich text) and
  the account event channel for unread counts. "Message nearby" and knocks
  are ephemeral socket events.
- **Persistence.** Attendance visits are rqlite rows (`org_office_visits`,
  names sealed with the data key). Desks, profiles, arcade scores, coffee
  counts and whiteboard items use the object's replicated `ctx.storage`;
  whiteboard writes are debounced to one batch per second.
- **Features.** Each client feature is a plug-in on `api`; on the server every
  message type is a handler in `HANDLERS` in `org_office_room.py`.

## Plug-in contract

Each client feature is `setup(api)` in `features/<name>.js`, listed in
`features/index.js`. The `api` object (end of `app.js`) gives:

- state: `me`, `others`, `mesh`, `ws`, `zoom`, `camera`, `path`, `account`
  (`{id, name, org, teams, role}`), `tasksEnabled`, `officeChat`,
  `iceServers`, `personActs(person)`, `mapCanvas`;
- actions: `send`, `setExtra(key, value)`, `notice`, `noticeError`,
  `hudNote`, `setNote`, `toast` (alias of `notice`), `walkTo`, `walkToTile`,
  `takeSeat`, `showPanel`, `openPalette`, `useObject`, `setMedia`, `emote`,
  `connect`, `leave`, `forkmesh(path, init)` (same-origin `/api/tasks*`
  only);
- hooks: `onMessage(type)`, `onInit`, `onExtra`, `onTick`, `onDrawUnder`,
  `onDrawWorld`, `onDrawScreen`, `dotColor`, `proximity`, `joinExtras`,
  `personBadge`, `personActions`, `paletteItems`, `onLeave`, `onMesh`,
  `onDenied`, `onObject(char)`, `onPerson`, `meMenu`, `face`, `tagAlpha`,
  `onEmote`, `onPose`;
- UI: `addPanel`, `addDockButton`, `addControl` (markup must be `html`).

Document events: `office:account`, `office:locked` (cancelable),
`office:panel`, `office:dm`.

Extras a client may set (validated in `EXTRAS`): `status`, `task`, `meeting`,
`sharing`, `coffee`. Server-set extras: `org`, `teams`, `fm`, `desk`, `wb`,
`tz`, `pic`, `focus`.

## Protocols and interfaces

- `GET /api/office/session[?org=<name>]`: `{signedIn, account, orgs, org,
  role, teams, unlocked, rooms, tasks}`; only the caller's own orgs.
- `POST /api/office/entry {org}` (same-origin, session): checks membership
  and returns a 60-second ticket bound to `org_bi`, `account_bi`,
  `session_id` and a nonce (HMAC of the data key, purpose
  `org-office-ticket-v1`), the socket path and `iceServers`.
- `GET /api/office/ws?ticket=` (Upgrade): checks Origin, ticket, that the
  cookie session matches, and membership; then hands off to
  `FORKMESH_ORG_OFFICE`, object `office:org:<bi>:v1`, path
  `/api/office/org/<bi>/v1/ws`, with the claim header
  `x-forkmesh-org-office-claim`.
- Socket messages: `join`, `m`, `sig`, `say`, `st`, `emo`, `wave`, `ex`,
  `pr-*`, `work-*`, `agents`, `meet-*`, `wb.*`, `chat*`, `play-*`. Identity
  comes from the claim, never from `join`.
- Desks: `work-deco {deco}` → `work-deco {ok}` or `work-err {reason}`.
- Focus: `focus-start {mode, mins?, rounds?, topic?, notify?}` or
  `focus-start {join}`, `focus-end`; errors `focus-err {reason}`; offers
  `focus-open {id, name, focus}`.
- Profiles: `prof-set {name?, tz?, av?}` → `prof-ok` or `prof-err {reason}`;
  broadcast `prof {id, name, av}`.

## Security

- **Identity:** only real ForkMesh sessions; nothing in the served code can
  mint an identity.
- **Isolation:** the object name comes from the ticket's `org_bi`, set after
  checking `org_members`; the object rejects claims for any other org and
  every store query is keyed by `org_bi`. A second socket for the same
  account replaces the first.
- **Revocation:** each socket rechecks session, account status, membership
  and teams every 30 s and is closed with 4003 when any lapses.
- **Tasks:** `/api/tasks` (`world/src/world_office_tasks.py`) answers only
  organization insiders (owner, admin, maintainer, member, auditor); a guest
  gets the same 403 `org_member_required` as an outsider, and an auditor may
  only read (`read_only_role` on every other method). Why: the board holds
  organization-private work, including the `security` department tasks filed
  from private vulnerability reports.
- **Limits:** frames over 16 KiB close the socket (1009); more than 120
  frames in a burst (60/s refill) closes it (1008). Per-action token buckets
  for movement, signalling, extras, chat, whiteboard ops and cursors. At most
  100 people per office. Chat 1000 characters, status 60 graphemes,
  whiteboards 4000 items or 1.5 MB.
- **Markup injection:** every `innerHTML` write goes through `setHtml`, which
  accepts only `html` template results. `/office` sends
  `require-trusted-types-for 'script'; trusted-types office`.
  `office/tests/html.test.mjs` fails on direct `innerHTML`, `outerHTML`,
  `insertAdjacentHTML`, `eval`, `new Function` or third-party URLs. Chat
  Markdown escapes first; whiteboard text is drawn on a canvas.
- **CSP:** `/office` and `/office/*` in `www/public/_headers`: `script-src
  'self'`, `connect-src 'self'`, `media-src 'self' blob: mediastream:`,
  `frame-ancestors 'none'`, and a Permissions-Policy allowing camera,
  microphone and display capture for this origin only.
- **WebRTC signalling:** the server relays `sig` (and `say`) only between
  members who may hear each other (same private room, same table, or within
  7 tiles in the open). ICE servers come from `/api/office/entry`.
- **Agents and tasks:** the agent panel only reads `/api/tasks` with the
  viewer's session; a relayed change applies only to bots owned by the
  relayer. `api.forkmesh` allows only same-origin `/api/tasks*`, in the
  office of `OFFICE_MARKETING_ORG`.
- **Retention:** chat follows site chat retention; attendance kept 14 days.
  Nothing logs message text or tokens.

## TURN relay

Each core runs coturn (`fmnext-turn.service`, `[turn]` in `deploy/fleet.toml`,
rendered by `deploy/core/render.py`) as a relay of last resort, so call media
never passes through a third party. It listens on 3478 UDP/TCP and 5349 TLS
with relays on UDP 49160-49999, using the app host's certificate from the
certsync export (reloaded by `fmnext-turn-cert.path`).

- **Credentials:** TURN REST with `OFFICE_TURN_SECRET` (from
  `next_fleet.py init-secrets`, delivered as an encrypted systemd credential
  and through the vault; the config with the secret is written to tmpfs).
  Client credentials expire after 10 minutes; the client refreshes them at 8.
- **Server-side request forgery:** `denied-peer-ip` and `IPAddressDeny=` block
  private, loopback, link-local, CGNAT, documentation, multicast and reserved
  IPv4/IPv6 ranges. Never add a range starting at `::` (coturn would deny every
  IPv4 peer; `deploy/tests/test_turn.py` fails on it).
- **Limits:** UDP relays only, no multicast peers, no CLI, 12 allocations per
  credential, 1200 per node, 512 KB/s per session, one-hour allocations;
  unprivileged with `ProtectSystem=strict`.
- The IDS expects `turnserver` in its unit with listeners on 3478 and 5349;
  relay ports stay above its `udp_max_port` (10240).

## Configuration

| Variable | Default | Purpose |
| --- | --- | --- |
| `OFFICE_STUN_URLS` | `stun:stun.cloudflare.com:3478`; on the cores rendered from `[turn]` | Comma list of `stun:`/`stuns:` URLs |
| `OFFICE_TURN_URLS` | unset; on the cores rendered from `[turn]` | Comma list of `turn:`/`turns:` URLs (4 at most); TURN is off while unset |
| `OFFICE_TURN_SECRET` | unset | coturn `static-auth-secret`; vault secret, never sent to clients |
| `OFFICE_MARKETING_ORG` | `INSTANCE_OWNER`, else `forkmesh` | The org whose office shows the task board and agents |

A self-hosted instance's office is its own organization, so the default is
`INSTANCE_OWNER` rather than `forkmesh` (which exists only on ForkMesh's
cores; pointing at a missing org makes `GET /api/tasks` answer 503
`office_organization_unavailable`).

`[turn]` in `deploy/fleet.toml`: `enabled`, `host`, `realm`, `port`,
`tls_port`, `relay_min_port`/`relay_max_port` (at most 4096 ports inside
32768-65535), `user_quota`, `total_quota`, `max_bps`,
`max_allocate_lifetime`, `fallback_stun`. The renderer refuses `[turn]` when
`OFFICE_TURN_SECRET` is not an app secret or when `production.vars` also sets
the URLs.

## Build, run, test

- Build: `python3 app/tools/build_site_assets.py app` (needs esbuild from
  `world/tools`, or `FORKMESH_ESBUILD`).
- Python: `app/tests/test_org_office_protocol.py` (client parity, needs
  node), `test_org_office_room.py`, `test_org_office_routes.py`,
  `app/tests/host/test_org_office_e2e.py` (starts the host).
- Node: `cd office && node --test tests/*.test.mjs`.
- Local run: build `app/dist`, then from `app/host` run
  `python -m forkmesh_host serve --port 8080 --no-cron --migrate --no-build`
  with `FORKMESH_HOST_ASSETS_DIR=app/dist`, a temp `FORKMESH_HOST_DATA_DIR`,
  `FORKMESH_HOST_OFFLINE=1`, `FORKMESH_VAR_DATA_KEY`,
  `FORKMESH_VAR_ADMIN_PASS` and the `FORKMESH_VAR_*_ORIGIN` variables set to
  `http://127.0.0.1:8080`. Sign up, verify with
  `python -m forkmesh_host ops verify-email <name>`, create an org with
  `POST /api/orgs`, open `/office`. The `/office` CSP is applied by Caddy,
  not the host.

## Performance

Budgets in `office/perf-budget.json`. Rendering is Canvas 2D (one pre-rendered
map, cached sprites, viewport culling); movement is batched to 20 Hz and the
proximity mesh recomputed every 250 ms.

The script budget is 156,300 bytes gzip (156,283 used). The Office bundles
the site chat's crypto (`chat-seal.js`, including the strict-room key
succession that carries a device's approval over to its new key, about
300 bytes; the ML-KEM-768 port `chat-mlkem.js`, the DM ratchet of chat
protocol v4, `chat-frame-auth.js` from the
[relay hardening](../app/chat-relay-hardening.md)) and the call quality widget
with its diagnostics (`call-quality-meter.js`, about 10 KB). Raise the budget
only by the measured size of what a change adds.

## Operations

- Deploy with the normal app deploy (migrations, the `FORKMESH_ORG_OFFICE`
  binding, which the host reads from `app/wrangler.toml`, rebuilt `app/dist`
  and Caddy routes).
- No account or organization may be named `office`.
- TURN rollout: `tools/next_fleet.py init-secrets --execute`,
  `tools/next_vault.py sync --execute`, then a core roll
  (`next_fleet.py roll --execute`; steps daemon, turn, files, start). The
  unit is skipped on a node without coturn or the credential.
- Check a node: `systemctl status fmnext-turn`, `journalctl -u fmnext-turn`,
  `turnutils_stunclient app.forkmesh.com`, and `relay` candidates in
  `chrome://webrtc-internals`.

## Traps

- Caddy, not the app host, applies `app/dist/_headers`.
- `/office` has no trailing slash, so every asset URL in
  `office/public/index.html` is absolute (`/office/...`); the build fails if
  one is missing.
- The Python rules must match the client's: change `map.js` or a shared
  helper and `test_org_office_protocol.py` fails until `org_office.py`
  follows.
- New markup must use `html` and `setHtml`; a plain string throws.
- A new floor character needs a `ZONES` entry and a painter in `FLOORS`, or
  the office never opens. A new chair character goes in `CHAIRS` on both
  sides.
- Never move existing furniture: desk claims and whiteboards are keyed by
  tile. Grow the map in directions that keep existing coordinates.
- The focus field and room card need `ROOMS[zone]` (filled by `room()` and
  `area()`); a bare `rect()` gets neither.
- `glassWall` expects a second wall row under the room's top wall.
