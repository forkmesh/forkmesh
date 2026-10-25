# Changelog

ForkMesh has one changelog, `changelog/changelog.json`. Two places show it:

- **forkmesh.com/changelog**: `www/templates/pages/changelog.html` is an
  empty page; `www/public/changelog.js` fetches `/changelog.json` and prints
  the entries in the browser ([www/templates.md](www/templates.md)). The
  build copies the file next to the page (`build_site_assets.stage_www`,
  `stage_app` and `stage_cutover`). A self-hosted instance serves the same
  page and file at its own `/changelog` and `/changelog.json` (the
  `changelog` plugin, on by default, see
  [INSTANCES.md](INSTANCES.md#plugins)).
- **The phone apps' What's new**, under the version row of the You hub
  ([native/NAVIGATION.md](native/NAVIGATION.md)). The iOS build step "Bundle
  the changelog" and the release host's Android APK build export the file into
  the app ([native/MAC_BUILD.md](native/MAC_BUILD.md)).

Until 2026-10-21 these were two changelogs: the website's hand-written
releases in `changelog.html`, and the apps' notes generated at build time
from `git log` between ad hoc builds plus `native/changelog/notes.json`. The
website stopped in July 2026 and the apps' notes were never on the website.
Both were merged into the file: the website's releases v0.1.0 to v0.7.0 as
they were, and the apps' notes of builds 218 to 302 as two entries
(2026-10-13 and 2026-10-21) rewritten in plain English.

## The file

```json
{
 "schema": 3,
 "releases": [
  {"date": "2026-10-21", "title": "Neighbours, stickers and runners", "pitch": "One sentence under the title."},
  {"date": "2026-07", "version": "0.7.0", "title": "The Living Code City"}
 ],
 "notes": [
  {"date": "2026-10-25", "area": "Chat", "text": "Stick stickers on messages."},
  {"date": "2026-07", "version": "0.7.0", "area": "Highlights", "text": "**Everyone enters.** Guests and members."}
 ]
}
```

- `notes` is a flat list, one note per line. A note's `date` (`YYYY-MM-DD`,
  or `YYYY-MM` for the early releases that only had a month) and optional
  `version` say which entry it belongs to; the website and the apps group the
  notes into one entry per date and version, newest first, whatever order the
  lines are in.
- `area` is one of Highlights, Market, Calls, Projects, Chat, Agents, World,
  Settings, iPhone, Android, Desktop, Server, Other improvements. Areas are
  shown in that order; the website prints the area names only when an entry
  has more than one.
- `text` is one plain sentence. `**Lead.**` prints in bold on the website and
  as plain text in the apps.
- `releases` is optional and names an entry: one line per date and version
  with a `title`, a `pitch` and the `version` the website shows as a `vX` tag
  and an anchor (`/changelog#v0.7.0`); entries without one are anchored by
  date. The five releases v0.1.0 to v0.5.0 share the month `2026-06`, which
  is why notes carry the version too.

`python3 tools/changelog.py check` validates the file and its layout (every
note and release on its own line, as `changelog.py format` writes it);
`tools/test_changelog.py` runs it, runs `changelog.js` under Node to check
the website groups the notes exactly as the apps do, and merges two
branches that each add a note.

### Why one line per note

Until 2026-10-25 (schema 2) notes were nested inside a day's entry and
area section, so two branches that added notes on the same day edited the
same lines, and resolving the conflict by keeping both sides dropped the
commas and the `]`/`},` lines between sections. On 2026-10-25 a chain of such
merges left `main` with invalid JSON; every `/changelog` page failed and the
deploy stopped on `app/tests/host/test_instance_changelog_e2e.py`. The
website was also a second copy: `www/public/changelog.html` had every entry
rendered into it and had to be re-rendered, and conflicted, with each note.

Now every note is a whole line ending in a comma, and `add` puts new ones on
the first line of `notes`, so the last line, the oldest note without a
comma, never changes. `.gitattributes` sets `merge=union` on the file: when
two branches both add lines at the top, git keeps both sides' lines instead
of stopping on a conflict, and the result is still valid JSON. The order of
the lines does not matter, so neither side has to come first.

Union merging only goes wrong when two branches change the same existing
line: both versions are kept, which `check` reports as a repeated note when
the text is the same and otherwise leaves two lines to clean up by hand. A
tool that ignores `.gitattributes` (a forge's web merge button) still stops
on a conflict; keeping both sides' lines is then the right resolution. Run
`python3 tools/changelog.py check` after any merge that touched the file.

## Adding a note

A change a user can notice adds a line:

```sh
python3 tools/changelog.py add --area Chat "Stick stickers on messages."
```

`add` puts the note, dated today (or `--date`), on the first line of
`notes`, and skips one that is already there on that date. Nothing needs
re-rendering: the website reads the file. Give a busy day's entry a `title`
and `pitch` by adding a line at the top of `releases`.

`python3 tools/changelog.py draft [--since DATE|COMMIT]` prints suggested
lines from the merged commits since the newest entry (`tools/changelog_draft.py`:
it drops merges, docs, tests, chores and code-name subjects, groups by area
and caps lines at 140 characters). Rewrite them in plain English before
adding them; the drafts are not shipped anywhere on their own.

## In the apps

`tools/changelog.py export --platform ios|android --out FILE` writes the
apps' format (schema 1, which the apps read since 2026-10-13): each entry
keeps its version, date, title, pitch and sections; `**` is dropped, the
iOS app leaves out the Android section and the Android app the iPhone one,
and each entry is capped at 8 lines per area and 24 in all.

The apps head an entry with its title, else "Version x", else its date, and
show the version and date under a title. They no longer add an empty
"Version x (build)" entry on top for the running build: the version row
already shows it, and every build would otherwise open on "No notes were
written for this build". That empty entry is only shown when the bundle has
no entries (a local build with the placeholder file).
