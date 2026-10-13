# name: Fix UI stalls
# description: Fix recorded GUI-thread freezes and push the slow work into the background.

Fix the UI stalls in the Qt app. The GUI thread is being blocked, which freezes
the window, so read both logs below and work from what they actually recorded.

Stall log: {{stallLog}}
  one appended record per freeze the watchdog caught, with a sampled backtrace.
App log: {{appLog}}
  everything the Log view shows: network calls, sync, agents, system messages —
  including the slow main-thread operations the watchdog never sampled.

(ForkMesh fills those two paths in when it drafts this shortcut into the
composer; by hand they are ~/projects/forkmesh/logs/diagnostics/stalls.log and
network_log.txt in the app-data folder — Settings ▸ Data lists both.)

For each stall, find the blocking call in the backtrace and fix it so the UI
stays responsive: move the slow work off the main thread, or skip it entirely
when nothing changed.

Then sweep both logs for any other work that never got backgrounded — git,
network or disk operations still running on the GUI thread, anything that pumps
the event loop to wait — and move those into the background too, not just the
frames that happened to be sampled. Watch out for references held across a
pumped wait; take them by value.

Do not fix a stall by widening the watchdog threshold or by silencing the log.
