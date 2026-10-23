# Desktop Genie spider and marketing coach

Part of [the desktop docs](README.md). The desktop port of the phone's Genie
spider ([../native/GENIE.md](../native/GENIE.md)): a pet whose health follows
code velocity and marketing velocity, and a marketing coach. It has no voice,
model or tools of its own; replies go to the Genie special agent. The web has a
tips-only variant ([../www/genie-spider.md](../www/genie-spider.md)).

## Where the code is

| Part | File | Tests |
| --- | --- | --- |
| Watch list, health, CRM and Marketing digests, shipped changes, README check, coach and suggestions, lessons, said-log, agent briefing | `desktop/src/GenieVitals.{h,cpp}` | `forkmesh-genie-vitals-tests` |
| The Genie special agent | `.forkmesh/workflows/genie.json` | `app/tests/test_genie_spider_special_agent.py` |
| QPainter spider, dances and effects (`GenieMagic`, `paintMagic`) | `desktop/src/GenieSpiderPainter.{h,cpp}` | none |
| Crawling widget, tip bubble, coach panel, desktop stage, silk and effects layers | `desktop/src/GenieSpiderWidget.{h,cpp}` | none |
| Landmark scan, keep-clear spot, interaction plans and bag | `desktop/src/GenieLandmarks.{h,cpp}` | `forkmesh-genie-landmarks-tests` (offscreen) |
| Wiring, menu, tips, settings, pet socket for packages | `desktop/src/MainWindowGenie.cpp`, `desktop/src/PetHostFeed.{h,cpp}` | none |
| "What the spider said" section | `fillTeammateHomeSaid` in `desktop/src/MainWindowTeammateHome.cpp` | none |
| Settings, General → GENIE SPIDER | `buildSettingsGeneralTab`, `buildGenieWatchSettings` | none |

## The spider

`GenieSpiderWidget` is a 78 px spider in a child of the page area
(`m_globalOverlayHost`), positioned by `positionGenieSpider`. It is created after
the first paint and never in window-test builds or on `offscreen`/`minimal`
platforms.

- It stands on the bottom of the page area or on top of the agent composer,
  wanders every 5 to 13 s near its home spot (`genie/spot`), and its eyes
  follow the pointer.
- It can be dragged and thrown; the flight uses the phone's physics (gravity
  2,200 px/s², quadratic drag, throw capped at 4,200 px/s, floor and wall
  restitution, skidding), stepped at 120 Hz.
- A plain click shows one 5–10 word line (`GenieCoaching::lines`) in a bubble
  for 5 s; a drag says nothing; a 0.55 s press opens the menu. The one line it
  says unasked announces a badge: when a fresh `badge` ping celebrates,
  `MainWindow::cheerGenieFor` plays a cheer and shows "You earned Stargazer!"
  or "You reached Silver Fan Club!" (`GeniePings::badgeLine`, tested in
  `test_genie_vitals.cpp`), logged as an `unasked` line. A hidden or tucked
  away spider stays quiet.
- Reactions (wave, cheer, nod, eureka, giggle, surprise) follow page changes,
  tips and health changes. `MainWindow::danceGenie` plays one of 10 dances
  (never the same twice in a row) when an agent starts or a branch is merged.
- Menu: How are you feeling?, Marketing coach, Measure again, Watch, Let out
  onto the desktop / Come back inside, Stay put (`genie/wanders`), Tuck away
  (`genie/parked`), Hide the spider.
- **Hiding.** The menu item and "Show the Genie spider" in Settings go through
  `MainWindow::setGenieSpiderShown` (`genie/shown`, on by default). Hidden, the
  spider and its parts are deleted, timers stop and no reads are made.

### Frame budget

The frame timer runs at 60 fps in flight, 24 fps while walking, reacting or
hovered, lower at rest and when the app is inactive, and stops while hidden or
minimized. Frames are painted to an image only when a tick changed them; the
head, turban, joints and eye sockets are cached images.

## Playing with the window and keeping clear

`GenieLandmarks::scan` walks the page area's widget tree (geometry only) and
returns landmarks (buttons, tabs, headings, labels, pictures, text boxes, list
rows, progress bars) and blocks it must keep clear (interactive widgets and
pictures). Caps: 300 landmarks, 400 blocks. Scans run only at rest, at most
every 2 s and shortly after page changes, never per frame.

- **Keep clear.** Every resting spot goes through `GenieKeepClear::clearSpot`,
  which scores candidate floor spots by covered block area and picks the least
  covered, nearest one. At rest over a block, it walks clear.
- **Interactions.** Half the time a wander is due, it plays with a nearby
  landmark instead, drawing from a shuffled bag (`GenieInteractionBag`) of 14
  plays (peek, read, wave, point, sniff, silk thread, hop, count, compare, …)
  and 30 genie effects (`GenieMagic`: poof, levitate, lamp, stardust, carpet,
  …). `GeniePlay::fits`/`plan` choose a landmark within reach and a clear spot
  beside it. Effects are drawn on a mouse-transparent layer (`genieMagic`) and
  never on the landmark itself.
- It never clicks, focuses or changes anything. Any user action, page change,
  resize or dance stops an interaction at once (`calm`, `stopPlay`,
  `calmMagic`), restoring pose, lift and size.

## Out on the desktop

Dragging the spider more than 12 px past the main window starts a real drag and
drop onto `GenieDesktopStage`, a frameless, transparent, always-on-top tool
window over the work area (drag and drop is used because Wayland exposes no
global pointer position). On the stage the spider roams the screen; the stage
masks itself to the spider's parts so clicks pass through. `genie/desktopSpot`
and `genie/desktop` persist it.

