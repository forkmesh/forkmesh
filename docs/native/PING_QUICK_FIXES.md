# Ping quick fixes

A ping that reports a problem leads with a one-tap fix. On the phone it is
the first row under **Actions** on the ping's page; on the desktop it is the
first entry in the right-click menu on the Pings page. For a run that failed
because the build Mac was not on the network, that fix is **Turn off this
action**.

The desktop decides which fixes to offer and in what order. It learns from
the fixes you pick, and once a fix has proven itself it applies that fix by
itself and tells you, with an undo.

## Code map

| Piece | Where |
| --- | --- |
| Causes, fixes, ranking, the learning ledger, trust | `desktop/src/PingFixes.{h,cpp}`, `desktop/tests/test_ping_fixes.cpp` (`forkmesh-ping-fixes-tests`) |
| Applying a fix, the `pings.` control family, the auto-apply hook, the Pings menu | `desktop/src/MainWindowPingFixes.cpp`; hooks in `recordNotification` and `onRunFinished` (`MainWindowActions.cpp`) |
| The ping id on the phone | `app/src/desktop_pings.py` puts the desktop's `pingId` in the row's `meta` |
| iOS | `FMControl/DesktopPingFixes.swift` (client), `InboxItem.desktopPingId` / `fromDesktop`, rows in `Features/Inbox/InboxItemScreen.swift` |
| Android | `inbox/DesktopPingFixes.kt` (client), `InboxItem.desktopPingId` / `fromDesktop`, rows in `feature/inbox/InboxItemScreen.kt` |

## Why the desktop owns it

The desktop raised the ping, holds the run, its log and the repository, and
is the only place a fix can run. Keeping the rules and the ledger there gives
one brain for every phone and the desktop's own Pings page, and an older
phone needs no update when the rules improve.

## The wire

Two methods in the owner-only `pings.` family on local control (LAN or
relay):

- `pings.fixes {pingId?, title, body, ts}` returns
  `{found, event, cause, signature, fixes: [{id, label, detail, icon, score, trusted, autoSafe, primary}]}`,
  best first.
- `pings.fix {…same, fix}` applies one and returns the refreshed list plus a
  `message` the phone shows inline under the ping's Actions. Errors: `unknown_ping`,
  `fix_unavailable`, `unknown_repo`, `unknown_run`, `unknown_agent`,
  `wrong_state`, `not_started`.

The desktop finds the ping by `pingId` (`<timestampMs>-<localId>`, as it was
forwarded), then by the same timestamp, then by the forwarded title and body
within 15 minutes. The Pings journal keeps each row's `runId`, so a run's ping
stays fixable after a restart.

## Causes and fixes

`causeOf` reads the ping's `Cause:` line (or the body) and sorts it into
host-offline, network, timeout, missing-tool, auth, disk, test, build or
unknown. The event comes from the ping: action failed, stopped, waiting for
approval, turned off automatically, or an agent that failed. Disk covers
the runner's own pre-checkout check ("Not enough free disk space on this
node", Desktop client CI tests run #6604 on 2026-10-22) as well as ENOSPC
from a step; that check used to land in unknown.

| Event | Fixes |
| --- | --- |
| Action failed | Turn off this action (or turn it back on), Run it again, Ask an agent to fix it |
| Action stopped | Run it again, Turn off this action |
| Waiting for approval | Approve and run, Reject this run |
| Turned off automatically | Undo: turn it back on |
| Agent failed | Tell the agent to try again |

Each fix has a starting weight per cause. An offline host puts turning the
action off first (0.85); a failing test or compile error puts the agent first
(0.8); a network error or timeout puts running it again first.

## Learning

`Ledger` keeps counts per fix under two keys: the exact signature
(event, cause, repository, workflow) and the family (event, cause), so what
you teach one workflow carries over to the same kind of failure elsewhere.
Counts: offered, chosen, auto-applied, ok, failed, undone, and the current
streak of successes. It is stored in `<dataRoot>/ping-fixes.json` and capped
at 400 signatures.

- **Offered** is counted when a fixable ping is filed.
- **Picking** a fix that finishes at once (turning off, approving) counts as
  a success.
- **Running it again** succeeds or fails with the new run.
- **An agent fix or retry** waits: if the same failure comes back within a
  day it failed; if not, it succeeded.
- **Turning an action back on** counts as an undo against turning it off.

The score blends the success rate and how often you pick the fix, each
smoothed toward the starting weight; the exact signature counts twice as much
as the family.

## Self-approval

A fix becomes trusted for one workflow when it is safe to apply unattended
(today: turning an action off), and you have picked it with success the last
three times that workflow failed the same way (`kTrustStreak`). The phone
then shows it with an **Auto** badge. On the next such failure the desktop
turns the action off itself and files "Action turned off automatically",
whose fix is **Undo: turn it back on**. An undo resets the streak, so that
fix needs three more picks before it acts alone again. Trust never carries
over to another workflow, and running again or starting an agent is never
applied unattended.
