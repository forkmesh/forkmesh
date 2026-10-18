# Spider Room package

`marketplace/packages/spider-room` (Market id `spider-room`, entry point
`builtin:spider-room` on the iPhone, off by default, `places: ["mobile"]`)
brings the Genie spider ([../native/GENIE.md](../native/GENIE.md)) into the
room through the camera. Installed, it adds **Spider Room** to the home grid
(Personal) and the package's Market page opens it. Deep links:
`forkmesh://spider-room`, `/dashboard/spider-room`.

## Code map

| Path | Responsibility |
| --- | --- |
| `native/ios/ForkMeshCore/Sources/FMAssistant/RoomSpiderSpace.swift` | `RoomMath` (vector helpers on `SIMD3<Float>`), `RoomSurface` (a detected plane as a rectangle), `RoomMap` (all planes, the floor, hit tests, neighbours, `summary`) |
| `.../FMAssistant/RoomSpider.swift` | `RoomSpider`: the simulation. Flight, landing, crawling, climbing between surfaces, leaps, silk, plans, holding and throwing |
| `.../FMAssistant/RoomSpiderActs.swift` | `RoomSpiderAct` (reactions and their lines), `RoomWeb`, `RoomDice` (seeded random) |
| `.../Tests/FMAssistantTests/RoomSpiderTests.swift` | `RoomSpiderTests` |
| `native/ios/ForkMesh/Features/SpiderRoom/SpiderRoomScreen.swift` | The page: camera permission, hints, Call the spider, Clear webs; `SpiderRoomView` hosts the `ARView` |
| `.../SpiderRoom/SpiderRoomDirector.swift` | `ARSession` (planes, tracking state), the per-frame step, gestures, haptics, the speech bubble; `SpiderRoomStage` is what the page observes |
| `.../SpiderRoom/SpiderRoomBody.swift` | The 3D spider built from RealityKit primitives (no assets) and its leg poses; `SpiderRoomSilk` draws the thread and webs |

Route wiring is the usual phone package set: `Route.spiderRoom`,
`RouteParser`, `RouteCatalog` (title, `arkit` symbol, `HubCatalog.personal`),
`PackageGate.items` / `kinds`, `MarketPackage.nativeDestinations`,
`PackageRoutes`, `PluginMenu.homeKinds` and `RouteFallbacks`.

## How the room is seen

- `ARWorldTrackingConfiguration` with horizontal and vertical plane detection.
  Each `ARPlaneAnchor` becomes a `RoomSurface`: the bounding box of its
  boundary polygon in the anchor's own x/z axes, its y axis as the normal, and
  its classification as the kind (floor, wall, ceiling, table, seat, window,
  door; anything else is `other`, or `wall` when vertical). Without
  classification (older phones) vertical planes are walls.
- **Which side is the front.** A new plane's normal is turned to face the
  phone; ceilings always face down and floors up. A refined plane keeps the
  side it first had, so walking behind a cabinet does not flip it.
- **The floor** is an unbounded surface at the lowest floor plane, else the
  lowest upright plane at least 0.9 m under the phone, else 1.35 m under the
  phone. The spider can never fall through the room.
- **Walls reach the floor.** ARKit rarely finds the bottom of a wall, so a
  vertical surface's rectangle is extended down to the floor height
  (`RoomSurface.covers`). Spiders on the floor run into walls and walls end on
  the floor.
- On phones that can rebuild the scene mesh (LiDAR), `sceneReconstruction` is
  `.mesh` and RealityKit occlusion is on, so real furniture hides the spider.
  The mesh is not used for the simulation.

## How the spider moves

All positions are the body centre, `RoomSpider.stand` (4.5 cm) off the surface.
`RoomSpider.step` runs every RealityKit frame (`SceneEvents.Update`).

- **Arriving.** Once a floor is found, or after 4 s of tracking, it lowers
  itself on silk from just above eye height 0.7 m in front of the phone and
  says hello. **Call the spider** repeats it wherever you are looking.
- **Holding and throwing.** A pan that starts within 60 pt (or 1.4 spider
  sizes) of the spider picks it up at its current distance (0.25 to 1.5 m)
  and follows the finger's ray. The release velocity is taken over the last
  0.1 s and capped at 7 m/s. Let go slowly (under 0.3 m/s) next to a surface
  and it holds on there.
