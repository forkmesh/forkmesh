# Aquarium package

`marketplace/packages/aquarium` (Market id `aquarium`, entry points
`builtin:aquarium` on the desktop and the iPhone, off by default) turns the
whole desktop window into an aquarium while ForkMesh is not the active app,
and lets the same fish swim over the iPhone app (see "On the phone"). Every agent is a fish on the
desktop, its look and behaviour set by its status, changed files and merge
conflicts (a running or waiting one on the phone);
the fish play with the window's own buttons, headings, rows and text boxes, and
with the Genie spider ([../desktop/genie.md](../desktop/genie.md)).

## Why

Issue 1738 (2026-10-07): the user asked that when the desktop is not active
the whole thing becomes an aquarium, with fish for the active agents, 30 ways
for the fish to play with the spider and 30 things they do with the desktop's
elements (looking at a button, text or anything significant), as a plugin
that is on for them. It ships as a package so it can be removed from the rail
or the Market like any other; `install.default` is false, so it is on only
where someone installs it (it was turned on for the requesting user's desktop
by setting `market/packages/aquarium/state=installed` in their settings).

Issue 1782 (2026-10-07): the user asked to show every agent in the tank, even
stopped ones, and for fish that look more real and differ by status and by
other factors such as files and conflicts. Agents that are not running used
to be dropped, so a finished run's fish swam away; now each keeps its fish
and the fish shows what state it is in.

Issue 1803 (2026-10-08): the user found the first click on the desktop did
not register after the tank arrived, asked that a pointer over the window
count as activity so the tank does not start, and that the window can be
moved by its top bar without draining the tank. Show it now's tank swallowed
the click that closed it, and a click on the background tank reached its
widget but the 0.25 s fade kept painting the old picture of the window over
the result, so the click looked lost; see Leaving under Architecture.

Issue 1900 (2026-10-12): the user asked that clicking an agent's fish in the
tank bring them to that agent. A left press on a fish (its body, or the name
tag under it) closes the tank and opens the agent on the Agents page, the same
way the activity feed does (`agentfeed::Host::showAgent`).

## Code map

| Path | Responsibility |
| --- | --- |
| `content/desktop/AquariumScene.{h,cpp}` | The simulation and drawing, with no widgets: fish (moods, looks, skins, `portrait`), the 60 tricks, particles, the water, sand, weeds and chest, and `dirtyRegion()`. Inputs are a picture of the window, a list of landmarks, the swimmers and the spider's sprite; outputs are spider cues and a log line per trick. |
| `content/desktop/AquariumOverlay.{h,cpp}` | `Overlay`: the child of the main window that covers it, takes the picture (`snapshotWindow`), finds the landmarks (`scanLandmarks`), steps the scene at 24 fps, forwards spider cues to the pet feed and fades in and out, and watches input while open to know when to leave. |
| `content/desktop/AquariumFeature.{h,cpp}` | The Aquarium rail page (switch, delay, Show it now, every agent's fish portrait, the trick list, a log of recent tricks) and the director: application state, the wait timer, the agent roster (`roster`, the agent's mood from its run state). |
| `content/desktop/tests/test_aquarium_scene.cpp` | `forkmesh-aquarium-tests`: every trick starts, is logged, finishes and keeps the fish in the tank; spider tricks make the spider react; no spider tricks while it is away; variety; fish leave with their agent; every mood has a fish and only working and finished ones play; a stopped fish sleeps on the sand and a failed one stays low; a stopping agent's fish drops its trick; more files make a bigger fish; each mood, a conflict and changed files change the portrait; a conflicted fish's line is repainted; dirty-region painting matches a full repaint with every mood and decoration.; a click on the tank reaches the button under it and closes the tank, a title-bar press or double-click keeps it, the app coming back drains it unless the title bar was pressed just before or after, and Esc or typing closes it; a click finds the fish under it, and a click on a fish opens its agent, drains the tank and does not reach the widget under it. |
| `desktop/src/AgentActivityFeed.h`, `MainWindowAgentFeed.cpp` | `agentfeed::Agent` carries `files`, `linesAdded`, `linesRemoved` and `conflicted` from the Agents list's cached diff stats (`m_agentDiffStats`), and `merged` and `stopped` from the session. |
| `desktop/src/PetHostFeed.{h,cpp}` | Host socket for the spider: `sprite`, `setSubmerged`, `react`, `walkTo`, `hop`, `lookAt`. |
| `desktop/src/MainWindowGenie.cpp` (`installGeniePetHost`) | Answers the socket from `m_genieSpider`. |
| `desktop/src/GenieSpiderWidget.{h,cpp}` | `setSubmerged`, `frameImage`, `lookAt`, `hop`, `walking`, `airborne`. |
| `desktop/resources/icons/forkmesh/fish.svg` | The rail icon (`fish`). |

