# Seasonal theme (Halloween)

Part of [the App docs](README.md). A Halloween theme that, like GitHub's,
swaps green for orange. It is one platform-wide setting that only a platform
administrator or moderator can switch: everyone on that ForkMesh sees it while
it is on.

## What changes

While the season is `halloween`:

- `cta` (the one create button) and `success` (status dots, passed checks)
  turn orange. The values live in `app/public/dashboard/theme.json` under
  `seasons.halloween`; `dashboard_shell.apply_theme` writes them at the
  shell's `/*#season halloween dark*/` and `/*#season halloween light*/`
  markers, inside `[data-season="halloween"]` rules. Primary blue, focus and
  links do not change ([dashboard-colors](dashboard-colors.md)).
- The branch activity graph uses GitHub's Halloween ramp
  (`USER_ACTIVITY_HALLOWEEN_DARK` / `_LIGHT` in `06-repo-content.js`).
- `www/public/season.js` draws a spider web with a dangling spider in the
  top-right corner (from 1500 px wide, where there is a free gutter), a
  smaller web bottom-left (from 1024 px), and a witch hat on the header
  profile picture. The spider stops under `prefers-reduced-motion`; nothing is
  drawn when printing.
- The Home page's empty Agents and Feed boxes show a pumpkin and spooky text.
- The Genie spider wears a witch hat instead of its turban and glows orange
  ([genie-spider](../www/genie-spider.md)).

Background, fonts and every other colour stay as they are (the user dropped
the bats, fog and purple background of the first preview).

## How a page knows

`season.js` loads synchronously from the dashboard shell's `<head>`. It sets
`data-season` on `<html>` from the last answer saved in
`localStorage["forkmesh.season"]` (`{season, until}`), so a returning visitor
sees no green-to-orange flash, then asks `GET /api/season` and saves the
answer. A saved answer past its `until` is ignored, so the theme ends on time
even offline. On a change it dispatches `forkmesh:season` on `window`;
`09-season.js` (`watchSeason`) redraws the Home empty states and the activity
graph. The Genie reads `data-season` every frame, so it changes without a
reload.

## The setting

Settings → Appearance has a **Halloween theme** card with a switch and a
**Turn off automatically** time (default 1 November, 00:00 local). The card
stays hidden unless `GET /api/season/settings` answers, which it does only for
a platform administrator (`users.is_admin` with TOTP, or a
`platform_administrator` grant) or a platform `moderator` grant
(`_has_role`). The server checks the role again on every change.

| Endpoint | Who | What |
| --- | --- | --- |
| `GET /api/season` | anyone | `{season, until}`; `season` is `null` when off or past `until`. Cached a minute |
| `GET /api/season/settings` | administrators, moderators | `{season, until, by, at, seasons, maxLeadMs}` |
| `POST /api/season/settings` | administrators, moderators | `{season: "halloween", until}` turns it on, `{season: ""}` turns it off |

`until` is required to turn the theme on and must be 5 minutes to 60 days
away, so a forgotten theme always switches itself off. The setting is stored
in `instance_settings` under `season` (`app/src/seasonal_theme.py`); every
change is audited as `admin.season` with the new season and `until`. The
public answer never says who turned it on.

## On the desktop

The desktop's own `halloween` theme sits in the Theme list
([ui-redesign](../desktop/ui-redesign.md)); Settings → General also has a
**Halloween theme** group under Appearance
(`MainWindow::buildHalloweenSettings`) so it can be found:

- **Use the Halloween theme on this desktop** (`halloweenLocalCheck`), for
  everyone. It sets `appThemeCombo` to `halloween` and, when unticked, back to
  the previous theme (`system` if none). Picking Halloween in the Theme list
  ticks it.
- **Turn on the Halloween theme for everyone on this ForkMesh**
  (`seasonEveryoneCheck`) with the same **Turn off automatically** time
  (`seasonUntilEdit`, default 1 November 00:00), inside `seasonAdminBox`. The
  box stays hidden until `GET /api/season/settings` answers 200 with the
  account's session token (`refreshSeasonSettings`, run each time General
  comes into view). Changes go to `POST /api/season/settings` with a bearer
  token; server error codes get the web card's wording.

The site-wide switch changes the website only; the desktop's colours follow
its Theme setting.

## Tests

- `app/tests/test_seasonal_theme.py`: who can change the setting, the
  auto-off window, an expired season reads as off.
- `app/tests/test_seasonal_theme_frontend.py`: the composed shell's season
  tokens, every dashboard page loads `season.js`, the hidden Appearance card.
- `app/tests/test_genie_halloween.py`: the Genie's hat and glow.
