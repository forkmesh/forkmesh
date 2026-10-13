# Sleep

Sleep is an optional ForkMesh package that any account installs for itself.
The ForkMesh iPhone app works out how long you slept from the time your phone
lay still by your bed while you were not using it, uses the sleep your Apple
Watch (or another sleep app) records in Apple Health when there is some, and
sends one entry per night to your account. The web dashboard
(`/dashboard/sleep`) and the desktop rail page (**Sleep**) show the same
nights. Nobody else can read them.

## What it does

- **Detects sleep from your phone.** Open **Sleep** in the app (You hub,
  Personal) and turn on **Track sleep with my phone**. The app asks for
  Motion & Fitness and for read access to Sleep in Apple Health. Leave the
  phone by your bed at night; in the morning, the stretch it lay still and
  unused becomes last night's sleep.
- **Uses Apple Health when it knows better.** Sleep that an Apple Watch or
  another app wrote to Apple Health (in bed, asleep, core, deep, REM, awake)
  is summed per night and shown instead of the phone's guess, with deep and
  REM minutes when they are there.
- **Writes back to Apple Health (optional).** **Save detected sleep to Apple
  Health** writes each night the phone detected (and Apple Health has no
  other sleep for) as an *Asleep* sample, so the Health app and other apps
  see it too. A later detection of the same night replaces the sample.
- **Goal, schedule and debt.** A sleep goal (8 hours by default), your usual
  bedtime and wake time (23:00 and 07:00), a 30-night chart with the goal
  line, the 7-night average and sleep debt, the 30-night average, the goal
  streak, and your average bedtime (with how much it wanders) and wake time.
- **Add a night by hand.** From any client, for a night the phone was not by
  your bed. A hand-added night replaces what the phone or Apple Health
  recorded for it until you remove it.

## How the phone detects sleep

The phone has no reliable way to know you are asleep, so Sleep reads two
signals it does have and treats a long, quiet stretch around your schedule as
sleep:

1. **The phone lay still.** Core Motion keeps about seven days of activity
   history (`CMMotionActivityManager.queryActivityStarting`), and the app can
   read it at any time without running in the background. A phone lying on a
   nightstand reports *stationary* with medium or high confidence; a phone in
   your hand usually reports low confidence or another activity, so only
   confident stationary stretches count.
2. **You were not using ForkMesh.** The app logs when it was in the
   foreground (`SleepUsageLog`, kept for eight days on the device) and cuts
   those times out. iOS does not tell an app when other apps are used, so
   motion is the main signal.

For each night, keyed by the day you woke up, the app looks in a window from
four hours before your bedtime to five hours after your wake time, then:

- joins still stretches separated by short gaps (up to 20 minutes, or up to
  75 minutes inside your bedtime–wake time schedule, such as checking your
  phone at 3 AM) and counts those gaps as awake time and interruptions;
- keeps stretches of at least three hours that overlap your schedule by an
  hour or more (a stretch far from your schedule needs five hours, so a phone
  left on a desk at work does not count);
- picks the one overlapping your schedule most, and subtracts ten minutes for
  falling asleep;
- rates it *high* (mostly inside your schedule, few interruptions, mostly
  confident), *medium* or *low* confidence. Low-confidence nights are shown
  but never written to Apple Health.

Changing your bedtime and wake time moves the window. The phone reads the
schedule from your account when Sleep opens.

Limits: a phone that lies still while you read or watch TV in bed counts as
sleep, and a night away from the phone counts as nothing. Add those nights by
hand, or wear an Apple Watch: its Apple Health sleep wins over the phone.

## When it syncs

When Sleep opens (at most every fifteen minutes; pull to refresh or **Sync
now** forces it), when the app comes to the foreground (the last three
nights), and in the background when Apple Health has new sleep (HealthKit
background delivery). Each sync sends the last seven nights' phone readings
and Apple Health readings. A night the phone found nothing for clears the
phone's earlier reading; Apple Health readings are only sent when Health
returned sleep samples, so a locked phone or missing permission never erases
them.

## Where it runs

| Surface | Entry | Notes |
| --- | --- | --- |
| Worker | `content/worker/sleep.py`, `/api/sleep` | Account-scoped, sealed per account. |
| Web | `content/web/view.html`, `page.js` | `/dashboard/sleep`. |
| Desktop | `content/desktop/SleepFeature.{h,cpp}` | Rail page **Sleep**, signed with `forkmesh-sleep-request-v1`. |
| iOS | `native/ios/ForkMesh/Features/Tools/SleepScreen.swift`, `ForkMeshCore/Sources/FMTools/SleepModels.swift`, `SleepDetector.swift` | Core Motion and HealthKit through `ForkMesh/Platform/HealthKitBridge.swift`. |

Android has no Sleep screen yet; Health Connect's sleep sessions and the
activity recognition API would be its counterpart.

## API

All routes need a signed-in session (Bearer token, website session, or the
desktop's signed request) and answer 409 `plugin_not_installed` until
`POST /install` has run.

| Method | Path | Body / query | Answer |
| --- | --- | --- | --- |
| GET | `/api/sleep?night=YYYY-MM-DD` | | `installed`, `profile`, `lastNight`, `nights` (30), `summary`, `sourceLabels` |
| POST | `/api/sleep/install` | `goalMinutes`, `bedtime`, `wakeTime`, `tzOffsetMinutes` | |
| POST | `/api/sleep/uninstall` | | the export, then everything is deleted |
| GET, PUT | `/api/sleep/profile` | `goalMinutes` (0–960), `bedtime`, `wakeTime` (`HH:MM`), `tzOffsetMinutes` | |
| POST | `/api/sleep/sync` | `source` (`phone`, `health`), `device`, `tzOffsetMinutes`, `nights: [{night, bedAt, wakeAt, asleepMinutes, awakeMinutes, interruptions, confidence, deepMinutes, remMinutes, coreMinutes}]` (at most 62) | replaces that source's reading for each night |
| GET | `/api/sleep/nights?from=&to=` | | every night in the range, empty ones included (at most 400 days) |
| PUT | `/api/sleep/nights/<night>` | `bedAt`, `wakeAt` (epoch ms), `awakeMinutes` | sets the hand-added night |
| DELETE | `/api/sleep/nights/<night>?source=manual\|phone\|health\|all` | | |
| GET | `/api/sleep/export` | | `forkmesh-sleep-export-v1` |

A night keeps one reading per source (`manual`, `health`, `phone`) and shows
the first in that order. `bedAt` and `wakeAt` are epoch milliseconds, at most
20 hours apart; `asleepMinutes` defaults to the span minus `awakeMinutes`. A
sync with no sleep for a night removes that source's reading. When no night
is given, the overview uses today in the account's time zone, which every
sync and profile save refreshes. Average bedtime and wake time are circular
means, so bedtimes either side of midnight average correctly.

## Data and privacy

Each account gets its own random key at install (`sleep_keys`, wrapped by the
host key). A night row (`sleep_nights`) keeps only the account's blind index
and the date in the clear; the readings are sealed with that key. Uninstalling
returns the export and deletes the rows and the key; deleting the account
does the same through `purge_account_data`. The phone's motion history and
its log of when ForkMesh was open never leave the device; only the nights
worked out from them are sent. Apple Health keeps its own copy of your sleep:
ForkMesh only reads it, and writes only the nights it detected when you turn
that on.
