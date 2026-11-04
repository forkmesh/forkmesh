# World mesh

Part of [World](README.md). All the Worlds (forkmesh.com and every
self-hosted instance with its own World) share one map, so you can see the
worlds around you and walk into them without changing page. This page covers
the map, single-hexagon neighbours and their published scenes, the directory,
watch and visit sockets, standing in another World, visas and the limits. The
protocol another instance implements to join is [federation](federation.md).

## Map

Each World is a flower of seven hexes: its city plus the six
desert hexes. Flowers tile the plane without gaps. The hub (forkmesh.com) is
at the origin. Directory slot `n` is `worldMeshSlotOffset(n)`
(`scene/layout.js`): slots 0–5 are the six flowers touching the hub, 6–11
the next six. Every client draws itself at the centre and each other world
at `offset(world) - offset(self)`. An instance therefore sees the hub and
its fellow instances where they really sit.

## Single hexagons

A slot 0–4 listed in the hub's `WORLD_MESH_HEX_SLOTS`
(comma-separated, `0` in the production fleet) is published with
`"shape": "hex"` and drawn as
one of the town's petals: it takes the place of desert petal
`NEIGHBOUR_HEX_PETALS[n]` (`scene/layout.js`; petal p lies at 60°·p, as in
`DESERT_HEXES`) and shares an edge with the town. Slot 0 is petal 1, slot 4
petal 5; petal 0 keeps the beach. That petal's sand, cacti and sign step
aside (`replacePetals` in `desert-plots.js`), the roads stay as they are,
and the town no longer claims the ground (`worldMeshOwnerAt`). From a
distance the petal shows the neighbour's published scene
([Neighbour scenes](#neighbour-scenes)). Stepping off the road onto its land puts the page in that World,
laid out the way the instance's own World draws itself ([Standing in an
instance's hexagon](#standing-in-an-instances-hexagon)). On the shared road strip the walker stays in
the World they came from. A spot on a neighbour's hexagon is not saved as
the World position (`neighbourHexLandAt`, and nothing is saved while the
page stands in a hexagon), and a position saved there before is replaced
by the fresh arrival at the campfire (`leaveRestoredNeighbourHex`), so
opening the World lands in it, at the last spot walked in it. Crossing never
reloads the page; `worldMeshCrossingURL()` still builds
`<world>/world/?from=forkmesh&slot=n&pos=x,z` for a neighbour's own page.
Arriving from such a page,
`?from=<host>&slot=n&pos=x,z` from a listed hexagon places the walker just
outside the edge they crossed, facing away from the petal with the
third-person camera behind them, and the parameters are removed.

## Neighbour scenes

A single-hexagon neighbour may publish what stands
on its hexagon at `GET <world>/api/world/neighbour-scene` (public, CORS,
cacheable). The hub lists that URL as `scene` on hex entries in
`/api/world/mesh`, and its World fetches it without credentials and reads
it with `normalizeNeighbourScene()` (`world-neighbour-scene.js`): version
1, at most 64 KB and 48 objects, types `plaza`, `sign`, `house`,
`terrarium` (up to 32 bugs), `lamp` and `tree`, positions within 330 of
the hexagon centre in World axes, `facing` in radians, `#rrggbb` colours,
plain text, and https links only. Bug icons must sit on the neighbour's own
origin, so a scene cannot make every hub visitor's browser call third-party
hosts; any other icon is dropped and the bug shows its initial. The body is
refused when it is over 64 KB in bytes, by `content-length` or once read.
`buildNeighbourScene()` draws it with shared primitives; a scene that
arrives after its hex layer was replaced is ignored, and when it is missing
or refused the logo hexagon stays. A `circuit` ground gets a tiled canvas of
traces in the accent colour, which glow faintly (`neighbourGroundMaterial`);
`plain` is a flat colour. The terrarium glass does not write depth, or it
hid every bug drawn after it. A house, its label and each bug with an https
`url` are `instance-hexagon` picks, so clicking one opens that link in a new
tab.

## Neighbour scene draw cost

A terrarium's bugs are one draw: their icons
share one canvas atlas and one `forkmesh-instance-hex-bug-board` mesh whose
vertex shader turns each quad to face the camera like a sprite. The board
uses `alphaTest` instead of blending, so overlapping icons need no sorting
and it casts no shadow. Each bug keeps a hidden `Sprite` with
`raycastProxy`, which carries the `neighbour-bug` pick. Unpicked meshes
(glass panes, frames, windows, lamps, trees) are then merged by material
(`mergeMeshesByMaterial` with `skip` for anything interactive), so a 32-bug
scene costs 19 draws (5 transparent) instead of 64. The hex layer is the `instance-hexes` World element
("Neighbour hexagons"), so the profiler names its draws.

## Bug icons through home

An instance's raw-file route sends no CORS
header, so a cross-origin icon cannot be drawn into a WebGL texture and
every bug fell back to its initial. The World loads each icon from its own
server instead: `GET /api/world/neighbour-icon?world=<id>&path=<path>`
(`neighbourIconURL()`, `world_neighbour_icon_handler`). The server answers
only for a hexagon neighbour in its directory that lists a scene (never
itself), builds the URL as that neighbour's `world` origin plus the path
(`neighbour_icon_source()`: starts with one `/`, at most 300 URL-safe
characters, no `#`), fetches it without credentials and with redirects
refused (4 seconds, 64 KB), and relays it only when the bytes are PNG,
JPEG, GIF, WebP or ICO (`icon_type()`; never SVG). The reply carries the
sniffed type, `access-control-allow-origin: *`, `nosniff`, a sandbox CSP
and an hour of public cache; the server keeps up to 96 icons in memory for
an hour, and a failure for a minute.

## Publishing a scene

Every instance World serves that route
(`world_neighbour_scene_handler`, `neighbour_scene()` in `world_mesh.py`)
with `access-control-allow-origin: *` and a 5-minute public cache; the
hub answers 404, since it is never anyone's neighbour. Without settings
it publishes a default scene: a plaza with lamps, a sign with
`INSTANCE_NAME` and the World's host, and a ring of trees at 280 (outside
the landmark at 0.55 of the apothem, inside the 330 reach). An operator
can replace it by putting their own scene JSON in `WORLD_NEIGHBOUR_SCENE`.
The server only checks the outline (64 KB, version 1, 1–48 object
entries) and falls back to the default if the outline is wrong; the hub's
World still checks every field.

## Directory

`GET /api/world/mesh` (`world_mesh.py`) answers
`{self, worlds:[{id, name, host, world, api, logo, slot}]}`. The hub builds
it from `instance_directory` (`world=1`, visible, slot 0–11) plus its own
entry (`id: "forkmesh"`, slot −1). An instance fetches the hub's copy from
`WORLD_MESH_HUB` server-side and caches it (5 minutes, 30 seconds after a
failure). `self` is the answering world, matched by host.

## Drawing

`createInstanceHex()` draws a neighbour as a meadow centre with
its logo, landmark and label, six sand hexes and one road net, all as
shared geometry. The neighbour's roads sit 0.02 below the home roads so the
shared border strip does not z-fight. Clicking the landmark explains how to
walk in; it opens no tab. When the World the page stands in is itself a
single hexagon, every neighbour is drawn as a single hexagon too
(`updateInstanceHexes`): from an instance on hub slot 0 (petal 1) the hub
is one hexagon on the instance's petal 4, mirroring how the hub draws the
instance (a whole flower there would lay its sand petals over the
instance's own town).

## Ownership

`worldMeshOwnerAt()` decides whose ground the player stands
on. The home hexes win the shared road strip; past it, the neighbour's
flower owns the point. The page decides with a margin of 8
(`WORLD_MESH_SWITCH_MARGIN`), so a walker on the strip's midline does not
switch Worlds back and forth; a single hexagon needs the walker on its land
(`onLand`), which already leaves the strip between.

## Watch sockets

`WorldMeshLinks` (`world-mesh.js`) opens
`wss://<api>/api/world/ws?watch=1` to up to four neighbours whose centre is
within 1700 units (released at 1900). The relay accepts a watcher only from
a mesh origin, tags it `watch` and sends a `welcome` snapshot. It then
forwards only `join`, `presence`, `move` and `leave` frames, and pings keep
the socket alive. Watchers are not participants: they get no avatar, are
not counted as online, and have their own cap (`WORLD_MAX_WATCHERS`, 128).
Their peers are shifted by the neighbour's offset and drawn in the space
`world-mesh`, which carries the district radius. They are named
`name@host`, except visitors from this world, who keep their plain name.

## Visits

Stepping onto a neighbour's flower or single hexagon swaps that world's watch
socket for a presence socket. The client sends its profile as
`town-square`, and its moves after subtracting the offset, so the
neighbour's own visitors see it walk in from their edge. Meanwhile the home
socket stays open (chat, Office and the rest are unchanged), but its
presence switches to the space `visiting` and its moves stop. Home viewers
hide `visiting` peers and see the visitor through their own watch socket
instead, so nobody is drawn twice. Walking back reverses the swap. A notice
says which world you entered or returned to.

## Hand-off

The home presence turns `visiting` only once the neighbour's
relay has welcomed the visit socket (`onVisitAdmitted`, `admittedId` in
`WorldMeshLinks`). Until then, and again whenever that socket drops, moves
keep going to the home socket converted back to home's frame
(`homeFrameMovement()`), while the visit link keeps the latest move to send
on open. The watch link's residents stay drawn until the visit's own
welcome replaces them, so the walker is never drawn in neither World while
a visa and socket are pending or refused.

## Standing in an instance's hexagon

The page enters an instance's World as that World draws itself, not as a flower and not as its published scene
alone ([INSTANCES.md](../operations/instances.md#an-instance-world-is-its-own-hexagon)):

- As soon as the hexagon is placed, the page reads the instance's
  `/api/instance` (`neighbourInstance()`: CORS, no credentials, no
  redirects, 6 seconds; the federation client's path list stays as it is)
  and its neighbour scene, and `territoryForNeighbour()` (`world-territory.js`)
  turns them into the same territory settings the instance builds from:
  name, style, accent, houses and the terrarium. The logo is the
  directory's logo, else `/instance-logo.png` through
  `/api/world/neighbour-icon`; the bugs are the scene's terrarium list with
  their icons through the same relay, since the instance's raw-file route
  sends no CORS header. Until this has loaded (or when it fails; it is
  retried after a minute) stepping onto the land stays a visit to the
  published scene.
- `enterMeshWorld()` then calls the scene's `enterTerritoryVisit()`
  (`scene/instance-territory.js`): it builds the hexagon with
  `buildInstanceTerritory()` at the town's centre (or shows the copy built
  ahead, below), moves ForkMesh's
  scenery (`TERRITORY_NEIGHBOUR_ELEMENTS`) next door by
  `forkmeshShiftAround()`, switches off `TERRITORY_HIDDEN_ELEMENTS` (only
  those the visitor had on), walks the hexagon and ForkMesh's tiles, and
  recentres the walker by minus the hexagon's offset. The Members Circle,
  repository portals and other town reads come from the instance
  (`reloadMeshRootData()`), its residents arrive over the visit socket,
  the houses and terrarium are walled and clickable as on the instance,
  and home's peers stay visible next door. ForkMesh's Office stays home's
  (`setOfficeOwner(null)`).
- The hub's single-hexagon layer is cleared while the page stands there
  (`placeMeshHexRoot`), and home is the one neighbour, at the shift the
  territory reports. Stepping onto ForkMesh's ground (its tiles grown to
  the road's midline, `territoryOwnerAt()`) returns home:
  `leaveTerritoryVisit()` moves the scenery back, gives back its clicks,
  turns the hidden elements on again and keeps the built hexagon, hidden,
  for the next visit.
- The hexagon is built before anyone steps in. Once the territory has
  loaded, an idle callback calls `prepareTerritoryVisit()`, which builds it
  and adds it to the World hidden (`userData.groundStandby`), so the GPU
  warm-up compiles its materials and uploads its plates in idle time and the
  crossing frame only shows it (building it then cost a half-second stall).
  A standby hexagon is skipped by
  the ground-height walk, has no clicks, and its terrarium bugs only move
  while it is shown in the scene.

## Standing in another World

A flower neighbour runs the same town, so its real town is this page's town with that World's data. Single-hexagon
neighbours are entered as their own hexagon
([above](#standing-in-an-instances-hexagon)).
`enterMeshWorld()` (`world.js`) moves the page into the flower whose ground
the walker steps onto:

- The scene recentres on it. `shiftPlayer(dx, dz)` (`scene/runtime/camera.js`)
  moves the walker and camera by minus that World's offset, keeping their
  heading, and `placeWorldMesh()` lays the mesh out from that World's
  slot, so the full town stands where its hexagon was and the World left
  behind becomes the neighbour hexagon. The switch is refused inside a
  building, a package district or on a ride; the walker then visits as
  before.
- The town's content is read from that World (`fetchMeshRootJSON`): the
  reads in `MESH_ROOT_READS` (`world-federation.js`: member directory,
  inactive members, arrival stats, organizations, events, fediverse boards,
  network overview, repositories, leaderboards) go to its API through the
  federation client without credentials, and `reloadMeshRootData()` reloads
  them on every switch. Reads in `MESH_ROOT_HIDDEN` (repository imports and
  `/api/repo/...` details, ads, the reward fund, the security board, media
  spaces) answer `not_in_this_world` there, so another World's town never
  shows home's data. Everything else (session, chat, notifications,
  rewards, any write) stays with home. A read that started before a switch
  is dropped (`world_changed`).
- The visit socket to that World is the page's presence there, and its
  residents are drawn with their activity and status like home peers.
  Home's own socket stays open with presence `visiting`; its peers outdoors
  are drawn shifted onto home's hexagon (`homeFramePlayer`), so people on
  both sides of the border stay visible.
- The Office in that town is its owner's (`setOfficeOwner`), entered as a
  visitor ([federation.md](federation.md)).
- The saved position is kept in home's frame, so a reload puts the walker
  back on the same ground and the page switches again.

A notice names the World entered (`Entering <name> · <host>`) or returned
to.

## Visas

A signed-in user's account lives on their
home world only. Before a visit the client asks home for a visa:
`POST /api/world/visa {destination}`. It needs a session and an allowed
origin, and the destination must be a neighbour in the directory. The visa
is an HMAC token over `{name, home, destination, nonce, issuedAt,
expiresAt}`, valid for 60 seconds. It travels as `?visa=` on the socket.
The destination admits the socket only when its `Origin` is a neighbouring
world equal to the visa's `home`. It then posts the visa to
`<home api>/api/world/visa/verify` (4-second timeout, redirects refused).
Home checks the signature, lifetime and destination and returns the name.
The visitor joins as `Registered` under that name, with the visa nonce as
its single-use ticket nonce. A guest travels without a visa and joins as a
guest. Either way the relay sets the public field `home` to the origin's
host. The visitor's account key is namespaced (`name@home`), so it never
displaces a local account with the same name. Visitors get no node belt, no
face image, no wallet and no admin rights. Where the owner supports it, the
visa is exchanged once for a 15-minute visitor token
([federation](federation.md#identity-visa-then-visitor-token)).

## Limits

The page shows at most 32 peers per neighbouring world. Flight
and rides keep `WORLD_RADIUS`, so travel is on foot. Peer coordinates on the
home socket are clamped to ±1900, so people walking the desert ring are not
clamped. Federated relay portals in the fediverse district
are a separate list and still open a tab.
