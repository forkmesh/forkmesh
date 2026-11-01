# Public member roster, avatars and boot latency

Part of [the App docs](README.md). `GET /api/accounts/users` is the public
member roster: active `user` accounts that are not `profile_private`, oldest
first. Chat, desktop, native apps, dashboard and World read it. This page
covers the roster and avatar endpoints, the privacy rules every public
surface shares, and how the App keeps the World's first requests fast
([Boot latency](#boot-latency-on-the-core-nodes)). How a profile page picks
an avatar is in [profile-links.md](profile-links.md#profile-page-avatars).
Code: `app/src/account_users.py`
(`_account_users_directory`, `_account_users_world_directory`,
`_account_avatar`), routes in `accounts_handler`. Tests:
`app/tests/test_account_avatar_world_roster.py`.

## Endpoints

| Route | Response |
| --- | --- |
| `GET /api/accounts/users` | Full roster with inline `avatarPng`, `no-store`; admin variant for signed admins. Cached server-side 120 s (`USERS_DIRECTORY_CACHE_KEY`). |
| `GET /api/accounts/users?view=world` | Public roster without `avatarPng`, with `avatarUrl` and (with Pillow) a 128×128 WebP `avatarThumb`. `public, max-age=30, stale-while-revalidate=300`, `CDN-Cache-Control: no-store`, weak `ETag`. Cached under `USERS_WORLD_CACHE_KEY`. |
| `GET /api/accounts/<name>/avatar[?v=<hash>&size=128]` | Stored PNG or WebP thumbnail; `immutable` when `v` matches, else `max-age=60`; 404 for anyone not on the public roster. |

## Rules

- Every roster change (signup, profile, status, admin edit, deletion) purges
  both cache keys via `edge_cache_delete`, which bumps an epoch so an in-flight
  build cannot restore a member who just went private.
- `profile_private` must take effect at once, so no CDN may cache roster or
  avatar responses; fields are public aggregates or coarse buckets only.
- The `/api/accounts/<name>` lookup reads the encrypted record and answers a
  private profile before it consults `_account_lookup_cached`, so a warm entry
  cannot outlive a privacy toggle
  (`app/tests/test_security_controls.py::test_private_profiles_and_repo_pages_revoke_cached_public_metadata`).
- Account search (`_WorldCommunityRuntime.search_accounts`, behind
  `GET /api/chat/direct-messages/users?query=` and the notes share picker)
  uses the same `_is_public_roster_member` test, so a prefix search never
  lists a private profile. Tests:
  `app/tests/test_account_search_private_profiles.py`.
- The follow state in `GET /api/accounts/<name>?viewer=<name>` comes only
  from the caller's own session (`_account_lookup_viewer`): a `viewer` that is
  not the signed-in account gets `isFollowing: false`, so nobody can rebuild
  the follow graph from query values. Tests:
  `app/tests/test_boot_endpoint_caches.py`.
- Referral surfaces use the same `_is_public_roster_member` test: the
  `referredUsers` names on a profile and on the referral leaderboard, the
  leaderboard rows themselves, and the directory's `referredBy` leave out
  private, inactive and missing accounts (`_public_referral_bis`). The
  referral preview page and `/api/referrals/<name>/card.png` refuse a private
  profile (`_referral_account(..., public=True)`); the `/r/<name>` redirect and
  the credit path still work for it. A profile edit and an admin profile save
  purge `REFERRAL_LEADERBOARD_CACHE_KEY`. Tests: `app/tests/test_referrals.py`.
- `GET /api/badges/<name>` answers 404 for a private profile unless the
  caller's session is the account or owns the node, and that answer is
  `private, no-store` (`app/tests/test_badges.py`).
- The network contributors board labels a private profile's row "Private
  contributor" (`app/tests/test_leaderboards_hub.py`).
- Thumbnails (`_avatar_render_thumbnail`, Pillow) refuse sources over
  4096×4096 and are cached in a 512-entry LRU keyed by content hash.
- The host cache is per process; lag between the core nodes is bounded by the
  cache TTLs. Builds re-decrypt only changed rows
  ([Boot latency](#boot-latency-on-the-core-nodes)).

## Boot latency on the core nodes

Each core ([architecture](../architecture.md)) runs one App process on a
single event loop, so every statement a boot request skips shortens the
queue for all others. The World itself is in [docs/world](../world/README.md).
Code: `app/src/account_users.py`, `app/src/entry.py` (`edge_cache_delete`,
`host_warm_caches`), `app/src/d1_read_guard.py` (`shared_read`),
`app/host/forkmesh_host/`.

- **Background refresh**: `env.HOST_BACKGROUND` runs one task per key and
  shares in-flight loads; without it (no host, as in unit tests) reads run
  inline.
- **Stale-while-revalidate copies**: world roster (fresh 30 s, kept 1 h, weak
  `ETag`), guest profiles (fresh 60 s), contributions (fresh 60 s,
  generation-keyed), wallet readings (last good value for 24 h).
  `edge_cache_delete` bumps `EDGE_CACHE_EPOCHS` so purges win over in-flight
  builds. Privacy checks always run before a cached copy is served.
- **Roster**: only changed user rows are fetched and decrypted.
- **Session memo**: on the host, GET/HEAD session and `_is_admin` lookups are
  reused for 5 s and cleared on writes to `users`, `account_sessions` or
  `account_devices` (`_d1_write_watch`).
- **Warm-up**: after the vault unseals, `host_warm_caches` builds the roster,
  table inventory and wallet readings. `FORKMESH_HOST_WARM=0` disables it.
- **Server-Timing**: every `/api/*` response carries `app` and `db` durations
  plus `Timing-Allow-Origin` for the allowed CORS origin.

Tests: `app/tests/test_boot_endpoint_caches.py`,
`app/tests/test_boot_api_latency_round5.py`,
`app/tests/host/test_host_server_timing.py`.
