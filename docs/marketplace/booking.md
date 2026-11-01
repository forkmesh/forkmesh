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
| `tests/test_booking.py` | Worker tests against SQLite with a fake runtime. |

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

The settings screen edits weekly hours as text times (`HH:MM`); an end of
`24:00` shows as `23:59` and is saved back as `24:00`, as on the web.

## Checking it by hand

`tests/test_booking.py` covers the worker; `BookingTests.swift`,
`BookingRouteTests.swift` and `BookingTest.kt` cover the phone apps' models,
API calls and routes. To see the pages, serve
`content/www` with the worker behind `/api/booking` (the tests' `Runtime` is
enough) and open `/meet?<slug>`; the screenshots for this change were taken
with the cached `chrome-headless-shell` run under `systemd-run`.
