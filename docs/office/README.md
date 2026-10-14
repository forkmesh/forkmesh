# ForkMesh Office

A Gather-style 2D office for each ForkMesh organization: walk around a pixel-art
floor, talk to people near you over WebRTC, sit at meeting tables, share a
screen, draw on whiteboards, chat, claim desks, see teammates' agent sessions,
and play. It arrived from Md Kaif Ansari as a Cloudflare Worker with one
Durable Object per office (PRs #233 to #242; #238 duplicated #237 and was not
merged). The served stack has no Workers, so the server half was rewritten in
Python and runs on the core nodes' app host like the World.

## Code map

| Path | What it is |
| --- | --- |
| `office/public/app.js` | Client shell: join, input, click-to-move pathfinding, render loop, proximity mesh, the feature `api` |
| `office/public/map.js` | Tile map, zones and walkability (DOM-free) |
| `office/public/render.js` | Procedural pixel art: the map is pre-rendered once; name tags are cached sprites |
| `office/public/avatar.js` | Avatar codes: the look fields, packing and validation (DOM-free) |
| `office/public/sprite.js` | Draws an avatar from its code; sprites are cached per code, direction, step and pose |
| `office/public/rtc.js` | Proximity WebRTC mesh with three fixed transceiver slots (audio, camera, screen); the smaller id offers; tracks switch with `replaceTrack`, no renegotiation |
| `office/public/html.js` | `html` tagged templates, `setHtml`, the Trusted Types policy |
| `office/public/notes.js` | Inline status lines: `setNote` (one line in a panel, card or dialog) and `noteLine` (the keyed lines above the dock); see "Status lines, not toasts" |
| `office/public/features/<name>.js` (+ `.css`, + a `<name>/` folder of helpers) | Client half of each feature, registered in `features/index.js` |
| `app/src/org_office.py` | Python port of the shared rules: map, zones, team gates, status validation, desks, meeting tables, whiteboard ops, chat helpers, pong physics, attendance |
| `app/src/org_office_room.py` | `OfficeCore` (all socket logic, runtime-free) and the `ForkMeshOrgOffice` Durable Object adapter; `SqlOfficeStore` for chat and visits |
| `app/src/org_office_routes.py` | `/api/office/session`, `/api/office/entry`, `/api/office/ws`, tickets, ICE servers |
| `app/migrations/0226_org_office.sql`, `0227_org_office_site_chat.sql` | `org_office_visits`; 0227 adds `org_office_channels` and drops the office chat tables |
| `app/src/org_office_chat.py` | Creates and syncs each org's office channels in the site chat |
| `app/tools/build_site_assets.py` `stage_office` | Bundles and minifies the client into `app/dist/office/` |
| `office/perf-budget.json` | Bundle and frame budgets |
| `office/tools/build_park_map.py` | Generates the water park's tiles and ride table into `office/public/park/layout.js` and `app/src/org_office_park.py` (`--check` fails when they are stale) |
| `office/public/park/art.js` | Water park floors, objects, set pieces, the road |
| `office/public/features/rides.js`, `rides/` | Ride engine: boarding, timelines, vehicles (the rollercoaster, the car and every park ride) |
| `office/public/features/park.js` | Water park animation: waves, river, volcano, bucket, sharks, sprays, slide tubes |
| `office/tests/*.test.mjs` | Node tests of the client modules (pure logic, rtc slots, html escaping) |
| `office/tests/parity_dump.mjs` | Dumps client values that `app/tests/test_org_office_protocol.py` compares with the Python port |

## Architecture

- **Where it is served:** `https://app.forkmesh.com/office` (path, not a
  subdomain). The office needs the account session cookie, the org and team
  tables and the task API, all of which live on the app origin; a path keeps
  every call same-origin, needs no DNS, Caddy site, TLS or CORS change, and
  follows `/dashboard`, `/notes` and `/hackathon`. The World has its own
  subdomain because it is also a public, anonymous site embedded by
  forkmesh.com; the office is neither.
- **Static client:** `stage_office` (run by `build_site_assets.py app`, which
  `tools/next_deploy.py` already runs) bundles `office/public/app.js` with
  esbuild into one ES module, concatenates and minifies `style.css` plus the
  feature stylesheets into `office.css`, stamps both with content hashes
  (`?v=`, served `immutable` by Caddy), and fails the build over the budget in
  `office/perf-budget.json` or if the bundle contains `eval(`/`new Function`.
  Caddy serves `/office` (`@app_page` finds `office/index.html`) and
  `/office/*` (`@app_file`); `!/office/*` in `app/wrangler.toml`
  `run_worker_first` keeps the `/*/*` repository route off the bundle, and
  `office` is a reserved route prefix in `static_routes.py`.
- **Server:** one `ForkMeshOrgOffice` Durable Object per organization, named
  `office:org:<org_bi>:v1`, placed on one core by the cluster host
  (`durable_cluster.py`, rendezvous hash) and reached from the other cores
  over the WireGuard mesh, exactly like `ForkMeshOfficeRoom`. The object keeps
  live presence in memory, batches movement into one `mv` frame per 50 ms and
  diffs `ex` (per-person extras such as status, desk, meeting) so unchanged
  values are never rebroadcast.
- **Chat is the site chat** (since migration 0227). Each organization's office
  rooms (general, engineering, design, coffee, lobby, garden) are private
  `chat_channels` rows of `kind: "office"`, named `office/<org>/<room>` (a
  name no user can pick, since user names cannot contain `/`), created by
  `app/src/org_office_chat.py` on the first office session and mapped in
  `org_office_channels`. Their `chat_channel_members` rows follow org
  membership, and for Engineering and Design the team gates; they are
  resynced on every `/api/office/session` and after any org member or team
  change (`app_runtime._org_office_resync`). Removing a row rotates the room
  key (the migration 0191 trigger) and closes the live room, exactly like a
  manual removal. Nobody can manage these channels from `/chat`
  (`office_channel_managed_by_org`), and they do not count toward the
  100-channel limit. The Office chat panel is a thin client of
  `/api/chat/channels` and `/api/chat/direct-messages` built on the site's
  `chat-room-transport`, `chat-seal`, `chat-moderation`, `chat-retention` and
  `chat-rich-text` modules, so messages, room keys, device seals, moderation,
  retention and DM unread counts are the site's; office DMs are site direct
  conversations. The panel holds the site's account event channel
  (`app/public/account-events.js`), so a new DM or a conversation read on
  another device (`chat-read`, [chat read sync](../app/chat-read-sync.md))
  updates its counts at once instead of on a 60 s poll. "Message nearby" and knocks stay ephemeral office socket
  events. The earlier office chat tables were dropped rather than migrated:
  they held a few hours of messages.
- **Persistence:** attendance visits are rqlite rows (`org_office_visits`, names
  sealed with the data key). Desks, arcade scores, coffee counts and
  whiteboard items use the object's replicated `ctx.storage` (rqlite
  `do_storage`); whiteboard writes are debounced to one batch per second.
- **Features** keep Kaif's plug-in contract on the client (hooks on `api`,
  `extras` keys, message types). On the server every message type is a
  handler in `HANDLERS` in `org_office_room.py`.

## Entering

