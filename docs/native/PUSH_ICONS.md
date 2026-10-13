# Push banner icons

Every push banner starts with a small emoji that says what kind of alert it
is, so a glance at the lock screen tells a mention (📣) from a release (🚀), a
new issue (🐛), a pull request (🔀), a code push (📤), a chat message (💬) or a
direct message (📨). The phone draws the ForkMesh logo on the left; the emoji
sits right next to it at the start of the title.

## Why an emoji in the title

- iOS always draws the app icon on a banner and lets an app change nothing
  about it. The only image slot, the attachment thumbnail, already holds the
  actor's portrait. The title is the one place every banner, on every iOS
  version, shows right beside the logo.
- An emoji needs no image assets, no app update and no extra payload bytes:
  the relay writes it once and every phone shows it.
- Android posts its ping notifications on the phone from the inbox rows, so it
  carries the same table and prefixes the title itself.

## Where the icon is chosen

| Piece | Where |
| --- | --- |
| The kind → icon table, the desktop agent-run icons, `with_icon` | `app/src/push_icons.py` |
| Pings and code pushes (payload and `GET /api/push/alert`) | `_push_alert_item` in `app/src/entry.py` |
| Chat, direct messages, the push test | `chat_message_alert`, `direct_message_alert`, `APNS_TEST_ALERT` in `app/src/apns.py` |
| Android | `native/android/core/.../push/PingIcons.kt`, used by `PingNotifier.post` and `directMessageAlert` |

The icon goes into the title the relay renders, not into the iOS
notification service extension: the extension replaces the title with the
one `GET /api/push/alert` returns, so a server-side title keeps the icon on
banners from phones running any build.

A desktop ping shows 🤖 when an agent finished and ⚠️ when it failed (the
ping's `meta.category`); other desktop pings show 💻. A kind missing from the
table falls back to 🔔. `with_icon` never adds the icon twice.

## Adding a ping kind

Add the kind to `PUSH_KIND_ICONS` in `push_icons.py` and to `PING_KIND_ICONS`
in `PingIcons.kt`. `test_every_ping_kind_gets_a_little_icon_beside_its_headline`
fails while `NOTIFICATION_KINDS` has a kind without an icon.

## Icons and read state in the phone Pings list

The Inbox (Pings) list on iOS and Android draws each row with a Material icon
and a short kind label rather than an emoji. They come from
`notificationIconForKind` (`FMApi/ViewModels/NotificationsLogic.swift`,
`core/.../account/NotificationsLogic.kt`) and `ForkNotification.kindLabel`.
Until 2026-10-02 both apps were missing `code_push`, `security_alert`,
`device_added`, `package_review` and the two hackathon kinds, so those rows
showed the generic bell and the label "Notification".
`test_phone_apps_give_every_ping_kind_its_own_icon_and_label` now fails while
`NOTIFICATION_KINDS` has a kind either app cannot draw or name. It also fails
when an icon name is missing from the iOS `MaterialIconCodepoints.swift`
table, because iOS draws a missing name as a question mark.

A row's read state shows in several ways at once. An unread row has an accent
wash behind it, a coloured icon tile, an accent time, a bold title and a dot
on the trailing edge. A read row has a neutral tile, a regular title in the
secondary text colour and no dot. VoiceOver and TalkBack begin each row with
"Unread" or "Read".
