# Chat stickers on messages

Anyone in a conversation can stick a sticker onto a message, the way iMessage
lets you peel a sticker onto a bubble. Stickers sit tilted over the top-right
corner of the message; reactions stay as chips under it. Home page:
[app/README.md](README.md). Clients: the desktop (`desktop/src/StickerCatalog.h`,
`MessageRow`), the web chat (`app/public/chat.js`), the dashboard chat
(`app/public/dashboard-chat.js`), iOS (`FMChat`) and Android (`core/chat`).

## Wire format

A sticker travels as a `reaction` frame with one extra field, `sticker`:

```json
{"type": "reaction", "conversation": "#general", "target": "<message id>",
 "emoji": "✅", "sticker": "lgtm", "reactorId": "<node>",
 "reactorName": "Pat", "added": true}
```

Reusing the reaction frame means stickers are already durable on the relay,
sealed in v3/v4 rooms, delivered over the mesh and checked like reactions
(`reactorId` must equal the frame's `node`), with no new frame type for every
relay, mirror and controller list to learn. `added: false` takes the sticker
off again.

`emoji` carries the sticker's fallback emoji as literal unicode, which every
client already maps onto its reaction vocabulary. A client that predates
stickers ignores `sticker` and shows the fallback as an ordinary reaction chip,
so a sticker never vanishes for someone on an older build.

A client that knows stickers:

- keys a sticker by its slug, separately from reactions, so a 🎉 reaction and
  the `party` sticker on the same message never undo each other;
- treats a `sticker` value it does not know (a newer catalog) as the fallback
  reaction, as an older client would;
- lets each person place each sticker once per message; placing it again
  removes it (`added: false`), like toggling a reaction;
- does not offer stickers in Discord conversations, which have no such thing.

## Catalog

The slugs, texts, colours, tilts and fallbacks are identical on every client;
add a sticker to all of them at once.

| Slug | Text | Background | Tilt | Fallback |
|---|---|---|---|---|
| `lgtm` | LGTM | `#8250df` | -6° | ✅ U+2705 |
| `ship-it` | Ship it | `#0969da` | 5° | 🚀 U+1F680 |
| `nice` | Nice! | `#bf3989` | -4° | 👍 U+1F44D |
| `thanks` | Thanks! | `#bc4c00` | 6° | 🙏 U+1F64F |
| `on-it` | On it | `#57606a` | -5° | 👀 U+1F440 |
| `wow` | Wow! | `#cf222e` | 4° | 😮 U+1F62E |
| `love-it` | Love it | `#d6336c` | -3° | ❤️ U+2764 U+FE0F |
| `party` | Party! | `#9a6700` | 7° | 🎉 U+1F389 |

Stickers are text badges rather than images so they render the same on every
platform without shipping artwork or trusting an emoji font: bold white text on
the background colour, a white die-cut border, a soft shadow, rotated by the
tilt. Several stickers on one message overlap slightly, in catalog order (the
same on every client); a sticker placed by more than one person shows `×N`, and its
tooltip (or long-press) lists who placed it. Clicking or tapping a sticker
toggles your own placement of it.

The picker opens from a "Add sticker" action next to "Add reaction" on each
message and shows the whole catalog.

## Desktop

`desktop/src/StickerCatalog.h` holds the catalog and the two wire helpers. Inside
the app a sticker is the reaction value `sticker:<slug>`, so it rides the same
`m_reactions` maps, `reactionChanged` signals, office outbox and pending
mutations as any reaction; only the frame boundaries translate it:
`stickers::writeWire` where `ServerNode` and `OfficeChannelMirror::directPlain`
build a reaction frame, `stickers::valueFromWire` where `ServerNode` and
`roomMutationFromPlain` read one. The mirror still checks the wire `emoji`
against its 32-character limit. `MainWindow::toggleChatReaction` refuses a
sticker in a Discord conversation.

`MessageRow::setReactions` splits sticker values out of the reaction chips.
Each sticker is a `QLabel` child of the row (not in a layout) painted by
`stickerPixmap`; `placeStickers` puts the strip just after the end of the
message text (the body's `QTextDocument::idealWidth`) on its first line, or
after the image, video or name when there is no text. The row's right margin
grows by the strip's width, so long text wraps before the stickers instead of
running under them; the strip is placed again whenever the anchor moves or is
resized. "Add sticker" sits next to "Add reaction" in the classic reaction bar
and as a second hover button in the Next theme. Tests: the sticker checks in
`testOfficeRoomMutationFrames` in `desktop/tests/test_chat_transport.cpp`.

## Android

`StickerCatalog` (`core/chat/ChatFrames.kt`) holds the catalog. `ChatExtras`
keeps stickers in their own map (message, slug, placer id and name), apart
from the reaction state, and reads a `reaction` frame as a sticker only when
its `sticker` slug is in the catalog; otherwise the frame goes down the reaction
path with its `emoji`. `stickers(id)` returns `StickerChip`s in catalog order,
and `ChatRow.stickers` carries them to the screen. `ChatHub.toggleSticker`
sends through `ChatExtras.toggleSticker` (relay rooms, undone if the send
fails) or `DirectMessageService.react(..., sticker)` (direct messages, which
also covers the mesh `direct-sealed` copies), and refuses mesh and Office
conversations (`StickerCatalog.canOffer`). The frame's `emoji` is the literal
fallback, not the reaction slug Android sends for its own catalog reactions,
so an older client of any platform reads it. Discord channels have their own
screens and row type, so they never reach this path. `ChatHistoryStore`
saves stickers under a separate `stickers` key, so an older build reading
the file drops them rather than misreading them as reactions.

`ChatStickers` in `feature/chat/ChatMessageParts.kt` overlays the strip on the
top-end corner of the message body, raised 10 dp so it overlaps the edge;
each badge is bold white text on the catalog colour, a 2 dp white border,
6 dp corners and a small shadow, rotated by the tilt. Tapping toggles your
placement; a long press opens a menu listing who placed it. The long-press
message sheet has an "Add sticker" row (first in the list, just under the
reaction strip and its "More reactions" button, because an eighth round
button does not fit narrow phones), which opens the `pickSticker` sheet. Tests:
the sticker tests in `ChatExtrasTest` and
`DirectMessagesTest.testStickersTravelBothWaysInADirectMessage`.

## iOS

- `StickerCatalog` (`FMChat/Threads/ChatFrames.swift`) holds the table above;
  `StickerEntry.rgb` is the background without alpha.
- `ChatExtras.applyReaction` checks the wire `emoji` first (present, at most
  16 characters, `reactorId` equal to the sender) for stickers and reactions
  alike. A `sticker` slug found in the catalog goes to a separate
  `stickerState` (message → slug → placer → name); any other value, unknown
  slugs included, falls through to the ordinary reaction path, so the fallback
  shows as a chip.
- `ChatExtras.stickers(for:)` returns `StickerChip`s in catalog order with the
  `×N` label. `ChatHub.toggleSticker` sends through the relay
  (`ChatExtras.toggleSticker`) or, in direct messages and office rooms, through
  `postRoomFrame` with `ChatExtras.stickerFields`; `DirectMessageService`
  carries the extra field through sealing untouched.
- Saved history keeps stickers under `stickers` next to `reactions`
  (`ChatHistoryCodec`); a restore drops slugs the catalog no longer has.
- The badges (`ChatStickerStack`, `ChatMessageParts.swift`) sit over the
  top-trailing corner of the room row and of the Messages-style bubble. On a
  received bubble that carries stickers the tapback badge moves to the leading
  corner so the two do not cover each other. Long-press on a badge lists who
  placed it; the Messages tapback overlay lists sticker placers too. "Add
  sticker" sits next to "React" in the message menu and next to the add
  reaction button under the chips. Discord conversations use their own screens
  and never get the action.
- Tests: `ChatExtrasTests` (`testStickers…`, `testPlacingASticker…`,
  `testStickerFieldsAndCatalog`) and
  `DirectMessageReceiptsTests.testStickersTravelBothWaysInADirectMessage`;
  `FM_ONLY=FMChat swift test --filter Sticker` runs them on Linux.

## Web

`app/public/chat.js` (the `/chat` page) and `app/public/dashboard-chat.js`
(`/dashboard/chat`, also loaded by World) each hold `STICKER_CATALOG` and a
`stickers` map (message → slug → placer id → name) next to `reactions`.
`applyReaction` hands a frame whose `sticker` is in the catalog to
`applySticker`; an unknown slug falls through to the reaction path with its
`emoji`. `toggleSticker` sends the same `reaction` frame as `toggleReaction`
plus `sticker`, so direct messages and sealed rooms carry it unchanged, and
every place that forgets a message's reactions forgets its stickers too.

`renderStickers` fills a `.chat-stickers` strip appended to the message row
(`.chat-msg` and `.chat-thread-message` on `/chat`, `.chat-message-row` on the
dashboard), positioned over the row's top-right corner. Each badge is a button
whose background and `--chat-sticker-tilt` come from the catalog as inline
style, so the theme never recolours them; your own placements get a blue ring
(`is-mine`). The row gets `has-stickers`, which moves the hover action toolbar
below the strip so it never covers a sticker you want to click. "Add sticker"
(a sticker icon on `/chat`, a "Sticker" button on the dashboard) sits right
after "Add reaction" and opens a popover of all eight badges: the shared
`.chat-sticker-picker` on `/chat`, an in-row picker on the dashboard that takes
the reaction picker's slot so only one is open.

Styles: `app/public/chat.css`, `app/public/dashboard/partials/views/chat.html`
(rebuild `dashboard/chat/index.html` with `app/tools/build_dashboard_assets.py`)
and `world/public/world/world.css` (native chat panel; hidden in the HUD
feed). The dashboard builds no actions or stickers for Discord rows
(`record.external`); `/chat` has no Discord conversations. Tests:
`app/tests/test_chat_stickers_frontend.py` runs both clients' sticker code in
node against the catalog table above.