The static page opens on a neutral "Opening your office…" state; nothing in
`index.html` is the sign-in gate. On load the client reads
`/api/office/session` (with `?org=` or the org remembered in `localStorage`
under `office-org`). Signed out, it replaces the page with
`/login?next=<office URL>`; that confirmed signed-out state is the only one that
renders "Let's get you in". Signed in with any organization, it enters at once:
the requested or remembered org when the account is still a member, otherwise
the org the server picked (the first of the account's orgs by name), with a
status line above the dock when a requested org was not the account's. It requests
`/api/office/entry` and connects, showing only "Entering the <org> office…"
until the socket's `init`. There is no org picker before entry: accounts in
several organizations switch offices from the account strip in the People
panel, which remembers the choice and reloads `/office?org=<name>`. A failed
session check shows "Could not reach ForkMesh", an account with no
organization gets "No office yet", a 401 from the entry request goes back to
login, and a refused entry or closed socket shows the reason with an Enter
button. Microphone and camera stay off until the person presses them.
`office/tests/identity.test.mjs` covers these paths, and
`app/tests/test_org_office_entry_client.py` runs it and checks the static page.

## Controls

- Walk with W A S D or the arrow keys; click a tile to walk there (breadth-first
  path, stopping next to furniture or a person); drag to pan; the wheel or the
  + and − controls zoom (0.75× to 3×); "Find me" re-centres the camera. The
  wheel zooms about the point under the mouse, which stops the camera following
  you until you walk or press "Find me"; the buttons zoom about the centre.
- Enter opens the chat, ⌘K (Ctrl+K) the command palette ("find anything":
  people, rooms, whiteboards, channels, games), ⌘F (Ctrl+F) the people search,
  Esc closes whatever is open.
- Clicking furniture walks up to it first: a monitor claims or opens a desk, a
  whiteboard tile opens the board, the arcade, ping-pong table, coffee machine
  and water cooler start their activity, a couch or bean bag seats you and
  offers a focus session.
- Whiteboard tools: V select and move, P pen, H highlighter, E eraser, T text,
  N sticky note, R rectangle, O ellipse, A arrow; Ctrl+Z undo, Ctrl+Shift+Z
  redo; "Clear" removes everything for everyone and cannot be undone.
- Phones get a joystick and pinch zoom (`features/play/touch.js`) and bottom
  sheets instead of side panels.

## Profiles

Clicking a person on the map or in the People list opens their profile card:
picture, display name, `@account`, local time, status, room and team, with
Message, Walk over, Wave and the feature actions (follow, ask to come over, go
to their desk). Your own card and the "You" menu open Edit profile.

- **Display name.** A member may set a name of up to 24 characters for one
  organization's office (`prof-set`). It replaces the account name on the map,
  in the People list, on desk plates and in attendance from the next visit.
  The account name stays the identity: `ex.fm` still carries it, task pings
  and agent ownership match it, and every card shows `@account` under the
  display name. A name that equals the display or account name of someone
  else in the office is refused (`prof-err`, `taken`), so nobody can pose as a
  teammate who is present. Saving an empty name, or the account name, removes
  the custom one.
- **Time zone.** An IANA zone name (`Asia/Kolkata`), shown as "23:01 local
  time" on the card. Nothing is shared until the member saves Edit profile,
  which preselects the browser's zone.
- **Picture.** The office shows the picture the account already has on
  ForkMesh; there is no separate office picture. The entry claim and the
  30-second recheck carry `pic`, the first 16 hex characters of the SHA-256 of
  the stored PNG, and the client loads
  `/api/accounts/<account>/avatar?v=<pic>`. Edit profile crops the chosen file
  to a centred square, scales it to 256 px (then 192, 128, 96 until the PNG is
  under 256 KB) and posts it to `/api/accounts/profile` as `avatarPng`, the
  same session-authenticated call the dashboard uses, so the picture changes
  everywhere on ForkMesh. Teammates see it after the next recheck.
- **Storage.** Name, zone and avatar are one record per member in the office object's
  storage, key `prof:a:<account_bi>`, so they are scoped to the organization
  and leave with the object. They are stored as plain text like desk names.

## Floor plan

The map is 66 by 44 tiles (`W`, `H` in `map.js`). The building is two rows of
rooms on either side of a corridor. The corridor's north door opens into the
Agent Room, which runs along the whole north side above Engineering and the
ping-pong hall; its east door leads out to the Garden and the Campfire:

| Row | West to east |
| --- | --- |
| North | Engineering, the ping-pong hall, Design, Library |
| South | Coffee Huddle, the arcade hall, Lobby, Media Room |

- **Coffee Huddle** (zone `c`): a kitchen along a two-tile glass back wall
  (fridge, counter with the coffee machine and a sink, a couch under the
  windows), one round table with four wooden chairs, and a door on the east
  wall into the arcade hall. `K`, `C`, `R`, `S`, `P`, `T` and the chairs are
  drawn in a café style when they stand on a `c` floor (`cafe()` in
  `render.js`). `glassWall` paints the back wall across both wall rows and
  `sideDoor` the wooden frame and open leaf of a door in an east wall.
- **Library** (zone `k`): bookshelves, two round reading tables with four
  chairs each, and two reading armchairs. Door on the corridor.
- **Media Room** (zone `m`): one equipment table with ten chairs, AV carts
  under a wall screen, cabinets and a water cooler. Doors on the corridor and
  on the Lobby's east wall, so people who just arrived can walk straight in.
- **Lobby Couches** (zone `s`): a five by four rug inside the Lobby with an
  L-shaped couch, two armchairs and an ottoman around a coffee table. It has
  no walls; the rug is the room. It is registered in `ROOMS` through `area()`
  so it gets the same focus field and room card as a walled private room, and
  `?room=s` links to it.
- All three are private (room audio, see below) and none is team-gated, so
  every member may walk in. None has its own chat channel; the chat panel
  stays on the channel it was showing.
- **Lounge seats** are the chair characters `n`, `u`, `d`, `c` (facing down,
  up, left, right) and `o` (ottoman). They are in `CHAIRS`, so sitting,
  "Take a seat" and the facing direction work as for any chair. The painter
  joins neighbours of the same character into a blue couch and draws a lone
  one as an armchair.
- **Room looks** come from the floor character under a tile: chairs and
  tables in the Library and Media Room reuse `v ^ < >` and `T` and are tinted
  or redrawn by zone (`SEAT_TONES`, `readingTable`, `mediaTable`), and the
  wall face above a room takes the room's colour (`WALL_FACE`).
- **Wall art** (`DECOR` in `map.js`: the Library painting, the Media Room
  screen, the picture over the couches) is drawn on the wall face and is not
  on the tile grid, so it never blocks a tile and the server does not know
  about it. A wall tile under a piece gets no window.

The rooms were added on the east side so that no existing tile moved: desks
are stored by tile and whiteboards are named by tile, so shifting furniture
would orphan claimed desks and saved boards. The Coffee Huddle is the one room
that was rearranged; it holds no desk and no whiteboard. Its table id changed
from `c-14-26` to `c-14-27`, and a table id lives only in the `meeting` extra
of the people seated at it.

## Avatars

An avatar is one integer, `av`, in every person's packed record.

- **Hashed codes** (0 to 9999) are what every member starts with: an FNV hash
  of the account. They decode to one of 5 skins, 7 hair colours, 8 tops and 3
  hair styles, exactly as before.
- **Custom codes** start at 2^24 and pack eight fields in mixed radix: skin
  (6), hair style (8), hair colour (12), top (12), bottom (8), facial hair (4),
  extra (8: glasses, sunglasses, cap, beanie, headphones, party hat, crown or
  none) and shoes (4), 7,077,888 looks in all. Keeping the code an integer
  left the wire format, the sprite cache key and the avatar tint untouched.
- **Editing.** Edit avatar (profile card, Edit profile, the "You" menu, the
  palette) has a category list, option tiles drawn with the member's current
  look, colour swatches, a walking preview that can be turned, and "Surprise
  me". Done sends `prof-set {av}`; the server accepts only a hashed code or a
  custom code in range (`clean_avatar`), stores it in the member's profile
  record and broadcasts `prof {id, name, av}`. `av: null` returns to the
  hashed look.
- There are no costumes: the first version is the eight fields above.

## South wing: Workspace and Focus Lounge

The map grew from 38 to 44 rows. A wing below the original building, entered
through two doors at the foot of the hallway beside the arcade machines, holds
two open zones:

- **Workspace** (`w`): three rows of desks, 24 in all, so the office has 28
  claimable desks instead of 4. The four original desks are inside the
  team-gated rooms, which members of other teams cannot enter.
- **Focus Lounge** (`f`): two five-seat couches facing each other across a low
  table, six bean bags, bookshelves and plants.

Neither zone is private: proximity audio works by distance, as in the hallway.
The wing was added below the building rather than inside it so that the
existing rooms keep their layout.

Couches are seats. `S` (faces down), `s` (faces up) and `j` (bean bag) are in
`CHAIRS` and `COUCHES`, so they are walkable, the avatar sits on them and "Take a seat"
finds them. That includes the sofas that were already in the Coffee Huddle
and the Lobby. A low table is `t` and is never a meeting table. The bean bag
was `n` on its own branch; it became `j` when it was merged with the Library,
Media Room and Lobby Couches work, which uses `n` for a lounge seat facing down.
Lounge seats are chairs but not couches, so they do not start a focus session.

## Desk decoration

The owner of a desk can decorate it: "Decorate" on the desk card, or "Decorate
my desk" in the palette, opens the Decorator beside the map.

- **What can change:** the desk surface (standard, oak, walnut, white,
  charcoal, mint), the chair colour (standard and seven colours) and four
  slots: desk left and desk right (plant, coffee mug, desk lamp, books, rubber
  duck, photo frame, headphones, trophy), the screen (code editor, sunset,
  charts) and the floor tile beside the chair (tall plant, backpack, cat).
- **Fixed slots, not free placement.** A decoration is `{desk, chair, slots:
  [left, right, screen, floor]}`, eight small integers. The server checks
  every value against the catalog and each item against its slot's kind
  (`clean_deco`), so a desk can never hold anything the client cannot draw,
  and the record stays tiny. Free placement with rotation was left out of the
  first version.
- **Editing.** The panel shows the desk at three times its size, the four
  slots, a search box, the items that fit the selected slot, and undo and
  redo over the last 60 changes. The draft is drawn on the map for the editor
  only; Save sends `work-deco`, Cancel discards it.
- **Storage and sharing.** The decoration is part of the desk record in
  `work-desks` and travels in `work-desks` broadcasts, so everyone sees it and
  it survives a restart. It moves with its owner to a new desk and is deleted
  when the desk is released. A stored decoration that no longer validates is
  dropped on load.
- **Drawing.** `features/work/deco.js` draws a decoration into a 64 by 80
  pixel sprite, cached per decoration, which is drawn under the avatars so a
  seated person covers the chair and a walker covers the floor item.

## Focus sessions

Clicking a couch or bean bag walks you to it, sits you down and opens Focused
coworking: Focused (15 to 120 minutes), Pomodoro (1 to 6 rounds of 25 minutes
with 5-minute breaks) or Freeform (no timer), an optional topic of up to 60
characters, and "Let others know?".

- Starting sends `focus-start`; the room object checks that the member is on a
  couch tile, stamps the start and end with its own clock and sets the `focus`
  extra. The client then sets the status to Focus (which already blocks
  proximity calls and refuses "come over" requests), mutes the microphone and
  shows the timer above the dock with End. Everyone sees the minutes left
  above the avatar.
- With "Let others know", members who are not themselves focusing get
  `focus-open` and a status line above the dock with Join, at most once a
  minute per host (it goes away when they start their own session). Join
  walks to the nearest free couch and sends `focus-start {join}`, which copies
  the host's mode, topic and end time. A joiner's session is their own: it
  keeps running if the host stops.
- A session ends when its time is up (a chime and "Focus session complete"),
  when End is pressed, or when the member walks off the couch, which the room
  object enforces on every move. The status from before the session comes
  back unless the member changed it by hand meanwhile.
- In a Pomodoro the status switches to "Pomodoro break" during each break and
  back to Focus for the next round, with a chime at each change.
- Timers use the room object's clock: `init` carries `now`, and the client
  applies the difference to its own clock. A session whose end has passed is
  drawn as over by everyone even if its owner's tab is asleep.
- There is no music link field. The client is tested to contain no
  third-party URL and its policy allows only same-origin connections.
- **Emoji.** The dialog offers ten emoji (🎧 💻 📚 ✍️ 🎨 🧠 🚀 🐛 📈 ☕). The
  choice travels as `emoji` in `focus-start`; `clean_focus` / `cleanFocus`
  keep one grapheme through the same check as a status emoji (`clean_emoji` /
  `cleanEmoji`) and drop anything else rather than refusing the session.
  Joiners copy it. It shows before the minutes above the avatar and in the
  timer.
- **Status is optional.** "Set my status to this focus" (on by default,
  remembered in `office-focus-status` in localStorage) decides whether the
  session sets the status to Focus with the session's emoji and topic, and
  whether the Pomodoro break status and the restore at the end happen. The
  microphone is muted either way.
- **Recent sessions.** Each session you start yourself is remembered in the
  browser (`office-focus-recent`, the newest five distinct settings: mode,
  length, topic, emoji, the status choice and "let others know"). They are
  listed at the top of the dialog, and a card in the bottom right shows them
  whenever you stand in the Focus Lounge. One click walks you to the nearest
  free couch and starts the same session again. Joined sessions are not
  remembered. They are kept per browser, not per account, so nothing new is
  stored on the server.

## Keeping your place on refresh

A reconnect already rejoins where you stood (`join` carries `me.x/y`), but a
page refresh starts with no `me`, so the room object used to pick a random
Lobby spawn. `features/place.js` remembers your tile in localStorage
(`office-place:<org>`, at most every 2 seconds and when the page is hidden or
closed) and sends it with the first `join`. The room object validates it as it
validates every join position: an unwalkable tile, a team room you may not
enter or the Campfire for someone outside the top list still falls back to a
spawn point. A remembered place older than seven days is ignored.

## Agents without a task

Until 2026-10-04 the office drew only agents attached to an organization task,
and the desktop opens no task for a run started from the Agents rail, a
prompt, an issue or a branch (`MainWindowAgentLaunch.cpp`), so most agents
never appeared. Three fixes:

- **Presence.** The desktop's signed agent-status batch
  (`flushOrgTaskAgentStatus`, over the node socket or `POST
  /api/tasks/agent-status`) now also reports runs with no task, as entries
  with an empty `task`: live runs, and finished ones for 8 hours, never
  deleted ones. The relay stores them in `organization_agent_presence`
  (migration 0233), one row per account and session, sealed: provider,
  model, starting bot, reasoning strength, session number, status, owner
  (the signing account) and start time, plus, since the Agent Room, the run's
  title and its latest words (see "Agent Room"). No prompt, repository or
  full transcript leaves the desktop. Rows are pruned 8 hours after a run ends,
  live ones after 24 hours without a report, 40 per account at most. `GET
  /api/tasks` (page one, not the trash) returns them as `agentRuns`, at most
  60, only for accounts that are still members of the organization. The
  office draws them as bots named `<owner>:<session>`, titled "Claude agent
  run" and so on. One batch carries at most 40 such reports, live runs
  first, after every task-linked status. If the relay refuses a batch that
  holds only such reports
  (an account outside the organization), the desktop stops sending them for
  30 minutes.
- **Who ran it.** An agent-status write now records `agent.runBy`, the
  signing account (only the server sets it; a `runBy` in a create or
  completion body is dropped), and the office places a task's bot beside that person
  rather than beside the task's creator, so a run started on a teammate's
  task shows up at your desk.
- **More than one page.** The office reads up to four task pages (100
  tasks) instead of the first 25, so agent tasks with a low priority are no
  longer cut off.

## Agent Room

The room has 48 stations in three rows of 16, twice the 24 agents the office
shows at most (`MAX_BOTS` in `features/agents/model.js`), so a full office
still leaves every other station free. It used to hold 24 and was always full
at the cap. It grew east (room `x` -5..30), and the Garden, Campfire, coaster
platform and gate moved 16 tiles east with it. Growing north instead would have
shifted the whole map down, and desk claims (`work-desks`) and whiteboards are
stored by tile, so every saved desk and board would have needed rewriting.

A room above Engineering holds every agent the office knows about, at a grid
of stations (`AGENT_STATIONS` in `map.js`), in
start order (`stationLayout` in `features/agents/model.js`). A console at its
west wall lists them and switches the pinned one; the arrows step through.
Each bot carries a small bubble (`features/agents/bubble.js`): two lines of
title and two of the agent's latest words, outlined in its state colour, a
pulsing dot while it runs. Wrapped lines are cached per bot until the text
changes.

The words come from the desktop. With each agent-status report it now adds
`agent.prose`, the last assistant text of the run (`m_lastAssistantText`), and
for a run without a task `agent.title`, the issue title. Both are redacted on
the desktop and again on the relay (`redactedAgentText` in
`MainWindowAgentLanding.cpp`, `_redact` in `world_office_tasks.py`): private
key blocks, common API token prefixes (`sk-`, `ghp_`, `xox?-`, `glpat-`, `AKIA`
...), JWTs, `password=`/`token:` style pairs, hex runs of 32+ and base64 runs
of 40+ become `[redacted]`. Then they are cut to 200 characters (prose) and
120 (title). A timer (`m_officeProseTimer`, every 20 s) re-sends a live run only
when its redacted words changed. This is a deliberate exception to "transcripts
stay end-to-end encrypted": the user asked for agents to say what they are
doing, and only this short, scrubbed snippet is shared with organization
members. Agent transcripts themselves are unchanged.

## Rollercoaster

A track runs one lap around the whole office, just inside the map edge, with
three vertical loops: on the east and west sides and on the south edge east of
the south wing (`features/coaster/track.js`, drawn once into a canvas by
`coaster/art.js`). Board from the platform north of the Garden
(`COASTER_PLATFORM`, ending at the gate `x`) by clicking the gate or "Ride the
rollercoaster" in the palette. The client sends the extra `ride`; the server
(`_extra_ride` in `org_office_room.py`) accepts it only from someone standing
on the platform and replies `{"at": <server ms>}`, so every viewer computes the
same position from the shared start time (`rideState`, speed 8 tiles/s, about
32 s per lap) with the clock skew taken from `init.now`. Riders sit in a cart
and are drawn lifted through the loops. Taking a step or reaching the end clears
the extra.

Until 2026-10-04 evening no ride ever started. `board()` sets `ride` to
`{"go": 1}` locally, and the tick that ends a finished ride saw a `ride`
without `at` on the very next frame, called it finished, and sent `null` right
behind the request, so the server's `{"at": ...}` was cleared again at once. A
request is now pending for up to 3 s until the server's `at` arrives.

The first fix still left rides dead: the browser's estimate of the server
clock (`init.now` minus local time) runs behind by the network delay, so when
`at` arrives `now - at` is about zero and often a little negative, and
`rideState` treated a negative time as no ride, so the tick cleared it again.
`rideState` now accepts up to `CLOCK_SLACK_MS` (5 s) early and holds the cart
at the start until the time catches up.

Outside the track the map ends, so a woodland ring is drawn beyond the map
edge (`buildOutskirts` in `render.js`, `OUTSKIRTS` = 4 tiles, seeded by tile so
every viewer sees the same trees). It is scenery only: not in `floor`/`obj`,
not walkable, not on the server. The camera may pan 3 tiles into it.

## Reconnecting banner

Every app deploy restarts the app on n1, n2 and n3 in turn (about 13 s each,
`next_autodeploy`), which closes every office socket on that core; a client
that reconnects to the next core may be closed again a minute later. The
"Reconnecting…" banner used to show the moment a socket closed, so one deploy
could flash it up to three times. It now shows only after 4 s without a
connection (`showBanner`/`hideBanner` in `app.js`) and hides on the next
`init`.

## Water park

A second scene east of the office: the map grew from 66 to 172 tiles wide
(`OFFICE_W` = 66, `PARK_X0` = 72, park 100 by 44, about 1.5 times the office).
Growing east moves no existing tile, so desk claims and whiteboards keep their
keys. Columns 66..71 are woodland with the road through it. The camera and the
minimap stay inside the scene you are in (`SCENES`, `sceneAt` in `map.js`) and
only span the gap while you drive, so each place feels like its own scene.

**Getting there.** A door in the hallway's east wall (rel 52,10..11) opens onto
a car park (`OFFICE_PARKING`, zone `D`) with a red car (`e`). Click it, or
"Drive to the water park" in ⌘K: you drive along the road (`ROAD` in
`park/art.js`) to the park entrance, about 6 s. A car at the entrance drives
you back.

**Layout and attractions** (park-local coordinates in the generator). Inspired
by Siam Park (vertical drop, long lazy river), Volcano Bay (volcano, water
coaster), Typhoon Lagoon (surf pool), Aquaventure (shark tunnel), drop-pod
launches, racing mat slides, FlowRider and tipping-bucket play towers, each
with something extra:

| Attraction | What it does |
| --- | --- |
| Surf Lagoon | Walkable wave pool (swim), rolling waves, and a big wave every 60 s on the shared clock; loungers and umbrellas on the beach |
| Jungle River | Raft lap of the volcano under four bridges and a waterfall, ~50 s |
| Leap of Doom | Climb the volcano, 3 s trapdoor capsule countdown everyone can see, near-vertical drop into the lagoon pool |
| Typhoon Twister | Tube slide into a giant funnel, spirals down into the reef pool |
| Krakatoa Blaster | Uphill water coaster with five water-jet launches, ends in its own pool |
| Riptide Racers | Four mat lanes; riders who board within a 3 s window launch together (`RACE_EVERY_MS`), and each gets a 1st/2nd/3rd line above the dock |
| Skull Rock | Climb the rock and dive into the reef pool |
| Endless Wave | FlowRider surf machine, a 22 s ride |
| Splash Harbor | Kids' splash pad with spray jets and a giant bucket that fills and tips every 45 s with a countdown |
| Shark Reef Tunnel | Walk-through tunnel between tanks with sharks swimming by |
| Royal Flush | Climb to the rim, spin seven times round a giant bowl, faster as it narrows, then drop through the centre pipe into the splash pool |
| Volcano | Smokes, and erupts every 90 s |
| Coconut Shack, cabanas | Scenery and lounge seats |

**Finding and starting a ride.** Every attraction's sign carries a pulsing
green "▶ START" pill ("🚗 DRIVE" on the cars), drawn every frame from
`ATTRACTIONS` in `rides.js`. Within 3 tiles of a sign, or anywhere on its
boarding area, a big green button at the bottom of the screen ("▶ Ride the
Royal Flush") boards you in one press (`nearestAttraction`, `.ride-cta` in
`play.css`). Clicking the attraction itself (the bowl, the funnel, the racer
tower or lanes, the surf machine, the volcano, Skull Rock) does the same. The
"Water park guide" (⌘K, and the button on the arrival line) lists every ride
with a Go button that walks you to its start and boards. People in the office
get a one-time "New: the ForkMesh Water Park" line above the dock (`fm.office.parkHint` in
local storage). Rides used to be reachable only through the small blue signs
and ⌘K, which people did not find.

**Rides are one engine.** `RIDES` (generated) gives each ride a gate tile, a
boarding rectangle and an exit tile. The client sends the extra
`ride: {"id": ...}` from inside the rectangle; `_extra_ride` checks the
rectangle, moves the rider to the exit at once (a `you` message plus the usual
move flush), and stores `{"id", "at"}`. Every client then plays the same
timeline (`rides/defs.js`, sampled every 50 ms by `rides/timeline.js`) from
`at` on the shared clock, drawing the rider at the timeline position and its
vehicle on top. The server only knows boarding and exits, never the path. The
rollercoaster is the ride `coaster`; its old `{"go": 1}` request still works.
The volcano stairs open a menu of the three slides; the racer tower picks a
free lane.

**Paths follow the art.** A ride's height (`lift`) is drawn as a raise on
screen (`y - lift`), so a path only lines up with a structure when its screen
point lands on what is drawn. The first park release put the volcano summit
2.5 tiles below the crater (the tubes started mid-slope and the stairs ended
early), spun riders 2.5 tiles above the funnel and the Royal Flush bowl, and
floated racers and the Skull Rock diver far above their tower and peak.
`ANCHORS` in `park/art.js` now holds the crater, both bowls, the flush deck,
the racer pads and the Skull Rock peak; the art draws from them and
`rides/defs.js` builds the paths from them, and "ride paths line up with the
structures they are drawn on" in `office/tests/rides.test.mjs` checks every
ride against them (and that the raft stays on river or bridge tiles).

**One layout, two copies.** The park's floor and object rows exist in both
`park/layout.js` and `org_office_park.py`, generated by
`office/tools/build_park_map.py` (edit the generator, then run it).
`test_org_office_park.py` fails if they are stale, and the existing map
parity test compares every tile. New park zones (`D Z B K A V`) are open
areas; loungers (`h`) are seats.

**Rendering.** Park tiles draw in the one pre-rendered map canvas (now 5504 by
1408 px), the set pieces on top (`drawParkArt`). The woodland outside the map
is four strips instead of one big canvas. The slide tubes are drawn once into
a park-sized canvas; the animations only run while the park is on screen.

## Coffee and the Calorie Counter

Grabbing a coffee (clicking the coffee machine, or "Grab a coffee" in the
palette) also offers to log it to the member's Calorie Counter plugin
(`marketplace/packages/calories`), from `features/play/coffee-log.js`:

- The office reads `GET /api/calories` with the member's session. When the
  plugin is installed, a status line above the dock asks "Log coffee with milk to Calorie
  Counter?" with the calories and portion; "Log it" sends
  `POST /api/calories/entries` as a `quick` snack on the member's local day.
  The drink is the member's own coffee quick food if they saved one (any name
  with coffee, latte, cappuccino, espresso, americano, flat white or mocha),
  otherwise the package's "Coffee with milk" (30 kcal).
