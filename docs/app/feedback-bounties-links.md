# Feedback, bounties and link rewards in the dashboard

Three dashboard surfaces backed by existing server APIs.

| Feature | Where | API |
| --- | --- | --- |
| Send feedback | Account menu, sidebar footer, `?feedback=1` on any dashboard page | `POST /api/feedback` with `source: "web"` |
| Bounties | `/dashboard/bounties` | `GET /api/tasks` (paged), `POST /api/tasks` with `kind: "bid"` |
| Link rewards | `/dashboard/links` | `GET` and `POST /api/world/link-kiosk` |

## Code map

| Path | Responsibility |
| --- | --- |
| `app/public/dashboard/partials/modals.html` (`data-feedback-modal`), `js/09-feedback.js` | Feedback dialog |
| `app/public/dashboard/partials/header.html` (`data-plugin-menu`), `js/09-plugin-menu.js` | ⋯ menu on plugin pages (`docs/marketplace/README.md`) |
| `app/public/dashboard/partials/views/bounties.html`, `js/09-bounties.js` | Bounties page (lazy chunk) |
| `app/public/dashboard/partials/views/links.html`, `js/09-links.js` | Link rewards page (lazy chunk) |
| `app/src/dashboard_shell.py`, `dashboard_bundle.py`, `static_routes.py` | Page registration, chunks and routes |
| `app/src/entry.py` (`_feedback_fields`, `FEEDBACK_NOTE_PATHS`) | Accepts the `web` feedback source |
| `app/src/admin_support.py` (`render_feedback`) | Admin feedback list |
| `world/src/world_office_tasks.py`, `world/src/world_link_kiosk.py` | Task and link APIs |

## Behaviour and permissions

All checks are on the server.

- **Feedback** is anonymous. The server stores the fixed path
  `/dashboard#feedback`, never the visitor's page, and pings admins.
- **Plugin feedback** is the same dialog opened from a plugin page's ⋯ menu.
  It retitles itself "Feedback on <plugin>" and posts to `/api/bug-reports`
  with the plugin id, the page id and source `web` instead of to
  `/api/feedback`. The report becomes a public issue, so the dialog says so;
  opening the dialog from the account menu resets it to general feedback.
- **Bounties** need an organization member; `GET /api/tasks` answers
  `org_member_required` otherwise. A bid is filed by and assigned to the
  signed-in member; the amount is a request, not a payment.
- **Link rewards** are public to read. Submitting needs a signed-in account,
  consent, and a public HTTPS URL; each account is limited to 20 links and the
  server never fetches the URL. Unpaid rewards with a Solana address show a
  `solana:` pay link and a copy button.

## Styling

`app/public/dashboard/tailwind.css` is prebuilt; regenerate it after adding
utilities:

```sh
cd app
npx tailwindcss@3.4.17 -c tailwind.dashboard.config.js -i tailwind.input.css \
  -o public/dashboard/tailwind.css --minify
```

## Tests

`app/tests/test_dashboard_bounties_links_feedback.py`,
`app/tests/test_feedback.py`, `test_feedback_email.py`,
`app/browser_tests/tests/dashboard-bounties-links-feedback.spec.js`.

## Notices and states

Bounties, link rewards, microjobs and referrals share one treatment for
event-only UI ([dashboard-components.md](dashboard-components.md)):

- Submit results are `fm-flash` success or danger inside the notice row;
  "log in to take part" prompts are blue info flashes; "members only" and load
  errors are `fm-blankslate`s; empty lists are blankslates with a muted icon,
  a short title and the original sentence.
- Status pills (`Completed`, `In progress`, `Open`, `Paid`, `Unpaid`, `you`)
  are `fm-label` variants, success for done or paid. Requested SOL amounts and
  rank 1 are foreground text, not amber or gold.
- Header count pills are `fm-label`, Refresh is `fm-btn-sm`. Each form's
  submit is the page's only green button; copy-link, pay-in-wallet and the
  microjob "Submit an issue" links are neutral `fm-btn-sm`.
- The issue and pull-request composer reward notices
  (`microjobIssueNoticeHtml`, `microjobPullNoticeHtml`) are neutral flashes with
  a coins icon instead of dashed boxes.