## Architecture

**When.** The page connects to `QGuiApplication::applicationStateChanged`.
When the app stops being active it starts a single-shot timer (Start after,
default 3 s, `market/packages/aquarium/data/delaySeconds`); when that fires and
the app is still inactive, the main window is visible and not minimized, the
pointer is not over the window, and the switch is on
(`market/packages/aquarium/data/enabled`, default on), the overlay opens. A
pointer resting on the window counts as activity: the tank waits, and the
pointer leaving the window (`QEvent::Leave` on the main window) starts the
full delay again (issue 1803). It never opens on the `offscreen` or `minimal`
platform or in a window-test build, so headless nodes and window tests never
see it. Show it now opens it while the app is active, and it turns into the
normal background tank if the user switches away.

**Leaving.** The overlay never takes the mouse (`WA_TransparentForMouseEvents`
always, no focus). While the tank is open it watches input with an
application event filter, at the `QWindow` level so each event is seen once:

- a press anywhere other than the main window's title bar
  (`windowChromeBar`, outside its buttons and focusable controls), a key, a
  wheel or a touch closes the tank at once, without the fade, and the event
  goes on to the real widget, so the first click does what it was aimed at;
  Esc is the one key the tank keeps;
- a left press on an agent's fish (`Scene::fishAt`: an ellipse a little larger
  than the body, or the name tag; the front fish wins, fish leaving or nearly
  faded out are skipped) closes the tank and is eaten, so the widget under the
  fish does not get it; on the next turn of the event loop `Overlay::picked`
  hands the fish id to the page, which calls `agentfeed::Host::showAgent` if
  the id is still an agent (the guest fish of an empty Show it now have none)
  (issue 1900);
- a press or double-click on the title bar leaves the tank open, and the bar
  itself moves the window (`QWindow::startSystemMove`) or maximizes it, so
  the window can be moved without draining the tank (issues 1749 and 1803);
- the app becoming active (`Overlay::activated`) drains the background tank
  in 0.25 s after a 400 ms grace, unless the title bar was pressed within the
  last second, before or after the activation: on Wayland the activation and
  the press come in either order. Show it now's tank ignores activation.

**What it draws over.** The overlay is opaque (`WA_OpaquePaintEvent`) and
transparent for the mouse. It paints a picture of the window rather than
letting the live widgets show through, because a translucent child repainted
24 times a second would repaint every widget under it each frame. The picture
is every visible child of the main window except the overlay, rendered with
`QWidget::render`, and is taken again every 8 s and on resize, so agent
output that changed shows up. A click goes straight to the real widget under
it; the tank closes without the fade, because the fade paints that picture
over the window and the click's result stayed hidden behind it.

**The spider.** The spider lives inside the page area, under the overlay. While
the tank is open the overlay calls `setSubmerged(true)`: the spider stops
painting itself (so it is not frozen into the picture) but keeps ticking at 24
fps, and the overlay draws its `frameImage()` at its place every frame. Its
eyes follow the fish nearest to it, or the fish playing with it (`lookAt`,
which wins over the mouse while submerged). A spider out on the desktop stage
is not in the window, so `sprite` answers false and no spider tricks start.

