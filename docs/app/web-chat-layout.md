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