- When it is not installed (`installed: false`), the same line says once per visit
  that the plugin can log coffee and send it to Apple Health, with a link to
  `/packages/calories`.
- The question comes at most every three minutes, so a second coffee in a
  row does not ask again.
- Apple Health: the server cannot write to Health. The entry reaches Apple
  Health when the member next opens Calories on an iPhone with "Write meals
  to Apple Health" on (the iOS app re-syncs the last 14 days), and the line
  says so. "Log it" turns the line into "Logging coffee…", then "Coffee
  logged" or "Coffee not logged" with the reason.
- The office calls only these two calories routes, through `api.calories` in
  `features/identity.js`; the general `api.forkmesh` helper still allows only
  `/api/tasks`. Both requests are same-origin with the session cookie, which
  the office CSP (`connect-src 'self'`) already permits. The calories API
  checks the origin and the session itself.

## Swimming

The Garden pond (`W`) is walkable on both sides (`WALKABLE_OBJ` in `map.js`
and `org_office.py`), so clicking the pond, or walking into it, puts you in
the water. Anyone whose tile is a pond tile is drawn swimming
(`features/swim.js`): legs hidden as when seated, arms in two alternating
stroke poses (`pose` 4 and 5 in `sprite.js`), water drawn over the lower body
with a ripple, and a splash when someone enters. The `pose` hook may now
return `sit` to hide the legs. The palette has "Go for a swim", which walks to
the nearest free pond tile. Swimming is client drawing only: the server knows
nothing beyond the position it already validates.

