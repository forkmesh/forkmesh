# Public member roster and avatars

`GET /api/accounts/users` is the public member roster: the accounts that
`_is_public_roster_member` admits (a `user` record, `status == "active"`, not
`profile_private`), oldest first. Chat, the desktop, the native apps, the
dashboard and the World read it. Code: `_account_users_directory`,
`_account_users_world_directory` and `_account_avatar` in
`app/src/account_users.py`; the per-user fields come from
`_account_chat_user_payload` in `app/src/entry.py`; the routes are in
`accounts_handler` (`app/src/entry.py`) and `ACCOUNT_AVATAR_RE`
(`app/src/urls.py`). Tests: `app/tests/test_account_avatar_world_roster.py`.

## Why the World has its own view

On 2026-09-29 the roster was 1.47 MB for 234 members (1.03 MB gzip, 1.01 MB
brotli). Only 8 members had an avatar, but each carried it inline as
`avatarPng`, a base64 PNG of 34 to 244 KB (128² to 400²): about 92% of the
bytes. The World paints a face as a 128×128 texture on a small disc, and its
`memberDirectoryAvatar` ignored any `avatarPng` over 90,000 characters, so
half of those faces never showed. The default response cannot change, because
every other client reads `avatarPng` from it, so the World gets its own view.

## Endpoints

### `GET /api/accounts/users` (unchanged)

The same bytes as before for every caller: the public roster with inline
`avatarPng`, `Cache-Control: no-store, max-age=0, must-revalidate`, and the
administrator variant (`publicUsers`, `quarantinedUsers`, signup evidence) for
a signed administrator. The public build is kept in the server cache under
`USERS_DIRECTORY_CACHE_KEY` for `USERS_DIRECTORY_TTL` (120 s).

### `GET /api/accounts/users?view=world`

The public roster (same users, same order, same fields) with two changes per
user:

- `avatarPng` is removed.
- A user with an avatar gets `avatarUrl`,
  `/api/accounts/<name>/avatar?v=<hash>`, the full-size image, and, when the
  server can make thumbnails, `avatarThumb`, a
  `data:image/webp;base64,…` URL of a 128×128 WebP (1.4 to 4.8 KB each).
  A user without an avatar has neither field.

It is always the public projection, even for an administrator: the handler
never checks administrator credentials, so no admin field can appear.

Headers: `Content-Type: application/json; charset=utf-8`,
`Cache-Control: public, max-age=30, stale-while-revalidate=300`,
`CDN-Cache-Control: no-store` (see the privacy notes below), and a weak
`ETag` (`W/"<first 32 hex of SHA-256 of the body>"`). A request whose
`If-None-Match` names it gets `304` with the same cache headers and no body.

It is built from the same roster build as the default response: the handler
reads the default roster's server copy (`USERS_DIRECTORY_CACHE_KEY`) and only
runs the database scan (through `_account_users_directory(...,
public_payload=True)`, which also refills that copy) when the copy is missing.
The world body is kept under its own key, `USERS_WORLD_CACHE_KEY`, for 90 s
(`USERS_WORLD_STALE_SECONDS`) with its build time
(`x-forkmesh-built`) and `ETag`. For 30 s (`USERS_WORLD_FRESH_MS`) it is
served as is; after that, on the Next host, it is served stale while
`env.HOST_BACKGROUND` rebuilds it (one rebuild at a time); without a host
(Cloudflare) it is rebuilt inline as before.
`_users_directory_cache_forget` and every direct purge of the roster copy
(signup, profile edits, custom status, admin edits, account deletion, the
Mailtrap webhook) drop both keys, all through `edge_cache_delete`. A purge is
never answered from the stale copy: the copy is gone, so the next request
rebuilds inline. `edge_cache_delete` also bumps `EDGE_CACHE_EPOCHS[key]`, and
a build that started before a purge does not store its result, so a member who
just went private cannot be written back by a build already in flight.

### How a build reads the users

The scan used to read `u.data` for every user: 2.1 MB of ciphertext on
2026-09-29 (235 users), decrypted in full on every rebuild. Now
`_users_directory_records` reads the users' plain columns plus a tag,
`substr(data,1,24)` (the base64 of the random 12-byte IV) and `length(data)`,
and fetches `data` only for rows whose tag is not in
`_USERS_DIRECTORY_RECORDS`, 50 per statement. Every write re-encrypts the
record with a new IV, so any change fetches the row again; users no longer in
the table are dropped from the memo. The activity join (world activity,
contributions, badges, office visits) no longer carries `data`, so its
`users-directory` shared copy is a few KB and is actually kept (the 2 MB copy
never fit the host cache); administrator fields (`is_admin`, `email_bi`, ...)
come from the fresh users read. Reasons and numbers:
[world-boot-latency.md](world-boot-latency.md).

Measured on the production roster of 2026-09-29 (234 members, 8 avatars):

| | raw | gzip -9 | brotli 11 |
|---|---|---|---|
| default | 1,466,539 B | 1,027,710 B | 1,012,966 B |
| `view=world` | 149,132 B | 30,488 B | 28,454 B |

Without Pillow (no `avatarThumb`) the world view is about 117 KB raw.

### `GET /api/accounts/<name>/avatar`

The stored avatar PNG (`rec["avatar_png"]`, decoded from base64).

- `v=<hash>`: the first 16 hex characters of SHA-256 over the stored base64
  string (a `data:image/png;base64,` prefix is stripped first). The world view
  writes it into `avatarUrl`.
