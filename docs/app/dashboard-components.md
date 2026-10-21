# Dashboard components

Part of [the App docs](README.md). Colour roles and density live in
[dashboard colors](dashboard-colors.md) and `app/public/dashboard/theme.json`;
this page covers the shared `fm-*` component classes defined in
`app/public/dashboard/shell.html`. Use them for every piece of UI that only
appears on an event or a state (banners, menus, dialogs, empty states) instead
of a per-page mix of utilities, so a notice on one page looks like a notice on
every other.

## The set

| Class | Use it for |
| --- | --- |
| `fm-btn` | Page-level actions (32px). Neutral by default. |
| `fm-btn-sm` | Buttons in rows, toolbars, cards, banners, popovers and dialogs (28px, 12px text). |
| `fm-btn-cta` | The one create or confirm action in a page, dialog or banner (green). |
| `fm-btn-danger` | Remove, revoke, delete: red text, red fill on hover. |
| `fm-btn-invisible` + `fm-btn-icon` | Close buttons and quiet icon-only controls. |
| `fm-flash` | Inline notices. Blue by default (info); `-success`, `-warning`, `-danger`, `-neutral`. Children: one 16px icon, `fm-flash-body` (`fm-flash-title` + muted `<p>`), `fm-flash-actions` on the right. |
| `fm-flash-full` | A flash that spans the column under the header (update notice, unverified email). No radius, no side or top border. |
| `fm-flash-icon-accent` | Paints a neutral flash's icon blue, for a hint that is news rather than a problem. |
| `fm-label` | Outlined status pills (`Public`, `Node`). `-accent`, `-success`, `-attention`, `-danger`; `fm-label-lg` for a 24px pill in the header. |
| `fm-counter` | Counts: a small grey pill. |
| `fm-popover` | Dropdowns, pickers and hovercards. `fm-popover-header` (with `fm-popover-title`) and `fm-popover-footer` add hairline-separated ends. |
| `fm-menu`, `fm-menu-item` | Menu rows (30px, 13px). `aria-checked="true"` or `aria-selected="true"` draws the blue check; `fm-menu-item-danger` for log out and delete; `fm-menu-heading`, `fm-menu-divider` to group. |
| `fm-dialog` | Modals: `fm-dialog-header` (`fm-dialog-heading` holding `fm-dialog-title` and the muted `fm-dialog-desc`, then the close button), `fm-dialog-body`, `fm-dialog-footer` with right-aligned `fm-btn-sm` buttons, cancel first. |
| `fm-blankslate` | Empty, loading and error states: one muted 24px icon, `fm-blankslate-title`, `fm-blankslate-desc`, optional `fm-btn-sm`. |
| `fm-segmented` | Two to four mutually exclusive buttons; the `aria-pressed="true"` one is filled. |
| `fm-toast` | Short-lived confirmations that float over the page. |
| `fm-tooltip` | Custom tooltips; native `title` stays the default. |

## Rules that keep them consistent

- Icons are muted grey and lift to the foreground on hover. Only status icons
  (inside a coloured flash or label) take a status colour.
- Green is only `fm-btn-cta`. Selection, focus and links are blue.
- No gradients, glows or coloured left bars, and no bordered box inside a
  bordered box: a dialog's sections are separated by hairlines, not cards.

## Display is overridable

Each class's `display` is declared through `:where(...)`, which has zero
specificity. The dashboard's JavaScript shows and hides most of these elements
by toggling Tailwind's `hidden` (and sometimes `flex` or `inline-flex`), and
`tailwind.css` loads before the shell's own styles; with a normal
`.fm-flash { display: flex }` the component rule would win and a hidden banner
would stay on screen. With the display under `:where`, `hidden`, responsive
`sm:` utilities and the `[hidden]` attribute all still apply.

## Shell-level pieces

- Update notice: see [update notice](update-notice.md).
- Unverified email: a full-width warning flash under the header with a small
  **Resend verification email** button. A failed resend paints its hint red.
