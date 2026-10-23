# Dashboard colours

Part of [the App docs](README.md). The dashboard follows GitHub's colour
roles and a compact control size. Both live in one file,
`app/public/dashboard/theme.json`:

- `colors`: every `--dashboard-<name>-rgb` token with a dark and a light
  value. `dashboard_shell.apply_theme` writes them into the shell where it
  has the `/*#theme dark*/` and `/*#theme light*/` build markers, so every
  composed page (and every marketplace page built from the shell) gets the
  same values. `app/tailwind.dashboard.config.js` reads the same file, so
  each token is also a Tailwind colour (`bg-cta`, `text-success`,
  `border-attention`, ...).
- `density`: the compact sizes (`--dashboard-control-font` 13px,
  `--dashboard-control-height` 32px, `-sm` 28px, `--dashboard-menu-row-height`
  30px, `--dashboard-control-radius` 6px), exposed to Tailwind as
  `text-control`, `h-control`, `h-control-sm`, `h-menu-row` and
  `rounded-control`. The shell also gives inputs, selects and textareas the
  13px size and blue checkboxes, radios and focus borders by default, through
  `:where()` rules a utility class still overrides.
- `roles` and `rules`: what each colour is for, in words an agent can follow.
  `AGENTS.md` points agents at this file.

## Why `primary` is blue

In October 2026 the user asked why every new page came out with green inputs,
selects, selected cards and icons when the rest of the product used blue.
The cause was the token: `primary` was GitHub green, and new pages naturally
used `primary` for focus, selection and icons. Rather than fixing pages one by
one, `primary` became blue (`#4493f8` dark, `#0969da` light), so the default
choice is now the right one. The green button colour moved to its own token,
`cta`, and solid `bg-primary` buttons were moved to `bg-cta`; green status
uses `success`.

| Role | Token | Used for |
| --- | --- | --- |
| Blue | `primary`, `accent`, `ring` | Links, focus, selection, checkboxes, the active or current item |
| Grey | `muted-foreground`, lifting to `foreground` on hover | Icons, secondary text |
| Green | `cta` | The one create/new button on a page |
| Green | `success` | Status only: online, passed, done |
| Amber | `attention` | Warnings, pending, high usage |
| Red | `destructive` | Errors and remove actions |

`app/tests/test_dashboard_theme_tokens.py` checks that the shell holds no
literal colour tokens, that `primary` stays blue and `cta`/`success` green,
that Tailwind knows every token, and that no dashboard source uses green for
focus, selection, checkboxes or links.
`app/tests/test_instance_theme.py` checks the instance-theme tokens against
the shell after `apply_theme` has filled the markers (the raw `shell.html`
only holds the markers), and that each one has a dark and a light value in
`theme.json`.

## Text on solid `primary`

White text on the dark-theme blue (`#4493f8`) reaches only 3.1:1, below the
4.5:1 text minimum in
`www/docs/engineering/daily-interface-budgets.json`, and the daily-interface
browser check failed on it. In the dark theme, `primary-foreground` is now the
page background (`1 4 9`), which gives 6.6:1 and keeps the blue unchanged for
links, focus and selection. The light theme keeps white text on `#0969da`
(5.2:1).

## Header search focus

The header search box (`data-global-search-shell` in
`app/public/dashboard/partials/header.html`) shows one focus state, like
GitHub's: the wrapper border turns `accent`. The shell's global
`:focus-visible` outline is loaded after `tailwind.css`, so it beat the
input's `outline-none` and drew a second blue box inside the wrapper. A
`[data-global-search]` rule in `shell.html` removes the outline and shadow
from the input itself.

## Ping icons

Ping kind icons (`data-notification-kind-icon`) in the bell dropdown, the
full pings list, the detail pane and the home feed are grey. Unread pings in
the dropdown used `text-primary`, which made them green; read and unread are
already told apart by the row's opacity. A `shell.html` rule lifts the icon to
`foreground` while its row is hovered or focused.

## Calendar today

The current day is blue. In the month grid
(`app/public/dashboard/js/09-calendar.js`) today's date pill is
`bg-accent text-accent-foreground`; in the week view today's column gets the
`fm-cal-today` accent border (defined in the calendar view's own `<style>`,
since it loads after `tailwind.css` and its `.fm-cal-day` border would win
over a utility), an accent weekday label and an accent date pill. Both carry
`aria-current="date"`. Calendar list dots keep their per-calendar colours,
and New event and Save event stay green as create actions.
