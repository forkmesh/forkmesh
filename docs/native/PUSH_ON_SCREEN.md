# No banner for what is already on screen

A push about something the user is looking at says nothing new: the Agents
list flips the row to done, the open conversation shows the message, the inbox
list shows the row. Each client that knows what is on screen drops the banner
(or, on the desktop, does not send the ping to the phone at all).

The relay does not decide this. It only knows which sockets are open, and a
socket counts as live for up to three minutes after its last heartbeat
(`ROOM_CLIENT_STALE_MS`), so a phone just put in a pocket or a minimised desktop
would look like a watcher and miss its push.

## The rule

A push counts as seen when the app is in the foreground, nothing covers the
tabs (a page, sheet, hub, palette, prompt or the lock screen), the push belongs
to the active project, it is not an alarm (the desktop power alarm always
rings), and one of these holds:

| Push | Seen on |
| --- | --- |
| Agent finished or failed | the Agents tab at its root, a filtered or "all agents" list, or that agent's page (`desktop:7` and `7` are the same agent) |
| Any ping | the Inbox tab at its root: the row is inserted live |
| Ping with a repository `href` | the record it links to (`InboxRoutes.route`), e.g. a pull request page for a comment on it |
| Task ping (`/dashboard/tasks/?task=<id>`) | that task's page |
| Direct message (iOS) | the Chat tab at its root with that conversation open (`direct:<id>`) |

Room chat on iOS was already hidden while the app is open
(`ApplePushBridge.willPresent`), and `#mesh` alerts already skip while the
room is on screen ([MESH_PUSH.md](MESH_PUSH.md)).

A ping that is seen is still recorded: iOS still inserts it into the inbox and
refreshes direct messages, and Android's alert ledger still marks it, so it
does not ring later when the user leaves the screen.

## Per client

- **iOS.** `HomeShell` publishes a `PushScreen` (tab, top route, covered, open
  conversation) to `PushNotificationService.screen` whenever it changes.
  `ApplePushBridge.willPresent` asks `PushNotificationService.isOnScreen` and
  answers with no presentation options when it is seen. Rule:
  `PushOpen.isShown(on:)` (`FMApi/Services/PushOnScreen.swift`), tests
  `PushOnScreenTests`.
- **Android.** `HomeShell` publishes a `PushScreen` (tab, top route, covered)
  to `PushScreens`; a pushed screen without a route counts as covered.
  `PingAlerts.deliver` drops the seen rows before `PingNotifier.post`. Rule:
  `pingOnScreen` (`core/push/PushOnScreen.kt`), tests `PushOnScreenTest`.
  Direct messages and local agent events were already skipped whenever the app
  is in the foreground.
- **Desktop.** `forwardPingToPhones` does not send a ping to the phone while
  the window is active and either the agent list is visible (for an agent
  ping) or the Pings page is open (`pingforward::seenOnScreen`, test
  `checkOnScreen` in `test_desktop_ping_forward.cpp`). In chat, a message in
  the conversation on screen no longer raises the mention notification and
  its Pings row is filed quietly (no flash or sound).
