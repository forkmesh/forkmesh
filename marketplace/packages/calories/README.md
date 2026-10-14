# Calorie Counter

Calorie Counter is an optional ForkMesh package that anyone with a ForkMesh
account can install for themselves. It is personal, not an organization
plugin: your food diary, weigh-in history, goals, and reminders belong to your
account and nobody else can read them. Install it once and the same diary
appears on the web dashboard, the desktop app, and the mobile app.

## What it does

- **Log meals several ways.** Photograph a plate and let the host's Workers AI
  model identify the foods and estimate calories and macros; scan a barcode
  with the browser camera (or type it) and resolve it on Open Food Facts;
  search foods by name; or type an entry by hand. Every suggestion is editable
  before it is saved.
- **Search as you type.** The food search box autocompletes: start typing a
  food or a brand (`trader joes buffalo chicken wrap`, even half-typed as
  `trader joes buffalo chi`) and matching products appear under the field,
  with calories per 100 g and per serving. Pick one with the mouse, or the
  arrow keys and Enter, to choose a portion and log it.
- **Type what you ate.** On the Calories page the Prompt dock's Enter logs to
  the diary instead of starting an agent. The desktop footer prompt box does
  the same on the Calories section, and so does the iOS prompt bar on the
  Calories screen: "2 eggs and toast for breakfast",
  "half a banana", "weighed 72.5 kg", "pizza yesterday". The page splits the
  text on commas and "and", reads amounts ("2", "half a", "a couple of") and
  the meal and day words, and logs the parts that match Quick add foods
  straight away with no AI call. Anything it cannot match goes to the host AI
  as a meal description. A meal photo attached in the dock is estimated like
  the Photo button, with the text as a hint; a photo sent with "receipt" or
  "grocery" in the text, or a file named like a receipt, is read as a
  receipt.
- **Quick add panel.** A side panel on the Diary tab (above the diary on
  narrow screens) lists My foods, foods you have logged more than once, and
  about thirty common foods. One click logs a serving to the meal picked in
  the panel (by time of day unless set), on the "Log to" day. A star saves a
  food to My foods and × removes it. My foods holds at most 60 foods and is
  kept inside the sealed settings row, so it is encrypted like the rest of the
  diary. On the desktop the panel sits right of the diary in a resizable
  split: double-click a food or press Log, and use Save and Remove for My
  foods.
- **Receipts fill My foods.** Upload a photo of a grocery or restaurant
  receipt, from the panel or the dock. The host AI lists the foods on it
  (skipping household goods, tax, and totals) with one typical serving each,
  and new ones are added to the top of My foods. Receipt reads count against
  the same hourly AI allowance as photo estimates. `POST /api/calories/receipt`
  reads one; `PUT /api/calories/quick-foods` replaces the list.
- **One diary, every day on it.** The Diary page lists everything you have
  logged, newest day first, grouped under each day with that day's calories.
  Nothing is hidden behind a day picker: the date field on the toolbar only
  says which day the next photo, barcode, search, or food is logged to, and an
  entry can be moved to another day from its editor. The calorie ring, macro
  bars, and streaks above the list still stand for today.
- **Goals and macros.** Daily calorie, protein, carbohydrate, and fat targets,
  a target weight, and metric or imperial units. Units default to *Automatic*,
  which shows pounds and inches to requests arriving from a country that
  weighs people in pounds and kilograms and centimetres everywhere else; pick
  Metric or Imperial in Goals to pin one regardless of where you are.
- **Reminders.** Up to eight daily reminders ("Log lunch", 12:30) delivered as
  ForkMesh pings from the host's per-minute maintenance run, optionally only
  on days with nothing logged yet.
- **Charts.** Today's calorie ring and macro bars, a 30-day calorie chart
  against the goal, a macro breakdown, and a weight trend with the target
  line. Every chart also has a table view.
- **Challenges (optional).** Weight-loss by percent or kilograms, days within
  the calorie goal, or days logged. Invite friends by their ForkMesh username;
  they install Calorie Counter for themselves and accept, and the leaderboard
  shows derived progress only. Only the creator can edit, close, or delete a
  challenge.

## Where it runs

Every surface talks to the same account API under `/api/calories`, so there
is nothing to sync and nothing to pick: sign in and your diary is there.

- **Web dashboard** (`content/web`): the full page, including the browser
  camera for photos and for live barcode decoding where the browser offers
  `BarcodeDetector`.
- **Desktop** (`content/desktop`): a native rail page registered through the
  generic `NativePackageRegistration` socket. It signs every call with the
  package's own request proof, picks meal photos from disk, takes barcode
  digits by hand (no camera decoder), and stores nothing on the node.
