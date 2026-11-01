# Desktop: special agents

Part of [Desktop](README.md). A special agent is a repository workflow whose
Special agent step holds a role: it runs on demand, on a cron cadence, on
events or continually with a decision gate, and each run is a workflow run
with its own record and output file
([workflows](../app/workflows.md#desktop-workflows-and-special-agents)). This
page covers how a run picks its agent and permissions, how sessions are
reused, and the **Special Agents** rail page. How to use them is in the
[desktop app guide](../qt-client.md).

## Definitions and names

Rail **Special Agents**. A definition lives in
`.forkmesh/workflows/<id>.json`; `src/TeammateStore.*` reads it through
`src/RepoWorkflow.*`, and `src/MainWindowTeammates.cpp` drives it. Legacy
`.forkmesh/teammates/*.json` files still load and move on their next save.
Error-log and crash repair are special agent tasks
([diagnostics.md](diagnostics.md)).

Only user-visible text says "special agent". Stored and wire names keep
"teammate": legacy `.forkmesh/teammates/`, `Teammate*` classes, `teammates/*`
settings, the `teammates.` local-control family, the `#team` room and `team`
sender id, `teammate-*.png` portraits. Pausing all automatic runs sets
`schedule.active` to `false` and keeps the cadence.

## Agent choice and permissions

A definition has no provider, model or mode. Every start uses
`teammates::recentAgentChoice()`: the composer's agent and model, else the
newest session no special agent started, else `defaultAgentProvider()`.

- `"lowCostModel": true` takes the chosen agent's cheapest model
  (`lowCostAgentChoice`; the spider's agent uses it, see [genie.md](genie.md)).
- `"marketingChannels"` attaches an agent to Marketing channel pages
  (`NativePackageHost::specialAgents`).
- The run mode is the composer's, clamped by `agentrules::teammateRunMode`:
  without `modify-code` it is Plan, and Auto without `run-unattended` becomes
  Edit. A UI-stall repair ignores the composer and starts in Auto when the
  agent holds `run-unattended`.

Missing permissions are refused at the external-call gate and, for Claude
Code, by `--disallowedTools` (`deniedTeammatePermissions`; see
[access rules](../agents/access-rules.md)):

| Permission missing | Refused |
| --- | --- |
| `external-tools` | External calls except approved destinations; WebFetch, WebSearch |
| `create-branches` | `git push`, new branches, `git worktree add`, `gh pr create`, MCP `open_pr_from_branch` |
| `manage-issues` | `gh issue` writes, MCP issue, milestone and project writes |
| `review-prs` | `gh pr review/comment/edit/close/ready` |
| `merge` | `gh pr merge` |

- Grants are recorded per session (`m_teammateRunGrants`).
- Special agents never open pull requests; prompts say to leave finished work
  for a person.
- `teammates/ownWorktrees` (off): while off, Claude Code and Codex runs work
  in the checkout and `create-branches` is dropped from the grant.

## Sessions

- A run is a workflow run: `runTeammate` starts the agent's workflow
  (`runTeammateWorkflow`) and its Special agent step starts a fresh session
  through `startTeammateSession`, filed in the **Workflows** agent folder. A
  mention or a direct prompt to an agent with a conversation continues that
  session instead, as chat, without a workflow run.
- `ownTeammateSession` names the newest session the agent started, which
  steering, Stop and the face's status use; `teammateWithRuntime` keeps
  runtime fields across saves.
- Mentions go through `deliverTeammateMessage` (steer while working, else
  continue). **Run team**, **Generate briefing** and `@all` use one session
  per member.
- **Run all in one session** (`runTeammatesInOneSessionNow`,
  `buildTeammateSharedPrompt`) holds only the intersection of the
  participants' permissions and splits the closing summary back by
  `## @Handle`.
- **Create needed teammate** runs a read-only analysis ending in one JSON
  definition, which the desktop saves (`teammateFromAgentOutput`) without
  `merge`, `process-payouts` or `run-unattended`, with schedules paused.
- A self-improvement cycle counts only when the summary adds a new
  `Improvement note:` lesson (`completeImprovementCycle`).

## Special Agents page

- **Grid.** Video tiles (`buildTeammateVideoTile`) show the live face and the
  last run's caption (each run ends with `Caption:`, read by
  `runCaptionFromSummary`). The roster's empty state is `#teammateEmpty`.
- **Home pages.** Each agent has a home page
  (`src/MainWindowTeammateHome.cpp`) with a conversation, notes, widgets and
  an about panel. Notes and widgets stay on the machine
  (`<data>/teammate-workspaces/<repository digest>/<id>.json`), are added to
  every run's prompt and never widen permissions. Widget changes the agent
  proposes are applied only with the operator's approval.
- **Faces and moods** (`src/MainWindowTeammateRail.cpp`) drive the page's
  tiles and cards. Each 54 px item has an animated face (`facePoseAt`, one
  shared clock), a status border, a staleness tint
  (`teammates::runStaleness`), a play button, a mood emoji and a menu
  (`showTeammateRailMenu`, which also holds **Create needed special agent**).
  The mood (`src/TeammateMood.*`) is scored from 100 weighted signals and sent
  to phones in `teammates.list`. `runWindowHours` sets a run reminder;
  `teammates/doneUnseenSessions` marks finished runs nobody has seen, and
  `restoreTeammateRunState` loads the finder sessions and done-unseen runs on
  a 30 s clock that also refreshes the home pages.
- **Face redraw budget.** The grid rebuilds only when its signature changes.
  One `RailFaceTicker` (40 ms) drives every face, so a tick is one repaint
  pass for the window. A face redraws only while its window is exposed and
  not minimized; a calm face (still, floating, dozing or paused, without a
  spinner ring) redraws every 100 ms; while the window is not the active one
  every face redraws at most every 200 ms (`kRailFaceInactiveFrameMs`); a face
  with no visible region skips its frame. Each face frame also repaints its
  item, name label and three HUD buttons (about 300 us), so one timer per face
  kept the app above 100% CPU. The mood badge paints from a cached pixmap.
- **Pins.** The menu's **Open in Workflows** and **Pin to rail** put the
  agent's workflow on the Workflows page or on the top, left, right or bottom
  rail ([workflows](../app/workflows.md#desktop-workflows-and-special-agents)).
  The right rail is the generic side rail, which has no special agent chips
  of its own ([side-rail-and-terminals.md](side-rail-and-terminals.md)).
- **Portraits** follow the agent icon rules
  ([agents-ui.md](agents-ui.md#portraits)); a new special agent portrait also
  needs phone crops (`teammate-face-<avatar>`, `teammate_faces/<avatar>.jpg`).
- **Phones** show the same faces, moods and status in an adaptive tile grid
  ([DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).

## Channel status rail

`src/MainWindowPackageRail.cpp` builds `#packageRail` (`ui/showPackageRail`)
from package tiles (`NativePackageWidget::railTiles`,
`PackageRailTileButton`); Marketing provides one square per channel with next
steps. Pages are prepared after startup sign-in resolves and every 15
minutes. The channel reminder (`ui/packageRailReminder`) files one
`channel_reminder` ping for the worst square after an hour without change.

## Tests

`forkmesh-teammate-workspace-tests`, `forkmesh-teammate-mood-tests`,
`forkmesh-native-package-tests`, `app/tests/test_desktop_package_rail_startup.py`.