- Runner sign-in attention and **Reconnect node**: `fm-label fm-label-lg`
  links in the header, amber; the runner chip turns red when its
  `data-state` is `expired`.
- Command palette (Ctrl/Command+K, `js/09-daily-interface.js`): styled after
  macOS Spotlight, `fm-spotlight-*` in the shell. A 20px borderless search
  field, commands grouped under **Go to** and **Actions** with an icon each,
  and a footer naming the keys. It is the one floating surface allowed a
  translucent, blurred fill and a drop shadow, because that is what makes it
  read as Spotlight. The selected row is a solid blue fill with white text;
  dark mode uses `rgb(31 111 235)` instead of the theme's lighter primary so
  white text keeps 4.5:1 contrast. Pointer hover moves the selection without
  re-rendering. The dialog name, `Filter commands` combobox and option roles
  are unchanged for `browser_tests/tests/daily-interface.spec.js`.

- Main menu (the header's menu button, `partials/sidebar.html`): two zones
  instead of six equal columns, because the list kept growing and every link
  carried the same weight. The left zone is the user's own space: **Workspace**
  as tiles (Settings added so the grid stays full), installed **Plugins** as an
  app grid, and **Top repositories** as up to six cards. The right zone,
  **Explore ForkMesh**, keeps Community, Product, Resources and Company as
  plain text lists on the header surface, with the legal links under them.
  The `<a>` markup is unchanged; the layout lives in the shell's CSS (the nav
  is `display: contents` so its groups sit in the menu's grid areas, keeping
  the main menu ahead of Top repositories in the DOM). Below 760px it stacks
  into one column with row-style tiles.
- Pings dropdown, account menu and the header's **Start an agent** composer:
  `fm-popover` with menu rows; the account menu's log-out rows use
  `fm-menu-item-danger`. Every account menu row leads with a Lucide icon
  and keeps its text in a `span`; the status and away rows mark theirs
  `data-account-menu-label`, which is what `04-account.js` rewrites, so the
  icon survives a status change. The underline of the profile and network
  page tabs is `border-[var(--dashboard-tab-underline)]`, not a fixed
  `#f78166`, so instance themes recolour it like the repository tabs. Instance themes restyle the rows (the Google
  theme draws 40 px full-bleed rows with 20 px icons, as in Drive's menus).
- Profile settings, about, pings, feedback, agent sign-in terminal and new
  repository dialogs: `fm-dialog`. The new repository source picker is an
  `fm-segmented` grid, and its desktop hand-off and migration progress are
  info flashes.
- The status editor (`user-status.js`) and the celebration card
  (`member-welcome.js`) also load outside the dashboard, so they keep their own
  inline styles but read the dashboard tokens under `[data-dashboard-theme]`
  and follow the same dialog shape: hairline header and footer, 28px buttons,
  30px suggestion rows.
- The membership panel (`membership-prompt.js`) is shared with the website and
  keeps the website's textured buttons on purpose.

## Tasks, Notes and Calendar copies

`/dashboard/tasks`, `/dashboard/notes` and `/dashboard/calendar` are served
from the marketplace packages' pages (`dashboard-plugins/<id>/default/`,
built from the shell by `app/tools/build_app_plugins.py`). The checked-in
copies `app/public/dashboard/{tasks,notes,calendar}/index.html` used to be
kept by hand and so missed the shell's `fm-*` styles, leaving their menus,
blankslates and flashes unstyled. They are now listed in
`dashboard_shell.PACKAGED_PAGE_COPIES` and composed from the shell and
`partials/views/<id>.html` by `app/tools/build_dashboard_assets.py`;
`test_dashboard_composer.py` fails when a copy differs from the composed page.

## Text field focus

Text fields (inputs, selects, textareas) take a 1px primary outline drawn on
their own border when focused, instead of the shared 3px offset ring that
buttons and links keep. The user found the ring "too much" on the repository
clone popover, where the read-only URL field is focused and selected as soon
as the popover opens; on a field the thin outline reads as a highlighted
border rather than a halo. Checkboxes, radios, ranges and file pickers keep the
shared ring.
