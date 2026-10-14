# Push banner icons

Every push banner title starts with a small emoji for its kind (mention,
release, issue, pull request, code push, chat message, direct message), so a
glance at the lock screen tells the alerts apart. The phone draws the app logo
on the left; the emoji sits next to it at the start of the title.

## Why the title

- iOS always draws the app icon on a banner and the attachment thumbnail holds
  the actor's portrait, so the title is the only slot every iOS version shows
  beside the logo.
- An emoji needs no assets, no app update and no extra payload: the relay
  writes it once.
- Android posts ping notifications on the phone from the inbox rows, so it
  carries the same table and prefixes the title itself.

## Where the icon is chosen

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
run, one for a failed run, one for other desktop pings. A kind missing from the
table falls back to a bell. `with_icon` never adds the icon twice.

## Adding a ping kind

Add the kind to `PUSH_KIND_ICONS` in `push_icons.py` and to `PING_KIND_ICONS`
in `PingIcons.kt`. `test_every_ping_kind_gets_a_little_icon_beside_its_headline`
fails while `NOTIFICATION_KINDS` has a kind without an icon.

## Icons and read state in the Pings list

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