- **iOS and Android apps** (`native/ios/ForkMesh/Features/Tools/CaloriesScreen.swift`,
  `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/tools/CaloriesScreens.kt`,
  reached from Tools): the same API over the account session. Photos come
  from the system photo picker and barcodes are typed; the apps carry no
  camera or barcode-decoder dependency, so live scanning stays a
  web-dashboard feature.

## Apple Health

On iPhone, **Goals > Apple Health > Write meals to Apple Health** writes the
diary to Health as Dietary Energy, Protein, Carbohydrates and Total Fat, one
sample of each per entry, with the food name as the Health food type. It
covers the last 14 days of entries, whichever client logged them, and runs
each time the Calories screen loads the diary: `CaloriesModel.reconcileHealth`
(`FMTools`) compares the entries with the samples ForkMesh wrote before (the
entry id and a version of its numbers and time are in each sample's
metadata), saves new entries, replaces edited ones, and deletes the samples
of entries removed on any client. Samples other apps wrote are never touched.
The switch is kept per device (`health.calories.enabled`); turning it off
leaves what was written in Health. The phone does the writing from the
diary it already reads; the worker is not involved.

## Data and privacy

The manifest declares `dataLifecycle.scope: "account"`. Installing creates
your profile row and mints a random 256-bit **diary key** for your account.
That key is stored only in wrapped form, encrypted under the Worker's own
`DATA_KEY` secret, and every row you own is encrypted with it: food names,
brands, portions, notes, barcodes, the calorie and macro numbers, your
weigh-ins, each day's totals, your goals, your reminder times and labels,
your optional USDA FoodData Central key, and the challenges you create.

Nothing is left in a plain column for SQL to add up. What remains beside the
envelope is only what a query needs to find the row again: your account's
blind index, the date, the row id, and a single flag telling the reminder
sweep that you have some reminder switched on. Because SQL cannot sum an
encrypted column, each day keeps its own encrypted totals row, so charts,
streaks, and challenge standings still cost one read per day.

What this buys you: a copy of the database on its own opens nothing, the
Worker secret on its own opens nothing, and a single leaked diary key opens
exactly one diary — not everybody's. It is not end-to-end encryption: the
Worker unwraps your key while it is serving your request, because the same
code has to draw your charts, deliver your reminders on a schedule, and score
a challenge you joined. Uninstalling or deleting your account deletes the key
along with the rows, so anything that outlives them by accident stays closed.

Photos are sent to the AI model for one analysis and never stored. Challenge
standings expose derived progress (percent lost, days logged), never the
diary; a leaderboard is computed inside the request that serves it. The
reminder ledger records only an irreversible digest of your account, your
local day, and the reminder time, so it never says when you eat.

Barcode and search lookups reach only the origins declared in the manifest:
Open Food Facts always (`world.openfoodfacts.org` for barcodes,
`search.openfoodfacts.org` for name search), and USDA FoodData Central once
you save your own free FoodData Central API key. A search sends the words you
typed and nothing about you. Successful barcode lookups are cached for thirty
days in a product cache shared by all accounts (product data only, never who
looked it up). That cache is public food data, so it is sealed with the host
key rather than anybody's diary key.

You can export your data at any time. Uninstalling downloads that export
first, then deletes your diary, weights, goals, reminders, settings, and the
challenges you created. Deleting your ForkMesh account removes the same rows.

Rows created by older versions (organization-scoped, or sealed under one
instance-wide key) are migrated automatically onto the account and re-sealed
under its diary key, either when the account next opens Calorie Counter or by
the per-minute reminder sweep.

Food name search uses Open Food Facts' Elasticsearch service,
`search.openfoodfacts.org` (open data, fast), rather than the rate-limited
classic `/cgi/search.pl` endpoint.
One search asks it up to four questions at once: the words as typed; the
words with the last one treated as a prefix of the product name, so a
half-typed word still matches; and the rest of the words limited to brands
starting with the first word, once after the first word and once after the
first two, so `trader joes buffalo chicken wrap` searches Trader Joe's
products for "buffalo chicken wrap". The Worker merges the answers (and USDA's,
with a key), ranks them by how many typed words appear in the brand and name
(apostrophes ignored, so `joes` matches "Joe's"), and returns the top twenty.
The classic endpoint is kept only as a fallback when the search service does
not answer. The web page, desktop, and mobile clients wait for a quarter to a
third of a second of quiet typing before asking and remember recent answers,
so backspacing never repeats a search.
