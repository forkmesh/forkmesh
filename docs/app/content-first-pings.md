# Content-first pings

A ping shows who acted and what they wrote first, and leaves out the generic
framing. "You have a new notification", "New comment" and "ForkMesh — new
issue" told the reader that something happened and made them open the app to
find out what. Each surface now leads with the actor, then the content, and
moves the event label (Issue submitted, New comment) to a small trailing line
or leaves it to the kind icon.

The stored ping row is unchanged: its `title` is still the event label written
by `enqueue_notification` callers. Each surface rebuilds the display from
`actor`, `repo`, `meta.number`, `title` and `body`, so older rows and every
client that reads the raw row keep working.

## The rule

| Row has | First line | Second line |
| --- | --- | --- |
| actor | `bob · acme/widgets #42` | the body, or the event label when there is no body |
| no actor | the event label, with `#42` in front on a phone banner | the body |

A row without an actor (a release, credits, a host going offline) has no
person to lead with. Its title already names the event.

## Surfaces

| Surface | Where | What changed |
| --- | --- | --- |
| Phone banner (iOS payload, `GET /api/push/alert`) | `_push_alert_item` in `app/src/entry.py` | Title is the kind icon plus `actor · repo #n`, body is the text, no subtitle. iOS shows two lines instead of label, byline, then text. |
| Email, instant | `_notification_instant_email` | Subject `bob: <text> · [repo] #n`. No heading and no "you have a new notification" intro: the item card comes straight after the logo. |
| Email, digest | `_notification_digest_email` | Subject is the newest item's lead plus `(+N more)`. The heading names who is waiting (`bob, carol and 2 more · 5 pings`) and the count line is gone. |
| Email item card | `_notification_email_item` | Avatar, name, repo, number and time on top, the text in 15px under it, the event label as a small link at the bottom. |
| Email inbox preview | `preheader` of `_forkmesh_email_card_html` | The first item's text, so the mail list shows the message rather than "Hi alice". |
| Web pings dropdown and Show all pings list | `renderNotificationPreview`, `renderNotificationModal`, `notificationLead` in `app/public/dashboard/js/08-repo-detail-network.js` | Avatar initial with a kind badge, `actor · repo #n` and the time on top, two lines of text, a blue dot on unread rows. |
| Desktop OS alerts and Pings rows | `alertLeadTitle`, `alertSnippet` in `desktop/src/PingSettings.h`, used by `MainWindowIssues.cpp`, `MainWindowPulls.cpp`, `MainWindowCoves.cpp`, `MainWindowRepoSync.cpp` | Title `bob · o/r #4`, body the issue title or the comment text. A batch keeps the repository as its title and the count as its body. |

Chat messages and mentions were already content-first (sender as title,
message as body) on every surface and are unchanged.

## Instance logo

Each ping row in the inbox shows the logo of the instance it came from, so
someone signed in to several instances can tell them apart at a glance.

- **Web** (pings dropdown and Show all pings list):
  `notificationInstanceLogoHtml` draws `/assets/logo.svg` at 16px, named with
  `platformName()`. On a branded instance `app/tools/apply_instance_brand.py`
  writes the instance's logo over that file, so the path is always the
  instance serving the page. The logo sits left of the kind icon from the
  `sm` breakpoint up and under it on narrower screens
  (`flex-col-reverse sm:flex-row`).
- **iPhone** (Inbox tab, `InboxRow` in
  `native/ios/ForkMesh/Features/Inbox/InboxScreen.swift`): the active
  project's `ProjectIcon` at 16pt under the kind icon, the same image as the
  top bar's project slot. The inbox only ever holds the active project's
  pings and tasks, so that project is the instance each row came from. Rows
  the phone filed itself (`InboxSource.local`) have no instance and show no
  logo. VoiceOver reads "From <project>" after the title.

## Email subjects

The `ForkMesh: ` prefix is kept only when an item has neither an actor nor
text, because a bare event label such as `Notification` means nothing in a
mail list without it. Everywhere else the sender name already says ForkMesh.

The plain-text alternative drops the "Hi alice" greeting and the "You have N
unread" line. The item lines are unchanged, and the account the mail went to
moves to the footer (`Sent to alice.`).

## Android

The relay change reaches Android only through the inbox rows: `PingNotifier`
builds its own title from the stored ping with `PingLead`, which leads with
the actor and the content the same way.

## Dashboard CSS

The dropdown uses only utilities already in the committed
`app/public/dashboard/tailwind.css`. Regenerating that file from the current
config drops classes other pages still use, so new markup sticks to classes it
already has (`bottom-0` rather than `-bottom-0.5`, for example).
