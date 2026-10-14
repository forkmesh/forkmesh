# #general, #random and #welcome are server channels (2026-10-14)

Part of the [app docs](../README.md). Code: `app/src/chat_channels_api.py`
(`EVERYONE_*`, `ensure_everyone_channels`, `post_welcome`), `app/src/entry.py`
(`_ChatChannelsRuntime.ensure_schema`, `everyone_members`,
`everyone_member_count`, the signup hook). Tests:
`app/tests/test_chat_channels_api.py` (`test_everyone_channels_*`,
`test_a_new_member_is_welcomed_*`).

## Why

The owner asked for every ForkMesh member to be in #general and #random, for
the phone's #general page to show the full member count, and for a #welcome
channel where each new member is greeted and everyone is asked to say hi. They
then asked for these to be real channels instead of labels.

Before this change, #general and #random were labels on frames inside one
shared relay room (`world-general`). The relay only stores ciphertext and has no
member list, so every client guessed the members from who was online and who
had posted. The owner chose to move every client to server channels. The old
room stays only as the World and visitor chat, shown as "World", so there are
not two #generals. The old room's #general history stays in World and is not
copied into the new #general.

## How

- **Three fixed public channels.** `ensure_everyone_channels` inserts
  `general`, `random` and `welcome` into `chat_channels` with fixed ids
  (`sha256("forkmesh-everyone-channel-v1:<name>")[:32]`), the record
  `{name, createdBy:"", visibility:"public", kind:"everyone", topic}`, and
  `ON CONFLICT DO NOTHING`. The real runtime runs it from `ensure_schema`
  (once per isolate), so no migration is needed: encrypted `data` cannot be
  written from SQL. Their `name_bi` uses the `chat-channel-name:everyone/`
  prefix so they cannot collide with a user's channel. Lookups go by id, so a
  rekey that randomises `name_bi` does not create duplicates.
- **Members are the account directory, not member rows.** Deleting a
  `chat_channel_members` row fires `trg_chat_channel_member_remove_rotate`,
  which bumps `key_version`. That moves the channel to a new room and empties
  its history, so a member row per account would reset #general every time an
  account was deleted. Instead, `GET /api/chat/channels/{id}/members` returns
  `{members:[{username, joinedAt}], everyone:true}` built from
  `_users_directory_public_payload`, and `GET /api/chat/channels` carries
  `memberCount` from `_public_member_count`. Both use the same rule as every
  member figure on the site (`_is_public_roster_member`: an active user whose
  profile is not private). A private-profile account can still read and post,
  since the channels are public, but it is not listed.
- **Fixed.** No one, admins included, can rename, delete, close, invite to,
  add webhooks to, or manage members of these channels:
  `_require_manager` returns 403 `everyone_channel_fixed` and `canManage` is
  false. Creating or renaming a channel to one of the three names returns 409
  `channel_name_taken`. They do not count toward `MAX_CHAT_CHANNELS`.
  `GET /api/chat/channels` lists them first, in the order general, random,
  welcome.
- **Welcome post.** When signup turns an account active, `post_welcome` sends
  `{type:"chat", senderId:"system:welcome", sender:"ForkMesh",
  channel:"#welcome", text:"🎉 Welcome to ForkMesh, @name! Everyone, come say
  hi 👋", webhook:true}` through `_chat_channel_deliver`. That is the path
  incoming webhooks use, so the room keeps it in `chat_history` and every
  client shows it, including clients that were offline at the time. It skips
  the per-user webhook bell, so a signup does not notify every account. The
  World fireworks frame (`_broadcast_new_member`) is unchanged.
- Notifications are unaffected. Public channels never used member rows for
  pushes or unread reminders (`push_chat_room_apns`, `channel_reminders`).

## Clients

- iPhone: `docs/native/NAVIGATION.md` 2.0.102 and `native/ios/docs/FMChat.md`.
- Web `/chat`: [chat-private-rooms.md](chat-private-rooms.md), section
  "#general, #random and #welcome on web `/chat`".
- Desktop: [docs/desktop/README.md](../desktop/README.md), the 2026-10-14 entry.
