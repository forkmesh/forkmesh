# Dashboard chat layout

`/dashboard/chat` is laid out like Discord (2026-10-12, at the user's request,
from a Discord screenshot). The desktop app got the same look in the same
change; see [../desktop/ui-redesign.md](../desktop/ui-redesign.md). Home page:
[README.md](README.md). Related: [chat-formatting.md](chat-formatting.md),
[chat-private-rooms.md](chat-private-rooms.md),
[web-composer.md](web-composer.md).

## Layout

Three columns inside the dashboard's content area, under the usual dashboard
header:

- **Channel rail** (240px). A 48px "ForkMesh" workspace header, a
  collapsible "Text Channels" section listing #general and the seven World
  space channels, and a "Direct messages" section once you are signed in.
  Channel rows are 32px with a muted `#`, a rounded hover and a selected
  background inset 8px from the rail edge. A direct-message row with unread
  messages is bold, with a red count pill. The + beside "Text Channels" opens
  the private channel directory at `/chat`. A user panel at the bottom shows
  your avatar with a presence dot, your name, and Online or Offline.
- **Channel pane.** A 48px header holds the muted `#`, the channel name in
  bold, a thin divider, the topic, the "Open private channels" link, the
  connection status and the member-list toggle. Below it is the transcript,
  then the composer.
- **Member list** (240px). "Online — N" and "Offline — N" sections with 32px
  round avatars. Online people have a green dot, and offline people are
  dimmed to 35% opacity. ForkBot is listed online with a BOT tag. Someone
  counts as online if they sent a frame in the last 3 minutes, the same rule
  the @-mention picker uses (`mentionIsOnline`). The offline list is the
  registered-account directory (`/api/accounts/users`), which the page now
  fetches as soon as the member list is visible. Only the first 200 names in
  each section are drawn, and only the first 24 offline rows fetch a profile
  picture.

The rail shows from 768px wide, and the member list from 1200px. Under 768px
the page keeps its old single-column layout: header, transcript and composer.
The direct-message list then moves back under the header. The member-list
toggle is remembered in `localStorage` (`forkmesh.dashboard.chat.membersHidden`).

Clicking a channel row switches rooms in place through `switchChatSpace`, the
switch the World already uses, and rewrites `?space=` in the address bar. The
`#fullChatChannel` select keeps its old behaviour, which reloads into the World
embed.

The rail's direct-message chips stay name-only: this page never opens or
decrypts a direct room (a chip links to `/chat`), so it has no last line to
show. The full chat page `/chat` and Office show the newest line of every
conversation and the other person's picture
([chat-private-rooms.md](chat-private-rooms.md#last-line-and-pictures-in-the-conversation-list-2026-10-13)).

## Transcript

- Avatars are 40px circles with a 16px gap. The name is semi-bold and the
  timestamp is faint 12px: "4:22 PM" today, "Yesterday at 11:48 PM", and
  "10/1/26, 12:27 AM" for anything older (`fmtChatStamp`). Hovering the time
  shows the full date.
- A message from the same sender, on the same day and within 7 minutes of the
  one before, collapses to just its text. Hovering it shows its time in the
  avatar gutter.
- A centred date divider ("October 8, 2026") starts each day.
- The whole row highlights on hover. The message actions (Reply, react, Issue,
  Edit, Delete) float as a toolbar at the row's top right. On touch screens
  they stay inline and visible.
- Reactions are small rounded chips, with a ☺ chip after them to add another.
- The "#general" source badge is hidden when it only names the current
  channel. Other source labels, such as Discord-bridged messages, show as a
  small app-style badge.

Grouping and date dividers come from a pass over the log's rows,
`regroupFullLog`. A `MutationObserver` on `#fullChatMessages` schedules it once
per frame, so history replay, older-history reveal, live rows, edits and
deletes all regroup without each code path calling it. Each row carries
`data-chat-sender`, `data-chat-timestamp` and `data-chat-short-time` for the
pass, and grouped rows get `data-chat-grouped`.

## Composer

The input is one rounded (8px) bar in the composer colour. It has a round +
attach button on the left, the message box (one line high, growing as you
type), and an emoji button, the send button and the Task button on the right.
The emoji button opens an 18-emoji picker that inserts at the cursor. The
channel, repository and action selects stay as a slim row above the bar,
because the composer's action modes (chat, task, issue, bot) depend on them.
The hint line sits under the bar.

## Theme

The palette is Discord's, in the dashboard's dark and light themes. Dark: rail
#2b2d31, user panel #232428, pane #313338, hover #35373c, selected #404249,
text #dbdee1, composer #383a40, brand #5865f2, unread #f23f43, online
#23a55a. Light: rail #f2f3f5, user panel #ebedef, pane #ffffff, hover #e8e9eb,
selected #d7d9dc, text #313338, composer #ebedef. The colours are `--dc-*`
custom properties on `[data-dashboard-chat-view]`, set in the
`<style data-dashboard-chat-discord-styles>` block of
`app/public/dashboard/partials/views/chat.html`. The styles are hand-written
rather than Tailwind classes because `dashboard/tailwind.css` is a hand-run
build.

## World embed

Every rule is scoped to `html:not([data-world-embed="1"])`. The World's
`?worldEmbed=1` chat panel loads the same view but hides the rail, the member
list, the topic and the emoji button. The JS changes only run when
`discordChatLayout()` is true, which is never in the embed
(`WORLD_EMBED_BUBBLES`) or in the World's native chat markup. The embed keeps
its bubble look.

## Code map

| Path | Responsibility |
| --- | --- |
| `app/public/dashboard/partials/views/chat.html` | Rail, header, member list and emoji button markup, plus the stylesheet |
| `app/public/dashboard-chat.js` | `discordChatLayout`, `fmtChatStamp`, `regroupFullLog`, `renderMemberList`, `syncDiscordChrome`, `mountDiscordChrome` and the composer emoji picker. It also moves the direct-message strip into the rail on wide screens |
| `app/public/dashboard/chat/index.html` | Built from the two files above by `app/tools/build_dashboard_assets.py`; never edit it by hand |