## Campfire

A fire ring east of the Garden (zone `h`, private audio) with eight log seats
facing the fire (`FIRE_SEATS`, the `v ^ < >` chairs drawn as logs on an `h`
floor). Only the organization's top members can walk in: `OfficeCore.can_enter`
refuses the move and sends `fire-locked`, and a join position inside the
campfire is moved to a spawn point. Someone who drops out of the top list
while sitting there is not moved out, as with team rooms.

**Fire score.** The room object ranks the organization's members with a fire
score adapted from the World campfire rings (`memberEngagementProfile` in
`world/public/world/scene/member-engagement.js`), computed on the server in
`fire_score` / `fire_ring` (`org_office.py`):

- recency: the World ladder (48 points within an hour down to 5 within 120
  days) applied to the newest of World activity, contributions and this
  organization's office visits;
- depth: World active time (up to 22), World steps (13), minutes in this
  organization's office over the last 30 days (13), office visits in those
  30 days (6) and contributions (log2, up to 14).

It leaves out the World's innermost "custom avatar" ring, which is not
activity, the "took one step" ring, and the readiness points (email,
payout address, nodes), which are not engagement. A member is **Blazing**
when active in the last day with a score of 70 or more, **Glowing** when
active in the last five days with 40 or more and at least 10 depth points.
Blazing ranks before Glowing, then by score, office minutes and active time.
The top eight hold the seats, in rank order (seat 1 north-west of the fire,
then clockwise).

- `SqlOfficeStore.fire_rows` reads one SQL query over `org_members`,
  `world_user_activity`, `contributor_activity` and `org_office_visits`.
  Candidates are checked against the users table (`roster_ok`: an active user
  account without a private profile), the same rule as the public roster.
- The list is recomputed when someone joins and on the 30-second sweep, at
  most every 10 minutes (`FIRE_REFRESH_MS`), and broadcast as `fire` when it
  changes; `init` carries it as `fire`. It holds account names, rank, tier
  and score only, and reaches only signed-in members of the organization.
- Clients show a flame badge with the rank beside each top member, the
  member's name on their seat while they are away, an animated fire, and two
  palette items: "Campfire: top members" and, for a top member, "Sit at the
  campfire".

## Applause

Reactions are the nine emoji of the React button. Three of them are
celebrations and do more than float above a head:

- 👏 plays a two-frame clap on the avatar for 2.2 s and four clap sounds for
  everyone in the same room or within 8 tiles.
- 🎉 throws 36 confetti squares from the avatar.
- 💃 makes the avatar dance for 3 s (arms up, turning and stepping).

When three different people in one room send a celebration within 4 seconds,
the room object sends `cheer {z, n, ids}` to everyone standing in that room:
the clients play 2.8 s of applause, throw confetti from each of those people
and show "Applause!". A room cheers at most once every 8 seconds, people who
left the room stop counting, and the count restarts after each cheer.

Sound is made in the browser with WebAudio (short band-passed noise bursts),
so there is no audio file, nothing to download and nothing for the
`media-src` policy to allow. Sound settings ("You" menu and the palette) has
an on/off switch, a volume slider and "Play applause"; the choice is kept in
`localStorage` under `office-sound` and never leaves the browser. A browser
plays nothing until the person has clicked or typed in the page.

