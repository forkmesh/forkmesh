# CRM package

CRM is an opt-in ForkMesh package for organization-shared contacts,
customizable pipelines, lists and views, CSV import and export, Zoho Mail and
Gmail integration, one-off Mailtrap email, founders outreach, emails and
Mastodon conversations linked to contacts from the browser, and contact sheets
shared with other ForkMesh users.

The package owns its web fragments, worker API and data hooks, native desktop
widget, manifest, preview and tests. The browser extension that links mail to
contacts is its own package, `browser-extension`. Platform code only exposes
generic package sockets and generated registries.

## Data model

- **Contacts** (`crm_contacts`, sealed): name, email, profiles (Mastodon,
  Bluesky, X, LinkedIn, GitHub), title, company, location, `flow` (pipeline)
  and `stage`, **Deal value**, **Next step**, `owner` (an account name),
  tags, notes, last-contacted date, source, custom fields in `fields`, and the
  lists the contact is on (`views`).
- **Lists** (sheets): created by CSV import (named after the file, columns in
  the file's order) or by hand. Columns can be renamed, hidden or reordered.
  A list can be bound to a pipeline (`flow`, owners and admins only).
- **Pipelines**: ordered stages. A contact is on a pipeline list when it is in
  that pipeline and the list is the first of the pipeline's lists (in the
  organization's order) among the lists it was put on; a contact on none of
  them shows on the pipeline's first list. The worker (`_contact_in_view`),
  the web page (`crmContactInView`) and the desktop (`contactInSheet`) apply
  the same rule. Renaming a pipeline never moves contacts; removing one leaves
  its contacts without a pipeline.
- **Pipeline types** (`FLOW_ICONS`, drawn by `CRM_FLOW_ICONS` on the web and
  `kFlowIconKinds` on the desktop): Pipeline, Investors, Hiring, Partnerships,
  Customers, Sales, Support, Projects, Events and Grants. New organizations
  start with Investment, Applicant tracking, Partnerships, Customers and
  **Grants** (`researching`, `eligible`, `drafting`, `submitted`, `awarded`,
  `declined`). `awarded` counts as won and `declined` as ended.
- **Stage placement**: every write that places a contact checks the stage
  against the saved flows (`_flow_placement`); an unknown flow becomes the
  first flow and an unknown stage the flow's first stage.
- **Views** (`tableViews` in settings, `{"id", "name", "columns"}`): column
  sets shared across the organization. **Default** and **All** are built in;
  any member may save `tableViews`, every other setting is for owners and
  admins. Deleted built-in columns are recorded in `removedColumns`.

## Editing contacts

- Moves: **Move to pipeline list** (keeps a stage of the same name, else the
  first), **Set stage** (only when the selection shares a pipeline), **Move
  to list**, **Add to list**, **Remove from**. A selection is one
  `POST /api/crm/<org>/contacts/bulk` with `{"edits": [{"id", "flow",
  "stage", "views", "setViews", "removeViews", "owner", "addTags",
  "removeTags"}]}`; the worker takes only those fields and answers with the
  saved contacts and any missing ids.
- **Merge into** (lists): moves every contact onto the target list (and its
  pipeline), carries its visible fields, then deletes the merged list.
- Header menus delete a column outright (`POST /contacts/delete-column`): a
  custom column's values leave every contact; a built-in column (other than
  Name, Email, Flow and Stage) is blanked and hidden, and can be restored.
- Filters: owner (**My contacts**, **Unassigned**, a member), **Next step
  set**, **Quiet 30+ days** (`CRM_QUIET_DAYS`), **Never contacted**, **My
  queue**, **Waiting on us** (newest linked email is inbound), **Possible
  duplicates** (`GET /contacts/duplicates`: shared primary address, profile,
  fediverse handle or multi-word full name; shared firm mailboxes, phones and
  websites are ignored), **Paying members**, **Stage behind evidence**.
- Bulk **Assign / Tag / Untag shown contacts** applies to the filtered rows in
  chunks of 500.

## Importing

- **CSV import**: up to 2000 rows per file; contacts are matched by email, so
  re-importing updates them; columns the CRM has no field for become custom
  columns.
