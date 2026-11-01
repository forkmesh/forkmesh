# Office features

Part of [Office](README.md). What a member sees and can do in the 2D office:
entering and controls, the map and its rooms, profiles, avatars, desks and
focus sessions, the Agent Room, rides, proximity audio, status lines and the
limits of each feature.

## Entering

On load the client reads `/api/office/session` (with `?org=` or the org
remembered under `office-org`). Signed out, it redirects to
`/login?next=<office URL>`. Signed in, it enters the requested or remembered
org when the account is a member, otherwise the server's pick, requests
`/api/office/entry` and connects. Accounts in several orgs switch from the
People panel (`/office?org=<name>`). Failure states: "Could not reach
ForkMesh", "No office yet" (no organization), 401 back to login, refused
entry with an Enter button. Microphone and camera stay off until pressed.
`office/tests/identity.test.mjs` and `app/tests/test_org_office_entry_client.py`
cover these paths.

A refresh rejoins at the remembered tile (`office-place:<org>`, under seven
days old); the server validates it like any join position. The
"Reconnecting…" banner appears only after 4 s without a connection.

## Controls

WASD or arrows, click-to-walk (breadth-first path), drag to pan, zoom 0.75×
to 3×; Enter opens chat, ⌘K (Ctrl+K) the command palette, ⌘F people search,
Esc closes. Clicking furniture walks up and uses it (desks, whiteboards,
arcade, ping-pong, coffee, seats). Phones get a joystick, pinch zoom and
bottom sheets.

## Map

The map is a tile grid (`W`, `H` in `map.js`); floor characters select
zones (`ZONES`) and painters (`FLOORS` in `render.js`). The office has two
rows of rooms on a corridor (Engineering, ping-pong hall, Design, Library;
Coffee Huddle, arcade hall, Lobby, Media Room), the Agent Room to the north,
a south wing (Workspace with 24 claimable desks, Focus Lounge), the Garden
with a pond and the Campfire. A second scene to the east is a water park
reached by car. `SCENES`/`sceneAt` keep the camera and minimap inside the
current scene.

- **Private rooms** (Coffee Huddle, Engineering, Design, Library, Media Room,
  Lobby Couches, Campfire) use room audio. Engineering and Design are
  team-gated; owners and admins enter every room; a knock admitted from
  inside grants entry for that visit.
- **Seats** are characters in `CHAIRS` (and `COUCHES` for couches and bean
  bags); walkable, sit-able and found by "Take a seat".
- **Room focus.** Inside a private room in `ROOMS`, the outside is dimmed,
  a room card shows the room and its people, and "Room link" copies
  `?room=<zone>` (accepted only for zones in `ROOM_ANCHORS`). Room labels
  are screen-space pills (`roomTag`), never on a door.
- Wall art (`DECOR`) and the woodland outskirts are drawing only, not on the
  tile grid or the server.

## People

- **Profiles.** A per-org display name (≤ 24 characters, `prof-set`) replaces
  the account name on the map; `@account` stays the identity in `ex.fm` and
  on every card. A name equal to someone present is refused (`taken`). A time
  zone (IANA name) and the account's ForkMesh picture (digest `pic`, loaded
  from `/api/accounts/<account>/avatar?v=<pic>`) complete the card. Edit
  profile posts a cropped PNG under 256 KB to `/api/accounts/profile` as
  `avatarPng`. Stored per member as `prof:a:<account_bi>`.
- **Avatars.** One integer `av`: hashed codes (0–9999) derived from the
  account, or custom codes from 2^24 packing eight fields in mixed radix
  (skin, hair style, hair colour, top, bottom, facial hair, extra, shoes).
  The server accepts only a valid code (`clean_avatar`); `av: null` returns
  to the hashed look.
- **Desks and decoration.** A desk owner can set the desk surface, chair
  colour and four fixed slots (left, right, screen, floor). A decoration is
  eight small integers checked against the catalog (`clean_deco`), stored in
  the desk record and broadcast in `work-desks`.
- **Focus sessions.** From a couch or bean bag: Focused (15–120 min),
  Pomodoro (1–6 rounds) or Freeform, optional topic (60 chars) and emoji.
  `focus-start` is validated and timed by the room object's clock; walking
  off the couch ends it. "Let others know" sends `focus-open` to others, who
  can join with `focus-start {join}`. Recent sessions are kept per browser
  (`office-focus-recent`).
- **Fire score and Campfire.** The room object ranks members by recency and
  depth of activity (`fire_score`/`fire_ring` in `org_office.py`, one SQL
  query over `org_members`, `world_user_activity`, `contributor_activity`
  and `org_office_visits`). The top eight hold the campfire seats; only top
  members may enter (`fire-locked`). Recomputed at most every 10 minutes and
  broadcast as `fire` (account names, rank, tier and score only).