The first time a browser opens the office after this change, a "What's new"
status line in the dock (key `news`, see "Status lines, not toasts") explains
applause, with Sound settings and "Got it!". It used to be a card that slid
in from the bottom-left corner on its own, which read as a toast, so it is now
an inline line with no animation and no timer. It is marked seen only when one
of its buttons is pressed, once per browser (`office-news` in `localStorage`);
raising `NEWS_ID` in `features/cheer.js` shows it to everyone again.

## Status lines, not toasts

The office has no toasts. The user asked for no floating, auto-dismissing
notification cards anywhere, with the information shown inline where it
belongs instead, so the old toast stack (`#toasts` in the top-right corner,
`toast()` in `app.js` and the private copies in `presence.js` and
`meetings.js`) was removed on 2026-10-09. Feedback is now a status line in
normal layout flow, rendered by `office/public/notes.js`:

- **In the panel, card or dialog the action came from** (`setNote`): the
  invite link under the Invite button, a wave under the people search or in
  the profile card, "Profile saved" and "Avatar saved" in your profile card,
  "Room link copied" in the room card, chat errors above the message box,
  task timer and update results above the task list, desk claim errors in the
  desk card, decorator save errors above its footer, whiteboard "full" and
  "slow down" under its toolbar, a bad status emoji in the status menu, a
  focus-session error in the focus dialog, "Sound is off" in the sound
  settings, the agents panel's "Walk to my agents" results above that button,
  and a failed org switch on the join screen.
- **Above the dock** (`#notes`, `noteLine`), for things that happen in the
  world and belong to no panel: arrivals, rides and race results, waves,
  coffee and the Calorie Counter offer, knocks (with Admit / Not now), "come
  over" requests (Accept / Not now), the focus "Join" offer, someone waving
  at you, the one-time "What's new" line, and similar. Each line has a key, so a new message of the same kind
  replaces the old one; at most three lines show, each with a dismiss button.
  The focus timer and the desk card sit in the same column, so nothing
  floats over them.
- Errors use the error colour and stay until replaced or dismissed. Plain
  info lines above the dock clear after 10 s; lines with a button stay until
  used or dismissed (a knock prompt still expires with the knock).
- Unsolicited updates go where they are already listed: task updates and
  @mentions show as lines with "Open task" at the top of the Tasks panel (at
  most three) and put a dot on its rail button; your agents' state changes
  put a dot on the Agents rail button and show in the Agents list. Pong
  results show in the pong window ("You win (a player left)").
- `api.toast` stays as an alias of `api.notice` so older plug-ins keep
  working; they now get a status line above the dock.
- `office/tests/notes.test.mjs` checks the line behaviour and fails if a
  toast container, toast styles or `api.toast(` calls come back.

## Proximity and audio

- In open areas (Hallway, Lobby, Garden, Workspace, Focus Lounge) you connect to people within 3 tiles
  (Chebyshev distance) and only drop them past 5, so the mesh does not flap at
  the edge; at most 8 peers per person, nearest first.
- Volume is full within 1 tile and fades smoothly to 0.15 at 5 tiles
  (`meetings/tables.js` `volumeFor`, smoothed over 250 ms).
- Private rooms (Coffee Huddle, Engineering, Design, Library, Media Room,
  Lobby Couches): everyone inside is connected at full volume, nobody outside
  hears. On the Lobby Couches that means everyone standing or sitting on the
  rug, and nobody in the rest of the Lobby.
- A meeting table connects exactly the people seated at it, at full volume,
  wherever they are.
- Focus status blocks calls outside a table. This is a client-side courtesy
  (`presence/logic.js` `focusBlocks`), not an access control; the server's
  signalling rule is the room and distance rule under Security.

## Room focus

- **Focus field.** While you stand in a private room that has an entry in
  `ROOMS` (every walled private room, and the Lobby Couches), everything
  outside the room is dimmed and the room gets a light outline, so the people
  you can hear are the ones still lit. It fades in and out over 240 ms and
  only starts once you have been inside for 180 ms, so walking into a locked
  door does not flash it. The name tags, chat bubbles and reactions of people
  outside the room fade with it (`tagAlpha`). It is drawing only: who hears
  whom is still the rule under "Proximity and audio".
- **Room card.** In the same rooms a card in the top-right corner shows the
  room name, the people in it, "Room link" and "Take a seat". "Room link
  copied" appears as a line at the bottom of the card. Its close button hides it until you
  leave the room and come back. It is not shown on a stage narrower than
  760 px, where the zone chip already names the room.
- **Room links.** "Room link" copies the office URL with `room=<zone>`.
  Opening it walks you to that room's anchor once you are in the office, and
  the parameter is then removed from the address bar. Only zones in
  `ROOM_ANCHORS` are accepted. A team-gated room still refuses the walk at its
  door and offers the knock.
- **Room labels** are pills drawn in screen space every frame (`roomTag`),
  under the avatars, so they stay sharp at every zoom. Below 1× zoom they
  shrink with the map, so a pill never grows past the wall it hangs on. The
  join preview paints them once with `drawLabels`; the minimap has none. A
  label goes on the wall beside a door, never on it
  (`office/tests/rooms.test.mjs` fails on a label within a tile and a half of
  a door).
- Code: `features/rooms.js` (the drawing and the card) and
  `features/rooms/logic.js` (pure helpers, `office/tests/rooms.test.mjs`).

## Feature limits

| Feature | Limits |
| --- | --- |
| Summon ("ask to come over") | one per person per 20 s; a pending ask lives 60 s; the 8 newest are kept; refused while the target is in Focus |
| Auto-away | after 10 minutes without input, or 10 minutes with the tab hidden |
| Attendance | visits merged across gaps under 90 s; boards show the top 10 for today and this week; kept 14 days |
| Knock | once per room every 15 s; an unanswered knock expires after 2 minutes; admission lasts for that visit |
| Screen share | 15 fps (at most 30), 1.5 Mbit/s cap, resolution preferred over frame rate |
| Agents | polled every 30 s, at most 24 bots, finished runs shown for 8 hours, at most 24 relayed changes per message |
| Arcade (Snake) | a score must be reachable at 3 points per second of play, at most 5000, runs at most 30 minutes; top 10 kept per office |
| Ping-pong | server-driven at 30 Hz, first to 5, 1.2 s serve delay; players must stay within 2 tiles of their end; people within 8 tiles watch |
| Coffee | one counted break per person per 5 minutes, daily count |
| Water cooler | one question per cooler every 6 s, heard within 3 tiles |
| Desk decoration | 6 desk colours, 8 chair colours, 14 items in 4 fixed slots; saves share the 5-per-burst, 1-per-second action bucket |
| Focus session | from a couch or bean bag only; 15, 25, 30, 45, 60, 90 or 120 minutes, or 1 to 6 Pomodoro rounds; topic 60 characters; one "let others know" per account per minute, across reconnects |
| Applause | 3 people in one room within 4 s; one cheer per room every 8 s; reactions share the 5-per-burst, 1-per-second action bucket |

## Plug-in contract

Each client feature is `setup(api)` in `features/<name>.js`, listed in
`features/index.js`. The `api` object (end of `app.js`) gives:

- state: `me`, `others` (id → person), `mesh`, `ws`, `zoom`, `camera`, `path`,
  `account` (`{id, name, org, teams, role}`), `tasksEnabled`, `officeChat`
  (room → site channel id), `iceServers`, `personActs(person)` (the actions
  every feature registered for a person), `mapCanvas` (the pre-rendered map);
- actions: `send`, `setExtra(key, value)`, `notice(title, body, action, fn,
  icon)`, `noticeError(title, body)`, `hudNote({key, title, body, tone,
  actions, ms})`, `setNote(element, note)`, `toast` (kept for older plug-ins:
  an alias of `notice`, so it now writes a status line above the dock instead
  of a floating card), `walkTo`, `walkToTile`,
  `takeSeat`, `showPanel`, `openPalette`, `useObject`, `setMedia`, `emote`,
  `connect`, `leave`, `forkmesh(path, init)` (same-origin `/api/tasks*` only);
- hooks: `onMessage(type)`, `onInit`, `onExtra`, `onTick`, `onDrawUnder`,
  `onDrawWorld`, `onDrawScreen`, `dotColor`, `proximity`, `joinExtras`,
  `personBadge`, `personActions`, `paletteItems`, `onLeave`, `onMesh`,
  `onDenied`, `onObject(char)`, `onPerson` (a person was clicked on the map),
  `meMenu` (items for the "You" menu), `face` (markup drawn inside a person's
  round avatar, used for pictures), `tagAlpha` (opacity of a person's name
  tag and bubbles, used by the room focus), `onEmote` (someone reacted) and
  `onPose` (override an avatar's direction, step and arm pose for a frame);
- UI: `addPanel`, `addDockButton`, `addControl` (markup must be `html`).
  A plug-in with its own panel or dialog reports there with `setNote` on an
  element of its own; `notice`, `noticeError` and `hudNote` are for feedback
  that belongs to no panel (see "Status lines, not toasts").

Document events: `office:account` (the signed-in account changed),
`office:locked` (a gated room refused a move; cancelable, the meetings feature
turns it into a knock prompt), `office:panel` (a side panel opened), and
`office:dm` (open a direct message with a person id).

Extras keys a client may set (server-validated in `EXTRAS`): `status`, `task`,
`meeting`, `sharing`, `coffee`. Server-set extras: `org`, `teams`, `fm` (the
account name, for matching agent owners and task pings), `desk`, `wb`, `tz`
(the member's saved time zone), `pic` (the account picture's digest) and
`focus` (the running focus session: `{mode, at, end?, mins?, rounds?, topic?,
host?}`).

## Decisions

- **One org's task board.** `/api/tasks` serves the task board of
  `OFFICE_MARKETING_ORG` only (World office marketing tasks), so the board,
  task pings and the agent bots are shown only in that org's office; elsewhere
  the panels say so and stop polling.
- **100 people, not 150.** The Python object fans every movement batch out to
  every socket 20 times a second; at 100 that is 2000 sends a second on one
  core, and the World's office rooms stop at 16.
- **Gated-room whiteboard thumbnails** are sent only to people who may enter
  that room (or stand in it), like the boards themselves.
- **Removed from the PRs:** `features/agents/demo.js` (a localhost preview),
  arcade runs in storage (`play:runs`; a run now lives on its socket) and
  waiting pong seats (`play:seats`; not worth a storage write per seat), and
  `package.json`, `wrangler.jsonc` and both `launch.json` files (Cloudflare and
  editor files).
- **Chat limits:** the office chat's own history and read rate limits (10 and
  20 requests, refilling 2 and 4 a second) went away with the office chat
  tables; the site chat's limits apply.

