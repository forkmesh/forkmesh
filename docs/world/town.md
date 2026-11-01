# The town

Part of [World](README.md). What stands in ForkMesh's own World and what you
can do there: the city hexagon, desert ring, roads and instance hexagons, the
Office tower, the board ring, the Members Circle, the mirror yard, floor
notes, and the rides and activities. The worlds next door are on
[World mesh](mesh.md).

## Layout

- **City.** The city lawn is a rounded hexagon, apothem
  `CONTINUOUS_CITY_RADIUS` (365), corner radius
  `CONTINUOUS_CITY_CORNER_RADIUS` (70), flat edges east and west.
  `roundedHexagonGeometry()` draws it and `roundedHexagonContains()` bounds
  walking, so the drawn and walkable edges match.
- **Desert ring.** Six desert hexagons (`DESERT_HEXES`) of the same shape tile
  around it at `WORLD_HEX_PITCH` = 2 × 365 + `WORLD_ROAD_WIDTH` (10).
  `DESERT_PLOTS` holds twelve plots, each with a "start your own instance"
  sign that opens `<site>/start?plot=<n>` ([INSTANCES.md](../operations/instances.md)).
- **Roads.** `createHexRoadNetwork()` draws a 10-unit road around every hex
  in three draws (asphalt, edge lines, instanced centre dashes).
  `createRoadGraph()` builds the matching graph (24 nodes, 30 roads) for
  bikes.
- **Neighbouring worlds.** Every World is a seven-hex "flower" (its city and
  its six desert hexes) on one shared map, and other worlds sit on the
  flowers around it. You can see them and walk straight in. See
  [World mesh](mesh.md).
