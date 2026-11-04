# Booking Page

An account plugin for scheduling: each account gets a public booking page at
one short link that it sends to anyone. The visitor picks a free time and
books; the meeting goes on the owner's ForkMesh calendar and ForkMesh emails
both sides. The owner asked for a calendar booking plugin where users send a
short link to their calendar, people book a meeting, and the meeting
information is emailed, all through ForkMesh.

Package: [`marketplace/packages/booking`](../../marketplace/packages/booking/README.md).
The owner manages the page at `/dashboard/booking` (the desktop rail opens the
same page) and in the Booking screen of the iPhone and Android apps; guests use
`/meet` on the website.

## Code map

| Path | What it does |
| --- | --- |
| `content/worker/booking.py` | The Worker API at `/api/booking`: the owner's page, meeting types, hours and bookings; the public page, open times and booking; the guest's manage link; emails; reminders; export and purge. |
| `content/www/meet.html`, `meet.js`, `meet.css` | The public page at `/meet`: meeting types, the month calendar and open times in the guest's time zone, the details form, the confirmation with calendar buttons, and the reschedule and cancel page. |
| `content/web/view.html`, `content/web/page.js` | The owner's dashboard page: the link and per-type links, totals, upcoming and past meetings with cancel, and the meeting type, hours and page and email settings. |
| `content/desktop/BookingLink.cpp` | Registers `builtin:booking` so the desktop rail opens the dashboard page. |
| `native/ios/ForkMeshCore/Sources/FMPackages/BookingModels.swift`, `BookingModel.swift`, `PackagesClient+Booking.swift` | The iPhone app's decoding, labels, settings draft and API calls. |
| `native/ios/ForkMesh/Features/Packages/BookingScreen.swift`, `BookingSettingsScreen.swift` | The iPhone Booking screen and its settings. |
| `native/android/core/.../market/Booking.kt` | The Android app's decoding, labels, settings draft and API calls. |
| `native/android/app/.../feature/booking/BookingScreens.kt` | The Android Booking screen and its settings. |
| `app/public/call.html`, `call-room.js` | The meeting room each video booking links to ([meeting rooms](../app/meeting-rooms.md)). |
| `tests/test_booking.py` | Worker tests against SQLite with a fake runtime. |
| `tools/forkmesh_mcp_server.py` (`booking_get_page`, `booking_update_page`) | Agent tools that read and change the account's page, signed with `forkmesh-booking-request-v1` like the apps; see [agent tools](../agents/tools.md). |

## The short link

The link is `https://forkmesh.com/meet?<slug>`, and `?<slug>/<type>` opens one
meeting type directly. A `www.page` route can only be one path segment
(`/meet`), and the website serves `meet.html` for `/meet` alone, so a path
such as `/meet/alice` would need new website and Caddy routing. The query form
works on every instance with no infrastructure change. The page also accepts
`?u=<slug>&t=<type>`, and `?b=<token>` is the guest's private manage link.

The slug defaults to the account name. It is 1 to 63 lowercase letters,
digits and dashes, unique across pages, never another account's name (so a
link never looks like someone else's), and not one of a few reserved words.

Links use the `PACKAGE_BOOKING_LINK_ORIGIN` secret when set, then the request
or public origin; `app.forkmesh.com` is shown as `forkmesh.com`, since `/meet`
is a website route. The owner's email links to the dashboard on
`PACKAGE_BOOKING_APP_ORIGIN`, else the app origin.

## Data

Account-scoped (`dataLifecycle.scope: "account"`, the Water pattern): each
account has one random key in `booking_keys`, wrapped with the host `seal`,
and every settings and booking row is sealed with it.

- `booking_pages`: one row per account, the install receipt. Slug and the
  paused flag are plain columns because the public route looks pages up by
  slug; the display name, headline, hours, meeting types, emails and options
  are sealed.
- `booking_bookings`: one row per meeting with the type, status
  (`confirmed`, `cancelled`), start and end, a blind index of the guest's
  email (for the per-guest limit), the calendar event id, and sealed guest
  name, email, phone, time zone, notes and the meeting's location. A partial
  unique index on `(account_bi, start_at) WHERE status='confirmed'` stops two
  guests taking the same start time at once.
- `booking_rate`: hashed counters for the abuse limits below, pruned by the
  reminder hook.
