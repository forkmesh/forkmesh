# Sugar Break

Sugar Break is an optional ForkMesh Health package that any account installs
for itself. You press **Start sugar break**, pick how long it lasts (one month,
30 days, unless you choose otherwise), and then tap **+1** every time you skip
sugar. The same break appears on the web dashboard (`/dashboard/sugar-break`),
the desktop rail page (**Sugar Break**) and the iPhone app (**Sugar Break**),
and on iPhone the dietary sugar logged in Apple Health shows beside each day.

## What it does

- **Start a break.** One button starts it. Lengths offered: 7, 14, 21, 30
  (the default, shown as "1 month"), 60 and 90 days; the API takes any
  length from 1 to 365 days, and the default can be changed in the profile.
  Starting a new break ends the one that is running.
- **+1 every time you skip sugar.** Each tap of **+1  I skipped sugar** is
  one `skip` event, so a day can collect many (every craving you turned
  down). **I had sugar** records a `slip`. Any event can be undone.
- **Days.** A day with at least one +1 and no slip is *sugar-free*; a day
  with a slip is *slip*; a past day with nothing logged is *open*; later
  days are *upcoming*.
- **Progress.** Day N of the break, sugar-free days out of its length
  (percent), the +1 total, the current streak of sugar-free days (today
  still open does not break it), the best streak, and days left.
- **End and history.** When the last day passes the break is *completed*;
  **End break early** marks it *ended*. Older breaks stay listed with their
  results (the last 20).
- **Apple Health.** On iPhone, with **Apple Health** turned on, the app reads
  Dietary Sugar (grams) from Health for each day of the break and shows it
  beside the day; when Health has sugar for today the card says so, so you
  can record the slip with one tap. ForkMesh never writes to Health for this
  package, and never marks a day itself: what counts is what you log.
- **No-sugar contract.** On iPhone, **Send a no-sugar contract** draws a
  contract card (dates, day count, one promise line, 1 to 12 signature lines
  with optional names) and sends it as a PNG with a short message through the
  share sheet. It covers the rest of the active break, or today plus the
  selected length when no break runs; the card is built on the phone and
  nothing is stored.

## Where it runs

| Surface | Entry | Notes |
| --- | --- | --- |
| Worker | `content/worker/sugar_break.py`, `/api/sugar-break` | Account-scoped, sealed per account. |
| Web | `content/web/view.html`, `page.js` | `/dashboard/sugar-break`. |
| Desktop | `content/desktop/SugarBreakFeature.{h,cpp}` | Rail page **Sugar Break**, signed with `forkmesh-sugar-break-request-v1`. |
| iOS | `native/ios/ForkMesh/Features/Tools/SugarBreakScreen.swift`, `SugarContractSheet.swift`, `ForkMeshCore/Sources/FMTools/SugarBreakModels.swift`, `SugarContract.swift` | HealthKit read of `dietarySugar` through `ForkMesh/Platform/HealthKitBridge.swift`; the contract image renders with `ImageRenderer`. |

## API

Every request may pass `?day=YYYY-MM-DD`, the client's local today; without
it the Worker uses the profile's `tzOffsetMinutes`.

| Method | Path | Body | Answer |
| --- | --- | --- | --- |
| GET | `/api/sugar-break` | | `installed`, `profile`, `lengths`, `current` (newest break), `days` (every day of it), `today`, `events` (today's), `summary`, `history` |
| POST | `/api/sugar-break/install` | `defaultLengthDays`, `tzOffsetMinutes` | |
| POST | `/api/sugar-break/uninstall` | | the export, then everything is deleted |
| GET, PUT | `/api/sugar-break/profile` | `defaultLengthDays` (1–365, 30), `tzOffsetMinutes` | |
| POST | `/api/sugar-break/breaks` | `lengthDays`, `startDay` | 201 with the break |
| PATCH, DELETE | `/api/sugar-break/breaks/<id>` | `lengthDays`, `end: true` | delete removes its events too |
| POST | `/api/sugar-break/events` | `kind` (`skip` or `slip`), `day`, `note` | 201; 409 `no_active_break`, 400 `day_outside_break` |
| DELETE | `/api/sugar-break/events/<id>` | | |
| GET | `/api/sugar-break/export` | | `forkmesh-sugar-break-export-v1` |

A day holds at most 200 events. Events can be logged for any day of the
running break up to today, so a missed day can be filled in later.

## Data and privacy

Each account gets its own random key at install (`sugar_break_keys`). A break
row keeps the account's blind index and the time it was started in the clear;
its start day, length and end are sealed. An event row keeps the break, the
day and the logging time in the clear; the kind, note and time are sealed.
Apple Health sugar never leaves the phone. Uninstalling returns the export and
deletes the rows and the key; account deletion does the same through
`purge_account_data`.