- **Instance hexagons.** `INSTANCE_HEX_SLOTS` (`scene/layout.js`) is a second
  ring of 12 tiles. An instance with its own world gets a meadow hexagon with
  roads, its logo as a ground decal, and a landmark board that opens the
  instance's address. `refreshInstanceDirectory()` reads
  `/api/start/instances` every 5 minutes and keeps entries with
  `world: true`, an https address, a 32-hex id and an unused slot. Nothing is
  built while no instance has a hexagon. An entry with a `worldUrl` is a
  portal: the board travels to that instance's own World at
  `?portal=<slot>`, and an instance's World draws one "ForkMesh World"
  hexagon that leads back; `?portal=<n>` places the arriving visitor in front
  of hexagon `n`
  ([../app/instance-federation.md](../app/instance-federation.md#world-connectivity)).
  On an instance with its own hexagon (a territory), the World is drawn as
  that one hexagon with the ForkMesh hexagons next door, and walking across
  its road puts the page in the ForkMesh World without a reload
  (`leaveOwnTerritory()`, [../INSTANCES.md](../operations/instances.md#an-instance-world-is-its-own-hexagon),
  Stepping into the ForkMesh World); while the walker stands in the
  territory `placeWorldMesh` draws no world-mesh neighbours, so the two
  layouts never overlap. Off a territory the [world-mesh](mesh.md) flowers
  take the place of a road crossing into an instance hexagon
  ([../INSTANCES.md](../operations/instances.md#an-instance-world-is-its-own-hexagon)).
- **Bounds.** `desertWalkSurfaceContains()` is the union of all hexes grown
  by the road width plus junction discs, the hexes of every placed
  neighbouring world included (`setWorldMeshFlowers()`). `WORLD_RADIUS` (1900) bounds remote
  avatars, spawns, flight and the server's saved-view check; the relay's
  `WORLD_COORD_LIMIT` is 16000.
- **Ground logo.** `groundLogoTexture()`/`applyGroundLogo()` (`scene/town.js`)
  patch the city grass shader to tint the ForkMesh mark into the lawn, faded
  by camera height; no extra mesh or draw.

## Office tower

The Office is a four-storey regular hexagon (`world-office-tower.js`) with a
flat side facing the town path.

| | Value |
| --- | --- |
| Across, corner to corner (`OFFICE_WIDTH` = 2 × `OFFICE_HEX_RADIUS`) | 85.45 |
| Front to back, flat to flat (`OFFICE_DEPTH` = 2 × `OFFICE_HEX_APOTHEM`) | 74 |
| Storey height (`OFFICE_FLOOR_HEIGHT`) / floors / tower height | 16 / 4 / 64 |
| Tower centre (`OFFICE_ISLAND_CENTER`) | (0, 0, -207) |
| Front door, World z (`OFFICE_ENTRANCE_Z`) | -170 |
| Elevator (`OFFICE_ELEVATOR_CENTER_X/Z`) | (12, -37), back face |

- Furniture positions are written in a reference plan and mapped with
  `officePlanX/Z`; wall-mounted objects stay on their wall
  (`OFFICE_HEX_SIDES` gives each face's normal, tangent and `rotationY`).
- `officeHexExtent(x, z)` drives every collision test
  (`officeInteriorContains`, `officeTowerShellContains`, wall clamps, camera
  clamp). Colliders and meshes read the same exported constants
  (`FLOOR_OBSTACLES`).
- Floors: lobby (reception, logo fountain, lounge, aquarium with reef
  controller, boards, warp doors), Marketing, Engineering (workstations,
  System Capacity, debug panel) and the rooftop (patio tables, zip-line
  station).
- A build stamp under the `FORKMESH OFFICE` sign shows the running World
  version, commit and deploy age from the `forkmesh-world-version`,
  `forkmesh-world-built` and `forkmesh-world-deploy` metas (falling back to
  `/api/version`).

## Board ring

Leaderboard Island's boards stand on one ring
(`scene/districts/billboard-circles.js`, sized by `BILLBOARD_RING` in
`scene/layout.js`), so the boards read as one plaza and each fetches only
when approached.

- **Layout.** Boards are grouped by section (`sections`: hub, social,
  leaderboards; a placement's `circle` option names the section, so existing
  packages keep working) and then by `order`. Each board gets the arc it
  needs, the spare arc is shared evenly, and an `entryGap` is left open
  towards the causeway. `minRadius` (38) already fits every board, X and
  Reddit banners included, so the ring is sized once and does not regrow as
  boards are placed at boot.
- **Refresh pads.** Every board with a `feed` has a small semicircle in front
  of it (`padRadius` 2.2). A board fetches only when you step onto its own
  pad: `updateBillboardPads()` runs with the other proximity checks at
  12.5 Hz, rejects anyone outside the pad band with one distance test, and
  reports `onBillboardNearby({ feed, refetch })` / `onBillboardAway({ feed })`
  to the shell. Leaving needs `padExit` more distance than entering, so
  standing on the edge cannot flap the request. Each board has its own
  cooldown (`cooldownMs`, 60 s; the QA board 5 s), so walking along the
  leaderboard cards refreshes each card once rather than the whole family.
  The shell's `startBillboardWatch(feed)` maps a feed to its loader; the QA
  deck polls only while you stand on its pad, and the Mastodon and social
  banner countdowns refetch only while you stand on their pads.
- **Spinners.** Only the board whose pad started a request spins. Feeds that
  share one request (all leaderboard cards read `/api/leaderboards`) still
  repaint together from that one snapshot, but only the approached card
  shows the spinner. A request started elsewhere (a QA action, copying a
  referral link) spins every board of that feed.
- **Draw cost.** The ring is one textured apron (a unit circle scaled to the
  ring), one rim, and one `InstancedMesh` holding every pad, so all pads are a
  single draw call and the active pad is lit by its instance colour (`muted`
  idle, `accent` underfoot). The rim and pads are flat decals with
  `polygonOffset`, so they never z-fight the apron. Geometry comes from the
  resource authority (the pad is a cached half-disc, laid flat by its
  instance rotation).

## Members Circle

Members sit in rings around a campfire by lifecycle band; members with a
custom avatar sit in the innermost band (`MEMBER_CIRCLE_AVATAR_BAND`). Each
band has a ground label with its name and count. A live peer's photo face is
used unless the peer explicitly chose the generated face; failed photo loads
retry three times.

## Mirror yard

Mirror-node cabinets are built from `loadFountainData()`.
`mirrorNodeServices(node)` turns the signed mirror record into up to six
services (`sync`, `git`, `browse`, `merge`, `actions`, `agents`), drawn as one
instanced batch of gears on the cabinet roofs: brass and spinning when
healthy, red and stopped otherwise. Rotation runs only near the yard, when
the element is shown and without reduced motion.

## Floor notes

`world-floor-notes.js`: a tap on empty town ground offers "Add a note here";
notes are `THREE.Sprite` cards (the 24 nearest within 48 units). Each note is
a thread; `@name` pings existing accounts (at most 10 per post, 25 per reply
including earlier participants) with a link to `?note=<id>`. Changes send a
signed control request (`/api/world/floor-notes-changed`, HMAC, one minute) to
`ForkMeshWorld`, which broadcasts `{"type": "floor-notes", "id": ...}`.

- API: `GET /api/world/floor-notes` (80 most recently active), `GET ?id=<id>`,
  `POST` with `action` `create`, `reply` or `delete`. Text ≤ 500 characters,
  positions bounded (|x|, |z| ≤ 1500, −20 ≤ y ≤ 200), 20 notes per account,
  one post per 5 s, 100 replies per thread.
- Storage: `world_floor_notes`, `world_floor_note_replies`; author and text
  encrypted in `data`, keyed by `account_bi`, removed with the account.

## Activities

- **Quadcopter flight** (`updateQuadcopterRide()`): flies where the camera
  looks; holding forward accelerates from 42 to 150 units/s.
- **Drone racing** (`scene/drone-race.js`, `scene/runtime/drone-race.js`):
  ten rings and a gate weaving around the coaster; passes are segment tests
  against each ring plane; the best lap and its ghost persist under
  `forkmesh.world.droneRace.v1`. Movement frames carry `race: {ring, lap,
  best}`, which the relay (`sanitize_message()` in `world/src/world.py`)
  clamps and never puts in presence snapshots.
- **Coaster** (`scene/coaster.js` maths, `scene/districts/coaster.js` meshes,
  `scene/runtime/coaster.js` ride): a lap around the town hexagon with loops,
  a ballistic jump, corkscrew and helix; the track is resampled into typed
  arrays and drawn as LOD chunks with one shared material; the train is one
  `InstancedMesh`. Presence frames carry `COASTER_RIDING_ACTIVITY`.
  In first person, `placeCoasterEye()` puts the eye `FIRST_PERSON_EYE_HEIGHT`
  along the seat's up axis and takes the seat's whole orientation, so the view
  pitches, banks and inverts with the car instead of staying level on world up.
  Drags while riding are kept as `ride.lookYaw` (seat-relative yaw, reset to
  forward when first person is switched on) and `firstPersonPitch` is applied
  in the seat frame, so a glance to the side holds through loops where
  `followHeading()` stops tracking the world heading. `seatRider()`
  re-orthogonalises the track's interpolated up against its tangent so the
  seat quaternion is a clean rotation. Test:
  `test_the_first_person_eye_turns_over_with_the_seat_and_keeps_a_glance`.
- **Bikes** follow the road graph (`districts/roads.js`); tapping left or
  right before a junction picks a branch, holding leaves the road, and riding
  onto a road along it rejoins. Tests: `world/tests/test_world_bike_junctions.py`.
  On the outer edge of the world (a road with only one drawn hex beside it)
  the default turn at each junction stays on the outer road (`outerTurn` and
  `outerIncoming` on each graph edge), so a rider circles the whole edge
  without steering and only goes back in after a tap toward the inside. Test:
  `test_riding_the_outer_edge_stays_on_the_outer_road_unless_steered_in`.
  The six outer junctions, where two drawn hexes meet open land, are T
  junctions: each carries a `bend` (the centre of the missing third hex and its
  corner), and the roads along that hex curve into each other around its
  rounded corner. `roadBendPositions()` lays that corner's asphalt, its centre
  dashes are kept through the junction while the side road's stop short, the
  road graph adds the through turn between the two edges that share no drawn
  hex, `roadBendContains()` makes the curve walkable, and the junction's fill
  triangle points along its three roads. Tests:
  `test_outer_junctions_curve_one_road_into_the_next`,
  `test_outer_junction_dashes_follow_the_through_road`.
- **Parachute** (`parachuteSinkVelocity()`), **gym** (`poseAvatarArms()`),
  **avatars** (the back name board is a child of the torso).
- **Office aquarium reef controller** (`scene/aquarium-panel.js`): a physical
  panel of buttons, a dial, switches and a lever on the tank (feed, tap,
  follow, school mode, backdrop, glass response, light), picked only in the
  lobby through invisible hit pads tagged `interactive: "aquarium-control"`.
  Settings persist in `localStorage` (`forkmesh-world-aquarium-*`); fish
  steering is a boids step over typed arrays (`scene/aquarium-schooling.js`).
