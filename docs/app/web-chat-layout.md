# Web chat layout

The website has one chat page, `/chat` (`app/public/chat.html`,
`app/public/chat.js`, `app/public/chat.css`): World `#general`, the
`#general`/`#random`/`#welcome` channels, private channels, direct messages,
calls, threads and channel settings. Part of [the App docs](README.md).
Related: [web-composer.md](web-composer.md),
[chat-private-rooms.md](chat-private-rooms.md),
[chat-member-stats.md](chat-member-stats.md) (the activity line under each
person).

## The retired `/dashboard/chat`

- `/dashboard/chat` and `/dashboard/chat/` 308 to `/chat`, keeping the query
  (`LEGACY_PAGE_PATHS` in `app/src/static_routes.py`), as does
  `/dashboard?section=chat`. They are permanent redirects, so the entries stay.
- The dashboard sidebar's Chat link, the `?section=` shim and World's page
  switcher and "open full chat" button open `/chat`. World's `#general` board,
  and an old `/dashboard/chat` link inside World, open World's own chat panel.
- The page is not in `dashboard_shell.PAGES`, so `/dashboard/chat` is not
  excluded from `run_worker_first` and reaches the app for the redirect.
- `app/public/dashboard-chat.js` is still the World chat client
  (`world/public/world/world.js` loads it), with the `#world-*` space
  channels, avatar emotes, the task and issue composer actions and
  organization Discord channels. Its rail, member list and day grouping
  do nothing without the old dashboard markup.

## Layout

- **Channel rail**: a filter (Ctrl/⌘ + K), channels, direct messages.
- **Conversation**: header (settings, add person, retention, calls,
  encryption), transcript, composer.
- **People pane**: Online, Offline, World guests and Nodes. Each account row
  links to the profile, has a message button and carries the activity line.
  Below 900px it opens as a drawer over the conversation.

`?embed=office` renders the same page inside the Office
([../office/README.md](../office/README.md)).

## Colours and event-only parts

`app/public/chat.css` has no shell classes, so the theme roles are written
there with its own tokens: `--primary` is blue (join, save, send, thread reply,
unread count), `--cta` is green and used only by the Create button in the
new-channel dialog, and `--online` is the green for presence and the connected
status dot. Its dialogs have a bordered header and footer row with 28px
buttons; Delete confirms and channel deletion use red text that fills red on
hover, errors are red-tinted alerts, the webhook-created box is a blue info
alert, and members and webhooks in settings are hairline-divided rows instead
of boxes inside the panel. The encryption chip still shows the lock or globe
emoji that `app/public/chat.js` writes.

World loads `dashboard-chat.js` but none of the dashboard shell's `fm-*` CSS.
Built by `dashboard-chat.js`: the empty and signed-out logs are `fm-blankslate`
blocks, the mention and slash suggestion lists are `fm-popover fm-menu` with
30px rows and a green dot for people active in the room, the thread dialog
uses the `fm-dialog-header` and `fm-dialog-footer` rows with an invisible icon
close, and message actions are small invisible buttons (Delete is
`fm-btn-danger`). The blankslate icon carries `width="24" height="24"` and the
close icons `width="16" height="16"`, and `world/public/world/world.css` gives
`#fullChatMessages > .fm-blankslate` one muted line with no icon in the HUD
island feed and a dashed box with a 20px icon in the native chat panel. The
World thread dialog's title rule matches `.fm-dialog-title` as well as the old
`<strong>`.

## Clearing the empty state

The "Type below to join" blankslate and the rail's "Type to join" line carry
`data-chat-empty`, and `clearEmptyState()` looks for that child before each
new row. Never read the logs' whole `textContent` per message: its cost grows
with the history, and it once made Discord refresh append slices (meant to
stop after 6 ms) take up to 1.3 s each.

## Links and the World embed

`/chat#room=<channel id>` and `/chat#direct=<conversation id>` open that
conversation. When the conversation is not known yet the page loads the
channel or direct-message list first, so a fresh page does not drop the hash
and fall back to #general.

`/chat?embed=world` is the World's conversation pane: the same page without
the site header, the room list and the people column, set on `<html>` before
first paint like `embed=office`. The World's channel list
([../world/voice.md](../world/voice.md)) opens channels and direct messages in
it, with every chat feature and call button working.
