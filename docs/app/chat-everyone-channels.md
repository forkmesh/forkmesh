# #general, #random and #welcome are server channels

Part of the [app docs](../README.md). Code: `app/src/chat_channels_api.py`
(`EVERYONE_*`, `ensure_everyone_channels`, `post_welcome`), `app/src/entry.py`
(`_ChatChannelsRuntime.ensure_schema`, `everyone_members`,
`everyone_member_count`, the signup hook). Tests:
`app/tests/test_chat_channels_api.py`.

Every member belongs to three fixed public channels. The shared relay room
`world-general` remains only as the World and visitor chat ("World").

- **Creation.** `ensure_everyone_channels` (run from `ensure_schema`) inserts
  `general`, `random` and `welcome` into `chat_channels` with fixed ids
  (`sha256("forkmesh-everyone-channel-v1:<name>")[:32]`), `kind:"everyone"`,
  `ON CONFLICT DO NOTHING`. Their `name_bi` uses the
  `chat-channel-name:everyone/` prefix; lookups go by id so a rekey cannot
  duplicate them.
- **Membership is the account directory**, not `chat_channel_members` rows
  (removing a member row rotates the channel key and empties history).
  `GET /api/chat/channels/{id}/members` returns `everyone:true` and the public
  roster; `memberCount` follows `_is_public_roster_member`.
- **Fixed.** Nobody can rename, delete, close, invite to, add webhooks to or
  manage members of them: 403 `everyone_channel_fixed`. Using one of the names
  for another channel returns 409 `channel_name_taken`. They do not count
  toward `MAX_CHAT_CHANNELS` and are listed first.
- **Welcome post.** When signup activates an account, `post_welcome` delivers
  a system message to `#welcome` through the incoming-webhook path, so it is
  stored in `chat_history`, without the per-user webhook bell.
- Pushes and unread reminders never used member rows for public channels.

Clients: [chat-private-rooms.md](chat-private-rooms.md) (web),
`native/ios/docs/FMChat.md` (iPhone), [docs/desktop/README.md](../desktop/README.md).
