# Desktop Genie spider and marketing coach

Part of [the desktop docs](README.md). Added 2026-10-06 (issue 1465). The user
asked for the phone's Genie spider ([../native/GENIE.md](../native/GENIE.md))
on the desktop too, as a Tamagotchi-style pet whose health follows code
velocity and marketing velocity, and which coaches the user on how to market
the project.

The desktop spider is a pet and a coach, not a voice assistant: it has no
speech, model or tools of its own. Replies to it go to the Genie special
agent, which runs on a low-cost model (see "Replying to the spider"). It shares the phone spider's look; its marketing score
is stricter than the phone's since issue 1480 (see "Health"). The web pages
have a third copy, a guide with tips rather than a pet
([../www/genie-spider.md](../www/genie-spider.md)).

## Where the code is

| Part | File | Tests |
| --- | --- | --- |
| Watch list, health, CRM counts and digest, Marketing digest, shipped changes, visit log, README check, marketing coach and its suggestions, taught lessons, the log of what it said and the Genie agent briefing | `desktop/src/GenieVitals.{h,cpp}` | `forkmesh-genie-vitals-tests` (`tests/test_genie_vitals.cpp`) |
| The Genie special agent the Reply button talks to | `.forkmesh/teammates/genie.json` | `app/tests/test_genie_spider_special_agent.py` |
| The spider drawn with QPainter (a port of `GeniePainter` in `GenieAvatar.swift`), and the 30 genie effects (`GenieMagic`, `paintMagic`) | `desktop/src/GenieSpiderPainter.{h,cpp}` | none |
| The crawling spider, the tip bubble, the coach panel, the desktop stage, the interactions' scripts, the silk thread and the effects layer | `desktop/src/GenieSpiderWidget.{h,cpp}` | none |
| Landmark scan of the page area, keep-clear spot, interaction plans and the shuffled bag | `desktop/src/GenieLandmarks.{h,cpp}` | `forkmesh-genie-landmarks-tests` (`tests/test_genie_landmarks.cpp`, offscreen) |
| Wiring: measuring, navigation, menu, tips, settings, the watch checkboxes, the pet socket for packages | `desktop/src/MainWindowGenie.cpp`, `desktop/src/PetHostFeed.{h,cpp}` | none |
| The What the spider said section of the Genie special agent's page | `fillTeammateHomeSaid` in `desktop/src/MainWindowTeammateHome.cpp` | none |
| The GENIE SPIDER section of Settings, General (Show the Genie spider, the watch topics, replies) | `buildSettingsGeneralTab` in `desktop/src/MainWindowSettings.cpp`, `buildGenieWatchSettings` in `desktop/src/MainWindowGenie.cpp` | none |

## The spider

`GenieSpiderWidget` is a 78 px spider (`kSide`) in a 94 px child of the page
area (`m_globalOverlayHost`), padded by 8 px (`kPad`) for its glow; `bodyRect()`
is the spider without the padding and anchors the bubble, panel and menu. It
shares its parent with the agent composer. `positionGenieSpider` runs at the
end of `positionGlobalFooterOverlays`, so it follows every resize and composer
move.
It is created after the first paint, together with the composer
(`initializeDeferredGlobalLogDock`). Like the Sponsored strip, it is never
created in a window-test build or on the `offscreen` or `minimal` platform, so
it neither catches test clicks nor reads the CRM on a headless node.