- `booking_visits`: one row per tracked event on the public page (see
  [Visits](#visits)): kind, meeting type, a short detail, a visitor key, the
  ping stage it raised, and the time. Plain columns, since every value comes
  from a fixed list or pattern.

## Visits

The owner asked to be pinged and to see tracking when anyone opens or clicks
around their booking page, so they know who is about to book (or gave up).
`meet.js` posts one small event to `POST /api/booking/-/p/<slug>/track`
(`{kind, detail, type, session}`, answered `202`) on these:

| `kind` | When | `detail` |
| --- | --- | --- |
| `view` | the page loaded | empty |
| `type` | a meeting type was clicked | empty (`type` is the type id) |
| `month` | the month was changed | `YYYY-MM` |
| `day` | a day was picked | `YYYY-MM-DD` |
| `slot` | a time was picked | the start in milliseconds |
| `form` | Next opened the details form | empty |
| `field` | a form field was first focused | `name`, `email`, `phone`, `notes` |
| `submit` | the booking form was sent | empty |
| `prefs` | time zone or clock changed | `timezone`, `12h`, `24h` |
| `link` | a calendar, manage, copy or credit link was used | `google`, `outlook`, `ics`, `copy`, `manage`, `forkmesh` |
| `leave` | the tab was hidden or closed before booking | the view: `types`, `calendar`, `form` |

Anything else is `invalid_event` (400): an unknown kind, a detail outside its
list or pattern, or `type` naming a type the page does not offer (for other
kinds an unknown type is dropped). Other body fields are ignored. Leaving is
sent with `navigator.sendBeacon` so it survives the tab closing. The
guest's manage link (`?b=`) and any link opened with `&preview` (what the
dashboard's own links add) send nothing.

**Privacy.** No address, user agent or text the visitor typed is stored. The
visitor key is the first 32 hex of `blind("booking-visitor:<account>:<ip>")`,
the same blinding the abuse limits use. In privacy mode the host never gives
the package an address, so the key blinds a random id the page keeps in
`sessionStorage` instead; with neither, the key is empty and the event counts
but cannot ping. The dashboard shows only the key's first six characters.

**Limits and retention.** One connection (or session in privacy mode) may send
120 events an hour and one page takes 1,500 an hour (`rate_limited`, 429,
through `booking_rate`). Each page keeps its newest 2,000 events, pruned on
each `view`, and nothing older than 90 days (the reminder hook also prunes).

**Pings.** A visitor's first `view` in a clock hour sends the owner a
`plugin_activity` ping, "👀 Someone opened your booking page". When that
visitor then picks a day or time, opens the form or fills a field, the same
ping (same dedupe key) is rewritten to "🗓️ Someone is picking a time on your
booking page" without buzzing the phone again. So one visitor makes at most
one ping per page per hour, and one page at most ten an hour. The ping opens
`/dashboard/booking?tab=visits`. "Ping me when someone opens my page or starts
picking a time" (`notifyVisits`, on by default) turns them off; bookings still
ping.

**Summary.** The owner overview carries `activity`: `{days: 30, views,
visitors, interactions, starts, submits, leaves, lastActivityAt, recent}`,
counted over the last 30 days. `interactions` is every kind but `view` and
`leave`, `starts` counts `form`, `submits` counts `submit`, and `recent` is the
50 newest events as `{kind, type, detail, label, visitor, at}` with a ready
sentence in `label`. The dashboard's Visits tab shows it; the export includes
it, and uninstalling deletes the rows.

## Open times

`_slots` walks each day of the requested range in the owner's time zone, cuts
each window of the weekly hours into starts every "start times every" minutes
(the meeting length by default), and keeps a start when it is after the
shortest notice, before the "book up to" horizon, under the daily limit, and
clear of every busy span widened by the gap between meetings. Busy spans are
confirmed bookings and, with "never offer times that clash" on, the owner's
ForkMesh calendar: timed events they own or accepted (including recurring
ones, expanded daily, weekly, monthly and yearly). All-day events do not
block, because they are usually birthdays, holidays and reminders. Booking
and rescheduling recompute the slots for the one requested start, so a time
that is no longer free is refused with `slot_unavailable`.

The public slots route takes `start` and `end` in milliseconds (at most 42
days); the page asks for one month at a time, padded for time zones, and
groups starts by day in the guest's time zone.

## Calendar

With "add booked meetings to my ForkMesh calendar" on, a booking inserts a
personal `calendar_events` row for the owner (sealed with the host `seal`, as
the Calendar does, with 60 and 10 minute reminders). Rescheduling moves it
and cancelling deletes it. The plugin writes only rows it created, and a
missing calendar table is ignored.

## Email

Every email goes through `runtime.send_mailtrap` (Postfix or Mailtrap, the
platform's sender, budgets and mail log) with `tracked: true`, so the
platform adds no footer. The package passes the owner's account as both
`organization` and `account`, so the host's per-sender daily cap counts each
owner's page on its own rather than every booking page together.

- The guest gets a confirmation, a reminder the day before (from
  `deliver_reminders`, skipped when the meeting was booked or moved within a
  day of it), and an email when it is moved or cancelled. Each has Google and
  Outlook links and the private reschedule or cancel link. Replies go to the
  owner's "Guests reply to" address, which defaults to their verified account
  email and can be left empty.
- The owner gets the booking, moves and guest cancellations at "Send my
  booking emails to" (on by default), with the guest's email as reply-to, plus
  a ForkMesh `plugin_activity` ping for each.

## Guest links

The manage token is `<booking id>.<first 24 hex of blind("booking-manage:<id>")>`,
checked with `hmac.compare_digest`. It can read the booking (without the
guest's email), list open times excluding the booking itself, reschedule, and
cancel until the meeting starts. Meeting links for video calls are only shown
after booking; the public page shows the kind of location, not its details.
The page shows a ForkMesh room as **Join the video call (end-to-end
encrypted)**.

## Video rooms

The owner asked that new bookings get a ForkMesh meeting link instead of
"Video call (link to follow)". A meeting type set to **Video call** with no
link of its own now gives every booking its own room; a type with its own
link (Zoom, Meet) keeps using it.

- **Making one.** `_book` draws a room id (`new_id`, 32 hex) and a key
  (SHA-256 of `new_secret`, 64 hex), and stores the location as
  `{kind: "video", detail: "<app origin>/call?<room>#<key>", room: true}`
  inside the sealed booking. `booking_rooms` maps `blind("booking-room:<room>")`
  to the booking and the account; the key is never in a plain column.
- **Where it shows.** The guest's and owner's emails get a **Join the video
  call** button and the link under Where; the calendar event's meeting URL,
  the guest's confirmation and manage page, the dashboard row and both phone
  apps' booking rows show **Join video call**. Rescheduling keeps the room.
- **Window.** The room opens 30 minutes before the start and closes two
  hours after the end. `GET /api/booking/-/call/<room>` answers `{state,
  title, host, accent, minutes, startAt, endAt, opensAt, closesAt, timezone,
  now}` with `state` `early`, `open`, `ended` or `cancelled`, never the guest's
  details. Outside the window the other routes answer 409 `room_<state>`.
- **Tickets.** `POST .../ticket` asks the host's `runtime.call_ticket(room)`
  ([`plugin_call_ticket`](../app/meeting-rooms.md#code-map)), a 60-second
  call-server ticket for the room `plugin:booking:<room>` with a per-join
  account hash, so the call server's limits work as for `/chat` (16 people).
  Without `FORKMESH_CALLS_SECRET` and `FORKMESH_CALLS_NODES` it answers 503
  `calls_unavailable`. One connection gets 90 tickets an hour and a room 240.
- **Relay.** `POST .../signal` with `{box}` stores one sealed signalling box
  (40 to 6,000 base64url characters, else 400 `invalid_signal`) in
  `booking_room_signals` and answers 202; `GET .../signal?after=<seq>` lists
  up to 200 newer boxes, or the last two minutes without `after`. The worker
  cannot open a box. A room keeps its newest 400, a connection may post 1,500
  an hour and a room 6,000, the reminder hook deletes boxes older than six
  hours, and uninstalling deletes rooms and boxes.

## Abuse limits

Booking needs no account, so a stranger can type anyone's email. The page
limits that: a hidden form field traps bots; one email can hold three upcoming
meetings per page; one connection can book six an hour and twenty a day (and
move twelve an hour); one page takes forty bookings a day; and the host's
per-sender mail cap still applies. The guest's email only receives a
confirmation naming the owner and the time.

## Phones

The iPhone and Android apps have the same Booking screen, under Workspace
next to Calendar (route `booking`; `/dashboard/booking?booking=<id>`, the link
in the owner's ForkMesh notification, opens it on that meeting). It covers
what the owner does on the web: create the page, copy, share and open the
link and each meeting type's link, pause the page, the four totals, upcoming
and past meetings grouped by day in the phone's time zone, cancelling with a
message, and the settings for the page, hours, meeting types and email. Export
saves the JSON (Android) or shares it (iPhone); uninstall exports first.
Guests never need the app: `/meet` is a web page that works in any phone
browser.

A booking with a ForkMesh room reads "ForkMesh video call, end-to-end
encrypted" with a **Join video call** link (`joinURL` on iPhone, `joinUrl()`
on Android), hidden once it is cancelled. The video location's hint says to
leave it empty for a private room.

The settings screen edits weekly hours as text times (`HH:MM`); an end of
`24:00` shows as `23:59` and is saved back as `24:00`, as on the web.

Both apps show the [Visits](#visits) summary as one line on the Booking
screen ("Last 30 days: 42 views · 12 visitors · 5 started · 3 sent · last
activity 2h ago", or "No visits in the last 30 days"), formatted by
`PageActivity` (`PageActivity.swift`, `PageActivity.kt`); the list of recent
events stays on the web. The settings screen has the `notifyVisits` toggle
next to the booking email one. An older server without `activity` reads as no
visits.

## Checking it by hand

`tests/test_booking.py` covers the worker; `BookingTests.swift`,
`BookingRouteTests.swift` and `BookingTest.kt` cover the phone apps' models,
API calls and routes. To see the pages, serve
`content/www` with the worker behind `/api/booking` (the tests' `Runtime` is
enough) and open `/meet?<slug>`; the screenshots for this change were taken
with the cached `chrome-headless-shell` run under `systemd-run`.