## Pet socket for packages

`desktop/src/PetHostFeed.h`, installed by `MainWindow::installGeniePetHost`,
lets packages such as the Aquarium ([../marketplace/aquarium.md](../marketplace/aquarium.md))
read the spider's `sprite`, `setSubmerged` it, trigger `react`, `walkTo`, `hop`
and `lookAt`. When the spider is hidden the socket answers "no spider".

## Add to agent

Bubbles from the menu carry, where agent work applies, a task line and an **Add
to agent** button that fills the composer with a self-contained prompt
(`GenieCoaching`, `GenieMarketingStep`, `GenieSuggestion`, `GenieTip`). Prompts
name what to read, what to produce and the known counts. Outreach prompts end
with "Reply with the drafts; do not send anything"; CRM prompts use the MCP CRM
read tools and never change the CRM. Suggestions an agent cannot act on get no
button.

## Replying to the spider: the Genie special agent

Each tip bubble has **Reply** and the menu has **Reply to the spider…**. Both
open the Genie special agent's page (`.forkmesh/workflows/genie.json`) with the
tip quoted in the composer. The agent is on-demand, read-only plus external
tools, and sets `"lowCostModel": true` (see [agents-ui.md](agents-ui.md)). Each
run's prompt carries `genieAgentBriefing`: health, recent tips, closed stages,
taught contacts and the teaching contract. The agent answers with a block:

```forkmesh-genie
{"skip": [{"contact": "contact@example.com", "reason": "Closed"}]}
```

`{"unskip": [...]}` reverses a skip. `learnFromGenieSpiderRun` parses blocks in
the closing summary (`GenieLessons::learn`) and stores lessons in
`genie/lessons` (newest first, at most 200), matched by CRM id, name or address.
Contacts in a closed stage (the last stage of each CRM flow plus a fixed list
and closed tags, `GenieLessons::closedStages`) are left out of suggestions.
Setting: `genie/replies` (on by default).

## What the spider said

`GenieSaidLog` records every bubble in `genie/said.json` under the data root
(not QSettings): exact words, task line, time and repeat count, and a `basis`
(the readings it was built from). At most 300 are kept, commented ones last to
go. The Genie page shows them with **Comment**; commented messages go into the
briefing, and the agent returns verdicts:

```forkmesh-genie
{"checked": [{"id": "s1abc", "accurate": false, "note": "Contact closed."}]}
```

## What it watches

`genie/watch` is a comma list of topics:

| Topic | Id | What it reads |
| --- | --- | --- |
| Commits | `code` | commits in local repositories over 7 days |
| Agent runs | `agents` | agent runs started and merged over 7 days |
| README | `readme` | whether the open repository's README has a picture |
| CRM | `crm` | the CRM plugin's summary: people emailed, replies, bounces, who wrote last, next steps |
| Marketing | `marketing` | the Marketing plugin's overview, performance, and what shipped since the last published item |
| Launch playbook | `playbook` | the nine steps of the coach panel |

Unwatched topics are not read and are zeroed in the readings. Members of the
project's own organization default to `crm,marketing`
(`GenieWatch::ownOrganization`).

`GenieMarketingCoach::suggestions` builds the panel's WHAT I SEE list (at most
12), each item naming its data source with a Go button: people who wrote last
without a reply (refused/spam mail excluded), bounces, stale next steps,
never-emailed contacts, unannounced commits, Marketing's own checks, unscheduled
or stuck queue items, analytics insights and funnel bottlenecks. A missing
plugin or failed read is stated rather than guessed around.

## Health

Code points: commits + 2 per agent run started + 3 per merged run. Marketing
points over the week: 2 per person emailed with a delivered send, 0.5 per
contact reached, 4 per person who replied, 2 per item published (URLs
normalised), minus 1 per bounced/failed/rejected/spam send; goal 16. With the CRM
watched, replies cap marketing at 45% with none and 70% with one or two.

Each side fills as `1 − e^(−1.6 · points / goal)`; health is
`0.55 · mean + 0.45 · weaker side` over watched sides (60% when nothing is
watched); readings older than 2 days fade towards 35%.

Measurement: `git rev-list --count --no-merges --since=7.days HEAD` in up to 12
local repositories off the GUI thread; agent sessions; the CRM and Marketing
reads the plugin pages make; page visits in `genie/activity`. It measures 8 s
after startup, then at most every 30 minutes, on panel open and on Refresh.
Readings persist in `genie/vitals`. Endpoints are listed in
`desktop/resources/network/endpoint-catalog.json`.

## The marketing coach

`GenieMarketingCoach::coach` maps the two velocities to a stage:

| Stage | When | Points at |
| --- | --- | --- |
| Nothing sent | CRM watched and no email sent this week | CRM |
| No replies | outreach this week and nobody replied | CRM |
| Asleep | both sides under 30% | Agents |
| Building in the dark | code at least 50%, marketing under 30% | Marketing |
| All talk | code under 30%, marketing at least 50% | Agents |
| Flywheel | both at least 50% | Analytics |
| Warming up | anything else | the weaker side |
| Not watching | nothing watched | Settings |

Below it is a nine-step launch playbook (audience, README picture, 20 people,
5 conversations, ship in public, launch, follow up, measure, collect proof).
Steps the desktop can observe tick themselves; the rest are ticked by hand in
`genie/playbook`.