- It stands on the bottom of the page area. Where the composer's visible parts
  (the footer dock's mask) or a floating composer are under it, it stands on
  top of them instead, climbs up when one appears under it and falls off an
  edge with gravity.
- It wanders: every 5 to 13 s (slower when unwell) it walks up to 260 px from
  where it is, staying near its home spot. Its walking speed also follows
  health.
- Its eyes follow the mouse pointer when it is within 480 px.
- Drag it anywhere and let go: it is thrown with the pointer's velocity
  (measured over the last 80 ms of the drag, including the release position),
  falls to the floor, lands with a squash and giggles if it hit the floor
  fast. Where it comes to rest becomes its home spot (`genie/spot`, a fraction
  of the width).
- Hovering perks it up (see "Look, feet and hover" below); a click waves and
  says one short line in a bubble (see "One line on click" below); the
  tooltip is the health summary. It never speaks unasked.
  Pressing and holding it for 0.55 s without moving opens the right-click menu,
  for touchscreens and one-button trackpads.
- It plays reactions from the phone (wave, cheer, nod, eureka, giggle,
  surprise): a nod on every page change, eureka on a marketing page visit or a
  tip, a cheer when a playbook step is ticked or the health level rises, and
  surprise when it falls.
- The frame timer runs at 60 fps in flight, 24 fps while it walks, reacts or
  is hovered, 15 fps while the pointer is within 200 px, 8 fps otherwise and
  4 fps when the app is not the active one (8 fps out on the desktop). It
  stops while the spider is hidden and drops to 2 checks a second, drawing
  nothing, while its window is minimized or not exposed.

Right-click or long-press menu: How are you feeling? (the summary in a bubble), Marketing
coach, Measure again, Watch (the topics below, ticked on and off), Let out onto the desktop / Come back inside (see "Out on
the desktop"), Stay put (stops wandering, `genie/wanders`) and Tuck away /
Come back (it walks into the bottom-right corner and only peeks out,
`genie/parked`; a click brings it back) and Hide the spider (see "Hiding it
completely").

## Hiding it completely

Issue 1831 (2026-10-09): the user asked to make the spider disappear
completely, from its long-press menu and from Settings.

Hide the spider, the last item of the right-click or long-press menu, and the
"Show the Genie spider" checkbox at the top of the GENIE SPIDER section of
Settings, General, both go through `MainWindow::setGenieSpiderShown` and save
`genie/shown` (on by default). Hiding deletes the spider, its tip bubble, its
coach panel and the desktop stage and stops the minute measuring timer, so
nothing is drawn, no frames or wandering run and no CRM or Marketing reads are
made; `setupGenieSpider` creates none of them at start while it is off.
Ticking the box again builds the spider afresh; it comes back where it last
stood, out on the desktop if it was there. The menu item acts after
`QMenu::exec` returns and the parts are removed with `deleteLater`, because
the menu is opened from inside the spider's own event handlers. The pet
socket stays installed and answers "no spider", so the Aquarium carries on
without it. The watch topics, lessons and the Genie special agent are kept.

## Dances

Issue 1723: the user asked for the spider to dance when an agent is started or
a branch is merged, with 10 different dances.

`MainWindow::danceGenie` calls `GenieSpiderWidget::dance`, which picks one of
the 10 at random, never the same one twice in a row, and plays it as the
`Dance` reaction for 3.6 s. It is called when:

- an agent starts: `startAgentForIssue`, `startAdHocAgentForRepo` (prompts,
  the Genie run and remote starts), `startAgentOnExistingBranch`, the pull
  request review, fix-findings and fix-conflicts agents, and the branch
  conflict fix;
- a branch is merged: a worktree merge that lands (`applyWorktreeMergeStep`),
  a pull request merge (`afterPullMerged`), a merge made from the web or phone
  (`recordWebBranchMerges`), and a merge noticed later by the landed sweep
  (`markAgentSessionsLanded`).

A dance already playing is not restarted, so one merge reported by two of
these paths dances once. While it dances the spider stands still (a walk
waits until the dance ends), and the incidental Nod, Eureka and Cheer (page
changes, tips, playbook ticks) do not cut it short; a click, a long press or
a throw still do.

All dances run at 2 beats a second, ease in and out over the reaction, add a
bigger grin and a feather that swings to the beat, and float music notes
around the head. `GenieSpiderPainter` draws them; the moves are in
`danceBob`, `danceSway`, `danceStep` (feet, in ground space), `danceArm`
(front legs, in body space) and `danceLook`:

| Dance | Moves |
| --- | --- |
| Disco | right front leg points up and then down across, left on the hip, lean with the point |
| Twist | squats low and twists from side to side, feet swivel, front legs swing |
| Jig | hops off the floor on every beat, feet splayed |
| Ripple | the legs lift one after another, front to back, left then right, like a stadium wave |
| Spin | arms overhead, on tiptoes, two full pirouettes |
| Jazz hands | both front pairs up and out, wiggling |
| Moonwalk | leans back, feet slide backwards, glides away and back, one leg tips the turban |
| Headbang | eyes shut, sharp down-beats, feather whips, air guitar |
| Robot | jerky steps 4 times a second: tilts, square arms that flip up and down, eyes snap side to side |
| Cancan | front legs linked on the hips, the middle legs kick high, side to side |

## Playing with the window and keeping clear

Issue 1750, 2026-10-07: the user asked for the spider to interact with things
in the window, on the desktop as on the phone and the web, and never to cover
a logo or a button.

### What it sees

`GenieLandmarks::scan` walks the page area (`m_globalOverlayHost`) and its
children, clipped to each parent, skipping hidden widgets, windows, the
spider, its tip bubble and the coach panel. It returns, in the page area's
coordinates:

- landmarks: buttons and tool buttons, tabs (one per tab, grouped by tab bar),
  headings (a title or heading object name, or a font at least 1.2 times the
  app's), labels (the text's own width when it does not wrap), pictures
  (labels with a pixmap or movie, `QSvgWidget`), text boxes (line edits, spin
  boxes, editable combo boxes, editable text edits under 200 px), list rows (at
  most 12 per view, grouped by view) and progress bars and sliders;
- blocks, the rectangles it must keep clear: buttons of every kind, tabs, combo
  boxes, line edits, spin boxes, sliders, small editable text edits, pictures,
  labels with a link or a pointing-hand cursor, and other pointing-hand widgets
  up to 360 by 160 px. Headings, plain labels, rows, progress bars and scroll
  bars are not blocks: it may stand in front of them.

At most 300 landmarks and 400 blocks are kept. `MainWindow` hands the scan to
the spider as its scene source (`setScene`); the scene is marked quiet while
the tip bubble or the coach panel shows, and is empty while the spider is out
on the desktop.

The scan runs only when needed: when the spider is at rest, at most every 2 s
(`kSceneEvery`), 0.25 s after a page change or a `positionGenieSpider` pass
(`rescanSoon`), 0.3 s after a landing or the end of an interaction, and before
choosing a wander target or an interaction when the last scan is over 1 s
old. Never while it walks, flies, is held, plays, is tucked away, is out on the
desktop or is under the aquarium's water.

### Keeping clear

The body it keeps clear is the 78 px box less 2 px at each side and 12 px at
the top (the feather), ending 1 px above the floor line
(`GenieKeepClear::body`), so a button just below the top edge of the composer
it stands on is not covered, while one above that edge is. Every place it comes
to rest goes through `GenieKeepClear::clearSpot`: the blocks are filtered to
those that can reach a body on any floor, and the
candidate spots (the wanted one, every 8 px of the floor, and just beside each
block, each with the floor under it) are scored by covered area. The least
covered wins and, among equals, the nearest to the wanted spot; with no clear
spot it takes the least covered one.

- Wander targets and the home spot when it comes back from Tuck away are moved
  to the nearest clear spot before it walks.
- When it is at rest over a block (the page changed under it, a throw or hop
  landed on a button, the window was resized), the at-rest check walks it to
  the nearest clear spot.
- It waits while hovered, held, dancing, playing, tucked away or under water,
  and does not apply out on the desktop. Tuck away still peeks out of the
  bottom-right corner, where the user put it.

### Interactions

When a wander is due, in the app (not out on the desktop, tucked away, held,
hovered, dancing or under water), standing still, with the app active and no
tip or coach showing, it plays with a landmark half the time instead of
wandering (`kPlayChance`). It draws the next interaction from a shuffled bag
(`GenieInteractionBag`) of all 44 (the 14 below and the 30 genie effects in
"Genie effects"), so each is used before one repeats. An
interaction that fits nothing on the page is skipped and stays in the bag for
later; when none of those left fits, the kinds already used this round are
added back, so a page with only buttons still gets played with, and the bag
never holds a kind twice.

`GeniePlay::plan` picks the landmark: of those it fits, within reach (centre
6 to 420 px above the floor under it, 90 to 620 for silk, not below its
floor), one of the 4 nearest to the spider. The spot is the clear spot nearest
the landmark's centre, keeping the landmark itself clear too (rows excepted),
and is dropped if it ends up over 170 px from the landmark's edge. It walks
there, then plays:

| Interaction | Landmarks | What it does |
| --- | --- | --- |
| Peek | button, picture, heading, label, text box | perks up on its toes, eyes on it |
| Read | heading, label, row (40 px wide or more) | eyes sweep along the text left to right, head leaning with them, then a nod |
| Wave | button, picture, heading | leans toward it and waves |
| Point | button, text box, tab, progress bar, picture | front leg points at it |
| Sniff | picture | leans right up to it, sniffs with little puffs from its mouth, then giggles |
| Cheer | progress bar | cheers at it, both front legs up |
| Tab nod | a row of 2 to 4 tabs | walks under each tab in turn, left to right, and nods under each |
| Silk | heading, picture, button, label | points up and shoots a silk thread to its bottom edge, tugs it three times, reels it back in |
| Hop | button, picture, label, text box, heading | walks to within 150 px, then hops and lands beside it, and giggles |
| Eureka | text box | eureka at it, the gem lights up |
| Startle | button | surprise and a small jump back |
| Giggle | label, heading | giggles at it twice |
| Count | a row of tabs or rows (2 to 5; the rows nearest its floor) | looks at each in turn with a nod for each, then eureka |
| Compare | two buttons side by side (up to 320 px apart) | looks back and forth between them, then nods at one |

It never clicks, focuses or changes anything: the scan only reads geometry,
and the silk thread is a child widget transparent for the mouse
(`genieSilk`), stacked under the spider and hidden as soon as the thread is
reeled in. Its eyes on the landmark win over the mouse pointer while it plays,
unless the pointer is on the spider.

The poses are new fields of `GenieSpiderPose` drawn by `GenieSpiderPainter`:
`point` and `pointAt` (the front leg on that side reaches 38 units out from
its shoulder, in screen directions), `lean` (up to 9 degrees), `sniff` (a fast
small bob and three puffs) and `tug` (pulls the body down 3.5 units). They
ease back to rest when an interaction ends. No interaction shows text: the
marks it used to pop above its head ("!?", "!", "1 2 3", "?") were removed in
issue 1819 along with the `sign` pose.

Anything the user does wins: hovering the spider, a press, a click, a long
press, the right-click menu, a drag or a throw, a dance, a tip or the coach
showing, a page change, a resize of the page area, Stay put, Tuck away, going
out onto the desktop, and the aquarium's `walkTo`, `hop` and `setSubmerged`
all stop an interaction at once (`calm`, `stopPlay`). An approach that has not
arrived in 15 s is dropped. The incidental reactions (a page nod, a level
change) play over it.

Resources: an interaction runs the frame timer at 24 fps for 2 to 9 s, about
once every 10 to 30 s while the app is active. A scan of a full settings page
is a walk of the widget tree with no painting; nothing is scanned per frame.

### Genie effects

Issue 1833, 2026-10-09: the user asked for the spider genie to do more genie
like effects, 30 of them. The list and the look are shared with the web and
phone spiders (`www/public/genie-spider.js`, [../native/GENIE.md](../native/GENIE.md)):
the same 30 names, the spider's palette (glow teal, lilac, violet, plum) plus
gold for the lamp, coins, halo and shimmer, no words or numbers, 2.4 to 3.4 s
each.

They are 30 more `GenieInteraction` values, `Poof` to `Whirlwind`
(`GeniePlay::magic` is true for them), so they go through the same path as the
14 plays: the same bag, `GeniePlay::fits` for the landmarks each suits,
`GeniePlay::plan` for the landmark and the clear spot beside it (the same
reach and keep-clear rules), the walk there, and the same list of things that
stop them. With 44 in the bag, about two picks in three are effects. Poof is
the one that does not walk: it stays where it is, vanishes in smoke, and
reappears at the spot, which must be at least 70 px away (as for Hop).

Once there, `GenieSpiderWidget::conjure` plays the effect for its length
(`GenieMagic::length`). Each frame it asks `GenieMagic::body` for what the
effect does to the spider itself, and draws the rest on a layer:

- the spider's own drawing changes through new `GenieSpiderPose` fields, read
  by `GenieSpiderPainter`: `fade` (the whole spider drawn through one
  transparent layer, so the legs do not show through the body), `size` and
  `twirl` (shrinks and turns about the turban), `spin` (turns round like the
  Spin dance) and `incense` (the aura turns lilac). Lifts move the spider
  widget up (`m_lift`, used by `place`) without changing where it stands, so
  the floor, the keep-clear checks and a throw are not affected. Grow is the
  one effect bigger than the 94 px spider widget: while it is over 1x the
  widget paints nothing and the layer draws the spider at 1.5x, standing on
  the same spot;
- everything else is drawn by `GenieSpiderPainter::paintMagic` on one more
  child of the page area, `genieMagic`, built like the silk thread:
  transparent for the mouse, no focus, stacked just under the spider, and
  sized each frame to the spider, the landmark and room above them
  (`GenieMagic::bounds`, clipped to the page area). The landmark itself is
  never touched: the effects are drawn over it, never on it.

| Effect | Landmarks | What it does on the desktop |
| --- | --- | --- |
| Poof | button, picture, heading, label, text box, progress bar | fades out in a puff of violet smoke with teal and gold sparks, and fades back in from a second puff at the spot beside it |
| Levitate | heading, picture, label, button | floats 30 px up with its legs spread, bobbing, on a curling smoke tail, with its shadow left on the floor, then settles |
| Lamp | text box, button, picture, heading | a gold oil lamp grows beside it, smoke curls from the spout to the landmark, sparks burst there, eureka |
| Wishes | button, picture, heading, progress bar | three orbs (gold, teal, lilac) arc from its turban jewel to three points on the landmark, one after another, each bursting into sparkles; a nod |
| Smoke rings | picture, heading, button, label | blows three violet rings from its mouth that widen, drift to the landmark and fade |
| Shimmer | heading, label, button, progress bar, row | points at it while a band of gold light sweeps across it left to right, a gold glint running along its edge |
| Stardust | heading, picture, button, label | 26 specks of gold, teal, white and lilac arc from its turban and settle twinkling along the landmark's top edge |
| Carpet | button, picture, heading, label, text box | a violet carpet with a gold border, a teal seam and tassels slides in under it, lifts it 22 px rippling, sets it down and slides away |
| Vortex | picture, button, text box | points while 16 sparks spiral in around the landmark and tighten to its centre, then a flash and a ring |
| Crystal | picture, heading, label, row | a crystal ball on a gold stand appears in front of it, mist and a swirl turning inside, a glint on the glass; its eyes stay on the landmark |
| Runes | picture, button, heading | points while six glowing rune shapes, strokes rather than letters, appear one by one on a slow orbit around the landmark |
| Halo | picture, heading, button | a gold ring rises from the landmark's centre to just above it, hovers with a glint, fades |
| Fireflies | picture, heading, label, row | ten blinking motes drift from its turban and wander about the landmark |
| Whirl | button, tab, heading, label, picture | spins round twice on the spot inside two rings of turning smoke |
| Grow | button, picture, heading | swells to 1.5x in a cloud of smoke, holds, shrinks back in a second puff, giggles |
| Bottle | text box, label, heading, picture | a glowing glass bottle appears at its feet; it shrinks and twirls down into the neck, the bottle wobbles and swirls, then it pops back out in a spray of sparks and giggles |
| Rainbow | picture, heading, progress bar, button | a six-band arc of light grows from its jewel over to the landmark, shimmering |
| Fireworks | progress bar, heading, button, picture | a spark climbs from the landmark's top and bursts gold, then two smaller teal and lilac bursts; it cheers |
| Bubbles | picture, text box, label, row | ten bubbles with a turning teal, lilac and gold sheen rise from the landmark and pop |
| Glitter | heading, label, row, picture | 34 flakes drift down over the landmark, flashing as they turn |
| Jewel beam | button, picture, text box, tab | the jewel lights up (eureka) and sends a widening teal beam to the landmark, which glows round its edge |
| Ripple | button, picture, text box, progress bar | four rings of light spread out from the landmark's centre |
| Moons | picture, button, heading | three small moons circle its turban, then fly one by one to the landmark and fade |
| Constellation | picture, heading, label | seven stars pop in around the landmark and are joined up by faint lines |
| Crescent | heading, picture | a gold crescent moon rises above the landmark, rocking, with five twinkling stars |
| Mirage | button, text box, picture, tab, heading | three wavy ghosts of the landmark's outline drift up from it and fade |
| Coins | button, progress bar, picture | seven gold coins tumble down beside the landmark (the side facing the spider), bounce, stack and glint |
| Flourish | heading, label, button, picture | points, and a looping gold trail draws itself from its front leg to the landmark, then fades from its tail |
| Incense | label, heading, row, picture | four wisps of lilac smoke rise from glowing embers around it while its aura turns lilac |
| Whirlwind | button, heading, picture, tab | a funnel of turning smoke builds up around it, lifting and turning it once, then scatters outwards |

Cleanup: `stopPlay` calls `calmMagic`, which hides the layer and puts the
pose fields, the lift and the size back at once, so an effect stopped half way
(a press, a drag, a throw, hovering, a dance, a page change, hiding, going
under water) leaves no smoke and no shrunken, faded or lifted spider. A Poof
stopped before the jump leaves it where it was; after it, at the new spot.

The desktop has no reduced-motion setting, so there is nothing to turn them
off by other than Stay put (no wandering, so no plays) or hiding the spider.

Resources: the layer repaints at the interaction's 24 fps for the length of
the effect, over the spider, the landmark and the room above them. Particles
are a fixed handful per effect (at most 34), placed from the interaction's
roll, so nothing is allocated or simulated between frames; a faded spider is
drawn once per frame into a 120-unit layer.

## Notices are not covers

Issue 1703 had the spider jump onto the floating notification cards (or step
out of their way), because they were raised over the page area it lives in.
Issue 1867 removed those cards: the user asked for no pop-up notifications anywhere and for
the information to be shown inline. A notice is now a strip in the page's own
layout above the page (see `docs/desktop/diagnostics.md`, "Inline notices"),
so it is part of the page the spider's scene scan already reads, and
`GenieSpiderWidget` has no cover source, `floorAt` only stands on the composer
and the avoid rectangles, and the per-frame dodge is gone.

## Look, feet and hover

Issue 1517: the user asked for the desktop spider to look more like the
phone's, for its legs to touch the ground, for it to react to the mouse, and
for it not to use too many system resources.

- Look: the widget used to be masked to an ellipse, which cut off the feet,
  the leg tips and the glow with a hard, jagged edge. The mask now only trims
  the empty top corners and everything more than 4 px below the floor line, so
  nothing the spider draws is clipped and clicks below it still reach the
  composer. Under the spider is the phone's cyan glow (the `.shadow` in
  `GenieSpiderLayer.avatar`: 25% glow, about 8 px soft, 3 px down): the frame
  shrunk to a quarter, blurred, tinted and drawn underneath, rebuilt at most
  every 0.12 s.
- Feet: all eight feet stand on the floor line (`kGround`, 96.2 of the 100-unit
  box, where `place()` puts the floor). The feet are placed in ground space and
  mapped into the body's frame, so the body's breathing bob and sway move the
  knees while the feet stay put; walking still lifts them in turn, and in the
  air they splay as before. The back knees sit a little further out so the
  longer back legs reach the floor, the claw's curve is capped at 5.5 units so
  long legs stay slim, and a soft contact shadow sits under the body while it
  stands. Issue 1819 gave the web and phone spiders the same feet, knees,
  claw cap and shadow, so all three look the same.
- Hover: entering the spider giggles (at most once every 4 s), it stops a
  wandering walk and will not start one while the pointer is on it, and it
  perks up for as long as the pointer stays: the body rises on straighter
  legs, the front feet tap, the eyes widen a little, the smile and the glow
  grow, and the halo is stronger and lower, like the phone's hover lift.
- Resources: the frame is painted into an image only when a tick changed it,
  so a repaint of the page under it only copies the image. The head and turban
  (everything but the feather, eyes, mouth and the thriving glint) are drawn
  once into a cached image at twice the device resolution and redrawn only
  when the glow strength, the eureka gem or sickness changes; the leg joints
  and each eye's socket are cached the same way, and the eyes no longer clip
  to a path. Measured offscreen with the widget at 2x scale: painting a frame
  went from 7.5 ms to about 3 ms of CPU, idle from about 123 to 48 ms of CPU a
  second, and hovered from about 167 to 137.

## Under water

Issue 1738: the Aquarium package ([../marketplace/aquarium.md](../marketplace/aquarium.md))
fills the window with water while ForkMesh is in the background, and its fish
play with the spider. A package cannot reach the widget, so the desktop
answers a small socket, `desktop/src/PetHostFeed.h`, from
`MainWindow::installGeniePetHost` (called by `setupGenieSpider`):

- `sprite`: the spider's frame and body rect in the asking widget's
  coordinates, its current frame image, and whether it walks or flies; false
  while it is hidden or out on the desktop stage.
- `setSubmerged`: while true the spider paints nothing itself (the aquarium
  draws `frameImage()` above its picture of the window, so no frozen copy is
  left in that picture) and ticks at 24 fps even in the background.
- `react`: Wave, Cheer, Nod, Eureka, Giggle, Surprise, Dizzy or a random
  Dance.
- `walkTo` an x, `hop` with a velocity (a throw from where it stands; skipped
  while parked, held or already moving), and `lookAt` a point for 0.6 s. A
  look target wins over the mouse while submerged, otherwise only when the
  mouse is not near.

## Out on the desktop

Issue 1480: the user asked to drag the spider out of the app onto the desktop,
where it can go all over the computer.

Dragging the spider more than 12 px past the edge of the main window hands it
to a real drag and drop (`GenieSpiderWidget::escaped`,
`MainWindow::releaseGenieToDesktop`): the in-app spider hides, a picture of it
follows the pointer, and `GenieDesktopStage` opens. The stage is a frameless,
transparent, always-on-top tool window covering the work area of the screen
under the pointer. While the drag is on, it takes input everywhere so the drop
lands on it; it records the drag moves and throws the spider from the drop
point with the pointer's velocity over the last 80 ms, including the release
position, like an in-app throw.
A drag dropped anywhere else (or cancelled with Esc) puts the spider back in
the app where it was.

A drag and drop is used rather than moving a window under the pointer because
on Wayland an app can neither place its windows nor learn where the pointer is
outside them; the drop gives the position in the stage's own coordinates. On
Wayland the stage is shown maximized so the compositor fits it to the work
area; elsewhere it is given the screen's available geometry.

On the stage the spider, its tip bubble and the coach panel are reparented
into the stage, which masks itself to their rectangles, so clicks everywhere
else go to the windows underneath. The floor is the bottom of the work area,
the walls are the screen edges, and it roams: every 5 to 13 s it walks to a
random spot anywhere along the screen, or (3 times in 10) hops with a random
leap and lands with the same gravity. Its spot is saved as `genie/desktopSpot`, and
`genie/desktop` brings it back onto the desktop on the next start (it drops in
from the top of the screen).

Come back inside (menu) puts it back in the app, dropping in from the top of
the page area, and raises the app. Coach buttons and Add to agent raise the
app as well. Limits: it stays on the screen it was dropped on, and on GNOME
Wayland the compositor does not honour always-on-top, so another window raised
over it hides it until the app is focused again.

## Add to agent

A bubble the spider shows from the menu (How are you feeling?) has an Add to
agent button when there is agent work behind it. It fills the agent composer with instructions an
agent can carry out, puts the cursor at the end and focuses the composer. The
text already in the composer goes to its history first, like the other drafted
prompts. Hovering the bubble keeps it open; it closes 4 s after the pointer
leaves.

Issue 1722: the button used to paste "The Genie spider said:" and the
marketing advice itself, which an agent cannot act on ("send one person a
note"). The user asked for the spider to say things that work as agent
instructions. Each coach stage, each playbook step and each suggestion from
what it read (see "What it watches") now has a `task` and a `prompt`
(`GenieCoaching`, `GenieMarketingStep`, `GenieSuggestion`, carried per tip by
`GenieTip`):

- the bubble shows the task under the advice, as "An agent can draft the
  announcement from this week's commits.";
- Add to agent puts the prompt in the composer: a self-contained instruction
  that names what to read in the repository (the README, the code,
  `git log --since=7.days --no-merges`, issues), what to produce and in what
  shape, and the counts the coach knows (commits this week, people reached);
- prompts that produce outreach (emails, posts, launch drafts, follow-ups,
  the list of places to find users) end with "Reply with the drafts; do not
  send anything", so an agent never posts or emails on the user's behalf;
  prompts that change the repository keep the change small (the README top,
  a changelog entry, one user-visible fix with a test).
- suggestions built from the CRM (people who wrote last, bounces, written next
  steps, contacts never emailed) name the contacts and the organization and
  tell the agent to read them with the ForkMesh MCP server's CRM tools
  (`crm_list_contacts`, `crm_get_contact`, `crm_list_activity`), treating what
  contacts wrote as evidence; they draft and do not change the CRM;
- suggestions from Marketing quote the plugin's note, Analytics insight or
  funnel bottleneck and ask for a draft or a proposed change, not an edit; the
  shipped-work suggestion lists the commit subjects to announce; the Reddit one
  leaves a slot for the threads, since no MCP tool reads Marketing;
- nothing an agent cannot see or do gets a prompt, and its bubble has no
  button: a plugin that is not installed or failed to read, Not watching,
  experiments due or over the limit, ads off their pace, and no funnel week
  logged.

How are you feeling? carries the stage's prompt.

## Replying to the spider: the Genie special agent

Issue 1739: the spider kept recommending someone the operator had already
closed a conversation with and passed on for investment. The user asked for
the spider to also be a special agent they can reply to, on a very low-cost
model, on by default, so it can be taught.

Two things changed:

- **Closed contacts are left out.** `GenieCrmDigest::read` skips contacts whose
  CRM stage is closed (`GenieLessons::closedStages`: passed, rejected, closed,
  inactive, lost, closed-lost, declined, churned, not-interested,
  disqualified; spaces and underscores count as dashes) when it lists people
  who wrote last, stale next steps, bounces and contacts never emailed. The
  CRM tools sentence in every CRM agent prompt now says to leave them out too.
  The counts behind health (emails sent, replies, bounces) are unchanged.
- **The spider can be taught.** Each tip bubble has a **Reply** button next to
  Add to agent, and the menu has **Reply to the spider…** (the last tip shown).
  Both open the Genie special agent's page (`.forkmesh/teammates/genie.json`)
  with the composer holding `About your tip “…”: ` (the "Tip n of m" counter
  dropped) and the cursor at its end. The first open repository with
  `genie.json`, the active Special Agents repository first, is used and made
  active; without one an inline notice on the page says where to get it.

The Genie special agent (`genie`, avatar `genie`: the spider itself, painted by
`GenieSpiderPainter` on a violet glow in `teammateAvatarArt`) is enabled and
on-demand, with Read repositories and External tools only. Its definition sets
`"lowCostModel": true` (see
[agents-ui.md](agents-ui.md#low-cost-special-agents-2026-10-07)), so a run
uses Haiku 4.5 under Claude, GPT-5.6 Luna under Codex and GPT-4.1 mini on an
OpenAI key. Every Genie run's prompt carries `genieAgentBriefing`: the
spider's health, its tips in click order (at most 15), the closed stages, the
contacts it was taught to leave out, and the teaching contract. The agent
checks the contact with the CRM MCP tools and answers with a block like:

```forkmesh-genie
{"skip": [{"contact": "jane@fund.vc", "reason": "Passed on investment"}]}
```

`{"unskip": ["jane@fund.vc"]}` lets the spider suggest someone again. When a
Genie run finishes, `learnFromGenieSpiderRun` reads every such block in its
closing summary (`GenieLessons::learn`), saves the lessons in `genie/lessons`
(JSON, newest first, at most 200), says what it learned in an inline notice, nods and
measures again. A lesson matches a contact by CRM id, name, "Name (Company)"
or any of its addresses, case-insensitively; a bounce to an unknown address
matches by address. Taught contacts are named in the CRM agent prompts
("these, whom I was told to leave alone"). The agent never changes the CRM: if
the record is stale (still "meeting" after a pass) it says which stage it
should move to.

Settings, General, GENIE SPIDER has **Let me reply to the spider to teach it**
(`genie/replies`, on by default); off hides the Reply button and menu item.

## What the spider said

Issue 1752: the user asked to keep track of what the spider says, accurately,
so that its messages can be commented on from the Genie special agent's page.

Every bubble the spider shows is recorded in `GenieSaidLog`
(`desktop/src/GenieVitals.{h,cpp}`), saved as JSON in `genie/said.json` under
the data root (its own file, not `QSettings`, so a tip does not rewrite the
settings file): a click line, How are you feeling? and the notice saying what
a Genie run taught it (older logs may also hold `unasked` tips, which it no
longer shows). Each message keeps:

- its words exactly as the bubble showed them (without "Tip n of m"), and the
  agent task line when the bubble showed one;
- when it was said, and how often: the same words of the same kind on the same
  basis count again and move to the top instead of repeating;
- its basis, `genieBasis` of the same facts the message was built from: when
  the readings were measured, what is watched, the week's counts for the
  watched sides, and whether the CRM and Marketing reads came from an
  organization, failed (with the error), were not read yet or have no plugin.
  The same words on a later measurement are a new message, so a comment is
  always about one set of readings;
- the operator's comments, and Genie's verdict on it.

At most 300 are kept; the oldest messages without comments go first.

The Genie page (`genie` only) has a **What the spider said** section at the top
of its side column: the newest 30 messages, plus every older one with a
comment, each with its kind, time, times said, words, task line and basis.
**Comment** asks for a comment and keeps it under the message (an x removes
it). A Reply from a bubble now names the message id in its quote
(`About your tip “…” (message s…):`) when the tip is in the log.

Every Genie run's briefing lists the commented messages, newest first and at
most 20, with their id, words, basis, comments and whether they were checked
since the last comment, and `GenieSaidLog::contract` asks the agent to check
each unchecked one against its basis and the CRM and answer in the same
`forkmesh-genie` block:

```forkmesh-genie
{"checked": [{"id": "s1abc", "accurate": false, "note": "Ana passed on Jun 3."}]}
```

`learnFromGenieSpiderRun` reads `checked` with `GenieSaidLog::learn` (an entry
needs a known id and a true or false `accurate`), and the card shows "Genie
checked it …: wrong" or "accurate" with the note, with a red or green border;
a newer comment puts it back to "Waiting for Genie to check it.". While
messages wait, the section header has **Ask Genie to check (n)**, which sends
Genie that request as a message. A message found wrong because of a contact is
also taught with `skip`; for any other cause the agent names the reading or
rule behind it.

## Throwing

Issue 1478: the user asked for more momentum when the spider is thrown, real
gravity and velocity, and for the desktop and phone spiders to match exactly.
The flight in `GenieSpiderWidget::flightStep` uses the phone's numbers
(`GenieGravity` in [../native/GENIE.md](../native/GENIE.md)), stepped at the
same 120 Hz:

- 2,200 px/s² of gravity and quadratic air resistance, so a fall approaches
  about 2,000 px/s rather than accelerating without limit; a throw is capped
  at 4,200 px/s;
- a floor hit keeps 30% of the speed and 78% along the floor until it hits
  slower than 240 px/s, then it lands; the left and right edges of the page
  area and its top keep 42% and 78% along them;
- landed but still moving sideways, it skids, slowing by 2,400 px/s² until
  under 20 px/s, and bounces off the side edges (42% kept) and off the side
  of a composer it runs into; skidding off the top of a composer it falls
  again with its speed.

The one difference: the desktop spider only stands on floors, so it never
clings to a side or the top the way the phone's does when it hits one slowly;
it always bounces off. A throw lands on the composer only when the spider was
above the composer's top; coming in lower it passes in front of it, lands on
the bottom and climbs up as before. The frame timer runs at 60 fps in flight.

## What it watches

Issue 1717: the user asked for settings for what the spider monitors and gives
suggestions for; for ForkMesh itself everything off except Marketing and CRM,
able to market everything it ships, with accurate suggestions that do not
guess and have the context they need.

Settings, General, GENIE SPIDER has one checkbox per topic (the menu's Watch
submenu holds the same list). The choice is saved as `genie/watch`, a comma
list of topic ids; an empty value watches nothing.

| Topic | Id | What it reads |
| --- | --- | --- |
| Commits | `code` | commits in the local repositories over 7 days |
| Agent runs | `agents` | agent runs started and merged over 7 days |
| README | `readme` | whether the open repository's README has a picture |
| CRM | `crm` | the CRM plugin's summary: people emailed, replies, bounces, who wrote last, written next steps |
| Marketing | `marketing` | the Marketing plugin's overview and Analytics performance, and what shipped since the last published item |
| Launch playbook | `playbook` | the nine general steps of the coach panel |

Until the user changes a box, the spider watches everything, except for
members of the `forkmesh` organization (`GenieWatch::ownOrganization`): the
first time the CRM or Marketing organization list names it,
`noteGenieOrganizations` saves `crm,marketing`. After that the saved ticks
win, so a ForkMesh member who wants the rest back ticks it.

What is not watched is not read: the git scan, the README check, the CRM and
Marketing reads only run for watched topics, and `GenieVitals::watched` zeroes
the readings of the others. Health follows only the watched sides (both, one,
or a resting 60% when nothing is watched), the coach panel hides the meter of
an unwatched side and the playbook unless it is watched, and the footer names
what is watched. A change re-measures at once, or right after a measurement
in flight.

### Suggestions from what it read

`GenieMarketingCoach::suggestions` builds the coach panel's WHAT I SEE list,
which follows the stage's headline and advice in `GenieCoaching::tips` (the
Genie special agent's briefing). Each item names the data
it came from, with a Go button to the page:

- CRM: people who wrote last (an inbound email or synced mail from one of the
  contact's addresses within 14 days) with no email from you since, newest
  first and each with the day they wrote; this week's bounces by name;
  contacts with a written next step and no contact for a week, grouped so
  that one step shared by several contacts is one line, not several; contacts
  with an address never emailed, counted against how many have an address at
  all.
- Shipped work: non-merge commits in the scanned repositories
  (`git log --since=14.days`), after Marketing's newest published item (a
  published content item or a tracked post) when that is within 14 days,
  quoted by subject with the item it is measured against. Without Marketing
  data it says it cannot see what was announced rather than claim nothing was.
- Marketing: the plugin's own checks (`suggestions` in the overview, warnings
  first, channel info notes left out), open Reddit threads, experiments due
  for review or over the running limit, ads off their budget pace, nothing
  scheduled for the next 7 days (with how many queued items have no date or
  a past slot), the Analytics insights from `/performance`, and the funnel's
  bottleneck reason (naming the stage when Marketing names one) or the lack of
  a logged week.
- README, when watched without the playbook: no picture yet.

When a watched plugin is not installed, or a read fails, the list says so
first and the tips leave that source out. At most 12 items are kept.

#### Why these readings were wrong, and what changed

The operator replied to the spider's health tip asking for tips that were
more informative and accurate, and an audit of all thirteen against the live
CRM found four defects with one root cause.

`GenieLessons::closedStages()` was a fixed list of stage names
(`passed`, `rejected`, `closed`, `inactive`, `lost`, `closed-lost`,
`declined`, `churned`, `not-interested`, `disqualified`). No contact in this
organization's CRM was in any of them: its flows end in `complete`, so the
filter matched nothing and no contact was ever left out for being closed. The
closing stage is now the last stage of each flow in the organization's own
settings, added to the fixed list, so a CRM that renames its stages still gets
them filtered; a contact carrying a closed tag (`declined` and the rest) is
left out too, since a tag is where an outcome often lands when the stage has
not moved. The same list is what the agent briefing and the agent prompts
name, so they no longer tell an agent to skip stages the CRM does not use.

An inbound email counted as "wrote last" whatever its status, so an email
marked spam made the spider tell the operator to answer a contact who had
already declined, and ahead of everyone else. Inbound with a refused status
(spam, bounce, reject, failed, unsubscribe) and synced mail filed in a Spam,
Junk or Trash box are no longer replies.

The no-replies stage fired on `emailsSent + contactsTouched` and then said
"change the message, not the volume". In a week where nothing was sent and
only a record was touched that is backwards: nobody could reply. A week with
no email sent is now its own Nothing sent stage that asks for a send, and the
health line says no email went out rather than that the message is not
working. The readings call that a contact record touched, not a contact
reached, since no message went with it.

`digest.contacts` counted every row including duplicates and rows with no id,
because it was incremented before the dedupe check; it is now counted after.
The never-emailed tip gave a bare number that could not be checked, so it now
names the population it is drawn from.

The marketing score also counts 2 points per item published in Marketing this
week (the same item once, comparing URLs without their query string, fragment
or trailing slash), and the reply cap applies only when the CRM is
watched, since without it the spider cannot see replies. Mail synced into the
CRM from a contact's address counts as a reply. Until this issue the replies
and bounces `GenieCrmCounts` counted were never copied into the readings, so
the spider said nobody had replied whenever there was outreach; they are now.

With code unwatched the stages come from outreach alone (Quiet, Warming up,
Flywheel, or No replies with the CRM), worded with the same scale as the
summary and listing what it saw; with outreach unwatched, from code alone;
with nothing watched the stage is Not watching and points at Settings.
"Building in the dark" now says the spider sees almost no outreach rather
than that nobody was told, which it cannot know.

The Marketing readings had four more defects, each fixed where it arises. The
published count counted a content item and a tracked post separately, and
deduped them only on an exact URL match, so one item published once as content
and once as a post counted twice whenever the two URLs differed by a query
string such as UTM tags, or by a trailing slash. The URL is now compared without
its query string and fragment, lowercased, and without one trailing slash. Both
are still counted, so the count is now labelled "published or posted" everywhere
the operator sees it, and the scoring weights are unchanged.

The health line said "Marketing is slowing down" without naming the
organization, although the digest had the name. `GenieVitals::summary` now takes
an optional organization and names it in the marketing sentence ("Marketing for
forkmesh is slowing down"). The "How are you feeling?" bubble and the agent
briefing pass the Marketing organization, falling back to the
CRM's, and with no name the line is what it was.

When Marketing's funnel report gave a bottleneck reason but named no stage, the
digest dropped the reason and the spider said no funnel week was logged,
although one was. The reason is now kept whenever it is given. Without a named
stage the tip reads "Funnel:" followed by the reason, without "That is the stage
to work on.", and the proposed change asks to fix what the reason describes
rather than a stage that was never named.

The nothing-scheduled tip counted only queued or scheduled items dated in the
next 7 days. An item queued with no date, or whose slot had already passed,
counted nowhere, so the tip said nothing was scheduled as if the queue were
empty. Those items are now counted as stuck, and the tip says how many in the
same parenthesis as the last published date, for example "(the last item went
out Oct 7; 2 queued with no date or past their slot)".

## Health

Code follows the phone's `GenieVitals`: commits plus 2 per agent run started
plus 3 per merged run. Each side fills as `1 − e^(−1.6 · points / goal)`,
health is `0.55 · mean + 0.45 · weaker side`, levels and hearts are the same,
and readings older than 2 days fade towards 35%.

Marketing is stricter than the phone's since issue 1480: the user found the
spider calling marketing healthy when it was not. The old count gave a point
for every CRM contact whose record changed (an import, a sync or a tag edit
touches hundreds), counted drafts and bounced or failed sends as sent, and
gave half a point for merely opening a marketing page. Now, over the week:

- 2 points per person emailed: distinct recipients (contact, else address) of
  outbound mail whose status is delivered, accepted, opened, clicked or logged
  from outside; drafts and sends still in flight do not count;
- half a point per contact reached (`lastContactedAt` this week, not
  `updatedAt`);
- 4 points per person who replied (inbound mail this week, distinct senders);
- minus 1 per bounced, failed, rejected, unsubscribed or spam-flagged send;
- nothing for page visits (they still feed the playbook's Ship in public and
  Measure steps).

The goal is 16 points, and replies cap the score: with no reply marketing
stays at most 45% ("slowing down"), with 1 or 2 replies at most 70%, so only
3 or more replies can make it hum. The summary lists people emailed, contacts
reached, replies and bounces, and says "Nobody has replied, so the marketing
is not working yet." when there was outreach and no answer. Readings saved
under the old count (7 values in `genie/vitals`) are dropped, so the first
start after the update waits for a fresh measurement.

What the desktop measures over the last 7 days:

- commits: `git rev-list --count --no-merges --since=7.days HEAD` in the first
  12 repositories with a local checkout, off the GUI thread;
- agent runs and merges: `m_agentSessions` created in the window;
- CRM: the same two reads the CRM page makes (`GET /api/crm`, then the
  summary of the organization the CRM page last showed, else the first one
  with the plugin installed), counted by `GenieCrmCounts::read`; nothing is
  read when the CRM plugin is not installed;
- marketing visits: opening the CRM or Marketing page, and Analytics or
  Competitors from the coach, recorded in `genie/activity` (the last 400, the
  same page twice within 20 s counts once);
- Marketing, when watched: `GET /api/marketing`, then the overview and
  `/performance` of the organization the CRM uses when it has the plugin,
  else the first one with it installed, read by
  `GenieMarketingDigest::read`; and the shipped changes above.

It measures 8 s after the composer is built, then at most every 30 minutes
(checked once a minute), when the panel opens and on Refresh. The last reading
is saved (`genie/vitals`) so the spider looks right at once on the next start.
The CRM and Marketing reads are listed in
`desktop/resources/network/endpoint-catalog.json`. Saved readings now have 10
values (published added); readings with the older 9 still load.

## The marketing coach

`GenieMarketingCoach::coach` turns the two velocities into a stage, a headline,
advice and one action:

| Stage | When | Points at |
| --- | --- | --- |
| No replies | outreach this week and nobody replied (checked first) | CRM (change the message, not the volume; follow up once) |
| Asleep | both sides under 30% | Agents (start small: one commit, one conversation) |
| Building in the dark | code at least 50%, marketing under 30% | Marketing (announce what shipped; the headline counts the commits) |
| All talk | code under 30%, marketing at least 50% | Agents (ship what contacts asked for) |
| Flywheel | both at least 50% | Analytics (double down on the channel that works) |
| Warming up | anything else | the weaker side: Agents or CRM |

Bounced or failed sends this week are called out at the end of the advice
("fix those addresses before you send more").

Below it is a nine-step playbook: say who it is for, polish the front door
(README with a picture), list 20 people with the problem, talk to 5 of them,
ship in public, launch where the users are, follow up within 48 hours, measure
and double down, collect proof. Steps the desktop can see tick themselves
(README picture in the open repository, 20 CRM contacts, 5 people emailed
with a delivered send, commits plus a delivered email plus a marketing visit
this week, Analytics opened this week); the rest are ticked by hand and saved in `genie/playbook`.
The first open step is expanded with its detail, and each step with a page has
a Go button.

Pages: CRM and Marketing open the plugin pages (or the Market with a message
when the plugin is not installed); Ads opens Marketing (its Ads manager tab);
Analytics and Competitors open `/dashboard/analytics` and
`/dashboard/competitors` on the configured host in the browser, since they have
no desktop page; Agents opens the current repository's agents and focuses the
composer; repositories opens the current repository's README.

The spider no longer shows a tip unasked (issue 1819); the 60 s timer only
measures the vitals again.

## One line on click

Issue 1468 kept dialogs for the right-click and long-press menu and made a
click cycle through marketing advice. Issue 1819 (2026-10-08): the user asked
for the spider to say nothing unless clicked on purpose, not dragged, and then
to say one helpful line of 5 to 10 words.

A press that moves more than 6 px is a drag and says nothing; one held for
0.55 s opens the menu. A plain click (`MainWindow::showNextGenieTip`) rebuilds
`GenieCoaching::lines` from the current facts and shows the next one with
`GenieTipBubble::say`: a bubble sized to the line, with no Reply or Add to
agent button, for 5 s. The lines are "Next marketing step: <the next open
playbook step>." (when one is open), then fixed lines about what feeds it,
the marketing coach, throwing it and tucking it away.
`test_genie_vitals.cpp` checks every line, for every playbook step, is 5 to
10 words. The position wraps around and is kept for the session; a click on
the bubble shows the next line; a click on a tucked-away spider still only
brings it back. The line is recorded as a `click` in `GenieSaidLog` and is
what Reply to the spider… replies to. The longer advice (`GenieCoaching::tips`)
still briefs the Genie special agent.

## Not verified yet

For issue 1833, the 30 effects' names, landmarks, place in the bag and the
Poof plan are covered by `forkmesh-genie-landmarks-tests`. An offscreen
harness built from `GenieSpiderWidget.cpp`, `GenieSpiderPainter.cpp`,
`GenieLandmarks.cpp` and `GenieVitals.cpp` rendered every effect with the
painter at a quarter, half and three quarters of its length, then drove the
real widget over a page with a button, a heading, a picture, a text box and a
progress bar: all 30 walked (or poofed) to a landmark, ran to their end and
left the layer hidden, the pose, the lift and the size back to normal; Levitate,
Bottle, Grow, Poof, Carpet and Whirlwind were also stopped half way by `calm`
and by hiding the spider, with the same result. `GenieLandmarks.cpp`,
`GenieSpiderPainter.cpp`, `GenieSpiderWidget.cpp` and `MainWindowGenie.cpp`
were compiled with the app's flags. Not tried in the running app: the effects
over real pages, the repaint cost of the layer over the biggest pages, and how
often the effects feel right next to the older plays.

For issue 1750, the scan, the keep-clear spot, the plans and the bag are
covered by `forkmesh-genie-landmarks-tests` (offscreen). An
offscreen harness built from `GenieSpiderWidget.cpp`, `GenieSpiderPainter.cpp`,
`GenieLandmarks.cpp` and `GenieVitals.cpp` drove the real widget over a page
with buttons, a logo, a heading, tabs, a text box and a progress bar: it
walked off the Save button it started on, all 14 interactions ran to their
end, a click and a dance stopped one, and a throw onto the Save button ended
with it walking clear. The painter's new poses were rendered offscreen.
`GenieSpiderWidget.cpp`, `GenieSpiderPainter.cpp`, `GenieLandmarks.cpp` and
`MainWindowGenie.cpp` were compiled and the `forkmesh` app target linked in a
Debug build. Not tried in the
running app: the real pages and composer, the cost of a scan on the biggest
pages, and how often the interactions feel right.

The pure logic is covered by `forkmesh-genie-vitals-tests`. The spider, bubble
and panel were rendered offscreen in a small harness (standing on a composer,
walking, tucked away, sick and thriving), and `MainWindowGenie.cpp` and
`MainWindowChat.cpp` were syntax-checked with the app's compile flags. Not yet
checked in the running app: placement over the real composer on every page,
the CRM reads against a live organization, and the stacking order against
other overlays.

For issue 1468, `GenieCoaching::tips` is covered by `forkmesh-genie-vitals-tests`,
and `MainWindowGenie.cpp` and `GenieSpiderWidget.cpp` were syntax-checked; the
click cycle and the long press were not tried in the running app.

For issue 1517, the spider was rendered in an offscreen harness (the painter
alone in eight poses, and the real widget on a floor, idle and hovered, at 1x
and 2x) and `GenieSpiderWidget.cpp`, `GenieSpiderPainter.cpp` and
`MainWindowGenie.cpp` were syntax-checked with the app's flags. Not tried in
the running app: the hover on a real pointer, the halo over the composer and
on the desktop stage, and the paused timer while minimized on each platform.

For issue 1480, the stricter marketing count and the No replies stage are
covered by `forkmesh-genie-vitals-tests`; `GenieSpiderWidget.cpp`,
`MainWindowGenie.cpp` and `MainWindow.cpp` were syntax-checked. The drag onto
the desktop, the stage's click-through mask on Wayland, X11, Windows and macOS,
and the Add to agent button were not tried in the running app.

For issue 1722, every stage and step having a task and a long prompt, the tips
carrying them, the counts and commit subjects in the prompts, and which
suggestions carry a prompt are covered by `forkmesh-genie-vitals-tests`;
`GenieVitals.cpp`, `GenieSpiderWidget.cpp`, `MainWindowGenie.cpp` and
`MainWindowSettings.cpp` were syntax-checked with the app's flags after
merging issue 1717. The bubble
with the task line and the composer filled by Add to agent were not tried in
the running app.

For issue 1703, the card handling was driven in an offscreen harness with
fake cover rectangles. Issue 1867 removed the cards and the cover handling
with them; `forkmesh-window-tests` builds the spider without it.

For issue 1717, the watch list, the digests, the shipped-change parsing and
the suggestions are covered by `forkmesh-genie-vitals-tests` (built here with
g++ against Qt Core). The Marketing digest was also fed the overview and
performance answers of the real Marketing worker, produced through its test
runtime with published, scheduled, posted, campaign and experiment records,
and its tips read back. `GenieVitals.cpp`, `MainWindowGenie.cpp`,
`GenieSpiderWidget.cpp` and `MainWindowSettings.cpp` were syntax-checked with
the app's flags. Not tried in the running app: the settings checkboxes, the
Watch submenu, the ForkMesh preset against the live organization list, and
the reads against live CRM and Marketing data.

For issue 1723, the 10 dances were rendered with the painter alone in an
offscreen harness at several moments of each, and `GenieSpiderPainter.cpp`,
`GenieSpiderWidget.cpp`, `MainWindowGenie.cpp`, `MainWindowAgentLaunch.cpp`,
`MainWindowAgentList.cpp`, `MainWindowBranches.cpp` and `MainWindowPulls.cpp`
were syntax-checked with the app's flags. Not tried in the running app: the
dances on a real agent start and merge.

For issue 1739, the closed-stage filter, the lessons (learning from a
`forkmesh-genie` block, matching by address, name and id, unskip, round-trip),
the leave-out sentence in CRM agent prompts and the Genie agent briefing are
covered by `forkmesh-genie-vitals-tests`; the low-cost model choice and the
`lowCostModel` round-trip by `forkmesh-teammate-tests`; `genie.json` by
`app/tests/test_genie_spider_special_agent.py`. Both C++ test programs were
built here with g++ against Qt Core, and the changed desktop sources were
syntax-checked with the app's flags. Not tried in the running app: the Reply
button and menu item, the Genie page opening with the quoted tip, a real
Genie run on Haiku teaching the spider, and the spider avatar on the rail.

For issue 1752, recording, repeats, the basis, comments, the `checked`
verdicts, the briefing and the log's round-trip and cap are covered by
`forkmesh-genie-vitals-tests` (built here with g++ against Qt Core);
`genie.json` by `app/tests/test_genie_spider_special_agent.py`.
`GenieVitals.cpp`, `MainWindowGenie.cpp`, `MainWindowTeammateHome.cpp` and
`MainWindowTeammates.cpp` were compiled with the app's flags. Not tried in the
running app: the section on the Genie page, the comment dialog and a real Genie
run marking a message.
