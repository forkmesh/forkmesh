# Seasonal theme (Halloween)

Part of [the App docs](README.md). In October 2026 the user asked for a
Halloween theme that, like GitHub's, swaps the green for orange, and that only
a platform administrator or a moderator can switch on and off. It is one
platform-wide setting: everyone on that ForkMesh sees it while it is on.

## What changes

While the season is `halloween`:

- `cta` (the one create button) and `success` (status dots, passed checks)
  turn orange. The values live in `app/public/dashboard/theme.json` under
  `seasons.halloween`; `dashboard_shell.apply_theme` writes them where the
  shell has the `/*#season halloween dark*/` and `/*#season halloween light*/`
  markers, inside `[data-season="halloween"]` rules. Primary blue, focus and
  links do not change.
- The activity graph on a branch uses GitHub's Halloween ramp
  (`USER_ACTIVITY_HALLOWEEN_DARK` / `_LIGHT` in `06-repo-content.js`).
- `www/public/season.js` adds a spider web in the top-right corner with a
  spider dangling from it (only from 1500 px wide, where the page has a free
  gutter), a smaller web in the bottom-left corner (from 1024 px), and a witch
  hat on the header profile picture. The spider stops moving under
  `prefers-reduced-motion`. Nothing is drawn when printing.
- The Home page's empty Agents and Feed boxes show a pumpkin and spooky text.
- The Genie spider wears a witch hat instead of its turban and its glow turns
  orange ([genie-spider](../www/genie-spider.md)).

The background, the fonts and every other colour stay as they are: the first
preview also had bats, fog and a purple background, and the user asked to
drop them.

## How a page knows

`season.js` is loaded synchronously from the dashboard shell's `<head>`. It
sets `data-season` on `<html>` from the last answer saved in
`localStorage["forkmesh.season"]` (`{season, until}`), so a returning visitor
sees no green-to-orange flash, then asks `GET /api/season` and saves the
answer. A saved answer past its `until` is ignored, so the theme goes away on
time even without a network. When the season changes it dispatches
`forkmesh:season` on `window`; `09-season.js` (`watchSeason`) redraws the Home
empty states and the activity graph. The Genie reads `data-season` on every
frame, so it changes without a reload.

## The setting

Settings → Appearance has a **Halloween theme** card with a switch and a
**Turn off automatically** time (default: 1 November, 00:00 local time). The
card stays hidden unless `GET /api/season/settings` answers, which it does
only for a platform administrator (`users.is_admin` with TOTP, or a
`platform_administrator` grant) or a platform `moderator` grant
(`_has_role`). The server checks the role again on every change.

| Endpoint | Who | What |
| --- | --- | --- |
| `GET /api/season` | anyone | `{season, until}`; `season` is `null` when off or past `until`. Cached a minute |
| `GET /api/season/settings` | administrators, moderators | `{season, until, by, at, seasons, maxLeadMs}` |
| `POST /api/season/settings` | administrators, moderators | `{season: "halloween", until}` turns it on, `{season: ""}` turns it off |

`until` is required to turn the theme on and must be between 5 minutes and
60 days away, so a forgotten theme always switches itself off. The setting is
stored in `instance_settings` under `season` (`app/src/seasonal_theme.py`),
and every change is written to the audit log as `admin.season` with the new
season and `until`. The public answer never says who turned it on.

## On the desktop

The desktop has had its own `halloween` theme since the view menu's Theme
section ([ui-redesign](../desktop/ui-redesign.md)), but it was only one entry
in the Theme list, so the user could not find it in Settings. Settings →
General now has a **Halloween theme** group under Appearance
(`MainWindow::buildHalloweenSettings`):

- **Use the Halloween theme on this desktop** (`halloweenLocalCheck`), for
  everyone. It sets `appThemeCombo` to `halloween` and, when unticked, back to
  the theme picked before (`system` if there was none). Picking Halloween in
  the Theme list ticks it.
- **Turn on the Halloween theme for everyone on this ForkMesh**
  (`seasonEveryoneCheck`) with the same **Turn off automatically** time
  (`seasonUntilEdit`, default 1 November 00:00), inside `seasonAdminBox`. The
  box stays hidden until `GET /api/season/settings` answers 200 with the
  account's session token (`refreshSeasonSettings`, run each time General
  comes into view), so only administrators and moderators see it, as on the
  web. Changes go to `POST /api/season/settings` with a bearer token; the
  server's error codes get the same wording as the web card.

The site-wide switch changes the website only. The desktop's own colours
follow its Theme setting.

## Tests

`app/tests/test_seasonal_theme.py` checks who can change the setting, the
auto-off window and that an expired season reads as off.
`app/tests/test_seasonal_theme_frontend.py` checks the composed shell's season
tokens, that every dashboard page loads `season.js` and the hidden Appearance
card. `app/tests/test_genie_halloween.py` checks the Genie's hat and glow.
