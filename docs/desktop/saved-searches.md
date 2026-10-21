# Desktop: saved searches and security alarms

Part of [the desktop docs](README.md).

## Saved searches

The search results page (`MainWindowRepoDetail.cpp`, `buildSearchResultsSection`)
keeps a column of saved searches. `SavedSearchStore` (`src/SavedSearchStore.{h,cpp}`)
writes them to `saved-searches.json` under the data root (not QSettings, whose
full-file rewrites are too costly for growing seen-key lists).

- A saved search is a query plus a repository scope (`owner/name`, or empty for
  all repositories).
- New results are detected by identity: each run hands the store one short key
  per result (`SavedSearchStore::resultKey`), and the store remembers seen keys.
  Record keys from other repositories include `owner/name`.
- Issues, pulls, discussions, projects and milestones are searched in every
  repository on the node, the open one first.
- **Project files** (`project-file`, `scanProjectFileHits` in
  `MainWindowSearch.cpp`) searches every file on disk in the repository's
  working folder: `git grep --untracked`, so uncommitted edits and new files
  count, not only the committed tree that **Code** reads. Files matched by
  `.gitignore` are skipped: including them took over 60 s on the ForkMesh
  checkout (build trees, `.worktrees/`) against about 4 s without. A mirror
  (no `.git` entry, so no working folder) and an unscoped search have no
  project folder. The results page and background runs both search it, and a
  hit opens the file from disk.
- The bell enables pings: `checkSavedSearchAlerts` reruns searches off the GUI
  thread every 15 minutes while any search has its bell or security flag on,
  and files one ping per run that found never-seen results.
- **Refresh** reruns every saved search in the background with the same ping
  and alarm rules.

## Security-pertinent searches

A saved search can be marked **Security pertinent**. While such a search has
unacknowledged results:

- a red banner (`#securityAlarmBanner`, `buildSecurityAlarmBanner` in
  `MainWindowSecurityAlarm.cpp`) shows on every page with **Open results** and
  **Acknowledge** (the octicon `check`, `resources/icons/octicons/check.svg`;
  before it was vendored the button drew no icon and Qt logged
  `Cannot open file ':/icons/codicons/check.svg'`);
- the Error cue plays every 3 seconds through `UiSounds::preview` (even with
  interface sounds off, but never offscreen or in test mode);
- when first raised by a run, the window requests attention and a warning Ping
  is filed. Alarms restored at startup file no second Ping.
- Pings never contain the searched words ("Security alarm: a security-pertinent
  saved search has N results"), because pings persist in `pings.json` and sync
  to a paired phone. Every saved-search Ping also passes through the
  hidden-term redaction. Only the on-screen banner names the search.

### State and acknowledgement

`SecurityAlarmState` (`src/SecurityAlarmState.{h,cpp}`) is pure logic: from the
store's searches it returns active, newly raised and cleared alarms.

A search alarms when it is security pertinent, its last run found results, and
its `resultFingerprint` (hash of the sorted, de-duplicated result keys) differs
from its `acknowledgedFingerprint`. Acknowledge copies the current fingerprint.
A later run with only previously seen results carries the acknowledgement
forward; any never-seen result re-raises. Zero results, unchecking the flag or
removing the search stops the alarm.

The alarm is re-evaluated only on events (shell build, a results-page run, each
background check, save/remove/flag toggle/Acknowledge), never by display
polling. Turning the flag on starts a background check at once; the tone timer
runs only while an alarm is active.

## Exclusions

Each row has **Exclude…**: a comma-, semicolon- or newline-separated word list
stored per search as `excluded` (`SavedSearchStore::parseExcluded`). A result is
dropped, case-insensitively, when its repository name, title, path or matched
text contains any word. Excluded results are removed before `recordRun`, so
they never count, ping or alarm; the status line reports how many were
excluded. Removing an exclusion returns those results as a changed set, so an
alarm can re-raise.

## Hidden from search and logs

**Edit hidden…** maintains a global list in `search-exclusions.json` (separate
from `saved-searches.json`), loaded by `SavedSearchStore::loadHiddenTerms` at
the top of the `MainWindow` constructor and held in `src/HiddenTerms.h` behind
a read/write lock.

- Search: matching repositories and results are removed from the results page,
  the Ctrl+K overlay and every saved-search run.
- Logs: `logSystemFrom` and the action telemetry journal
  (`logs/diagnostics/actions.jsonl`) replace each term with `[hidden]` before a
  line is stored, shown or published. Existing lines are not rewritten.
- **Also hide every Actions variable value** (`#searchHideVariablesCheck`,
  QSettings `search/hideVariableValues`, on by default) adds every Actions
  variable and secret value (`ActionStore::variables`) as a hidden term.
  `terms()` returns the user's list; `activeTerms()` adds the values and is used
  for hiding and redaction. The caption shows only a count. Values shorter than
  8 characters (`kMinVariableValueLength`) are skipped; longer values are
  redacted first. `refreshVariableValues` runs in the constructor and in
  `ActionStore::setVariables`.

## Scrub a term

**Scrub…** (results page header and `#savedSearchScrubButton` on each row)
removes a word from a repository's source and history. A confirmation dialog
(`scrubSearchTerm` in `MainWindowShortcuts.cpp`) asks for a replacement
(default `[redacted]`), whether to rewrite history on every branch (default on)
and the term typed again, then runs the **Scrub a term from source and
history** shortcut `.forkmesh/shortcuts/scrub-term.sh` in the Shortcuts tab.
The repository's own script is used if present, otherwise the built-in copy
(`:/shortcuts/scrub-term.sh`). Inputs: `SCRUB_TERM`, `SCRUB_REPLACEMENT`,
`SCRUB_HISTORY`, `SCRUB_BACKUP_DIR` (`<data>/scrub-backups`).

The script (`app/tests/test_scrub_term_shortcut.py`):

1. Refuses a term under 3 characters, newlines or `<`/`>` in term or
   replacement, a replacement containing the term, a detached HEAD, or
   uncommitted tracked changes.
2. Replaces the term case-insensitively in tracked text files and commits.
   Paths containing it are listed, not renamed.
3. With history on: writes every ref to a `git bundle` backup, rewrites all
   refs except `refs/stash` via `git fast-export` | filter |
   `git fast-import --force` (contents, messages and identity lines; binary
   blobs skipped), expires reflogs, runs `git gc --prune=now`, and verifies.

It does not touch `refs/stash`, other worktrees' checked-out files, remotes or
other clones, and it keeps the backup bundle (which still contains the term)
as the way back. This is the user-confirmed history-rewrite case in
[destructive actions](../engineering/destructive-actions.md).

## Tests

- `forkmesh-saved-search-tests` (`tests/test_saved_search_store.cpp`): store
  persistence, fingerprints, exclusions, variable values as hidden terms.
- `forkmesh-security-alarm-tests` (`tests/test_security_alarm_state.cpp`): the
  alarm state machine.
- `forkmesh-window-tests` (`checkSavedSearches`): banner, tone, acknowledge,
  exclusions, refresh, hidden terms and log redaction.

The iOS and Android apps keep their own saved searches without a security flag;
desktop Pings reach a paired phone.
