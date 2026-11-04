# #general, #random and #welcome are server channels

Every member belongs to three fixed public channels; this page covers how the
server keeps them and the genie spider welcome posted in `#welcome`. The
shared relay room `world-general` remains only as the World and visitor chat
("World"). Part of [the App docs](README.md).

## The channels

Code: `app/src/chat_channels_api.py`
(`EVERYONE_*`, `ensure_everyone_channels`, `post_welcome`), `app/src/entry.py`
(`_ChatChannelsRuntime.ensure_schema`, `everyone_members`,
`everyone_member_count`, the signup hook). Tests:
`app/tests/test_chat_channels_api.py`.

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
  toward `MAX_CHAT_CHANNELS_PER_CREATOR` or the hourly creation budget and
  are listed first.
- **Welcome post.** When signup activates an account, `post_welcome` delivers
  a system message to `#welcome` through the incoming-webhook path, so it is
  stored in `chat_history`, without the per-user webhook bell.
- Pushes and unread reminders never used member rows for public channels.
- **Genie spider welcomes.** The web chat's Welcome button in `#welcome`
  posts an animated genie spider ([below](#genie-spider-welcomes-in-welcome)).

Clients: [chat-private-rooms.md](chat-private-rooms.md#everyone-channels-on-web-chat)
(web), `docs/native/ios/FMChat.md` (iPhone),
[../desktop/README.md](../desktop/README.md).

## Genie spider welcomes in #welcome

Code: `app/public/chat-genie-welcome.js`
(the 10 scenes, `pickGenieWelcome`, `drawGenieWelcome`, `mountGenieWelcome`,
`loadGeniePainter`), `app/public/chat.js` (`isWelcomeChannel`,
`sendGenieWelcome`, `renderGenieWelcome`), the `#chat-genie-welcome` button in
`app/public/chat.html`. Tests: `app/tests/test_chat_genie_welcome.py`.

### The button

In the everyone channel `welcome` a signed-in reader
sees a **Welcome** button in the channel header. It posts an ordinary `chat`
frame with two extra parts:

- `genieWelcome`: an integer 0–9, the scene, picked at random by
  `pickGenieWelcome`, never the same as this tab's previous press;
- `text`: that scene's sentence ("🪔 A genie spider pops out of a magic lamp
  to say welcome!"), so clients that do not know the field (desktop, phones)
  still show a readable message.

The button rests for 5 s after a press so one person cannot flood the channel.

### The scenes

`silk` (drops in on a thread), `lamp` (pops out of a magic lamp in purple
smoke), `confetti`, `carpet` (flies in on a magic carpet), `twirl` (spins in
with sparkles), `bounce` (three hops with dust puffs), `balloons` (floats down
on three balloons, then lets them go), `web` (spins a web and writes
WELCOME!), `fireworks` and `peekaboo` (peeks out of a hole, then hops up).

Each scene is a function of time over 4 s on a 280 × 150 stage. It returns the
spider's place, scale, turn and pose (reactions such as wave, cheer, giggle,
nod and eureka) plus effects drawn behind and in front of it. Every reaction
ends before 4 s, so the last frame is a calm spider, which is what a message
shows when it is not playing.

### Drawing

The spider is the web Genie painter (`ForkMeshGenie.paint` in
`www/public/genie-spider.js`, [../www/genie-spider.md](../www/genie-spider.md)),
loaded once on demand. The chat page has no wandering spider, so the loader
sets `data-genie="off"` on `<html>` before the script runs and only the
painter is used. If the script fails to load, the stage and its effects still
draw.

A message with a valid `genieWelcome` gets a stage (a button) under its text.
It plays once when it first scrolls into view if it is your own or less than
10 minutes old, and not again when the channel is redrawn
(`playedGenieWelcomes`). Clicking it replays it. With
`prefers-reduced-motion` it shows only the last frame. The stage and the
button carry `data-instance-plugin="genie"`, so a white-label instance with the
`genie` plugin off hides both.

Only the web chat has the button and the animation; other clients show the
sentence.