- `size=128`: a 128×128 WebP thumbnail instead of the PNG. Any other `size`
  is `400 {"error":"bad_size"}`.

Responses:

- `200` with `Content-Type: image/png` (or `image/webp` for `size=128`),
  `X-Content-Type-Options: nosniff`, `CDN-Cache-Control: no-store`, and
  `Cache-Control: public, max-age=31536000, immutable` when `v` equals the
  current hash, or `Cache-Control: public, max-age=60` when `v` is missing or
  stale. A stale `v` is answered with the current image and the short cache,
  not a redirect: the World only ever asks for the URL the roster gave it, so
  a stale hash means the roster copy is at most a few seconds old, and a
  redirect would cost every such load an extra round trip.
- `404 {"error":"not_found"}` with `Cache-Control: no-store` for a name that
  is not a valid account name, an unknown account, a node account, an account
  that is not active, a `profile_private` account, an account without an
  avatar, a stored value that is not a base64 PNG, and (for `size=128`) when
  the server cannot make thumbnails.

`GET /api/accounts/<name>` already returns `avatarPng` for any public
(non-private) account, active or not. The avatar endpoint is narrower: it
serves only roster members, so it never shows more than the roster and the
profile lookup already do.

## Thumbnails and the Pillow dependency

`_avatar_render_thumbnail` opens the stored PNG with Pillow, refuses a source
over 4096×4096 pixels (a small PNG can declare a huge canvas), converts to
RGBA so transparency is kept, centre-crops to a square, resizes to 128×128
with Lanczos (Pillow premultiplies alpha while resampling, so edges do not
darken) and encodes WebP at quality 80, method 6. Results are kept in an
in-process LRU of 512 entries keyed by the content hash (`_AVATAR_THUMBS`), so
a changed avatar gets a new entry and an old one ages out. A source that
cannot be decoded is cached as "no thumbnail".

Cold cost on the production avatars: 7 to 21 ms each, plus about 50 ms once
per process for loading Pillow's WebP encoder; the full cold world build was
94 ms and a warm one 4 ms.

Pillow is pinned in `app/host/requirements.txt` (`Pillow==12.3.0`, which has a
`cp313` `manylinux_2_28_x86_64` wheel, one of `WHEEL_PLATFORMS` in
`tools/next_deploy.py`). The Cloudflare rollback stack (Pyodide, kept until
2026-10-08) has no Pillow: `_avatar_image_module` then returns `None`, the
world view carries `avatarUrl` without `avatarThumb`, and `size=128` answers
404.

## Privacy audit of the public fields

The roster has been public and unauthenticated for a long time, so the only
new question is whether a shared cache holding it for a while changes
anything. Fields, and what the World reads (`normalizeMemberDirectory` in
`world/public/world/world.js`):

| field | what it is | World reads |
|---|---|---|
| `name`, `createdAt`, `kind`, `status` | public identity | yes (`kind` no) |
| `nodes` | linked node names, public like the catalog | yes |
| `emailVerified` | boolean only | yes |
| `issues`, `pulls`, `commits`, `discussions`, `achievementBadges` | public aggregates | yes |
| `totalActiveMs` | rounded down to a public bucket, no timestamps | yes |
| `totalSteps`, `officeVisits` | bounded lifetime counts, no samples or times | yes |
| `activityBucket` | coarse recency bucket, never the raw touch time | yes |
| `lastEmailAt`, `lastEmailStatus` | send time rounded down to the hour, delivered/failed; no address, kind or count | yes |
| `countryCode`, `flag`, `browser`, `os` | the coarse client the member chose to publish in the World | yes |
| `solana` | payout address, public profile data | yes |
| `mastodon`, `mastodonUrl` | public profile field | no |
| `userStatus` | away flag and custom status; expiry applied at read time and `expiresAt` checked by clients | no |
| `avatarUpdatedAt`, `referredBy` | public profile data | no |

None of them becomes more sensitive by being cached for 30 seconds. What a
shared cache does change is revocation: the App promises that turning
`profile_private` on stops new public reads at once (the profile save purges
the roster copies). A CDN or proxy holding `view=world` or an immutable avatar
would keep serving a member who just went private. So:

- the world view and the avatar responses carry `CDN-Cache-Control: no-store`
  (Cloudflare, Fastly and Akamai honour it), and the only shared copy is the
  server's own, which every privacy change purges;
- browsers may keep the world view for 30 s and reuse it for another 300 s
  while revalidating, and keep a hashed avatar for a year. A browser that
  already showed a face keeping it is not a new disclosure.

No field is dropped from the world view: the World reads almost all of them,
and the ones it does not read (`userStatus`, `mastodon`, `avatarUpdatedAt`,
`referredBy`, `kind`) are the same public values the default response has
always served; keeping them makes the world view the default roster minus
`avatarPng`, which is easy to check.

## Traps

- The server cache on the Next host is per process
  (`app/host/forkmesh_host/js.py` `_Cache`: 8 MB, entries over 2 MB are not
  kept). A purge on one core does not reach the others; the world copy's 90 s
  life and the 120 s roster TTL bound the lag. If the default roster grows past 2 MB it
  stops being cached, and every world rebuild then runs the full scan.
- `avatarUrl` is relative. `/api/*` on world.forkmesh.com goes to the App
  (`run_worker_first` in `world/wrangler.toml`, `deploy/core/caddy_routes.py`),
  so it works from both hosts.
