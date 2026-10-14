# Dashboard chat layout

`/dashboard/chat` uses a three-column chat layout matching the desktop app
([../desktop/ui-redesign.md](../desktop/ui-redesign.md)). Home page:
[README.md](README.md). Related: [web-composer.md](web-composer.md).

- **Channel rail** (from 768px): channels, direct messages (name-only links
  to `/chat`; this page never decrypts direct rooms), user panel.
- **Channel pane**: header, transcript, composer.
- **Member list** (from 1200px): online (a frame in the last 3 minutes) and
  offline (`/api/accounts/users`), 200 names per section. Hidden state is in
  `localStorage` `forkmesh.dashboard.chat.membersHidden`.

Under 768px the page is a single column. `regroupFullLog`, scheduled by a
`MutationObserver` on `#fullChatMessages`, groups consecutive messages from one
sender within 7 minutes and adds day dividers. Colours are `--dc-*` custom
properties; all rules are scoped to `html:not([data-world-embed="1"])` so the
World chat embed keeps its own look.

| Path | Responsibility |
| --- | --- |
| `app/public/dashboard/partials/views/chat.html` | Markup and stylesheet |
| `app/public/dashboard-chat.js` | Layout, grouping, member list, emoji picker |
| `app/public/dashboard/chat/index.html` | Built by `app/tools/build_dashboard_assets.py`; do not edit |

## Colours and event-only parts

The `--dc-*` properties now point at the dashboard theme tokens
(`--dashboard-*-rgb`), so the page follows `theme.json` in both themes instead
of a separate Discord palette. Selection, focus and links are blue: the current
channel gets the accent tint with a blue `#`, the composer gets a blue border
when focused, the send button is primary blue, and the unread count is a blue
pill. Green is used only for online presence. Avatars sit on the neutral
secondary fill, and source and member tags are outlined grey labels rather
than filled capitals.

Built by `dashboard-chat.js`: the empty and signed-out logs are `fm-blankslate`
blocks (centred in `#fullChatMessages` and excluded from the boxed notice rule),
the mention and slash suggestion lists are `fm-popover fm-menu` with 30px rows
and a green dot for people active in the room, the thread dialog uses the
`fm-dialog-header` and `fm-dialog-footer` rows with an invisible icon close,
and message actions are small invisible buttons (Delete is `fm-btn-danger`).
`dashboard-chat.js` is not scanned by the dashboard Tailwind build, so any
utility class it adds must also appear in a scanned source.

The standalone `/chat` page (`app/public/chat.css`) has no shell classes, so
the same rules are written there with its own tokens: `--primary` is blue
(join, save, send, thread reply, unread count), `--cta` is green and used only
by the Create button in the new-channel dialog, and `--online` is the green for
presence and the connected status dot. Its dialogs have a bordered header and
footer row with 28px buttons; Delete confirms and channel deletion use red text
that fills red on hover, errors are red-tinted alerts, the webhook-created box
is a blue info alert, and members and webhooks in settings are hairline-divided
rows instead of boxes inside the panel. The encryption chip still shows the
lock or globe emoji that `app/public/chat.js` writes.

World also loads `dashboard-chat.js` (`world/public/world/world.js`) but none
of the shell's `fm-*` CSS. So the blankslate icon carries `width="24"
height="24"` and the close icons `width="16" height="16"` (the shell's CSS
still sets the dashboard sizes), and `world/public/world/world.css` gives
`#fullChatMessages > .fm-blankslate` the same treatment as the old
`.rounded-lg` notice: one muted line with no icon in the HUD island feed, a
dashed box with a 20px icon in the native chat panel. The World thread
dialog's title rule matches `.fm-dialog-title` as well as the old `<strong>`.
