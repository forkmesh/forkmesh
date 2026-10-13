# Feedback, bounties and link rewards in the dashboard

Until 2026-09-29 these lived in the World's Office lobby as four props: a
feedback kiosk, a task bounty desk, the Link Lab and the link rewards board
beside it ([world/README.md](../world/README.md), "Lobby kiosks moved to the
website"). They were hard to find, needed WebGL and a walk across the lobby,
and did not work well on phones. They are now ordinary dashboard surfaces that
reuse the same server APIs.

| Feature | Where | API |
| --- | --- | --- |
| Send feedback | "Send feedback" in the account menu, "Feedback" in the sidebar footer, and `?feedback=1` on any dashboard page | `POST /api/feedback` with `source: "web"` |
| Bounties | `/dashboard/bounties`, Community group next to Microjobs | `GET /api/tasks` (paged), `POST /api/tasks` with `kind: "bid"` |
| Link rewards | `/dashboard/links`, Community group next to Referral | `GET` and `POST /api/world/link-kiosk` |

## Code map

| Path | Responsibility |
| --- | --- |
| `app/public/dashboard/partials/modals.html` (`data-feedback-modal`) | Feedback dialog markup, on every dashboard page |
| `app/public/dashboard/js/09-feedback.js` | Opens the dialog (menu, sidebar, `?feedback=1`), posts it, closes on Escape; part of the core bundle |
| `app/public/dashboard/partials/views/bounties.html`, `js/09-bounties.js` | Bounties page (lazy chunk `dashboard-page-bounties.js`) |
| `app/public/dashboard/partials/views/links.html`, `js/09-links.js` | Link rewards page (lazy chunk `dashboard-page-links.js`) |
| `app/src/dashboard_shell.py`, `dashboard_bundle.py`, `static_routes.py`, `app/wrangler.toml` | Page registration, chunks and routes |
| `app/src/entry.py` (`_feedback_fields`, `FEEDBACK_NOTE_PATHS`) | Accepts the `web` feedback source |
| `app/src/admin_support.py` (`render_feedback`) | Admin feedback list: "Website" filter and count |
| `world/src/world_office_tasks.py`, `world/src/world_link_kiosk.py` | Unchanged task and link APIs |

## Behaviour and permissions

The pages enforce exactly what the World did, because the checks are on the
server:

- **Feedback** stays anonymous (no session needed). The server stores a
  fixed path, `/dashboard#feedback`, never the page the visitor was on, and
  pings admins the same way it did for World notes ("New website feedback").
  The founders' "How are we doing?" email now links to
  `https://forkmesh.com/dashboard?feedback=1`; older emails point at
  `/world/?landmark=office&feedback=1`, which the World forwards to the same
  address.
- **Bounties** need a signed-in account that belongs to the organization.
  `GET /api/tasks` answers `org_member_required` otherwise, and the page then
  explains that an owner has to add the account instead of showing a form.
  A bid is always filed by and assigned to the signed-in member; the amount
  is a request, not a payment. The list shows every task with `kind: "bid"`
  (Open, Completed, All), and each row opens the task in
  `/dashboard/tasks?task=<id>`.
- **Link rewards** are public to read. Submitting needs a signed-in account,
  the consent box, and a public HTTPS URL; the server limits each account to
  20 links and never fetches the URL. Each row shows the reach score,
  potential visits, the SOL appreciation estimate, paid or unpaid, and, when
  the submitter has a Solana address and the reward is unpaid, a `solana:`
  pay link and a copy-address button. The lobby board's QR codes are not
  repeated: on a phone the wallet link is the useful form, and on a desktop
  the copy button is.

## Styling

The pages follow the Referral and Challenges pages: one card with a header,
uppercase section labels, numbered explainer tiles and divided rows. The
committed `app/public/dashboard/tailwind.css` is prebuilt, so the new
utilities (`flex-col-reverse`, `aria-pressed:*`, `line-clamp-2`,
`items-stretch`) were added by regenerating it:

```sh
cd app
npx tailwindcss@3.4.17 -c tailwind.dashboard.config.js -i tailwind.input.css \
  -o public/dashboard/tailwind.css --minify
```

The regenerated file also dropped two utilities nothing used any more
(`max-w-52`, `whitespace-normal`).

## Tests

- `app/tests/test_dashboard_bounties_links_feedback.py`: page registration,
  routes, navigation, API contracts and stylesheet coverage.
- `app/tests/test_feedback.py`, `test_feedback_email.py`,
  `test_email_contrast_and_actions.py`: the `web` source and the email link.
- `app/browser_tests/tests/dashboard-bounties-links-feedback.spec.js`: the
  three surfaces in Chromium at 1440 and 390 px wide, with every `/api/**`
  call routed. It runs against `python3 -m http.server` over `app/public`;
  set `FORKMESH_WEB_KIOSK_SHOTS=<dir>` to save screenshots.
