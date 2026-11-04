# Push banners: icons and what is already on screen

Part of [the native docs](README.md).
How every push banner looks on the phone (the kind icon in its title and the
Pings list rows) and when a banner is dropped because the user already sees
what it reports. Desktop pings are in [DESKTOP_PINGS.md](DESKTOP_PINGS.md),
`#mesh` wake pushes in [MESH_PUSH.md](MESH_PUSH.md), and the APNs setup in
[mobile-push-notifications.md](../operations/mobile-push-notifications.md).

## Banner icons

Every push banner title starts with a small emoji for its kind (mention,
release, issue, pull request, code push, chat message, direct message), so a
glance at the lock screen tells the alerts apart. The phone draws the app logo
on the left; the emoji sits next to it at the start of the title.

### Why the title

- iOS always draws the app icon on a banner and the attachment thumbnail holds
  the actor's portrait, so the title is the only slot every iOS version shows
  beside the logo.
- An emoji needs no assets, no app update and no extra payload: the relay
  writes it once.
- Android posts ping notifications on the phone from the inbox rows, so it
  carries the same table and prefixes the title itself.

### Where the icon is chosen

| Piece | Where |
| --- | --- |
| The kind to icon table (`PUSH_KIND_ICONS`), desktop agent-run icons, `with_icon` | `app/src/push_icons.py` |
| Pings and code pushes (payload and `GET /api/push/alert`) | `_push_alert_item` in `app/src/entry.py` |
| Chat, direct messages, the push test | `chat_message_alert`, `direct_message_alert`, `APNS_TEST_ALERT` in `app/src/apns.py` |
| Android | `native/android/core/.../push/PingIcons.kt` (`PING_KIND_ICONS`), used by `PingNotifier.post` and `directMessageAlert` |

The icon goes into the title the relay renders, not into the iOS notification
service extension: the extension replaces the title with the one
`GET /api/push/alert` returns, so a server-side title keeps the icon for every
app build.

Desktop pings use the ping's `meta.category`: one icon for a finished agent
run, one for a failed agent run, ❌ for a failed CI run, one for other desktop
pings. A kind missing from the
table falls back to a bell. `with_icon` never adds the icon twice.

### Adding a ping kind

Add the kind to `PUSH_KIND_ICONS` in `push_icons.py` and to `PING_KIND_ICONS`
in `PingIcons.kt`. `test_every_ping_kind_gets_a_little_icon_beside_its_headline`
fails while `NOTIFICATION_KINDS` has a kind without an icon.

### Icons and read state in the Pings list

The Inbox (Pings) list on iOS and Android draws each row with a Material icon
and a short kind label, from `notificationIconForKind`
(`FMApi/ViewModels/NotificationsLogic.swift`,
`core/.../account/NotificationsLogic.kt`) and `ForkNotification.kindLabel`.
`test_phone_apps_give_every_ping_kind_its_own_icon_and_label` fails while
`NOTIFICATION_KINDS` has a kind either app cannot draw or name, or when an icon
name is missing from the iOS `MaterialIconCodepoints.swift` table (iOS draws a
missing name as a question mark).

An unread row has an accent wash, a coloured icon tile, an accent time, a bold
title and a trailing dot. A read row has a neutral tile, a regular title in the
secondary text colour and no dot. VoiceOver and TalkBack begin each row with
"Unread" or "Read".

### Good news is green with a check

`operational_alert` carries outages and failures, but also their recoveries
("Mainnode recovered", "GitHub Actions: … passes again") and other good news
(a finished self-deploy, cores or runners rolled, a core caught up, the deploy
agent updated, a desktop installed). Every sender of such a ping sets
`meta.state` to `"up"`; an outage sets `"down"`. A ping is good news when its
kind is `operational_alert` and `meta.state` is `"up"` (`pingIsGoodNews` in
both `NotificationsLogic` files and in `08-repo-detail-network.js`,
`webPingIsRecovery` on the desktop):

- phones: a green `check_circle_outline` and the label "Good news" in the
  Inbox, Pings and ping detail (`ForkNotification.typeLabel` keeps "Alert" for
  the ping-settings text);
- push banners: ✅ instead of 📟;
- web Pings and Home feed: a green `circle-check`; the World: ✓;
- desktop Pings: a green check and "Good news".

The self-deploy agent's good-news keys (`node-rolled:`, `runners-rolled:`,
`agent-updated:`, `core-caught-up:`) are listed in `OPERATOR_ALERT_GOOD_KEYS`
(`app/src/operator_alerts.py`); a new success notice in
`tools/next_autodeploy.py` adds its key prefix there.

The Inbox row also colours other kinds by outcome: a failed run
(`action_failed`, a desktop `ci-failed` or `agent-failed` ping) is red, checks
waiting for approval and a security advisory are amber, and a finished desktop
agent run is green. Android colours the row icon the same way as iOS.

## No banner for what is already on screen

A push about something the user is looking at says nothing new: the Agents
list flips the row to done, the open conversation shows the message, the inbox
list shows the row. Each client that knows what is on screen drops the banner
(or, on the desktop, does not send the ping to the phone at all).

The relay does not decide this. It only knows which sockets are open, and a
socket counts as live for up to three minutes after its last heartbeat
(`ROOM_CLIENT_STALE_MS`), so a phone just put in a pocket or a minimised desktop
would look like a watcher and miss its push.

### The rule

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

Room chat on iOS is hidden while the app is open
(`ApplePushBridge.willPresent`), and `#mesh` alerts skip while the room is on
screen ([MESH_PUSH.md](MESH_PUSH.md)).

A ping that is seen is still recorded: iOS still inserts it into the inbox and
refreshes direct messages, and Android's alert ledger still marks it, so it
does not ring later when the user leaves the screen.

### Per client

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
  Direct messages and local agent events are skipped whenever the app is in
  the foreground.
- **Desktop.** `forwardPingToPhones` does not send a ping to the phone while
  the window is active and either the agent list is visible (for an agent
  ping) or the Pings page is open (`pingforward::seenOnScreen`, test
  `checkOnScreen` in `test_desktop_ping_forward.cpp`). In chat, a message in
  the conversation on screen raises no mention notification and its Pings row
  is filed quietly (no flash or sound).
