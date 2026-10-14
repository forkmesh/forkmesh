# Aquarium package

`marketplace/packages/aquarium` (Market id `aquarium`, entry points
`builtin:aquarium` on the desktop and the iPhone, off by default) turns the
desktop window into an aquarium while ForkMesh is not the active app, and lets
the same fish swim over the iPhone app. Every agent is a fish whose look and
behaviour follow its status, changed files and merge conflicts; the fish play
with the window's own buttons, headings, rows and text boxes, and with the
Genie spider ([../desktop/genie.md](../desktop/genie.md)).

## Code map

| Path | Responsibility |
| --- | --- |
| `content/desktop/AquariumScene.{h,cpp}` | Simulation and drawing, no widgets: fish (moods, looks, skins, `portrait`), the tricks, particles, water, sand, weeds, `dirtyRegion` |
| `content/desktop/AquariumOverlay.{h,cpp}` | `Overlay`: covers the main window, takes its picture (`snapshotWindow`), finds landmarks (`scanLandmarks`), steps the scene, routes input |
| `content/desktop/AquariumFeature.{h,cpp}` | The rail page (switch, delay, Show it now, fish portraits, trick list, recent tricks) and the director |
| `content/desktop/tests/test_aquarium_scene.cpp` | `forkmesh-aquarium-tests` |
| `desktop/src/AgentActivityFeed.h`, `MainWindowAgentFeed.cpp` | `agentfeed::Agent` with `files`, `linesAdded`, `linesRemoved`, `conflicted` |
| `desktop/src/PetHostFeed.{h,cpp}`, `MainWindowGenie.cpp` | Host socket for the spider: `sprite`, `setSubmerged`, `react`, `walkTo`, `hop`, `lookAt` |

## Architecture

- **When.** On `QGuiApplication::applicationStateChanged` to inactive, a
  single-shot timer (default 3 s, `market/packages/aquarium/data/delaySeconds`)
  opens the overlay if the app is still inactive, the window is visible and
  the pointer is not over it, and the switch
  (`market/packages/aquarium/data/enabled`) is on. A pointer leaving the
  window restarts the delay. Never on the `offscreen` or `minimal` platform or
  in window-test builds. Show it now opens it while active.
- **Leaving.** The overlay is `WA_TransparentForMouseEvents` with no focus;
  an application event filter at the `QWindow` level closes the tank on any
  press outside the title bar, key (Esc excepted), wheel or touch, without
  the fade, and lets the event reach the real widget. A left press on a fish
  (`Scene::fishAt`) closes the tank, is eaten, and opens that agent
  (`agentfeed::Host::showAgent`). Title bar presses keep the tank open and
  move or maximize the window. App activation drains the background tank in
  0.25 s after a 400 ms grace.
- **What it draws over.** An opaque overlay painting a picture of the window
  (`QWidget::render` of every visible child), retaken every 8 s and on
  resize, so live widgets are not repainted every frame.
- **The spider.** While the tank is open `setSubmerged(true)` stops the
  spider painting itself; the overlay draws its `frameImage()` each frame.
- **Landmarks.** `scanLandmarks` records up to 400 visible widgets clipped by
  their parents: buttons, toggles, tabs, headings, text rects of labels,
  pictures, text boxes, documents, progress bars and sliders, and up to 20
  visible rows of item views.
- **Fish.** One per agent in the activity feed (read every 1.5 s), any state,
  at most 60, active ones first. Species and colours come from a hash of name
  and model. The run state is the fish's mood:

  | Mood | Agent | Behaviour |
  | --- | --- | --- |
  | Working | running | wanders, a trick every 2 to 5 s |
  | Waiting | waiting for the user | hovers high, orange "?", "I need you!" |
  | Queued | idle, not merged | faded, drifts, no tricks |
  | Done | finished | slower, a trick every 7 to 16 s |
  | Merged | merged | purple rim and sparkles |
  | Failed | failed | grey, lists near the bottom, no tricks |
  | Stopped | stopped | sleeps on the sand |

  Changed files lengthen the fish (`1 + 0.09·log2(1 + files)`, at most 1.45)
  and add freckles; changed lines grow fins (at most 40%); a merge conflict
  tangles it in red line with a hook. Each fish has a cached skin sprite drawn
  in four sheared strips that follow the spine's wave.
- **Tricks.** 30 tricks with window landmarks (peek, boop, read, underline,
  polish, measure, trace, carry, magnify, …) and 30 with the spider (hello,
  tag, orbit, race, photo, …, each with a spider reaction). Each family is a
  shuffled bag; a fish picks a spider trick 45% of the time when the spider
  is free. Goals are clamped to the visible tank.

## Performance

Water tint, rays, sand and props are baked with the window picture; each
frame repaints only `Scene::dirtyRegion()`. Fins and tail are baked into
sprites blitted with antialiasing off, and labels are cached images, because
on the raster engine rotated antialiased `drawImage` and translucent path
fills are expensive. The timer runs at 24 fps, drops to 2 checks a second
while the window is minimized or not exposed, and stops when the tank drains.

## Build, run, test

`forkmesh-aquarium-tests` builds from `build.json` `testSources` (with
`PetHostFeed.cpp` from `testHostSources`) and needs only Qt Widgets; run with
`QT_QPA_PLATFORM=offscreen`. The overlay checks send events to the main
window's `QWindow`, as the platform does. To look at it, open the Aquarium
page and press Show it now.

## Traps

- `emit` is a Qt macro; the particle helper is called `puff`.
- A trick's paint state is set by its step function, so the first step runs
  in the frame the trick starts.
- The water picture must not contain the spider (`setSubmerged`).
- The overlay raises itself above new children of the main window once a
  frame.

## On the phone

- **When.** Only when the per-phone switch is on (`PhonePackages.aquarium`,
  `packages/aquarium/on`, off by default), while the app is active and the
  composer or keyboard is not up.
- **What.** `AquariumLayer` is mounted in `HomeShell` under the spider's
  layer, takes no touches and is hidden from VoiceOver: a light tint,
  bubbles, up to 12 fish for running or waiting agents (three guests when
  there are none), name tags and speech bubbles.
- **Never covering a button or logo.** Landmarks come from the spider's
  scanner (`GenieScreenScan`, every 2 s). Every resting or performing spot is
  chosen so the fish's box, grown by 8 pt, touches no interactive element or
  picture (`AquariumClear.spot`, `waypoint`); a fish crossing a button fades
  to 20% (`AquariumClear.opacity`).
- **Performance.** `AquariumTank` is a pure value stepped by
  `AquariumDirector` in a `TimelineView` at 30 fps and drawn with one
  `Canvas`; at most 160 particles. Reduce Motion slows fish to 40% and drops
  particles.
- **Code:** `native/ios/ForkMeshCore/Sources/FMAssistant/AquariumFish.swift`,
  `AquariumTank.swift`, `GenieLandmarks.swift`;
  `native/ios/ForkMesh/Features/Aquarium/AquariumLayer.swift`.
- **Tests:** `AquariumTests` and `MarketTests`:
  `FM_ONLY=FMAssistant,FMPackages swift test --filter "AquariumTests|GenieScreenPlayTests|MarketTests"`
  from `native/ios/ForkMeshCore`.