## Run it locally

1. Build: `FORKMESH_ESBUILD=<world/tools/node_modules/.bin/esbuild> python3
   app/tools/build_site_assets.py app` (or `stage_office` alone) and
   `python3 app/tools/build_worker_python.py`.
2. Serve: from `app/host`, with a venv holding `app/host/requirements.txt`,
   run `python -m forkmesh_host serve --port 8080 --no-cron --migrate
   --no-build` with `FORKMESH_HOST_ASSETS_DIR=app/dist`,
   `FORKMESH_HOST_DATA_DIR=<tmp>`, `FORKMESH_HOST_OFFLINE=1`,
   `FORKMESH_VAR_DATA_KEY`, `FORKMESH_VAR_ADMIN_PASS` and the
   `FORKMESH_VAR_*_ORIGIN` variables set to `http://127.0.0.1:8080` (see
   `app/tests/host/test_org_office_e2e.py`).
3. Sign up through `/signup` (Turnstile test keys), verify with
   `python -m forkmesh_host ops verify-email <name>`, create an organization
   with `POST /api/orgs`, and open `http://127.0.0.1:8080/office`.
4. The host alone does not add the `/office` CSP (Caddy does); to test under
   it, front the host with Caddy and the routes from `deploy/core/caddy_routes.py`.

## Protocols and interfaces

- `GET /api/office/session[?org=<name>]`: `{signedIn, account, orgs, org,
  role, teams, unlocked, rooms, tasks}` for the cookie session. Only the
  caller's own organizations are listed.
- `POST /api/office/entry {org}` (same-origin, session): checks membership and
  returns a 60-second ticket bound to `org_bi`, `account_bi`, `session_id` and a
  nonce (HMAC of the data key with purpose `org-office-ticket-v1`), the socket
  path, and `iceServers`.
- `GET /api/office/ws?ticket=` (Upgrade): checks the Origin, the ticket, that
  the cookie session is the one the ticket names, and membership again; then
  forwards through the websocket handoff (`_validated_websocket_handoff` only
  accepts `FORKMESH_ORG_OFFICE`, object `office:org:<bi>:v1`, path
  `/api/office/org/<bi>/v1/ws`, and the one claim header
  `x-forkmesh-org-office-claim`).
- Socket messages are Kaif's protocol (`join`, `m`, `sig`, `say`, `st`, `emo`,
  `wave`, `ex`, `pr-*`, `work-*`, `agents`, `meet-*`, `wb.*`, `chat*`,
  `play-*`); `join` no longer carries a name, avatar or ticket, the server
  takes identity from the claim.
- Desks: `work-deco {deco}` answers `work-deco {ok}` or `work-err {reason}`
  (`deco`, `no_desk`) and rebroadcasts `work-desks`, whose entries now carry
  `deco` when a desk has one.
- Focus: `focus-start {mode, mins?, rounds?, topic?, notify?}` or
  `focus-start {join}`, `focus-end`; the server answers `focus-err {reason}`
  (`seat`, `gone`, `bad`) and may send `focus-open {id, name, focus}`.
- Profiles: `prof-set {name?, tz?, av?}` answers `prof-ok {name, custom, tz,
  av}` or `prof-err {reason}` (`taken`, `avatar`, `store`) to the sender and
  broadcasts `prof {id, name, av}` when the shown name or avatar changed; the
  zone travels as the `tz` extra.

## Security

- **Identity:** only real ForkMesh sessions. Guests, the demo office, portable
  grants, `identity/mock.js`, `OFFICE_DEV_MOCK_FORKMESH` and the
  `agentsTestAcct` hook were deleted with the Worker; nothing in the served
  code can mint an identity.
- **One office per org, no cross-org access:** the object name comes from the
  ticket's `org_bi`, which the server set after checking `org_members`; the
  object rejects a claim for any other org. Every store query is keyed by
  `org_bi`. The same account opening the office twice replaces its first socket.
- **Revocation:** each socket rechecks its session, account status, membership
  and teams every 30 s (on the next frame and from a sweep timer), and is
  closed with 4003 when any of them lapses. Team changes update the gates live.
- **Team-gated rooms:** Engineering and Design need a matching team (owners and
  admins enter every room); a knock admitted by someone inside grants entry
  for that visit only. The `engineering` and `design` chat channels follow the
  same gate and are never delivered to anyone outside it.
- **Limits:** frames over 16 KiB close the socket (1009); more than 120 frames
  in a burst (60/s refill) closes it (1008). Per-action buckets: movement
  20/12 s⁻¹, signalling 200/50 s⁻¹, extras 10/2 s⁻¹, chat and other actions
  5/1 s⁻¹, whiteboard ops 30/12 s⁻¹ and 64 KB/24 KB s⁻¹, cursors 40/30 s⁻¹.
  At most 100 people per office. Chat text is 1000 characters, status text 60
  graphemes, whiteboard boards 4000 items or 1.5 MB, strokes 2700 numbers.
- **XSS:** every `innerHTML` write goes through `setHtml`, which only accepts
  `html` template results; interpolations are escaped unless they are
  themselves markup. `/office` sends `require-trusted-types-for 'script';
  trusted-types office`, so a raw string sink throws in Chromium.
  `office/tests/html.test.mjs` fails on any direct `innerHTML`, `outerHTML`,
  `insertAdjacentHTML`, `eval`, `new Function` or third-party URL in the
  client. Chat Markdown (`renderText`) escapes first and only emits fixed tags
  and `http(s)` links with `rel="noopener noreferrer nofollow"`. Whiteboard
  text is drawn on a canvas.
- **CSP:** `/office` and `/office/*` in `www/public/_headers`: `script-src
  'self'` (no inline script, no CDN), `connect-src 'self'`, `media-src 'self'
  blob: mediastream:`, `frame-ancestors 'none'`, and a Permissions-Policy that
  allows camera, microphone and display capture for this origin only (the
  site-wide policy disables the camera).
- **WebRTC:** the server relays `sig` only between members who may hear each
  other (same private room, same meeting table, or within 7 tiles in the open)
  and drops everything else except `bye`; `say` (message nearby) uses the same
  rule. ICE servers come from `/api/office/entry` (sign-in, an allowed origin
  and org membership required): `OFFICE_STUN_URLS` and, only when
  `OFFICE_TURN_URLS` and a 16+ character `OFFICE_TURN_SECRET` are set, TURN
  REST credentials that expire after 10 minutes (username `<expiry>:<alias>`,
  HMAC-SHA1 of the secret). The relay is the self-hosted coturn of the core
  nodes; see "TURN relay" below for what it refuses. Microphone and camera
  are requested only when the person presses the buttons; screen share uses
  the browser's own picker.
- **Profiles:** a display name is cleaned like a desk name (control
  characters and angle brackets removed, 24 characters) and can never replace
  the account name in `ex.fm`. The only account API the profile feature calls
  is `POST /api/accounts/profile` with `avatarPng`, same-origin with the
  session cookie; it sends no other field.
- **Agents:** the agent panel only reads `/api/tasks` (its tasks and its
  `agentRuns`) with the viewer's own session and relays state changes; a relayed change is applied only to bots
  owned by the person who relayed it. The office never starts, stops or
  controls an agent.
- **Task board:** `api.forkmesh` only allows same-origin `/api/tasks*`, and
  only in the office of the org the task API serves (`OFFICE_MARKETING_ORG`);
  the task API applies its own membership and assignee checks.
- **Retention:** chat follows the site chat's retention and moderation (the
  same modules; the per-account `chat-retention` setting, at most 7 days).
  The office chat's own 500-message cap and its message-id based trim are
  gone with its tables. Attendance visits are kept 14 days (the weekly board
  needs the current and previous week). Nothing logs message text or tokens.

## TURN relay

Table calls, screen share and spatial audio are browser-to-browser WebRTC.
Behind symmetric NATs and strict company firewalls a direct path never forms,
so each core node runs coturn as a relay of last resort. It is self-hosted on
the three ForkMesh Next cores instead of a third-party TURN service, so call
media never passes through another company.

