# World interface

Part of [World](README.md). The HTML chrome around the 3D scene: the light
theme, inline notices instead of toasts, the HUD island with chat and the
agent composer, the movable HUD rails, the fresh-start reset and crash
reports. Voice, camera and the channel list are on [voice](voice.md).

## UI chrome

Everything that is not the 3D scene is a light, low-chrome UI in
`world/public/world/world.css`: light tokens in the `:root` blocks, green only
for buttons and blue for inputs, focus, links and selection, and a
`.fm-world` component layer at the end of the file that new chrome rules
belong in. Panels have fixed sizes (Settings is one centered modal; phones get
a full-screen sheet). In-world panels are canvases painted through a proxied
2D context (`worldPanelContext`) that maps colours to the light theme; art
that must keep its colours uses `canvasArtTexture()`. Icons are inline SVG
(`WORLD_UI_ICON_PATHS`, `worldIconSvg()`).

## Notices are inline

The World has no toasts.

- **Feedback for an action** goes through `notice(message, { kind, at })`. A
  capture-phase listener remembers the control the visitor last used;
  `notice` writes a `<p class="world-inline-status" role="status">` into the
  closest host (`WORLD_NOTICE_HOSTS`: form, fieldset, section, article,
  aside, details, dialog). Host chains are stored as selectors so a result
  arriving after a panel re-render still finds its panel; the line stays
  until replaced or the view changes.
- **Actions with no panel** use `hudNotice`, one line under the HUD island's
  action bar; status lines clear after 12 s, errors stay until dismissed.
- **Unsolicited notices** (pings, announcements, celebrations, a new World
  build) go to the chat transcript through `notifyChatArea`.
- Persistent state with its own controls (the update notice, the runner
  attention pill) stays as is.

## HUD island

One `<section data-world-hud-island>` holds the avatar action toolbar, the
chat feed (`#fullChatMessages`, two lines collapsed, expandable) and the
composer with a Chat or Agent switch (always Chat after a reload). It stays
above the software keyboard (`--world-chat-covered-bottom`).

- **Channel switcher:** `#general` and the World rooms
  (`ForkMeshDashboardChat.spaces()`) switch in place (`switchSpace`); private
  channels and DMs (`/api/chat/channels`, `/api/chat/direct-messages`) open
  `/chat`. The last room is kept in `forkmesh.world.chatChannel.v1`.
- **Agent prompts:** use the owner agent queue
  (`/api/repo/<owner>/<repo>/agent-sessions`): `GET` lists `targets` and
  `models` (cached ten minutes); `POST` sends `{prompt, provider, model,
  targetNode}` with the Desktop or Runner target always named. Models come
  from `ORG_AGENT_MODEL_ALIASES`; choices persist in
  `forkmesh.world.agentComposer.v1`.
- **Moving it:** drag the grip at the left of the toolbar (or the island's
  empty edge) to put the island anywhere in the World; arrow keys on the grip
  nudge it 24px, and Home or a double-click puts it back. The offset from the
  default bottom-centre spot is stored in `forkmesh.world.hudIslandOffset.v1`
  and applied as `--world-hud-island-x` / `-y` in its transform. Every resize
  re-clamps it 8px inside the World, so expanding near the top pushes it down
  instead of off screen, and the saved offset comes back when it collapses.
  In the top half (`data-placement="top"`) the channel, model and runner menus
  open downwards. At 600px and below the island is full width, so the grip is
  hidden and the saved offset is ignored.
- **Feed fit:** the island's feed rules carry `.world-hud-island` so they beat
  the roomier `.world-native-chat` rules (32px avatars), which would clip the
  two-line view. Each row is 24px (20px avatar, 20px line), so the 48px
  collapsed feed shows exactly the last two messages.

## HUD rails

The HUD buttons sit in four rails, each marked `data-world-dock`: `top` (the
launcher row, `[data-world-top-actions]`), `right` (a group inside the right
rail, Share view by default), `bottom` (the avatar action toolbar in the HUD
island) and `left` (`.world-dock-left`, empty and hidden by default). Every
movable button carries `data-world-tool="<id>"`; the avatar badge is the
launcher and stays put.

- **Arrange mode** starts from Settings > View > "Arrange buttons", or by
  pressing and holding any button for `WORLD_HUD_LONG_PRESS_MS`. It keeps the
  launcher rails open, outlines the four rails and shows a panel with Reset
  and Done. Clicks on buttons are swallowed while arranging.
- **Dragging** uses pointer events on `window`, so mouse, pen and touch work
  alike (buttons get `touch-action: none` while arranging). The button joins
  the rail nearest the pointer and the slot by button centres
  (`dragHudTool`).
- **Keyboard:** arrow keys move the focused button one slot within its rail;
  Shift with an arrow sends it to that edge's rail; Escape or Done leaves.
  Each move is announced in `[data-world-dock-status]`.
- **Buttons move, they are not re-rendered** (`placeHudTool`), so the
  delegated `closest("[data-world-…]")` handlers and the emote buttons' own
  listeners keep working anywhere.
- **Resizing to the rail:** a button away from its home rail drops its home
  class (`world-top-link`, `world-share-view-button`) for `.world-dock-tool`,
  which takes the rail's size: 40px squares at the top and sides (42px in the
  phone layout), 30px flat icons in the bottom bar, labels visually hidden.
  The top rail still hides them until the launcher opens; the left rail is
  always visible.
- **Persistence:** per device in `forkmesh.world.hudLayout.v1` as
  `{top, left, right, bottom}` id lists. The default layout is read from the
  markup before the saved one is applied; "Reset to default" restores it and
  removes the key. Unknown ids are skipped, so a button that is not rendered
  (Upgrade for supporting members) is harmless.

## Restore everything and start fresh

Settings > Debug > "Restore everything and start fresh"
(`startWorldFresh()`) cancels pending position and preference writes, posts
empty settings to `/api/world/preferences`, removes every local and session
storage key starting with `forkmesh.world.`, `forkmesh.worldComposer.` or
`forkmesh-world-` (except saved views and the loader's reload guard), and
reloads. The login, theme, consent and API origin stay.

## Crash reports

The heartbeat records `document.visibilityState` on every visibility change.
A session whose last beat was `hidden` is reported as "World reloaded after
the browser closed it in the background"; one that stopped while visible is
"World crashed and reloaded". Background closes and discards are sent as
kind `discard` (stored in `error_log` without a notification) and do not
count toward the safe-mode reboot; visible crashes are kind `crash`.
