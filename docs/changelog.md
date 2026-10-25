# Changelog

ForkMesh has one changelog, `changelog/changelog.json`. Two places show it:

- **forkmesh.com/changelog**: `www/templates/pages/changelog.html` prints the
  entries through the `changelog_releases` variable that
  `app/tools/site_templates.py` fills from the file
  ([www/templates.md](www/templates.md)). The site also serves the file itself
  at `/changelog.json` (`build_site_assets.stage_www`). A self-hosted
  instance serves the same rendered page at its own `/changelog`
  (`build_site_assets.stage_app` bundles it; the `changelog` plugin, on by
  default, see [INSTANCES.md](INSTANCES.md#plugins)).
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
{"schema": 2, "entries": [
  {"date": "2026-10-21", "version": "", "title": "Neighbours, stickers and runners",
   "pitch": "One sentence under the title.",
   "sections": [{"title": "Chat", "items": ["Stick stickers on messages."]}]}
]}
```

- Entries are newest first. `date` is `YYYY-MM-DD`, or `YYYY-MM` for the
  early releases that only had a month.
- `version` is optional; the website shows it as a `vX` tag and an anchor
  (`/changelog#v0.7.0`), entries without one are anchored by date.
- `title` and `pitch` are optional.
- Section titles are areas: Highlights, Market, Calls, Projects, Chat,
  Agents, World, Settings, iPhone, Android, Desktop, Server, Other
  improvements. The website prints the area names only when an entry has more
  than one section.
- Items are one plain sentence each. `**Lead.**` prints in bold on the
  website and as plain text in the apps.

`python3 tools/changelog.py check` validates the file, including an area
that appears twice in one entry;
`tools/test_changelog.py` runs it and checks that the rendered page shows
every line the apps show.

### Merge conflicts

Two branches that add notes on the same day conflict in today's entry.
Deleting the conflict markers and keeping both sides' lines is not a
resolution: on 2026-10-25 a chain of such merges into `main` left the file
missing the commas and `]`/`},` lines between sections, and two entries with
the same area twice. Every `/changelog` page then failed, and the deploy
stopped on `app/tests/host/test_instance_changelog_e2e.py`. Resolve by
taking one side's file and re-adding the other side's lines with
`changelog.py add` (or merging them into the existing section by hand), then
run `python3 tools/changelog.py check` and re-render the site before you
commit the merge.

## Adding a note

A change a user can notice adds a line:

```sh
python3 tools/changelog.py add --area Chat "Stick stickers on messages."
python3 app/tools/site_templates.py
```

`add` puts the line in today's entry, creating it at the top when the newest
entry is older, keeps the areas in order and skips a line that is already
there. Re-render the website so `www/public/changelog.html` matches
(the website deploy refuses a stale page). Give a busy day's entry a `title`
and `pitch` by editing the file.

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