- **Where:** `fmnext-turn.service` on every core (`[turn]` in
  `deploy/fleet.toml`; `deploy/core/render.py` renders
  `/etc/forkmesh-next/turnserver.conf`, the unit and
  `/opt/forkmesh-next/bin/fmnext-turn-config`). It listens on the node's
  public IPv4 and IPv6 only: 3478 UDP and TCP (STUN and TURN), 5349 TCP (TURN
  over TLS), and relays on UDP 49160-49999. The clients get
  `turn:app.forkmesh.com:3478` over UDP and TCP, `turns:app.forkmesh.com:5349`
  and `stun:app.forkmesh.com:3478` (then `stun.cloudflare.com`), rendered into
  `app.env` as `FORKMESH_VAR_OFFICE_TURN_URLS` / `FORKMESH_VAR_OFFICE_STUN_URLS`.
  The app host's existing round-robin A/AAAA records reach the relays, so no new
  DNS name is needed.
- **TLS:** coturn reads the app host's certificate pair from the certsync
  export (`/var/lib/forkmesh-next/certsync/export/<host>.{crt,key}`, group
  `fmnext-certsync`, which the unit joins). `fmnext-turn-cert.path` watches the
  certificate and reloads coturn (`SIGUSR2`) when certsync installs a renewal.
  DTLS is off.
- **Credentials:** coturn uses `use-auth-secret` (the TURN REST scheme). The
  shared secret `OFFICE_TURN_SECRET` is generated like every fleet secret
  (`next_fleet.py init-secrets` writes a 64-character `token_urlsafe` value to
  `secrets.env`, mode 0600). It reaches coturn as a systemd credential
  encrypted with the node's host key (`systemd-creds encrypt` on stdin, like
  certsync's secret) and the app through the vault (`next_vault.py sync` seals
  it into `host_secrets`), so it is never in the repository, on a command line
  or in plaintext on a node's disk. `fmnext-turn-config` runs as root before
  coturn starts and writes the full config with the secret into
  `/run/fmnext-turn` (tmpfs, 0400, owned by `turnserver`).
- **SSRF:** a TURN relay sends packets wherever an authenticated client asks,
  so it must never reach the node's own networks. `denied-peer-ip` blocks
  0.0.0.0/8, 10/8 (the WireGuard mesh 10.77.0.0/24), 100.64/10, 127/8,
  169.254/16 (the cloud metadata service), 172.16/12, 192.168/16, the
  documentation and benchmark ranges, multicast and 240/4, and in IPv6 `::1`,
  IPv4-mapped, NAT64, Teredo, 6to4, `100::/64`, ULA (`fc00::/7`), link-local,
  site-local and multicast. The unit adds the same ranges as `IPAddressDeny=`,
  so the kernel drops them even if coturn's check failed. The list
  deliberately has no range starting at `::`: coturn 4.6.1 matches every IPv4
  peer against such a range (checked against the Debian binary on 2026-09-30),
  which would turn the relay off. The fleet's own public addresses are not
  denied, because relay-to-relay (both browsers behind TURN, possibly on two
  cores) is the case TURN exists for; over UDP the only wildcard listeners on
  a core are WireGuard and Caddy's HTTP/3, which are public anyway.
- **Limits:** `no-tcp-relay` (UDP relays only, so no TCP connection to any
  peer), `no-multicast-peers`, `no-cli`, `no-rfc5780` and
  `no-stun-backward-compatibility` (less reflection), 12 allocations per
  credential (`user-quota`), 1200 per node (`total-quota`), 512 KB/s per
  session (`max-bps`), allocations end after an hour. The unit runs as
  `turnserver` with no capabilities, `ProtectSystem=strict` and 200 MB of
  memory.
- **Client:** `office/public/rtc.js` uses the entry's ICE list and falls back
  to `stun:stun.cloudflare.com:3478` when the list is empty. The entry is
  fetched before the socket opens; while anyone is nearby, `want()` asks
  `identity.js` to fetch a new list once the current one is 8 minutes old,
  so a call started later still gets valid credentials. Allocations made
  earlier keep working after their username expires.
- **IDS:** the IDS expects `turnserver` running `/usr/bin/turnserver` in
  `fmnext-turn.service`, listeners on 3478 and 5349, and stops the unit in a
  lockdown.

## Configuration

| Variable | Default | Purpose |
| --- | --- | --- |
| `OFFICE_STUN_URLS` | `stun:stun.cloudflare.com:3478` (`app/wrangler.toml`); on the cores `stun:<app host>:3478,stun:stun.cloudflare.com:3478` from `[turn]` | Comma list of `stun:`/`stuns:` URLs given to clients |
| `OFFICE_TURN_URLS` | unset; on the cores rendered from `[turn]` | Comma list of `turn:`/`turns:` URLs (4 at most); TURN is off while unset |
| `OFFICE_TURN_SECRET` | unset | coturn `static-auth-secret`; vault secret, never sent to clients |

`[turn]` in `deploy/fleet.toml`: `enabled`, `host` (default the served app
host), `realm` (`forkmesh.com`), `port` (3478), `tls_port` (5349),
`relay_min_port`/`relay_max_port` (49160-49999; at most 4096 ports inside
32768-65535), `user_quota` (12), `total_quota` (1200), `max_bps` (512000),
`max_allocate_lifetime` (3600) and `fallback_stun`. The renderer refuses
`[turn]` when `OFFICE_TURN_SECRET` is not an app secret, or when
`production.vars` also sets the URLs.
| `OFFICE_MARKETING_ORG` | `forkmesh` | The org whose office shows the task board and agents |

## Build, run, test

- Build: `python3 app/tools/build_site_assets.py app` (needs esbuild from
  `world/tools`, or `FORKMESH_ESBUILD`).
- Python: `app/tests/test_org_office_protocol.py` (parity with the client,
  needs node), `test_org_office_room.py` (the socket logic over the real
  migration in SQLite), `test_org_office_routes.py`,
  `app/tests/host/test_org_office_e2e.py` (starts the host; needs uvicorn).
- Node: `cd office && node --test tests/*.test.mjs`.
- Browser: the integration run drove `/office` through the host in headless
  Chromium with the `/office` CSP injected, two acme members, one globex
  member, a member with no team and eight walking bots.

## Performance

- Bundle: 267 KB minified, 99.4 KB gzip script; 56 KB, 11.2 KB gzip style
  (budget 110 KB and 14 KB gzip), with both office stacks of 2026-10-04
  merged: the rooms, Coffee Huddle and room focus, and the avatars, applause,
  south wing, focus sessions and desk decoration. The Agent Room bubbles, the
  rollercoaster and the zoom about the mouse brought the script to 110.8 KB
  gzip, so its budget went to 116 KB.
- Rendering is Canvas 2D: the map is one pre-rendered canvas; avatars and name
  tags are cached sprites; the frame loop culls to the viewport and reuses its
  arrays. With 10 people moving, headless Chromium held p50 and p95 frame
  intervals at 16.7 ms with no frame over 50 ms at 1280×800 and at 390×844.
- Movement is batched to 20 Hz and the proximity mesh is recomputed every
  250 ms; each browser holds at most 8 peers.

## Operations

- Deploy with the normal app deploy: migration 0226 (`tools/rqlite_migrate.py`,
  run by the deploy), the new `FORKMESH_ORG_OFFICE` binding (read from
  `app/wrangler.toml` by the host), and the rebuilt `app/dist` and Caddy routes.
  A core roll picks up the new Durable Object class.
- Before the first deploy, confirm no account or organization is named
  `office`: `/office/<repo>` now serves the office bundle.

- **TURN rollout** (once): `tools/next_fleet.py init-secrets --execute`
  (adds `OFFICE_TURN_SECRET` to `secrets.env`), `tools/next_vault.py sync
  --execute` (seals it for the app), then a normal core roll
  (`next_fleet.py roll --execute`; `ROLL_STEPS` is now daemon, turn, files,
  start). The `turn` step masks Debian's `coturn.service`, installs the
  package and pushes the encrypted credential; `files` installs the config,
  units and firewall; `start` enables and restarts `fmnext-turn.service`.
  A roll checks the secret before draining the first node. The vault sync
  makes every app re-read the vault; the TURN URLs arrive with the roll's
  `app.env` and app restart, so a node hands out TURN credentials once it is
  rolled. The unit is skipped (not failed) on a node without coturn or the
  credential.
- Check a node: `systemctl status fmnext-turn`, `journalctl -u fmnext-turn`,
  `turnutils_stunclient app.forkmesh.com` from outside, and in a browser
  `chrome://webrtc-internals` shows `relay` candidates.

## Traps

- Caddy, not the app host, applies `app/dist/_headers`. Never put a `! Name`
  and a `Name:` for the same header in one rule expecting Cloudflare order;
  the route generator drops the delete, and a lone `! Name` stays a deferred
  delete.

- `/office` has no trailing slash (`drop-trailing-slash`), so every asset URL in
  `office/public/index.html` is absolute (`/office/...`); the build rewrites
  exactly those tags and fails if one is missing.
- The Python rules must match the client's: change `map.js` or a shared helper
  and `test_org_office_protocol.py` will fail until `org_office.py` follows.
- A new client file that builds markup must use `html` and `setHtml`; a plain
  string reaching `setHtml` throws.
- A new floor character needs a `ZONES` entry and a painter in `FLOORS`
  (`render.js`); without the painter `buildMapCanvas` throws and the office
  never opens. A new chair character goes in `CHAIRS` on both sides.
- Do not move existing furniture when adding rooms: desk claims and
  whiteboards are keyed by tile.
- Room labels are not on the map canvas. Anything that shows a copy of the
  map and wants them calls `drawLabels`, as the join preview does.
- The focus field and the room card need `ROOMS[zone]`, which `room()` and
  `area()` fill. A private zone made with a bare `rect()` gets neither.
- `glassWall` expects a second wall row under the room's top wall
  (`rect(2, 16, 14, 16, '#')` for the Coffee Huddle, mirrored in
  `org_office.py`); without it the wall face is painted over walkable floor.
