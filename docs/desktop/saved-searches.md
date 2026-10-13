# Desktop: saved searches and security alarms

Part of [the desktop docs](README.md).

## Saved searches

The search results page (`MainWindowRepoDetail.cpp`, `buildSearchResultsSection`)
keeps a column of saved searches. `SavedSearchStore` (`src/SavedSearchStore.{h,cpp}`)
writes them to `saved-searches.json` under the data root rather than
QSettings, because the seen-key lists grow with the result counts and every
QSettings write rewrites the whole ini on the GUI thread.

- A saved search is its query plus the repository it ran against
  (`owner/name`, or empty for all repositories).
- New results are detected by identity, not count: each run hands the store
  one short key per result (`SavedSearchStore::resultKey`), and the store
  remembers the keys it has seen. Log lines get no key, so a ping filed about
  a saved search never matches the next run.
- The bell turns on pings: a background check (`checkSavedSearchAlerts`,
  every 15 minutes while any search has its bell or the security flag on)
  reruns the search off the GUI thread and files one ping per run that found
  never-seen results.

## Security-pertinent searches (2026-09-30)

A saved search can be marked **Security pertinent**: the checkbox sits in the
save prompt next to "Ping me about new results" and on every row in the saved
search column. A marked row carries a "Security" badge, which turns red and
reads "Alarm" while that search is alarming.

While a security-pertinent search has one or more results that have not been
acknowledged:

- a red banner (`#securityAlarmBanner`, built by `buildSecurityAlarmBanner`
  in `MainWindowSecurityAlarm.cpp`) sits under the wallet notices on every
  page. It names the first alarming search, its result count and scope, and
  how many other searches are alarming, with **Open results** (runs the first
  alarming search) and **Acknowledge** (acknowledges every alarm shown);
- the Error interface cue plays every 3 seconds. It plays through
  `UiSounds::preview`, so it sounds even when interface sounds are switched
  off in Settings, but never on the offscreen/minimal platforms or in test
  mode (`UiSounds::defaultEnabled`). The volume preference still applies;
- when an alarm is first raised by a run, the window is flagged for
  attention and a warning Ping is filed with a `search` link, so it reaches
  the Pings list, the OS card and a paired phone. Alarms restored at startup
  show the banner and sound, but file no second Ping.
