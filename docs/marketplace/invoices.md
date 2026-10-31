# Invoicing

An organization plugin for billing: members create invoices, link them to CRM
contacts, email them with a private view-and-pay link, and see when each one is
opened, viewed and paid. The owner asked for invoicing in the Market that
links to the CRM and everything else it touches, sends invoices by email, and
shows when an invoice is opened.

Package: [`marketplace/packages/invoices`](../../marketplace/packages/invoices/README.md).
Members manage invoices on the web (`/dashboard/invoices`, laid out for
phones too), in a native desktop page, and in native iPhone and Android
screens; the client reads theirs on a public page at `/invoice`. The owner
asked for every part to be manageable on mobile and desktop, so each app
offers the same actions as the web page: create, edit, duplicate, email and
remind, record payments, void, reopen, delete, settings, install, and the
analytics.

## Code map

| Path | What it does |
| --- | --- |
| `content/worker/invoices.py` | The Worker API at `/api/invoices`: invoices, settings, CRM contact reads, sending, payments, tracking routes, the overdue sweep, export and purge. |
| `content/web/view.html`, `content/web/page.js` | The dashboard page: totals, the invoice table, recent activity, the editor, the detail sheet with history, and the send, payment and settings dialogs. |
| `content/www/invoice.{html,js,css}` | The client's page at `/invoice?t=<token>`, printable to PDF. Built with no site header, analytics or `innerHTML`. |
| `content/desktop/InvoicesPage.{h,cpp}` | The native desktop page and its registration (`/api/invoices`, proof `forkmesh-invoices-request-v1`). |
| `content/desktop/InvoicesModel.{h,cpp}`, `content/desktop/tests/test_invoices_model.cpp` | The page's logic without widgets (money, totals, filters, allowed actions, labels) and its Qt test, target `forkmesh-invoices-tests`. |
| `native/ios/ForkMeshCore/Sources/FMPackages/Invoice*.swift`, `PackagesClient+Invoices.swift`; `native/ios/ForkMesh/Features/Packages/Invoice*.swift`, `CrmContactInvoices.swift` | iPhone models, client, screen model, screens and the CRM card section. Tests: `FMPackagesTests/InvoicesTests.swift`, `FMApiTests/InvoiceRouteTests.swift`. |
| `native/android/core/.../market/Invoices.kt`, `native/android/app/.../feature/market/InvoiceScreens.kt` | Android models, client, screen state and screens. Test: `core/src/test/.../market/InvoicesTest.kt`. |
| `marketplace/packages/crm/content/web/page.js` (`loadCrmContactInvoices`, `renderCrmContactInvoices`) | The Invoices section on a CRM contact card. |
| `tests/test_invoices.py` | API, tracking, sweep, lifecycle and page contracts against sqlite. |

## Apps

**Web.** Below the `sm` breakpoint the invoice table becomes a list of cards,
the five totals sit two to a row, and each editor line item puts its
description on its own row. Every class the page uses is in the hand-built
`app/public/dashboard/tailwind.css`; a class missing from that file has no
style at all, so check before adding one.

The dialogs are modal, so `showModal()` makes the page behind them inert and
covers it with the backdrop: an error written to the page flash while one is
open is never seen. Each dialog carries its own `data-invoice-dialog-error`
slot (the detail also has `data-invoice-dialog-notice`), and `message()`
writes to the topmost open dialog's slot, falling back to the page flash.
Stacked dialogs (email or payment over the detail) come later in
`view.html`, so the last open dialog in document order is the one on top.
`showDialog()` clears a dialog's slots as it opens, and a dialog that closes
on failure closes before its message is written. Each dialog is named by its
heading through `aria-labelledby`. `tests/dialog_message_checks.js` runs the
page's own functions against the real markup.

**Desktop.** A native page in the rail with the overdue count as its badge,
search hits on number and client, and `exportData()` like Analytics. In
Control Tower the list and the open invoice sit side by side with every
action as a button. In Focus the list sits beside the recent activity, and an
invoice opens as its own page with a back button and the less common actions
under ⋯. The host does not tell a package page when the layout mode changes,
so the page reads it each time it is shown and on Refresh. "Open on the web"
opens the dashboard page.

**iPhone and Android.** A Business hub entry that appears once one of the
user's organizations installs Invoicing (both apps gate it like the CRM). The
route is `invoices(organization, invoice)` on both, and
`/dashboard/invoices?org=<org>&invoice=<id>` links, which Pings use, open
that invoice; without `org` the apps look through the installed
organizations. Dates are typed as UTC days, like the web.

## Data

Three tables, created when an organization installs the plugin and dropped
when the last install is removed:

- `invoice_records`: one row per invoice. The columns the summary and the
  overdue sweep need are plain (status, currency, total, paid amount, dates,
  open, view and click counters, first-open and first-view times); the client,
  line items, tax rate, notes, terms, pay link, payments and the address it was
  sent to are sealed with the instance row key. Numbers are unique per
  organization; 10,000 invoices per organization.