- Never add a `denied-peer-ip` range that starts at `::`; coturn then denies
  every IPv4 peer (`deploy/tests/test_turn.py` fails on it).
- The relay ports must stay above `udp_max_port` (10240) of the IDS, which
  otherwise reports each relay socket as an unexpected listener.

## Recent changes

- 2026-10-13: the chat conversation list shows the newest line of each
  conversation with its time, and direct-message rows show the other
  person's picture and presence dot
  ([chat-private-rooms.md](../app/chat-private-rooms.md#last-line-and-pictures-in-the-conversation-list-2026-10-13)).
  Edits and deletes from others apply again (round 1 had dropped `mutate`).
  The script bundle is 128 952 of the 129 000 gzip bytes in
  `office/perf-budget.json`; the budget was not raised, so the next addition
  will need it raised.

- 2026-10-13: own chat messages show a sending, sent, delivered or read mark,
  and deleting a direct message also withdraws the relay's stored copy
  ([chat-private-rooms.md](../app/chat-private-rooms.md#delivery-ticks-on-every-own-message-all-clients-2026-10-13)).
  The script budget in `office/perf-budget.json` rose from 128 KB to 129 KB
  gzip: the marks, the receipt handlers and the transport's `supersede` added
  about 0.9 KB, and the build was already at 127.4 KB (now 128.2 KB).

- 2026-10-09: no toasts. Every toast became an inline status line in the
  panel, card or dialog it belongs to, or a keyed line above the dock; the
  toast stack and its styles are gone (see "Status lines, not toasts"). The
  focus timer and the desk card moved into the dock column with the lines.
  The "What's new" card became a dock line too. The script bundle grew by
  about 1.4 KB gzip (to about 126.4 KB of the 128 KB budget).

- 2026-10-04: the Royal Flush; START signs, the ride button, the park guide
  and click-to-ride on attractions (see "Water park").

- 2026-10-04: the water park, a second scene reached by car (see "Water
  park"); the rollercoaster became one ride of the shared ride engine. The
  script budget in `office/perf-budget.json` rose from 116 KB to 128 KB gzip
  for the park's layout, art and rides (the build was at 121.7 KB).

- 2026-10-04: the Agent Room is twice the agent cap (48 stations) and spans
  the north side; the Garden, Campfire and coaster platform moved 16 tiles
  east; rides start again; woodland outside the track; the reconnecting banner
  waits 4 s (see "Agent Room", "Rollercoaster", "Reconnecting banner").

- 2026-10-04: the Agent Room above Engineering (station grid, console, bubbles
  with each agent's title and redacted latest words), the rollercoaster
  around the office, and the wheel zooming toward the mouse (see "Agent Room",
  "Rollercoaster", "Controls"). The script budget in `office/perf-budget.json`
  rose from 110 KB to 116 KB gzip; the first auto-deploy of this change failed
  at `build_site_assets.py app` over the old budget.

- 2026-10-04: swimming in the Garden pond, the Campfire with the fire score,
  focus emoji, optional Focus status and recent focus sessions, logging a
  coffee to the Calorie Counter, and agents without a task in the office (see
  "Swimming", "Campfire", "Focus sessions", "Coffee and the Calorie
  Counter", "Agents without a task", "Keeping your place on refresh"). The map grew a Campfire zone
  east of the Garden; no existing tile moved.

- 2026-10-04: both office stacks merged (pulls #253 to #262). The bean bag
  became `j`, because `n` is the Lobby's lounge seat. The "let others know"
  minute is now kept per account on the room object, not per socket, so
  reconnecting no longer resets it and cannot be used to spam the office. A
  desk decoration that has not changed no longer causes a storage write or a
  `work-desks` broadcast.

- 2026-10-04: the Lobby, Media Room and Library labels were centred on those
  rooms' doors and hid them. They moved onto the wall two and a half tiles to
  the left of each door, and labels now shrink with the map below 1× zoom.

- 2026-10-03: Coffee Huddle redesign and room focus (see "Floor plan" and
  "Room focus"). The Coffee Huddle was a checkerboard floor with grey boxes
  for a kitchen, and nothing on screen showed that a private room closes the
  conversation to the people inside it. The room is now a kitchen with a
  glass back wall, a round table and wooden chairs, and standing in any
  private room dims the rest of the office and lists who is in the room. The
  doors moved from the corridor wall to the east wall so the back wall could
  be one piece of glass; the room has no desk or whiteboard, so no stored tile
  moved. Room labels left the map canvas because text baked into it blurred
  when zoomed. The script budget rose from 84 KB to 92 KB gzip.

- 2026-10-03: Library, Media Room and Lobby Couches (see "Floor plan"). The
  office had no quiet room, no room for more than four people at one table,
  and the Lobby's only seats were its meeting table. The map grew 14 tiles to
  the east (52 to 66 wide) for the two rooms, and the couches went on a rug
  in the Lobby's empty north-east corner. The Lobby's sofas were decoration
  that nobody could sit on; the new couch pieces are chairs.

- 2026-10-03: desk decoration (see "Desk decoration"). Every desk looked the
  same. A desk's owner can now pick the desk and chair colours and place
  items in four slots; the choice is validated by the room object, stored
  with the desk and drawn for everyone.

- 2026-10-03: the south wing and focus sessions (see "South wing" and "Focus
  sessions"). Couches were scenery and the office had four desks, all in
  gated rooms. The map gained the Workspace and the Focus Lounge, couches
  became seats, and a couch now starts a timed focus session that sets Focus
  status, mutes the microphone and can be joined by others. `init` now
  carries the server time. Merged on 2026-10-04 with the Library, Media Room
  and Lobby Couches: the map is 66 by 44 and the bean bag is `j`.

- 2026-10-03: applause (see "Applause"). Reactions were silent emoji. Claps,
  confetti and a dance now animate the avatar, the room object counts
  celebrations per room and announces a cheer, and `sound.js` synthesizes the
  claps. `sprite.js` gained three arm poses and the shell two hooks
  (`onEmote`, `onPose`). 💃 joined the reaction list.

- 2026-10-03: avatar editor (see "Avatars"). A member's look was a hash of
  the account and could not be changed. Avatar drawing moved from
  `render.js` to `sprite.js` with the code model in `avatar.js`, the look
  gained hair styles, facial hair, bottoms, shoes and one extra, and the
  profile record stores the chosen code. The bundle budget in
  `office/perf-budget.json` went from 84 KB to 110 KB of gzip script and from
  12 KB to 14 KB of gzip style: the script was at 78.8 KB before profiles and
  avatars, 85.1 KB with them, and the applause, focus and desk decoration
  work that follows needs the rest.

- 2026-10-03: profile cards and Edit profile (see "Profiles"). Members were
  shown only by account name with a letter in a coloured circle, and nothing
  about a person could be opened or edited. The office object now keeps a
  display name and time zone per member, the account picture reaches the room
  through the entry claim, and the client gained the card, the edit dialog
  and three plug-in hooks (`onPerson`, `meMenu`, `face`). The play overlay's
  key handler no longer swallows keys typed in a `select`, or Enter and Space
  on a focused button of a dialog that has no game keys.

- 2026-09-30: self-hosted TURN/STUN for the office calls (see "TURN relay").
  Before this the clients had only a public STUN server, so calls between
  people behind symmetric NATs or strict firewalls never connected. coturn now
  runs on every core, the app hands out its 10-minute credentials, and the
  client refreshes them during long visits and falls back to public STUN.

- 2026-09-30: signed-in members no longer see "Let's get you in". The
  earlier auto-enter still left two paths to it. `office/public/index.html`
  hard-coded that heading as the page's default, and the client only replaced
  the small box under it, so every signed-in load showed the gate heading
  through the session check, the entry request and the socket handshake, and
  for as long as a reconnect lasted. An account in several organizations with
  no org remembered in that browser (a new device, a cleared profile, or the
  first visit after the change) also stopped on that screen with an org
  picker. The page now opens on a neutral loading heading, the gate heading is
  written only for a confirmed signed-out session, every signed-in member
  with an organization enters straight away, and switching offices moved to
  the account strip inside the office.

- 2026-09-30: the Office chat became the site chat (migration 0227, see
  Architecture). Audit fixes: task pings and the `fm` alias match the full
  account name (display names stay 24 characters); the "working on" task is
  stored per org and cleared in offices without the task board, which also
  stops polling; a full office says so instead of retrying; a server's
  `denied` reason is no longer replaced on close; gated whiteboard thumbnails
  are gated.

- 2026-09-30: the `/office` CSP and Permissions-Policy were missing in
  production. `deploy/core/caddy_routes.py` turned a `_headers` `! Name` line
  into `header -Name`; Caddy defers every header operation of a handler that
  deletes one, so the delete ran after the replacement and removed both (the
  World's `/world` CSP was lost the same way). A `! Name` followed by `Name:`
  in the same rule is now emitted as the replacement alone.
  `deploy/tests/test_caddy_served_headers.py` runs the generated routes in a
  real Caddy (`FORKMESH_CADDY` or `caddy` on PATH) and fails when `/office`
  lacks its CSP or media permissions.

- 2026-09-30: merged #233 to #242 (except #238), ported the Worker to the app
  host, replaced guest and grant sign-in with the app session, added the
  escaping layer, CSP, signalling limits and the bundle build.

## Related docs

- [World](../world/README.md), [app](../app/README.md),
  [cron and Durable Objects](../app/cron-and-durable-objects.md),
  [host](../host/README.md), [PLAN](../PLAN.md).