- A file whose every row names the same pipeline in its **Flow** or
  **Pipeline** column opens its new list bound to that pipeline. When an owner
  or admin imports such a file for a starter pipeline the organization does
  not have yet (for example `grants` in an organization created before Grants
  existed), the import adds that starter pipeline first and audits
  `crm.flows.add`; a member's import never changes pipelines. Files that mix
  pipelines open a plain list, as before.
- **GitHub contributors** (desktop): clones the repository with
  `git clone --bare --filter=tree:0 --no-tags` (commits only, no credentials,
  no prompts, 10 minute limit, temporary folder removed), reads `git log
  --all` authors, committers and `Co-authored-by:` trailers, drops masked and
  unmailable addresses and bots, merges by address, and imports
  `Name,Email,Commits,Last commit` through the ordinary CSV path.
- **ForkMesh users** (desktop, ForkMesh administrators only): imports active
  accounts with a valid email from the administrator view of the user
  directory (`GET /api/accounts/users`), newest first, as the list **ForkMesh
  users**; **Refresh** re-imports and never removes. The same read supplies
  profile pictures for contacts whose address belongs to a ForkMesh user.
- **Mastodon followers** (desktop).

## Sharing

A list can be shared with the whole organization, with picked teammates, or
with any ForkMesh user by name. Users outside the organization see it
read-only under **Shared with me**, with only the columns it shows, and can
copy it into their own organization (`POST /api/crm/<org>/views/copy`).
`GET /api/crm/-/shared` lists sheets shared with the signed-in account.

**Shared for editing** (`shareAccess: "edit"`): outside readers can add
contacts to the sheet (`POST /api/crm/-/shared/<sheet>/contacts`) and move them
between the pipeline's stages (`PATCH /api/crm/-/shared/<sheet>/contacts/<id>`).
They may change only visible columns from `SHARED_EDIT_FIELDS` and visible
custom columns; flow, owner, lists, deletes and merges stay with the
organization. A contact outside the sheet answers 404, and an address already
in the organization answers `contact_exists` without revealing which contact.
This lets outside partners run their own pipeline without seeing the
organization's other contacts.

**Links**: `https://<instance>/dashboard/crm?contact=<id>` or `?list=<id>`.
`GET /api/crm/-/open/<id>` resolves a link for the signed-in account: its own
organization, a shared list (opened under **Shared with me**, or in a
shared-only mode for readers with no organization of their own), or 404.
Copying a link shares nothing new.

## Contact detail card