- `invoice_events`: the history (created, sent, reminder, failed, opened,
  viewed, clicked, paid, reopened, voided, overdue), with the reader's mail
  client sealed in the row. Open, view and click events stop at 300 rows per
  invoice so a client reloading the page cannot grow the table without bound;
  the counters keep counting.
- `invoice_settings`: business name and address, sender, default currency,
  days until due, tax rate, number prefix and next number, default pay link,
  payment instructions and footer.

Amounts are integers in minor units (cents), and every currency is shown with
two decimals. Tax is a rate in basis points (`1000` is 10%). Status is one of
`draft`, `sent`, `paid` and `void`; `overdue` (sent and past due) and
`partial` (sent with a payment recorded) are worked out when reading. Draft
and sent invoices can be edited; only drafts and void invoices can be deleted,
so a sent invoice keeps its history. Reopening a paid invoice sets its paid
amount back to zero; the payments stay in its history.

## CRM links

The plugin reads CRM tables directly (both run on the same database with the
same row key) and only when the organization has the CRM installed:

- `GET /api/invoices/<org>/contacts` lists contacts for the editor's picker.
  Choosing one fills the client's name, email and company.
- `GET /api/invoices/<org>/contacts/<contact>` returns that contact's invoices
  and billed, paid and outstanding totals per currency. The CRM contact card
  calls it and shows an Invoices section with a New invoice button
  (`/dashboard/invoices?org=<org>&contact=<id>` opens the editor with the
  contact chosen). A 404 (no Invoicing in this build) hides the section; a 409
  asks the user to install it.
- With no From email in Invoicing settings, sending uses the CRM's sender
  (`crm_settings.fromEmail` and `fromName`).

The iPhone and Android CRM contact screens show the same Invoices section.
On the desktop the CRM page cannot reach another package's API through its
own `request`, so its registration lists `/api/invoices` in `accountPaths`
and `forkmesh-invoices-request-v1` in `resourceProofs` (the way Calendar
reads other APIs) and signs that one request itself. Its New invoice and
invoice rows open the web page, because the host has no way for one package
to open another package's desktop page.

The invoice detail links the client back to the contact
(`/dashboard/crm?org=<org>&contact=<id>`). Invoicing writes nothing to CRM
tables, so either plugin can be removed without breaking the other.

## Sending and tracking

`POST /api/invoices/<org>/items/<id>/send` sends one email through
`runtime.send_mailtrap` (Postfix or Mailtrap, whichever the instance uses, with
the platform's daily mail budget and mail log) with `tracked: true`, so the
platform adds no footer of its own. The first send moves a draft to `sent`;
later sends are reminders. A failed send leaves the invoice as it was and adds
a `failed` event.

Each invoice has a token, `<invoice id>.<first 24 hex of blind("invoice-view:<id>")>`,
checked with `hmac.compare_digest`. The email holds:

- a 1×1 image at `/api/invoices/-/t/o/<token>.png` that records an open and
  redirects to the logo. Opens in the first 30 seconds after a send are
  ignored, as in the CRM, because mail scanners fetch images straight away;
- a View invoice link to `/invoice?t=<token>`. That page fetches
  `/api/invoices/-/public/<token>`, which records a view and returns only what
  the client needs (no ids, CRM link or email address). Drafts return 404;
- the pay link on that page, `/api/invoices/-/t/c/<token>`, which records a
  click and redirects to the invoice's own https pay link. The target only
  ever comes from the stored invoice, so the route is not an open redirect.

The links use the `PACKAGE_INVOICES_TRACK_ORIGIN` secret when set, then the
sender's own https origin, then the instance's public origin (the desktop and
phone apps send no `Origin`), then `https://app.forkmesh.com`. `/invoice` is a
website route, so the App host 308s it to the website.

The first open, first view and first pay click each ping the person who sent
the invoice (`plugin_activity`, linking to
`/dashboard/invoices?org=<org>&invoice=<id>` so the apps can open it
directly),
naming the mail client when the user agent shows one. Apple Mail and Gmail
load images through proxies, so an open can mean the mail app fetched the
image rather than a person reading it; the page view is the firmer signal.

## Overdue sweep

`deliver_reminders` runs in the host's per-minute phase. It takes up to 25
sent invoices past their due date that have not been flagged, marks each,
adds an `overdue` event and pings the sender once. Changing the due date or
reopening clears the flag. It sends no email to the client.

## Analytics

The overview's `summary` is computed from the plain columns over every invoice
in the organization: counts by state, outstanding and overdue balances and the
amount paid in the last 30 days (each per currency), how many sent invoices
were opened, viewed and had their pay link clicked, the open and view rates,
the average days from first send to paid, and the average hours from first
send to first open. `activity` is the latest 30 send, open, view, click,
payment and overdue events.

## Lifecycle

Install, export and uninstall use the shared organization plugin engine
(`app/src/plugin_data.py`). The export (`forkmesh-invoices-export-v1`) holds
the settings and every invoice with its history. Uninstalling purges the
organization's rows, so every emailed link answers 404 from then on.