- **Applause.** 👏, 🎉 and 💃 animate the avatar; three people celebrating in
  one room within 4 s triggers `cheer {z, n, ids}` (at most once per 8 s).
  Sound is generated with WebAudio; the setting stays in `office-sound`.

## Agents

- **Agent Room.** A grid of stations (`AGENT_STATIONS`) holds every agent the
  office knows, up to `MAX_BOTS`, in start order, with a console and
  per-bot bubbles showing title and latest words.
- **Sources.** The office reads `/api/tasks` (up to four pages) with the
  viewer's session: task-linked agents and `agentRuns`. Runs without a task
  come from the desktop's signed agent-status batch, stored sealed in
  `organization_agent_presence` (provider, model, status, owner, start time,
  title and latest words); rows are pruned 8 hours after a run ends or 24
  hours without a report, 40 per account. `agent.runBy` (server-set) places a
  bot beside the person who ran it.
- **Redaction.** `agent.prose` (≤ 200 chars) and `agent.title` (≤ 120) are
  redacted on the desktop and again on the relay: private key blocks, common
  token prefixes, JWTs, `password=`/`token:` pairs and long hex/base64 runs
  become `[redacted]`. Only this scrubbed snippet is shared with org members;
  transcripts stay end-to-end encrypted
  ([encryption](../encryption.md)).
- The office never starts, stops or controls an agent.

## Rides and play

`RIDES` (generated) gives each ride a gate tile, a boarding rectangle and an
exit tile. The client sends the extra `ride: {"id": ...}` from inside the
rectangle; `_extra_ride` validates it, moves the rider to the exit and stores
`{"id", "at"}`. Every client plays the same timeline (`rides/defs.js`,
`rides/timeline.js`) from `at` on the shared clock (skew from `init.now`, up
to `CLOCK_SLACK_MS` early accepted); the server never knows the path. Ride
paths are built from `ANCHORS` in `park/art.js` and checked against the art by
`office/tests/rides.test.mjs`. The water park layout is generated into both
`park/layout.js` and `org_office_park.py` (`test_org_office_park.py` fails when
stale). Swimming is client drawing only; coffee can log to the Calorie
Counter package through same-origin `api.calories`.

## Proximity and audio

- Open areas: connect within 3 tiles (Chebyshev), drop past 5; at most 8 peers,
  nearest first. Volume full within 1 tile, fading to 0.15 at 5.
- Private rooms: everyone inside at full volume, nobody outside.
- Meeting tables connect exactly the people seated, wherever they are.
- Focus status blocking calls is a client courtesy, not access control.
- Live quality widget: while you talk with someone nearby, or
  with your microphone on, a pill under the room name shows signal bars, a
  voice waveform (you up, nearby people down) and the round-trip time; click
  it for a minute of network and video charts. It is the call widget from
  `app/public/call-quality-meter.js` (see
  [chat-direct-calls.md](../app/chat-direct-calls.md), "Live quality widget"), bundled like the chat modules, reading every mesh
  peer connection. A round button next to the pill turns red when the call has a
  problem; pressing it explains the problem, the fixes and an AI prompt that
  learns from earlier calls on this device ("Diagnostics" in the same page).

## Status lines

The office has no toasts. Feedback goes in the panel, card or dialog it came
from (`setNote`), or above the dock (`#notes`, `noteLine`) for events with no
panel: keyed lines, at most three, each dismissible; info lines clear after
10 s, errors and lines with buttons stay. `api.toast` is an alias of
`api.notice`. `office/tests/notes.test.mjs` fails if a toast container comes
back.

## Feature limits

| Feature | Limits |
| --- | --- |
| Summon | one per person per 20 s; pending 60 s; 8 newest kept; refused in Focus |
| Auto-away | 10 minutes without input or with the tab hidden |
| Attendance | gaps under 90 s merged; top 10 today and this week; kept 14 days |
| Knock | once per room per 15 s; expires after 2 minutes |
| Screen share | 15 fps (max 30), 1.5 Mbit/s |
| Arcade (Snake) | ≤ 3 points per second of play, ≤ 5000, runs ≤ 30 minutes; top 10 per office |
| Ping-pong | server-driven at 30 Hz, first to 5; players within 2 tiles of their end |
| Coffee | one counted break per person per 5 minutes |
| Water cooler | one question per cooler per 6 s, heard within 3 tiles |
| Focus session | couch or bean bag only; one "let others know" per account per minute |
| Applause | 3 people within 4 s; one cheer per room per 8 s |