- **Flight** at 120 Hz: 9.8 m/s² down and light drag. A step that crosses a
  surface from its front, inside its rectangle, lands. Floors and table tops
  bounce it when it hits faster than 1.8 m/s into them (35% kept, at most
  three bounces); walls and ceilings always catch it. Landing faster than
  3.6 m/s leaves it dizzy; a good throw sometimes earns "Again! Again!".
  After 6 s in the air it is put on the floor.
- **Crawling** at 22 cm/s, 30% slower climbing up a wall, 20% faster down.
  It turns at most 4.5 rad/s toward where it is going.
  - Walking into another surface ahead (a wall from the floor, the ceiling or
    a table top from a wall) moves it onto that surface, turning its heading
    by the same rotation as its feet (`RoomMath.rotate`), so walking at a wall
    becomes walking up it. Only surfaces it is in front of count.
  - At the edge of a surface it steps onto a neighbouring plane that touches
    the edge. With none, from a table top or heading down it lowers itself on
    silk (35 cm/s) to the surface below; climbing towards a ceiling it leaps
    up; otherwise it turns round.
  - It leaps (a ballistic hop of 0.35 to 0.9 s) onto a higher table or seat
    within 0.8 m, or from a wall onto furniture within 1 m.
- **Plans.** Left alone it picks a goal every few seconds: on the floor a wall
  spot, a piece of furniture, a spot near your feet, or a wave; on a wall a
  web, another spot, the ceiling, or back down; on furniture a spot or back
  to the floor; on the ceiling a spot or a silk drop. A goal it cannot reach
  in 14 s (28 s for one you tapped) is dropped. Coplanar planes (a floor
  plane and the unbounded floor) count as the same surface.
- **Tapping** a detected plane sends it there ("On my way!"); a tap on an
  estimated horizontal plane near the floor counts as the floor. Tapping the
  spider tickles it.

## Reacting to the room

The first time it reaches a surface, and 30% of later times, it reacts to
what that surface is (`RoomSpiderAct.arriving`): naps on seats, dances on
tables, peeks out of windows, knocks on doors, hangs from the ceiling, calls
out "Wall climbing!" on walls and sniffs other furniture. On a wall it sometimes spins a web (16 to
26 cm, at most six, not within 35 cm of another); **Clear webs** removes
them. Every act has a line shown in a bubble above the spider (a UIKit view
inside the `ARView`, so SwiftUI is not redrawn each frame) and announced to
VoiceOver. Landings play a medium haptic, picking it up and tickling a light
one.

While the page is open the 2D spider over the app is hidden (`HomeShell`
passes `router.currentRoute?.kind == .spiderRoom` to `GenieSpiderLayer`), so
there is only one spider.

## Privacy

The camera permission prompt (`NSCameraUsageDescription`) names Spider Room.
Frames go only to ARKit on the phone; nothing is recorded, saved or sent, and
the package keeps no data (`dataLifecycle.scope: none`). The session pauses
when the page closes.

## Build, run, test

- `FM_ONLY=FMAssistant swift test --filter RoomSpiderTests` from
  `native/ios/ForkMeshCore` runs the simulation on Linux: orientation,
  throws onto walls, bounces, dizziness, gentle placing, tapped wall, table,
  floor and ceiling trips, silk off a table, lost planes, and a four-minute
  random walk per seed that must never sink through the floor.
- Route wiring: `PackageRoutesTests`, `RouteParserTests`.
- The app files are parsed by `native/ios/tools/check_swift_syntax.sh`; ARKit
  and RealityKit compile only in the Mac/Codemagic build, and AR needs a real
  iPhone (the simulator has no camera tracking).

## Traps

- ARKit merges planes: a removed anchor drops a spider standing on it to
  whatever is below (`crawl` falls when its surface is gone).
- ARKit delegate callbacks arrive on the main queue (no `delegateQueue`), which
  `MainActor.assumeIsolated` relies on.
- Android has no Spider Room yet. Its `NATIVE_DESTINATIONS` has no
  `spider-room`, so Android's Market lists the package with no phone page to
  open (it is filtered out of the "This phone" list).
