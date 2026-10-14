# Private Admin Chat Channels Design

> **Superseded security boundary (August 28, 2026):** This design accurately
> records the original relay-readable implementation, not current protected
> chat. Private channels and direct messages now add automatic X25519 recipient
> encryption inside the relay-derived room cipher; public channels remain
> controller-readable for integrations. See
> [`security/sealed-chat.md`](../../security/sealed-chat.md) for the shipped
> protocol, migration behavior, and active-controller limitations.

## Summary

Public `#general` is unchanged. Private channels are reachable only by platform
administrators (`users.is_admin = 1`, read from the database on every
privileged request) and explicitly invited registered users. An invitation
grants membership at once. Each channel has its own relay-derived shared key
and room namespace, so membership is enforced before a client receives the key
or enters the room. Because the relay holds `DATA_KEY`, this layer alone is
shared-key encryption, not end-to-end encryption.

## Authorization

- Every request needs an active user session; node-only and anonymous sessions
  cannot list or open private channels.
- Access requires admin status or a membership row. Only admins create
  channels and add or remove members.
- Missing and unauthorized channels return the same `404 not_found` on
  metadata, key and WebSocket routes, so no route is an existence oracle.

## Storage

- `chat_channels`: random opaque `channel_id`, blind-indexed normalized name
  (unique), encrypted display data, creator, timestamps, `key_version`.
- `chat_channel_members`: `(channel_id, member_bi)` primary key, inviter,
  join time, index on `member_bi`.
- Names are lowercase slugs of 1 to 40 characters. Limits: 100 channels,
  500 members per channel.

## Keys, rooms and tickets

- The passphrase is derived from `DATA_KEY`, `channel_id` and `key_version`
  and returned only after authorization. The room key includes both
  `channel_id` and `key_version`.
- Adding a member does not rotate the key. Removing one increments
  `key_version` in the same operation; old-version history is not replayed
  into the new version and expires with the normal seven-day retention.
- WebSocket upgrades use a signed ticket valid for 60 seconds that binds the
  channel, key version, requester and expiry. The relay rechecks authorization
  before routing. Tickets and keys stay in memory only.

## API

| Route | Who | Result |
| --- | --- | --- |
| `GET /api/chat/channels` | Any user | Channels the caller may open, with `canManage` |
| `POST /api/chat/channels` | Admin | `201`; `400 invalid_channel_name`, `409 channel_name_taken`, `429 too_many_channels` |
| `GET/POST/DELETE /api/chat/channels/{id}/members` | Admin | List, add (idempotent) or remove (rotates the key) |
| `GET /api/chat/channels/{id}/room-access` | Admin or member | Key version, passphrase, ticketed WebSocket URL; `no-store` |
| `GET /api/chat/channels/{id}/ws?ticket=…` | Ticket holder | WebSocket upgrade after re-authorization |

Errors: `401 invalid_session`, `403 admin_required` (audited),
`404 user_not_found`, `404 not_found`, `500 unavailable`. Audit records carry
counts and reasons only, never names, keys or tickets.

## Attachments

All browser chat surfaces send desktop-compatible encrypted attachments
(`fileName`, `fileMime`, base64 `file`) inside the same encrypted message. One
attachment per message, at most 1 MiB decoded. Images preview; other files
render as download cards. Rooms keep a 16 MiB retained-byte budget alongside
the seven-day and 500-frame limits, dropping the oldest frames first.