Clicking a contact shows every field, custom columns, lists, who added and
last edited it, linked messages and Mastodon conversations, deck link opens,
ForkMesh account evidence, and **Recent updates** (the newest public posts of
the contact's Mastodon, Bluesky and GitHub accounts, up to five each).
Pictures come from the ForkMesh user, Mastodon, Bluesky, GitHub, then
Gravatar (`gravatar.com/avatar/<sha256 of the email>?d=404`). Posts and
pictures are read on demand from public, unauthenticated endpoints (those
services see the reading device's IP address and the account asked about; see
`www/docs/external-dependency-map.md`), kept in memory for the session;
desktop pictures are cached on disk for a week, including negative results.
Nothing is stored server-side.

## Reach out and follow up

The top of the CRM shows two cards (desktop and web) or two sections (phones):

- **Reach out**: the best never-contacted, reachable lead outside a won or
  ended stage, ordered by next step, deal value, a `rank`/`*_rank` column
  (lowest first), a `score`/`*_score` column (highest first), having an email,
  and age on the CRM.
- **Follow up**: an unanswered reply first (newest first, any stage), then
  contacts last contacted 14 or more days ago with nothing back (longest
  silence first, open stages only); a next step goes first in each group.

Each card shows **Why them**, and offers write/message, **Mark contacted** and
**Skip** (session only). Won stages: `won`, `closed-won`, `customer`,
`renewal`, `committed`, `signed`, `paid`, `active`; ended stages: `passed`,
`rejected`, `closed`, `inactive`, `hired`, `lost`, `closed-lost`, `churned`,
`declined`, `unsubscribed`, `do-not-contact`. The desktop and phones rank the
open list; the web ranks the whole organization. Code:
`CrmFeature::nextContact` (desktop), `crmNextContact` (web),
`CrmNextContact.pick` (iOS and Android). Tests:
`tests/test_qt_plugin_contracts.py`, `tests/test_web_layout.py`,
`tests/contact_detail_checks.js`, `CrmParityTests.swift`, `CrmParityTest.kt`.

### Write with AI

**Write with AI** (cards and detail card, web and desktop) builds one editable
prompt for an email, a direct message or a toot: the goal (first contact or
follow-up, angled by list or pipeline), the organization's brief (setting
`messageBrief`, up to 8000 characters; owners and admins can save it), every
filled field and recent notes, the contact's recent public posts, the six
newest linked messages (quoted tails cut, 900 characters each) and the card's
reason. A fourth format, **Grant application**, is the default for a contact
on a Grants-type pipeline: the prompt asks for the full application in the
funder's own questions, order and limits, a milestone budget in their range,
and a closing checklist of the documents and facts only we can supply (the
rules live in `CRM_AI_APPLICATION_RULES` and the desktop's `aiPrompt`). Lists
named for grants get the grant-funder angle.
**Open in ChatGPT** / **Open in Claude** open the provider's site with
the prompt in the URL (or on the clipboard past 14,000 characters). Nothing is
sent from the CRM and nothing is stored except a saved brief. Tests:
`tests/test_ai_writer.py`.

## Email

### Zoho Mail connection

- **One click** (when the host runs a ForkMesh Zoho app): an owner or admin
  calls `POST /api/crm/<org>/zoho/connect`, accepts Zoho's consent page, and
  `GET /api/crm/-/zoho/callback` exchanges the code in the data center Zoho
  names, reads the first mail account, and stores `{region, accountId,
  clientId, refreshToken, managed: true}` sealed in the organization's
  settings. The `state` is a sealed `{org, account, redirect, expires}` record
  valid for ten minutes; the callback re-checks the role and install. The app's
  client secret never enters organization settings.
- Host secrets: `package_secret(name)` returns only `PACKAGE_<PLUGIN>_<NAME>`
  variables, so the CRM reads `PACKAGE_CRM_ZOHO_OAUTH_CLIENT_ID`,
  `PACKAGE_CRM_ZOHO_OAUTH_CLIENT_SECRET` and optional
  `PACKAGE_CRM_ZOHO_OAUTH_REDIRECT_URI`. Register a server-based client at
  `api-console.zoho.com` with the redirect URI
  `https://app.forkmesh.com/api/crm/-/zoho/callback` and multi-DC enabled.
- **Self Client code**: `POST /api/crm/<org>/zoho/exchange` with `{clientId,
  clientSecret, code, region}` (owners and admins) redeems the code in the
  chosen data center, then every other entry of `ZOHO_REGIONS`, reads the
  mail account and saves the connection. Errors: `zoho_code_incomplete`,
  `zoho_account_missing`, `zoho_code_rejected` (with `zohoError`).
- The in-app **Zoho setup guide** covers both paths.

A sync reads the Sent folder after the others, reserving half the sync limit
for it, and records each sent-folder message as an outbound CRM email unless
that provider message is already linked. Email cells open a compose window in
the organization's Zoho data center (`https://<data center>/zm/comp.do?ct=mailto:<address>`)
or Gmail.

### Sending and tracking

- One-off sends go through the host's Mailtrap capability; Mailtrap keys are
  request-only and never stored. Sends carry `forkmesh_sender`; delivery,
  bounce, open and click events update `crm_email_sends`
  (`mailtrap_webhook_event`), and a status never moves backwards. Mail ends
  with a small tracked footer image.
- Emails linked by the browser extension (`POST /api/crm/<org>/emails/link`),
  recorded by hand (`POST /emails`) or sent (`POST /emails/send`) update the
  contact's last-contacted date. A newly tracked email files a
  `plugin_activity` ping with `meta.celebrate = "crm-email-tracked"`; clients
  celebrate a fresh (under five minutes), unread ping once per device.
- **Mastodon toots** are linked messages with `provider: "mastodon"` and show
  in the **Linked toots** column. Three ways in:
  - *Automatically*: an owner or admin saves the organization's Mastodon
    account and an access token with `read:statuses` under Settings →
    Mastodon toots (`PUT /settings {"mastodon": {"account", "token"}}`; the
    token is sealed and never returned, `{"mastodonDisconnect": true}` drops
    it). `POST /emails/toots` reads the account's own statuses, private
    mentions included (the public API answers 404 for those, so a token is
    the only way to see them), newer than the last one read (`sinceId`; the
    first check reads the newest 200), and links every status that mentions a
    known contact as an outbound toot. It matches handles the way linking does
    (`_contact_handles`) and never creates contacts. A status naming several
    contacts gets one row per contact (`<id>/<handle>` after the first), and a
    status already linked is left alone. Web and desktop call it with
    `{"auto": true}` after the CRM loads; the worker skips a check within ten
    minutes of the last. There is no package cron, so toots are found the next
    time someone opens the CRM. **Check toots now** skips the ten-minute wait.
    The instance must be one of `MASTODON_HOSTS`, which match the manifest's
    `outboundOrigins`.
  - *By hand*: **Log toot** on a contact card asks for the toot's link and
    calls `POST /emails/toot {"url", "contactId"}`. The worker reads the
    status when it can (with the token on the organization's own instance)
    for its text and mentions. If it can't, the link alone is enough: a
    Mastodon status id carries its timestamp (`id >> 16` is Unix
    milliseconds), so the date is still right. Without `contactId`, the
    toot's mentions pick the contacts. A toot written by the contact is
    logged as inbound.
  - *From the browser extension*: on the toot's page, the CRM header's FM
    panel links it (`POST /emails/link`), as for any Mastodon conversation.
    Opening one of your own toots on its permalink
    (`https://<instance>/@<you>/<id>`) links it on its own, one
    `POST /emails/link` per person it mentions with `createContact: true`, so
    a person the CRM does not know becomes a contact. "Your own" is the
    account signed in to Mastodon in that tab or the account saved under
    Settings → Mastodon toots. Each new link pings you: "🎉 New CRM contact
    from Mastodon" when it created the contact, "🎉 Mastodon post tracked in
    the CRM" otherwise; owners and admins get "Mastodon post linked in the
    CRM". Switch it off with **Add people I mention on Mastodon** in the
    extension's popup.
- **Unsent drafts**: a note paragraph starting `DRAFT, not sent.` (optional
  `Subject:` line). **Drafts (N)** lists them; sending or confirming a
  Mastodon reply rewrites the marker to `DRAFT, sent <date> by <name>.`.

### For the Marketing package

- `email_channel_report(runtime, org_bi, since)`: outbound email, linked
  replies, closed deals (won stages) with summed deal value, and whether Zoho
  is connected; `{"status": "not_installed"}` without the CRM.
- `email_suggestions(runtime, org_bi, now)`: contacts to reply to, follow up
  or introduce, with filled templates and compose URLs.

## Founders outreach

One-off sponsorship, investment, partnership, support and follow-up asks from
the organization's default sender, through the host's Mailtrap capability.
Owners and admins can always send and manage the roster; other members must be
on the outreach team (`POST /api/crm/<org>/outreach/team`, audited).
`GET /outreach` returns access, templates, sender, the per-sender daily cap and
recent sends; `POST /outreach/send` sends one email. Every attempt is written to
`crm_outreach_log` with recipient and subject sealed.

## Investor deck

Deck links (`POST /decks`) are minted only for contacts in an investor pipeline
(icon `investors` or flow id `investors`/`investment`); otherwise
`409 deck_needs_investor_pipeline` (audited). `POST /emails/send` and
`/outreach/send` refuse a body carrying a `/pitch-deck/` link or the
`investor` template for a contact outside an investor pipeline. Nothing sends
the deck automatically. Opens are recorded per visit and page
(`crm_deck_sessions`, `crm_deck_page_views`), and the first beacon of a visit
pings whoever minted the link (not the HTML fetch, so link-scanning mail
gateways do not trigger it). On the desktop, **Write email with Claude Code**
(or Codex) asks the default agent for a draft that includes the contact's own
deck link; it opens in the compose window and is never sent automatically.

## ForkMesh account evidence

**Check ForkMesh accounts** (owners and admins; the host also requires a
ForkMesh administrator and the `platform.accounts.evidence` permission) calls
`POST /api/crm/<org>/contacts/evidence` and stores on each contact the
matching account and signup time, the best paid membership, any promotion or
trial, and a `suggestedStage` (`customer` or `trial`). It never moves a stage
by itself; a button moves it after confirmation and appends a dated note.
Evidence is not a column, so exports and shared sheets omit it.

## Team leaderboard and activity history

- **Team leaderboard**: members ranked over 7 or 30 days by emails sent or
  linked, accepted outreach and contacts added (`activity` in
  `GET /api/crm/<org>`, days cut in the caller's `?tz=` offset).
- **List activity history** (`crm_list_activity`): opens (at most one per
  person per list every 30 minutes), list changes, contact changes (field
  names only, never values) and imports. Actor and action are stored in the
  clear; names, field names and subjects are sealed. Rows older than 90 days
  are dropped. `GET /views/activity?view=<id>` returns up to 200 entries, only
  for lists the caller can open.

## Activity pings

Every successful change raises a `plugin_activity` ping (`ACTIVITY_PINGS` in
`content/worker/crm.py`), opening `/dashboard/crm`: contacts added, edited,
merged, deleted or bulk-updated, columns removed, account evidence, emails
tracked, sent, linked, edited or deleted, Mastodon toots logged, mailbox
messages moved or deleted, outreach sends and team changes, deck links, list
changes, settings and a Zoho connection.

- **Who**: the member who acted ("By you in acme") and the organization's
  owners and admins (at most 10 in all). A tracked, sent or linked email
  already celebrates on the sender's devices, so it pings only the others;
  an import pings the importer itself and the others get one folded ping.
- **Quiet**: reads, lookups, list opens, list reordering, an import preview,
  a column-width (`tableViews`-only) settings save and failed requests.
- **Syncs ping only for something new**: a Mastodon sync pings when it logged
  toots ("3 Mastodon posts logged in the CRM"); a Zoho Mail refresh pings
  when new messages arrived ("New email in the CRM mailbox"), not for
  re-read ones.
- **Bursts fold**: contact, email and list edits, bulk updates, settings and
  mailbox refreshes share one ping per member, action and item for 10
  minutes. The first rings the phone; later ones update it silently.

## Leads from the platform

`record_lead(runtime, org, lead)` is the hook `plugin_data.deliver_lead` calls
(today only the affiliate program, `docs/app/affiliates.md`). It adds one
contact to the organization's CRM with the flow, stage, source, tags, custom
fields and note it is given, and returns `created`. It never touches a
contact that already has the email; it returns `exists` and that contact's
id. It returns `not_installed`, `invalid_email` or `crm_full` instead of
raising, so the caller can keep the lead.

## Surfaces

- **Web**: lists and pipelines, the contacts table, board, detail card,
  activity, Zoho boxes, import, settings, team, outreach and drafts.
- **Desktop** (`CrmFeature.cpp`): a three-column page (lists, table, detail
  panel), spreadsheet-style editing, a People grid of contacts with pictures,
  imports, and a rail count of never-contacted contacts. Data is read when the
  CRM opens and on **Refresh**; nothing re-reads itself. Sorting and list
  membership are computed off the GUI thread.
- **iOS and Android**: under Business in the You hub; read `GET /api/crm` and
  `GET /api/crm/<org>?since=<revision>&tz=<minutes>` and offer the same
  editing, sharing, email, outreach, deck, drafts and Zoho Self Client
  connection. GitHub, Mastodon and ForkMesh-user imports and Recent updates
  are desktop/web only.
- **Browser extension** (`marketplace/packages/browser-extension`): links Gmail,
  Zoho Mail and Mastodon conversations to contacts. This package pins the
  oldest compatible extension build (`CRM_EXTENSION_MINIMUM` in
  `content/web/page.js`).

## Install and remove

Install or remove CRM from the Market page. Removing it calls the package's
organization data-lifecycle hook before the install record is deleted, so
contacts, imported mail, email events and encrypted provider settings are
purged. To remove CRM from a source distribution, delete this directory,
remove the `crm` entry from `marketplace/catalog.json`, and rerun the
dashboard, market-catalog, worker and CMake generators.

## Package tests

```sh
python3 -m pytest -q marketplace/packages/crm/tests
```