**Landmarks.** `scanLandmarks` walks the visible widgets of the window, clipped
by their parents, and records up to 400: buttons (text, else tooltip),
toggles, tabs (each tab of a `QTabBar`), headings (a label whose object name
says title or heading, or whose font is 1.2 times the application font),
labels (the rect of the text itself, not the whole label), pictures, text
boxes (line edits, editable combos, spin boxes, editable text edits under
200 px), documents (read-only or tall text edits, with the first visible
line), progress bars and sliders, and the visible rows of item views (up to
20, with the first non-empty column's text). It does not descend into item
views or text edits. It is redone with each picture.

**Fish.** One per agent in the agent activity feed
(`agentfeed::host().agents()`, read every 1.5 s while the tank is open),
whatever its state: running, waiting, queued, finished, merged, failed and
stopped agents all swim (issue 1782). Running and waiting ones are listed
first, then queued, failed, finished and stopped, and the tank holds at most
60. The species and colours come from a hash of the name and model
(goldfish, clownfish, angelfish, puffer, tetra, betta). New agents drop in
from the surface with splash rings; a fish leaves with a Bye only when its
agent is deleted. When the run's turns or tokens grow, the fish lets out a
few bubbles. Show it now with no agent at all brings three guest fish.

The agent's run state becomes the fish's mood (`Mood`), and the mood sets how
it looks and swims:

| Mood | Agent | Look | Behaviour |
| --- | --- | --- | --- |
| Working | running | full colour | wanders the whole tank, plays a trick every 2 to 5 s |
| Waiting | waiting for the user | wide eyes, orange "?" | hovers in the top 40%, says "I need you!" |
| Queued | idle, not merged | 30% less colour, 72% opaque | drifts slowly in the middle band, no tricks |
| Done | finished | full colour | swims slower, a trick every 7 to 16 s |
| Merged | finished or idle and merged | purple rim, purple sparkles | as Done |
| Failed | failed | grey, a plaster on its side, a drooping eyelid, frown | lists to one side near the bottom, sighs "Oops…", no tricks |
| Stopped | stopped by the user | greyed, eyes shut | sinks to the sand and sleeps with Zzz, shuffles a little every 12 to 25 s, no tricks |

A change of mood is announced ("Done!", "Merged!", "Oh no…", "Time for a
nap", "Back to work!") and a fish whose agent stops or fails mid-trick drops
the trick. Other factors:

- **Changed files** (`files`, the Agents list's diff count): the fish is
  `1 + 0.09·log2(1 + files)` times longer, at most 1.45, and carries one
  pale freckle per file along its flank, at most 16.
- **Changed lines** (added plus removed): fins and tail grow by
  `0.1·log10(1 + lines)`, at most 40%.
- **Merge conflict** (`conflicted`, the Agents list's `merge-tree` probe of
  the branch against its base): the fish is tangled in red line, has a hook
  in its lip with a fishing line up to the surface, glows red, carries a red
  "!" and now and then tugs and says "Merge conflict!" or "I'm snagged!". It
  says "Untangled!" when the conflict clears.
- The label under the fish is a status dot, the name and the mood, file count
  and "conflict" in the mood's colour.

The page lists every agent's fish (`Scene::portrait`) with its status, files,
lines and conflict in the tooltip, and a tally by mood.

**How a fish is drawn.** Each fish has a skin: a sprite rendered once (when
its mood, files, lines or conflict change, and at the screen's pixel ratio)
with the dorsal, anal and pelvic fins and their rays, a countershaded body
(dark back, pale belly) from a per-species profile, the species pattern,
scales, a dashed lateral line, the freckles, a gill cover, a highlight and the
mood's marks. The tail fin is a second sprite. Each frame the skin is drawn in
four vertical strips, each sheared to follow a travelling wave along the spine
(`flex`), so the body bends as it swims; the tail turns with the slope of the
spine's end. The pectoral fins, the eye (iris colour per species, pupil that
follows the gaze, catchlights) and the mouth are drawn live.

**Tricks.** Between tricks a fish wanders for 2 to 5 s. Then it picks: 45% of
the time a spider trick if the spider is there, no other fish is playing with
it and it has rested 2.5 to 5 s since the last one; otherwise an element trick
that has a fitting landmark. Each family is a shuffled bag, so all 30 are used
before one repeats. A trick swims to its start point (up to 6 s), writes its
line to the page's LATELY log, and runs for its length. Goals are clamped to
the visible tank so a fish never perches off the top edge.

Around the window:

| Trick | Target | What happens |
| --- | --- | --- |
| Peek | button, tab, switch, picture | hovers beside it, eyes on it, then "!?" |
| Boop | button, tab | three nose boops, a ring flashes round it each time |
| Read | heading, label | swims along the top, reading; each letter floats up in a bubble |
| Underline | label, heading | lays a line of bubbles under the text |
| Peekaboo behind | big panel, picture, row | hides behind its edge and peeks out three times |
| Polish | button | rubs it with sparkles, then a gold ring |
| Nibble | label, button | nibbles the corner; crumbs in the corner's own colour sink |
| Mirror | text box | a reflection of the fish appears inside the box; a heart |
| Type | text box | the fish's name appears in the box letter by letter and floats away |
| Bubble tick | switch | draws a check mark of bubbles over it |
| Surf | progress bar, wide row | rides along the top edge with spray |
| Tab hop | 3+ tabs (or buttons) in a row | hops from one to the next |
| Row patrol | 2+ list rows | checks each row with a tick |
| Perch | button, heading | naps on top, Zzz |
| Lantern | heading, label | an angler's lure lights the text as it passes |
| Measure | anything 80–900 px wide | dashed tape along the top, then "N px" |
| Trace | button, picture, box | bubble frame round the perimeter |
| Blow a kiss | heading, picture | puckers, a heart flies to it |
| Count | text up to 24 letters | numbers pop over each letter, then says the total |
| Startle | button | creeps up, "!", dashes off with speed lines |
| Fetch a letter | document, label, row | dives in and comes back with its first letter |
| Bubble ring | small button, switch, picture | circles it with bubbles that pop into sparkles |
| Highlight | label, heading | a marker stroke in the fish's colour |
| Shout | button | says the button's text with "!" |
| Quote | label, heading, row | says the text in quotes |
| Carry | small button, switch, picture | lifts that piece of the window and swims it about, then puts it back |
| Wobble | button | bumps it and the button wobbles |
| Magnify | label, document, row | a lens shows the text 1.9 times larger |
| Squeegee | document, row, picture | wipes the water off it, showing it clear |
| Trampoline | small button | bounces on it; it squashes on each landing |

With the spider (the spider's reaction in brackets; Walk and Hop move it):

| Trick | What happens |
| --- | --- |
| Hello | "Hi, spider!" and a heart (Wave) |
| Nose boop | dash in, boop (Giggle) |
| Tag | "Tag! You're it!" and flees (Surprise, Walk after it, Giggle) |
| Orbit | two laps round it (Dizzy, stars) |
| Bubble crown | five bubbles on the turban (Cheer); they float off after |
| Dance party | swims in figure eights with notes (Dance) |
| Launch | dives under it (Hop, Surprise) |
| Feather tickle | nibbles the feather (Giggle twice) |
| Hide behind | hides behind it and peeks; the spider looks the wrong way (Look, Surprise at "Boo!") |
| High five | fin to foot, stars (Cheer) |
| Race | "Ready, set, go!" along the sand (Walk, Cheer) |
| Pearl | drops a glowing pearl that settles on the sand (Eureka) |
| Serenade | notes fly to the spider (Nod, Nod) |
| Shadow | follows above it across the tank (Walk, Nod) |
| Bubble bath | pours bubbles over it (Giggle, Cheer) |
| Pop out of the sand | sneaks along the floor, "Boo!" with a sand puff (Surprise) |
| Ride | sits on its back, "Giddy up!" (Walk, Cheer) |
| Nap | both doze, Zzz (Nod on waking) |
| Kiss | on the cheek, hearts (Cheer) |
| Play fetch | throws a ball onto the sand (Walk to it, Cheer) |
| Juggle | three bubbles over its head (Eureka) |
| Barrel roll | two spins (Cheer) |
| Ink cloud | gets spooked, ink and a dash (Surprise) |
| Leg count | counts its legs 1 to 8, "Eight legs!" (Nod) |
| Photo | "Say cheese!", a flash, a polaroid of the spider floats up (Wave) |
| Leapfrog | leaps over it (Surprise, Nod) |
| Silk swing | swings on a silk thread from its head (Eureka) |
| Copy dance | copies its moves (Dance) |
| Spotlight | lights it with the lure (Eureka) |
| Balloon | ties a bubble balloon to it, which pops (Hop, Giggle, Surprise) |

## Performance

The water tint, light rays, sand, rocks and chest are baked into one image
with the window picture each time the picture is taken. Each frame repaints
only `Scene::dirtyRegion()`: the rects of every fish (with its label and
speech), every particle, the acting tricks' targets, the spider, the weeds and
the surface band, this frame's and last frame's. Measured offscreen at
2560×1440 with 8 fish: 11 ms a frame with full repaints and drawn rays, 4.4 ms
after baking the rays and painting the dirty region only. With every agent in
the tank (issue 1782) the fish had to get cheaper per fish: on the raster
engine a rotated `drawImage` with antialiasing costs about 110 µs for a 70×40
sprite and 29 µs without it, and a translucent path fill about four times an
opaque one (Qt sends fills of 96 or more spans to its thread pool). So the
fins that bend with the body are baked into the skin, the tail into its own
sprite, both are blitted with antialiasing off (their edges are already
smooth inside the sprite), and each label is a cached image redrawn only when
its text changes. Measured offscreen at 1400×800 with 32 fish of every mood,
full repaints, CPU time: about 10 ms a frame, against about 11 ms for 32 of
the old flat fish and 31 ms for the first version drawn live. The skin is drawn
in four strips, each sheared so the bend is continuous across the joins (a
plain vertical shift per strip left a visible step in the outline). The timer runs at
24 fps and drops to 2 checks a second while the window is minimized or not
exposed; it stops when the tank drains. The spider runs at 24 fps while
submerged (it falls back to 4 fps in the background otherwise).

## Build, run, test

- `forkmesh-aquarium-tests` builds from `build.json` `testSources` (with
  `PetHostFeed.cpp` from `testHostSources`) with `FORKMESH_BUILD_TESTS`. It
  needs only Qt Widgets, so it also builds alone from the repository root:
  `g++ -std=c++20 -fPIC -Imarketplace/packages/aquarium/content/desktop
  -Idesktop/src $(pkg-config --cflags Qt6Widgets)
  marketplace/packages/aquarium/content/desktop/tests/test_aquarium_scene.cpp
  marketplace/packages/aquarium/content/desktop/AquariumScene.cpp
  marketplace/packages/aquarium/content/desktop/AquariumOverlay.cpp
  desktop/src/PetHostFeed.cpp $(pkg-config --libs Qt6Widgets)`, run with
  `QT_QPA_PLATFORM=offscreen`. The overlay checks send mouse and key events
  to the main window's `QWindow`, the way the platform delivers them, so they
  go through the same event filter and `childAt` routing as a real click.
- To look at it: open the Aquarium page and press Show it now.
- `preview/market.png` was rendered offscreen by running `Overlay` over a
  sample Agents window with a stand-in spider host drawn by
  `GenieSpiderPainter`.

## Traps

- `emit` is a Qt macro; the scene's particle helper is called `puff`.
- A trick's paint state (lens position, lifted piece, silk pivot) is set by its
  step function, so the first perform step runs in the same frame the trick
  starts. Drawing before it smeared the magnifier's handle from (0, 0) across
  the window, and the dirty-region test caught the stray pixels.
- The water picture must not contain the spider, or a frozen copy stays where
  it stood; that is what `setSubmerged` is for.
- The overlay raises itself above new children of the main window (popups and
  dialogs raise themselves) once a frame.

## On the phone

**Why.** Issue 1750 (2026-10-07): the user asked to add the fish tank to the
mobile app when it is enabled as a plugin and let it interact with things,
and that neither the fish nor the spider cover any logo or button. The
package went to 1.2.0 with an `ios` entry point, a phone permission
(`ui.overlay.phone`) and a summary that names both devices.

**When it shows.** Only when the package is on for this phone: the Market
page's "Installed on this device" switch (`PhonePackages.aquarium`, saved as
`packages/aquarium/on`, off by default because `install.default` is false).
Turning it on for the desktop does not turn it on for the phone. Until it is
on, `MarketModel.isInstalled` reads false, so the Market lists it under Not
installed. The phone tags it "On this phone" (`nativeDestinations`) with no
Open link. While on, the fish swim while the app is active and the composer
or keyboard is not up, the same rule as the spider.

**What it draws.** `AquariumLayer` is mounted in `HomeShell` right under the
spider's layer, so the spider, its bubble and its menu stay on top. It takes
no touches and is hidden from VoiceOver. There is no water: a 3.5% blue tint,
a few rising bubbles, the fish, their name tags and what they do. Speech is
a small white bubble.

**Fish.** One per running or waiting agent from the Agents list
(`AgentsScreen` puts them in `ShellCounts.swimmers`: title as the name, the
watch face name, else the provider, as the model), at most 12. Species
(goldfish, clownfish, angelfish, puffer, tetra, betta), colours and size come
from an FNV-1a hash of name and model (`AquariumLook.of`). With no agent,
three guests (Bubbles, Finn, Coral) keep the tank from being empty, as the
desktop's Show it now does. New fish drop in from the top with a splash;
finished ones say Bye and swim out. A waiting fish hovers in the top band
with an orange "?" and says "I need you!" every 9 to 15 s, and plays no
tricks.

**Tricks.** Between tricks a fish rests 2 to 5 s at a clear spot. Then, 45%
of the time when the spider is free (shown, not held or tumbling, nothing
holding it still, no busy session, hand play off, not doing its own screen
play) and no other fish is playing with it, and 2.5 to 5 s after the last
spider trick, it picks a spider trick; otherwise an element trick that has a
fitting landmark not taken by another fish. Each family is a shuffled bag
(`GenieBag`). A trick swims to its spot (up to 6 s) and runs for its length.

| Trick | Target | What happens |
| --- | --- | --- |
| Peek | button, tab, switch, picture, link | hovers beside it, then "!?" |
| Boop | button, tab, link | three nudges towards it, a ring flashes round it each time |
| Read | heading, text | each letter floats up from it in a bubble |
| Underline | text, heading | a dotted line of bubbles under it |
| Polish | button | sparkles round its edge, then a gold ring |
| Bubble ring | small button, switch, picture | ten bubbles circle it and pop into sparkles |
| Measure | anything 60 to 900 pt wide | a dashed tape along its top, then "N points!" |
| Highlight | text, heading | a faint marker band in the fish's colour |
| Quote | text, heading, button | says its text in quotes |
| Count | up to 24 letters | numbers pop over each letter, then "N letters!" |
| Startle | button, link | creeps closer, "!", dashes off with speed lines |
| Perch beside | button, heading | naps beside it, Zzz |
| Trace | button, picture, text box | a dotted frame drawn round it |

With the spider (its reaction in brackets, through `GenieCues`; Walk is
`GenieCrawler.stroll`, Look turns its eyes to the fish):

| Trick | What happens |
| --- | --- |
| Hello | "Hi, spider!" and a heart (Look, Wave) |
| Nose boop | dashes in and boops it (Look, Giggle) |
| Tag | "Tag! You're it!" and flees (Surprise, Walk after it, Giggle) |
| Orbit | two laps round it (Look, Dizzy) |
| Bubble crown | five bubbles over its turban (Cheer) |
| High five | fin to foot, stars (Cheer) |
| Race | "Ready, set, go!" to the far side (Walk, Cheer) |
| Serenade | notes fly to it (Nod, Nod) |
| Leg count | 1 to 8, "Eight legs!" (Nod) |
| Photo | "Say cheese!", a flash, a polaroid floats up (Wave) |
| Kiss | hearts (Cheer) |

The spider only walks when it is not tucked away, wandering is on, nothing
holds it still, hand play is off, and Reduce Motion and VoiceOver are off.

**Never covering a button or logo.** Landmarks come from the spider's screen
scanner (`GenieScreenScan`, refreshed every 2 s while the tank runs and on
tab or page change; see [../native/GENIE.md](../native/GENIE.md)). A fish's
box covers its body, fins, name tag and, when waiting, its "?". Every place a
fish rests or performs (its arrival point, wander waypoints, the spot beside
an element or the spider) is chosen so that box, grown by 8 pt, touches no
button, link, tab, text box, switch, slider or picture, and does not touch
the element being played with either (`AquariumClear.spot`, `waypoint`).
Performing nudges stay within that margin. A speech bubble goes above,
right, left or below the fish, whichever is clear, and is not shown when none
is. A fish that has to swim across a button fades to 20% while it overlaps
(`AquariumClear.opacity`). The tests check every frame of long runs.

**Performance.** `AquariumTank` is a value stepped once per frame by
`AquariumDirector` inside a `TimelineView` at 30 fps (a step is capped at
0.1 s) and drawn with one `Canvas`. The timeline pauses when the package is
off, the composer or keyboard is up or the app is not active, and the tank
is dropped when the package is turned off. At most 12 fish and 160
particles. With Reduce Motion the fish swim at 40% speed and there are no
particles (no bubbles, letters or sparkles; Read says the heading instead).

### Code map (phone)

| Path | Responsibility |
| --- | --- |
| `native/ios/ForkMeshCore/Sources/FMAssistant/AquariumFish.swift` | Swimmers, species and looks, the 24 tricks and what they fit, particles, marks, a fish's box, the clear-spot rules (`AquariumClear`). |
| `native/ios/ForkMeshCore/Sources/FMAssistant/AquariumTank.swift` | `AquariumTank`: roster, wandering, choosing and playing tricks, spider asks, fading, speech placement, particles. Pure and tested on Linux. |
| `native/ios/ForkMeshCore/Sources/FMAssistant/GenieLandmarks.swift` | Landmark kinds and tidying, shared with the spider; `GenieBag`. |
| `native/ios/ForkMesh/Features/Aquarium/AquariumLayer.swift` | `AquariumDirector` (steps the tank, feeds it landmarks and the spider, plays the spider's reactions), `AquariumLayer`, `AquariumPainter`. |
| `native/ios/ForkMesh/Features/Genie/GenieScreenScan.swift` | The landmark scanner. |
| `native/ios/ForkMeshCore/Sources/FMPackages/PhonePackages.swift`, `FMAccount/SettingsService.swift` | The per-phone switch. |
| `native/ios/ForkMesh/Features/Agents/AgentsScreen.swift`, `Shell/ShellBottomBar.swift` | `ShellCounts.swimmers`. |
| `native/ios/ForkMesh/Shell/HomeShell.swift` | Mounts the layer under the spider; marks the scan stale on tab and page changes. |

### Tests

- `AquariumTests` (FMAssistant): looks come from name and model and cover
  every species; three guests with no agent; one fish per agent, at most 12,
  leaving with a Bye; waiting fish stay in the top band and call; over four
  seeds of 400 s each, every element trick plays, no performing or resting
  fish, nor any speech bubble, overlaps a button, a performing fish never
  overlaps its target, a fish overlapping a button for more than 0.6 s is
  under 30% opacity; every spider trick plays and asks for Wave, Cheer,
  Giggle, Surprise, Dizzy, Nod, Walk and Look, one at a time; no spider
  tricks while the spider is busy or away; Reduce Motion has no particles and
  swims at most 40% speed.
- `MarketTests` (FMPackages): the switch starts off, turns on and off, and
  `isInstalled` follows it; the package is "On this phone" with no route.
- Run: `FM_ONLY=FMAssistant,FMPackages swift test --filter
  "AquariumTests|GenieScreenPlayTests|MarketTests"` from
  `native/ios/ForkMeshCore`.
- Not verified: the SwiftUI layer, the painter and the scanner were only
  parsed on Linux (`swiftc -parse`), not compiled for iOS or run on a phone.
  Check on the next Codemagic `ios-native-build` and on a phone that the fish
  read well over light and dark pages, that the frame rate holds with 12 fish,
  and that the scanner finds SwiftUI buttons.

## Recent changes

- 2026-10-07: added (issue 1738).
- 2026-10-07 (1.1.0): the window can be dragged by its top bar while Show it
  now has the tank open (issue 1749).
- 2026-10-07 (1.2.0): the fish swim over the iPhone app when the package is on
  for that phone, play with the screen and the spider, and keep off buttons
  and logos (issue 1750).
- 2026-10-07 (1.3.0): on the desktop every agent swims, stopped and finished
  ones too; the fish are drawn with bending bodies, fins with rays, scales and
  real eyes, and look and behave by status, changed files, changed lines and
  merge conflicts; the page shows each agent's fish (issue 1782). The phone
  still shows running and waiting agents only.
- 2026-10-08 (1.3.1): the first click on the tank reaches the widget under it
  and closes the tank at once; the tank does not start while the pointer is
  over the window; the window can be moved by its top bar while the
  background tank is open (issue 1803).
- 2026-10-12 (1.4.0): clicking an agent's fish closes the tank and opens that
  agent (issue 1900).