- the Ping never contains the searched words (2026-10-02): it reads
  "Security alarm: a security-pertinent saved search has N results" and
  links to the search by id. A bell Ping for a security-pertinent search is
  worded the same way ("A security-pertinent saved search found N new
  results"). Pings persist in `pings.json` and travel to a paired phone, so
  the words a security search looks for must not be copied into them. Every
  saved-search Ping is also passed through the hidden-from-search redaction.
  The banner still names the search, because it is only drawn on screen.

### State and acknowledgement

`SecurityAlarmState` (`src/SecurityAlarmState.{h,cpp}`) is the pure logic:
from the store's searches it returns the active alarms and which of them
were newly raised or cleared since the last evaluation. It plays nothing and
touches no widget, so `tests/test_security_alarm_state.cpp` covers it with no
audio device.

A search is alarming when it is security pertinent, its last run found at
least one result, and its `resultFingerprint` differs from its
`acknowledgedFingerprint`.

- Every run stores `resultFingerprint`, a hash of the sorted, de-duplicated
  result keys.
- Acknowledge copies the current fingerprint into `acknowledgedFingerprint`.
- A later run whose results are all ones the store has seen before (results
  dropping out, or acknowledged results coming back) carries the
  acknowledgement forward to its new fingerprint. A run with any never-seen
  result does not, so new results re-raise the alarm.
- Zero results, unchecking the flag or removing the search stops the alarm.
  Checking the flag again with unacknowledged results raises it again.
- Files written before the flag existed load with `securityPertinent` false.
  A search that ran before fingerprints existed gets a stand-in fingerprint
  from its last run time and total, so it can still be acknowledged.

### When the alarm is evaluated

There is no new display polling (see the 2026-09-19 rule against periodic
display timers). The alarm is re-evaluated only on events that change the
results or the flag:

- when the main shell is built (the persisted state from the last run);
- after a watched run on the results page (`finishSearchPageRun`);
- after each background check run;
- on save, removal, toggling the flag, and Acknowledge.

Turning the flag on starts a background check at once, and the first
`scheduleSavedSearchAlerts` call at startup queues one check a minute later
when any search is security pertinent; after that the existing 15-minute
saved-search check refreshes the results. The 3-second tone timer runs only
while an alarm is active and stops with it.

## Exclusions (2026-10-02)

Each saved search row has **Exclude…**, which takes a comma-separated list
of words (semicolons and new lines also separate them). A result is left out
when its repository name (`owner/name`), title, path or matched text
contains any of them, ignoring case. Typical use: keep a security-pertinent
search for a leaked name from alarming on a fork or vendored copy that
legitimately carries it.

- The list is stored per search as `excluded` in `saved-searches.json`
  (`SavedSearchStore::parseExcluded` trims and de-duplicates it); files from
  before it load with none.
- Excluded results are dropped before `recordRun`, so they never count, never
  ping and never raise or keep a security alarm. On the results page they are
  hidden, bucket counts drop them, and the status line says how many were
  excluded. Each row stores its match text in `kSrMatchTextRole`.
- A row with exclusions shows "Excluding …". Changing the list reruns the
  search when it is the one on screen, otherwise starts a background check
  when its bell or security flag is on.
- Removing an exclusion brings its results back as a changed result set, so a
  security-pertinent search alarms again even if those results had been
  acknowledged before they were excluded.

## Hidden from search and logs (2026-10-02)

The saved search column shows "Hidden from search and logs: ..." with
**Edit hidden…**. The list (comma-separated, not case-sensitive) lives in
`search-exclusions.json` under the data root, separate from
`saved-searches.json` so a desktop build that predates it cannot drop it
when it rewrites that file. `SavedSearchStore::loadHiddenTerms` /
`saveHiddenTerms` read and write it; `src/HiddenTerms.h` holds the loaded
list behind a read/write lock so worker threads can use it.

- Search: a repository whose `owner/name`, description, clone or mirror path
  matches is left out of the repository list, the git catalog (branches,
  worktrees, tags, files) and worktree code, in both the results page and the
  Ctrl+K overlay. Any other result whose repository, title, path or text
  matches is dropped too, and saved-search runs treat the list as extra
  exclusions for every search.
- Logs: `logSystemFrom` and the action telemetry journal
  (`ActionTelemetry`, `logs/diagnostics/actions.jsonl`) replace each term
  with `[hidden]` before the line is stored, shown or published. Lines
  already on disk are not rewritten by the desktop.
- The list is loaded at the top of the `MainWindow` constructor, before the
  telemetry writer starts.

## Actions variable values hidden everywhere (2026-10-07)

Under the hidden list, **Also hide every Actions variable value**
(`#searchHideVariablesCheck`) treats the value of every variable and secret
in Settings > Secrets & Coves (`ActionStore::variables`) as one more hidden
term, so a token or key never shows up in any search, saved-search run, log
line, telemetry entry or Ping. It is on by default and stored in QSettings
as `search/hideVariableValues`.

- The values are kept in `HiddenTerms.h` apart from the editable list:
  `terms()` is only what the user typed (the caption and Edit hidden show
  it), `activeTerms()` adds the variable values and is what `hides`,
  `redact` and the saved-search exclusions use. The caption only counts the
  values ("3 Actions variable values"); it never shows them.
- `refreshVariableValues` runs in the `MainWindow` constructor and inside
  `ActionStore::setVariables`, so adding, editing, deleting or importing a
  variable (Settings, the phone's `secrets.set`, a control-node restore)
  updates the list at once.
- Values are trimmed and de-duplicated ignoring case. Values shorter than 8
  characters (`kMinVariableValueLength`) are skipped, since `true`, `main`
  or a port number would hide ordinary text everywhere. Longer values are
  redacted first, so a value that contains a shorter one is replaced whole.
- A non-secret variable such as a zone name (`forkmesh.com`) is hidden too
  while the box is ticked; untick it to search for such values.

## Refresh

**Refresh** next to the "Saved searches" heading reruns every saved search
in the background (not only those with a bell or the security flag) and
updates its counts. It reads "Refreshing…" until the queue drains. Pings and
alarms follow the same rules as the 15-minute check.

## Records from every repository (2026-10-02)

Issues, pull requests, discussions, projects and milestones are searched in
every repository on the node, the open one first, not only in the open
repository. A hit from another repository carries `owner/name` in its detail
line (and in front of a milestone's title), and opening it switches to that
repository. Its saved-search key adds `owner/name` to the record number, so
keys for the open repository's records are unchanged.

## Scrub a term (2026-10-13)

**Scrub…** removes a word from a repository's source and history. It sits in
the results page header (for the search on screen) and on each saved search
row (`#savedSearchScrubButton`, next to Exclude…). Both open a confirmation
dialog (`scrubSearchTerm` in `MainWindowShortcuts.cpp`) and then run the
**Scrub a term from source and history** repository shortcut,
`.forkmesh/shortcuts/scrub-term.sh`, in the Shortcuts tab, where its output,
progress and Stop button live like any other shortcut run.

- Target: the saved search's repository, or the open one for a search with
  no repository. The buttons are hidden or disabled when it has no local
  checkout (`scrubTargetRepoIndex`).
- Script: the repository's own `.forkmesh/shortcuts/scrub-term.sh` when it has
  one, otherwise the copy built into the app (`:/shortcuts/scrub-term.sh` in
  `resources/icons.qrc`), written to `<cache>/shortcuts/scrub-term.sh`, so it
  works in every repository (`scrubTermShortcutPath`).
- Dialog: replacement text (default `[redacted]`), **Also rewrite history on
  every branch** (on by default), and the term typed again before **Scrub**
  is enabled. A second scrub cannot start while one is running.
- The desktop passes the choices as `SCRUB_TERM`, `SCRUB_REPLACEMENT`,
  `SCRUB_HISTORY` and `SCRUB_BACKUP_DIR` (`<data>/scrub-backups`);
  `runShortcut` takes extra environment for this. Running the card from the
  Shortcuts tab without them prints its usage and changes nothing.

What the script does (`app/tests/test_scrub_term_shortcut.py`):

1. Refuses a term under 3 characters, a term or replacement with a newline,
   `<` or `>`, a replacement that contains the term, a detached HEAD, or
   uncommitted changes to tracked files.
2. Replaces the term, ignoring case, in every tracked text file (no NUL byte)
   and commits "Scrub a term from the source". Paths that contain it are
   listed, not renamed.
3. With history on: writes every ref to a `git bundle` backup, then pipes
   `git fast-export` of every ref except `refs/stash` through a filter that
   replaces it in file contents, commit and tag messages and author,
   committer and tagger lines, into `git fast-import --force`. Binary blobs
   are skipped with the same NUL test as step 2, so the rewritten HEAD tree
   equals the checkout and `git status` stays clean. Reflogs of the rewritten
   refs, HEAD and every worktree HEAD are expired and `git gc --prune=now`
   drops the old objects; then every remaining blob, commit and tag is
   checked and the result is printed.

What it leaves, and says so: `refs/stash` and its reflog (the stash stack is
shared with other sessions), files checked out in other worktrees, remotes
and other clones (no push), and the backup bundle, which still holds the
term and is the way back (`git bundle list-heads` shows its refs, `git fetch`
brings them back). Delete the
bundle once the result is checked. Rewriting history changes every commit id
from the first that held the term; this is the user-confirmed history
rewrite case in [destructive actions](../engineering/destructive-actions.md).

## Tests

- `forkmesh-saved-search-tests` (`tests/test_saved_search_store.cpp`):
  persistence of the flag and both fingerprints, fingerprint stability,
  loading a file from before the flag, parsing, matching and persisting
  exclusions, and turning Actions variable values into hidden terms.
- `forkmesh-security-alarm-tests` (`tests/test_security_alarm_state.cpp`):
  raise, no double raise, acknowledge, re-raise on new results, carry-over
  when results drop or return, stop on zero results and on unchecking, and
  that non-pertinent searches never alarm.
- `forkmesh-window-tests` (`checkSavedSearches` in
  `tests/test_window_resize.cpp`): ticking the row checkbox raises the banner
  and tone; Acknowledge clears both; excluding the repository name drops
  every result and clearing the exclusions brings them back; Refresh reruns
  every search; a hidden repository name empties the page and is redacted
  from the log; an Actions variable value is counted in the caption without
  being shown, redacted from the log, and shown again once unticked.

## Native apps

The iOS and Android apps keep their own saved searches (query, scope, last
count) with no security flag yet; the follow-up is tracked under 1.7 in
[../native/PROGRESS.md](../native/PROGRESS.md). The Ping filed by the desktop
already reaches a paired phone.
