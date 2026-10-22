# Public member roster and avatars

`GET /api/accounts/users` is the public member roster: active `user` accounts
that are not `profile_private`, oldest first. Chat, desktop, native apps,
dashboard and World read it. Code: `app/src/account_users.py`
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
  uses the same `_is_public_roster_member` test. Before 2026-10-22 it checked
  only active `user`, so a prefix search listed private profiles the roster
  hides. Tests: `app/tests/test_account_search_private_profiles.py`.
- Thumbnails (`_avatar_render_thumbnail`, Pillow) refuse sources over
  4096×4096 and are cached in a 512-entry LRU keyed by content hash.
- The host cache is per process; cross-core lag is bounded by the cache TTLs.
  Builds re-decrypt only changed rows ([world-boot-latency.md](world-boot-latency.md)).
