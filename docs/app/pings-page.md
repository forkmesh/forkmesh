# Pings page

`/dashboard/pings` is the web inbox for an account's pings. The header bell is
a link to it on every dashboard page. It replaces the bell dropdown (five
newest pings, pause controls, "Show all pings") and the pings modal it opened,
so there is one place that lists pings instead of two.

## The bell

`#notificationToggle` in `app/public/dashboard/partials/header.html` is an
`<a href="/dashboard/pings">`. It keeps:

- the unread dot (`data-notification-badge`), shown while the account has any
  unread ping;
- the bell and bell-off icons, swapped by `renderPingPause` while
  notifications are paused, with the pause deadline as its `title`;
- an `aria-label` of `Pings` or `Pings, N unread` (`renderNotificationBadge`);
- `aria-current="page"` while the Pings page is open.

Guests do not see it (`html[data-guest] #notificationToggle`, as before).

## The page

Wiring is the same as any composed dashboard page: `dashboard_shell.PAGES`
(`pings`, no sidebar item), `static_routes.DASHBOARD_PAGE_ASSETS`, the
`run_worker_first` exclusions in `app/wrangler.toml`, the view
`app/public/dashboard/partials/views/pings.html`, and `initPingsPage` in
`app/public/dashboard/js/09-pings.js` (in the core bundle, not a lazy chunk,
because `loadNotifications` calls `renderPingsPage` on every page).

What it shows:

- **All / Unread**: an `fm-segmented` filter with the unread count. The choice
  is kept in the URL as `?filter=unread`. The relay has no unread-only read,
  so the filter applies to the pings already loaded; Load more fetches older
  ones.
- **Rows**: `notificationRowHtml` in `08-repo-detail-network.js`, built from
  the same helpers the dropdown and modal used (`notificationLead`,
  `notificationIcon`, `notificationInstanceLogoHtml`, `notificationTimeLabel`).
  A ping with a target is a link to it: `notificationTargetHref` runs
  `homeFeedHref`, so a mention or subscription ping opens its comment
  (`#comment-<id>`). A ping without a target is a button that opens its
  details.
- **Row actions**: Mark as read (unread rows only), Show details, and the
  per-kind settings gear (`pingSettingsHref`, the kind's row on
  `/dashboard/settings/notifications`).
- **Details**: `notificationDetailHtml`, the old modal detail pane, shown
  under the row: full text, organization task fields, Accept/Decline on a
  pending mirror request, Open context, and "Turn off ... pings".
- **Header**: Mark all as read and Ping settings.
- **Pause**: the pause status, Resume and the Pause notifications menu that
  used to sit in the bell dropdown, using the same `data-ping-pause-*`
  handlers as the settings page.
- **Empty, signed-out and error states**: `fm-blankslate`. A guest gets a Log
  in link back to the page.
- **Load more**: `GET /api/notifications?node=&limit=40&offset=N`, shown while
  fewer rows are loaded than the relay's `total`. Older pages are merged with
  the first page by id, so a ping arriving meanwhile is not shown twice.

## Read state and live updates

Opening a ping, expanding an unread one, Mark as read and Mark all as read
update the row and the badge at once, then `POST /api/notifications` (ids or
`all`) through `markNotificationsRead`, which reloads the first page. The POST
is sent with `keepalive` so a click on a row link still marks it read while
the browser leaves the page.

The page needs no polling of its own: the account event channel's `pings`
topic already calls `loadNotifications` (`02-helpers.js`), which now
re-renders the badge and the page.
