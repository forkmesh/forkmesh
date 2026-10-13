# Water Intake

Water Intake is an optional ForkMesh package that any account installs for
itself: a log of the water and other drinks you have each day, a daily goal
and a streak. The same log appears on the web dashboard (`/dashboard/water`),
the desktop rail page (**Water**) and the iPhone app (**Water**), and on
iPhone every drink is shared with Apple Health.

## What it does

- **Quick add.** One tap logs a glass: 250, 500 and 750 ml, 8, 16 and
  24 fl oz for accounts shown in imperial units, or 1, 2 and 3 cups. **Other drink** / **Add
  drink** takes any amount of water, sparkling water, tea, coffee, juice,
  milk or something else.
- **Typed on the desktop.** On the Water section the footer prompt box logs a
  drink instead of starting an agent: "500 ml", "a glass of water", "12 oz
  tea", "2 cups", "1.5 l".
- **Units.** Automatic (fluid ounces in the US, Liberia and Myanmar,
  millilitres elsewhere), millilitres, fluid ounces or cups. A cup is the US
  cup of 8 fl oz (about 237 ml); amounts in cups show to one decimal place.
  Everything is still stored in millilitres, so switching units never changes
  the log.
- **Goal and streak.** A daily goal (2 litres by default), the 30-day chart,
  the 30-day average, and the goal streak.
- **Apple Health, both ways.** With **Share with Apple Health** on, the app
  writes every drink logged anywhere (web, desktop or phone) in the last
  seven days to Health as Water, and brings in water that other apps (a
  smart bottle, a watch app) wrote there. Edits and deletions follow: a drink
  deleted on the web disappears from Health the next time the app opens
  Water.

## Where it runs

| Surface | Entry | Notes |
| --- | --- | --- |
| Worker | `content/worker/water.py`, `/api/water` | Account-scoped, sealed per account. |
| Web | `content/web/view.html`, `page.js` | `/dashboard/water`. |
| Desktop | `content/desktop/WaterFeature.{h,cpp}` | Rail page **Water**, signed with `forkmesh-water-request-v1`. |
| iOS | `native/ios/ForkMesh/Features/Tools/WaterScreen.swift`, `ForkMeshCore/Sources/FMTools/WaterModels.swift` | HealthKit through `ForkMesh/Platform/HealthKitBridge.swift`. |

## API

| Method | Path | Body / query | Answer |
| --- | --- | --- | --- |
| GET | `/api/water?day=YYYY-MM-DD` | | `installed`, `profile` (with `presetsMl`), `today`, `entries` (that day), `days` (30), `summary` |
| POST | `/api/water/install` | `goalMl`, `tzOffsetMinutes` | |
| POST | `/api/water/uninstall` | | the export, then everything is deleted |
| GET, PUT | `/api/water/profile` | `goalMl`, `unitsPreference` (`auto`, `metric`, `imperial`, `cups`) | |
| GET, POST | `/api/water/entries` | `from`, `to` / `day`, `amountMl`, `drink`, `at`, `source`, `externalId` | 201 with the entry; 200 with `duplicate: true` for an `externalId` already logged |
| GET, PATCH, DELETE | `/api/water/entries/<id>` | `amountMl`, `drink`, `note`, `day`, `at` | |
| POST | `/api/water/import` | `entries: [{day, amountMl, at, externalId}]` (at most 200) | `imported`, `skipped` |
| GET | `/api/water/export` | | `forkmesh-water-export-v1` |

Amounts are whole millilitres, 1 to 5000 per drink; a day holds at most 200
drinks.

## How the Apple Health sync works

The app does the whole reconcile in `WaterModel.reconcileHealth`
(`FMTools`), over the last seven days:

1. Reads the Water samples in Health. Samples written by other apps whose
   HealthKit UUID is not yet an entry's `externalId` go to `POST /import`
   with `source: "health"`.
2. Every other entry becomes one Water sample written by ForkMesh, with the
   entry id and a version (amount and time) in its metadata
   (`ForkMeshEntry`, `ForkMeshVersion`). `HealthReconcile.plan` compares them
   with ForkMesh's own samples: missing ones are saved, changed ones replaced,
   and samples whose entry is gone deleted.

Drinks that came from Health are never written back, and ForkMesh never
deletes another app's samples.

## Data and privacy

Each account gets its own random key at install (`water_keys`). An entry row
(`water_entries`) keeps the account's blind index, the day and the logging
time in the clear, and a blind digest of the Health UUID for de-duplication;
the amount, drink, note and the UUID itself are sealed with the account's key.
Uninstalling returns the export and deletes the rows and the key; account
deletion does the same through `purge_account_data`.
