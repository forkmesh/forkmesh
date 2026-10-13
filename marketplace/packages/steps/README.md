# Pedometer

Pedometer is an optional ForkMesh package that any account installs for
itself. The ForkMesh iPhone app counts your steps through Apple Health and the
motion coprocessor and sends one total per day to your account; the web
dashboard (`/dashboard/steps`) and the desktop rail page (**Steps**) show the
same days. Nobody else can read them.

## What it does

- **Counts steps on iPhone.** Open **Steps** in the app (You hub, Personal) and
  turn on **Count steps with Apple Health**. The app asks for read access to
  Steps, Walking + Running Distance, Flights Climbed and Active Energy, and
  for Motion & Fitness. While the screen is open the total ticks up live from
  the motion sensor (`CMPedometer`). Under Today the app shows your live
  pace: steps per minute, speed in miles per hour, and the minutes left to
  reach your daily goal at that pace.
- **Syncs one total per day.** The app reads daily sums with a HealthKit
  statistics collection query (so iPhone and Apple Watch steps are not
  counted twice), takes the larger of that and the motion sensor's count for
  the last seven days, and posts them to `POST /api/steps/sync`. It syncs when
  Steps opens (at most every fifteen minutes), on pull to refresh or **Sync
  now** (last 30 days), when the app comes to the foreground, and in the
  background through HealthKit background delivery (hourly at most).
- **Goal and streaks.** A daily step goal (10,000 by default), a 30-day chart
  with the goal line, the last seven days, the 30-day average, the goal
  streak, and the best day.
- **Add steps by hand.** From any client, for a walk your phone missed. Hand
  steps sit next to the phone's total for that day and are added to it.
- **Units.** Distance in kilometres or miles; *Automatic* follows the country
  the request comes from.
- **Heart rate by tapping (iPhone).** The **Heart rate** card has a heart to tap
  with each beat while you feel your pulse. After five steady taps it shows
  the beats per minute, and **Save to Apple Health** writes one Heart Rate
  sample (marked as entered by the user) spanning the taps. The reading goes
  only to Apple Health, never to your ForkMesh account.

## Where it runs

| Surface | Entry | Notes |
| --- | --- | --- |
| Worker | `content/worker/steps.py`, `/api/steps` | Account-scoped, sealed per account. |
| Web | `content/web/view.html`, `page.js` | `/dashboard/steps`. |
| Desktop | `content/desktop/StepsFeature.{h,cpp}` | Rail page **Steps**, signed with `forkmesh-steps-request-v1`. |
| iOS | `native/ios/ForkMesh/Features/Tools/StepsScreen.swift`, `ForkMeshCore/Sources/FMTools/StepsModels.swift` | HealthKit through `ForkMesh/Platform/HealthKitBridge.swift`. |

Android has no Pedometer screen yet; Health Connect support would be its
counterpart.

## API

All routes need a signed-in session (Bearer token, website session, or the
desktop's signed request) and answer 409 `plugin_not_installed` until
`POST /install` has run.

| Method | Path | Body / query | Answer |
| --- | --- | --- | --- |
| GET | `/api/steps?day=YYYY-MM-DD` | | `installed`, `profile`, `today`, `days` (30), `summary` |
| POST | `/api/steps/install` | `goal`, `tzOffsetMinutes` | |
| POST | `/api/steps/uninstall` | | the export, then everything is deleted |
| GET, PUT | `/api/steps/profile` | `goal`, `unitsPreference` (`auto`, `metric`, `imperial`) | |
| POST | `/api/steps/sync` | `source: "health"`, `device`, `days: [{day, steps, distanceM, floors, activeKcal}]` (at most 62) | replaces the phone's reading for each day |
| GET | `/api/steps/days?from=&to=` | | every day in the range, zeros included (at most 400 days) |
| PUT | `/api/steps/days/<day>` | `steps`, `distanceM` | sets the hand-added steps for the day |
| DELETE | `/api/steps/days/<day>?source=manual\|health\|all` | | |
| GET | `/api/steps/export` | | `forkmesh-steps-export-v1` |

A day keeps one reading per source (`health`, `manual`); its total is their
sum. A sync with zero steps and zero distance removes the phone's reading for
that day.

## Data and privacy

Each account gets its own random key at install (`steps_keys`, wrapped by the
host key). A day row (`steps_days`) keeps only the account's blind index and
the date in the clear; the readings are sealed with that key. Uninstalling
returns the export and deletes the rows and the key; deleting the account
does the same through `purge_account_data`. Apple Health keeps its own copy
of your steps: ForkMesh only reads them. A heart rate you tap out is written
to Apple Health only, after the app asks for write access to Heart Rate.
