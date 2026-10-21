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
- An organization holds at most 5000 contacts (`MAX_CONTACTS`; the D1 trigger
  `trg_crm_contact_limit_5000` aborts the insert with
  `crm_contact_catalog_full`, and replaced the 2000-contact
  `trg_crm_contact_limit` on 2026-10-20). Once it is full, an import still
  updates the contacts it already knows and skips each new row with the row
  error `crm_full` ("was not added because the CRM already holds 5000
  contacts"), which the finish ping lists. Adding one contact by hand, from a shared sheet or
  from a linked mail message answers `409 {"error": "crm_full"}`. Before
  2026-10-20 the import let the trigger's D1 error escape as a 500.
- A file whose every row names the same pipeline in its **Flow** or
  **Pipeline** column opens its new list bound to that pipeline. When an owner
  or admin imports such a file for a starter pipeline the organization does
  not have yet (for example `grants` in an organization created before Grants
  existed), the import adds that starter pipeline first and audits
  `crm.flows.add`; a member's import never changes pipelines. Files that mix
  pipelines open a plain list, as before.
- **Ranked grants** (1.37.0): an empty Grants-type pipeline offers **Load the
  100 ranked grants**, on the web as a button where the empty list would be
  and on the desktop in the pipeline tab's right-click menu. It is one
  `POST /api/crm/<org>/contacts/grants` with `{"flow", "viewId"}`, for owners
  and admins only. The worker reads `content/worker/crm_grants.csv`, which
  `tools/grants_build.py` writes from `docs/finance/grants/grants.json`, puts
  every row on the requested Grants-type pipeline (`grants` when none is
  named, adding the starter pipeline if needed), leaves out programs whose name
  is already on that pipeline, and imports the rest through the CSV import
  into the open list, or a new list named after the pipeline. Loading again
  adds nothing. The rows have no email, so this name check is what keeps a
  second load from duplicating them.
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

### Notes

**Add a note** on the card saves a dated note to the contact and pings the
people connected to its lists. Notes are separate from the free-text
**Notes** field and never overwrite it.

- **API**: `POST /api/crm/<org>/contacts/note` with `{"contact": "<id>",
  "text": "..."}` (any organization member; up to 4000 characters, else
  `413 note_too_long`; blank is `400 note_required`; an unknown contact is
  `404 contact_not_found`). It answers `201` with the note, the saved contact
  and `pinged`, the number of people pinged.
- **Stored**: in the sealed contact as `noteLog`, `[{"id", "at", "by",
  "text"}]`, oldest first, the newest 100 kept (`CONTACT_NOTE_KEEP`). Other
  edits keep it; merging two contacts keeps both logs. Shared sheets never
  carry it. Saving a note does not change the record's "last edited".
- **Logged**: the card's **Activity** lists each note ("Note · <author>"); the
  history of every list the contact is on gets a `contact.noted` entry ("added
  a note on Grace: “...”", the first 140 characters, sealed); the audit log
  gets `crm.contact.note`.
- **Pinged** (`plugin_activity`, title "Note on <contact>", body "By <author>
  in <org> · <list>: <first 140 characters>", one ping per note via
  `dedupe = crm-note:<note id>`), never the author, at most 50 people:
  - a list visible to the organization: every member;
  - a private list: its owner, the organization's owners and admins, and the
    members it is shared with;
  - everyone a list is shared with outside the organization (for example a
    partner sheet shared for editing). Their ping opens the list
    (`/dashboard/crm?list=<id>`) and carries the note text only when the list
    shows its Notes column;
  - a contact on no list: every member.
  Members' pings open the card (`/dashboard/crm?contact=<id>`).

## Reach out and follow up

The top of the CRM shows two cards (desktop and web) or two sections (phones):

- **Reach out**: the best never-contacted, reachable lead outside a won or
  ended stage, ordered by their newest public Mastodon post (newest first;
  contacts without a handle or a post follow), then next step, deal value, a
  `rank`/`*_rank` column (lowest first), a `score`/`*_score` column (highest
  first), having an email, and age on the CRM. The card shows that post's
  date, excerpt and link, and lists the next five in line with theirs.
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

### Newest Mastodon post on Reach out

Rendering the cards queues the Mastodon handles of the first 40 reach-out
candidates (in the order above) for the same read the **Mastodon last update**
column uses: `GET /api/v1/accounts/lookup?acct=<user>` on the contact's own
instance, then `GET /api/v1/accounts/<id>/statuses?limit=1&exclude_replies=true&exclude_reblogs=true`.
The reads are public and unauthenticated, made by the browser or desktop app
(not the worker), and kept in memory for the session:

- a read is fresh for 15 minutes, a handle shared by several contacts is read
  once, and the account id is remembered so a refresh asks only for statuses;
- at most four reads run at once, and at most two against one instance;
- a `429` holds that instance back for `Retry-After` or five minutes, whichever
  is longer, and every handle on it shows why until then;
- a missing account (`404`/`406`/`410`), a timeout or an unreachable instance
  shows as "Mastodon post unavailable" and the contact keeps its place among
  those without a post; a failed re-read keeps the post already shown.

Finished reads re-rank the cards (at most every 400 ms), so the order settles
as posts arrive. The iOS and Android pick does not read Mastodon. Code:
`crmQueueReachMastodon`, `crmPumpMastodonLatest`, `crmNextPostHtml` (web);
`CrmFeature::queueReachMastodon`, `refreshMastodonUpdates`, `nextPostLabel`
(desktop). Tests: `tests/contact_detail_checks.js` (mocked instances),
`tests/test_qt_plugin_contracts.py`.

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
- Scopes: `ZohoMail.messages.READ`, `ZohoMail.messages.CREATE` (1.36.0, so
  **Send AI email** can send from the mailbox), `ZohoMail.folders.READ` and
  `ZohoMail.accounts.READ`. A connection made before 1.36.0 holds only the
  read scopes: reconnect it (**Reconnect with Zoho** in settings, or a new
  Self Client code with all four scopes) before automated email can go
  through Zoho.

**Zoho status and missing steps** (1.39.0). After 1.36.0 an organization
connected earlier kept syncing, so it looked healthy, but every automated
email was refused with `zoho_send_not_allowed`: a refresh token keeps the
scopes it was granted, and only a new consent adds `ZohoMail.messages.CREATE`.
Syncing again could never fix it, and nothing on the CRM page said so. Now:

- The sealed `zoho` settings record what the token can do: `connectedAt`
  (set whenever a new refresh token is saved, which also clears the fields
  below), `scope` (from Zoho's token response at connect and on every sync's
  refresh; one-click connects store the requested scopes when Zoho omits it),
  `sendRefusedAt` / `lastSentAt` (the last refused and accepted Zoho send),
  `tokenRejectedAt`, and `lastSyncError` / `lastSyncErrorAt`.
- `canSend` is the newer of a refused or accepted send when there is one,
  else whether `scope` holds `ZohoMail.messages.CREATE` (or `.ALL`), else
  whether the connection has a `connectedAt`. Connections made before 1.39.0
  have none, so they read as read-only until a sync's token response shows
  the send scope or a reconnect saves a new token.
- `settings.zoho` (`_zoho_project`) carries `canSend`, `lastSyncAt`,
  `lastSentAt`, `lastSyncError` and `steps`: `[{key, action, text}]` with
  `key` one of `connect`, `self_client`, `token`, `send`, `sync_failed`,
  `sync`, and `action` one of `connect`, `reconnect`, `sync`, `guide`.
- The web CRM shows a Zoho Mail bar under the page header: a status word,
  the account, last synced and last sent, and each step with its button
  (one-click reconnect goes straight to Zoho's consent page; without the
  ForkMesh Zoho app it opens settings for a Self Client code). The iPhone CRM
  header shows the same bar, and its settings add **Reconnect Zoho Mail**
  when sending is not allowed.
- The web page syncs Zoho by itself when the last attempt is 15 minutes old
  (`POST /mail/import {auto: true}`; the worker answers `skipped: true`
  inside `ZOHO_AUTO_SYNC_MS`, so open tabs do not multiply syncs).
- `_opened_settings` used to pass the stored record through the input
  sanitiser, which kept only the four credentials, so `lastSyncAt` was lost
  on every read and the page always said "not synced yet". It now keeps the
  stored fields.

Tests: the last four tests in `tests/test_auto_email.py`.

**Desktop Zoho bar and exact refusals** (1.40.0). 1.39.0 told users to tap
Reconnect Zoho Mail at the top of the CRM, but the desktop CRM had no such
bar: its only way back was the credential form in Settings, and its error
still pointed there. Now:

- The desktop CRM shows the same Zoho Mail bar between the toolbar and the
  summary, in the amber attention banner, whenever `settings.zoho.steps` is
  not empty: the state (not connected, read only, last sync failed), the
  account, last synced and last sent, the numbered steps, and up to two
  buttons for their actions. **Reconnect Zoho Mail** opens Zoho's consent page
  when the server has ForkMesh's Zoho app and CRM settings (Self Client code)
  otherwise; members who are not owners or admins see it disabled. A refused
  auto-send adds the reconnect step to the bar at once, without a reload that
  would hide the error line. The desktop guide no longer says the CRM only
  reads mail.
- `_zoho_send` keeps the scope from the refresh it just made and stores it.
  A 401 or 403 is reported as `zoho_send_not_allowed` (read-only sign-in,
  reconnect) only when Zoho's code names the OAuth scope or the token's scope
  lacks `ZohoMail.messages.CREATE`/`.ALL`. When the token can send and Zoho
  still refuses, the answer is `send_failed` with Zoho's error code or status
  description, and the bar does not ask for a reconnect that would not help.

Tests: the two `forbidden_send` tests in `tests/test_auto_email.py` and the
desktop bar test in `tests/test_note_drafts_and_zoho_guide.py`.

A sync reads the Sent folder after the others, reserving half the sync limit
for it, and records each sent-folder message as an outbound CRM email. Email
cells open a compose window in the organization's Zoho data center
(`https://<data center>/zm/comp.do?ct=mailto:<address>`) or Gmail.

**Sent mail marks its recipients contacted** (1.33.0). Before 1.33.0 a sync
stored sent mail with no contact, so a lead you had just written to through
Zoho stayed on the **Reach out** card at its first stage until someone pressed
**Mark contacted**. Now each address on a sent message's `To` (HTML entities
decoded, the sender's own mailbox skipped) is matched against every address a
contact carries (`_contacts_by_address`: the email first, then custom-column
addresses), and each matched contact gets the message linked through
`_link_message` (the first contact keeps the Zoho message id, the others get
`<id>/<address>`), so it shows in the contact's email history and files a
`contact.emailed` list activity entry. Linking stamps **Last contacted** with
the send time (forward only) and moves the contact to the `contacted` stage
when its pipeline has one and the contact sits before it (`sourced`,
`prospect`, or a stage the pipeline does not know). Contacts at or past
`contacted` (`meeting`, `passed`, `customer`, ...) keep their stage; a
pipeline without a `contacted` stage (Customers, Applicants, Grants) only gets
the date. A recipient who is not a contact is stored unlinked, as before, and
no contact is created.

The sync is idempotent: a message already linked to a contact is not
re-linked, it only re-applies the same date and stage rule (a no-op once
applied), and a message stored unlinked by an earlier sync is linked the first
time a sync finds a contact for it. So mail imported before 1.33.0, or before
the contact was added, is backfilled by the next **Sync Zoho Mail**, as far
back as the sync reads (the newest messages of the Sent folder, half of the
200-message limit). The response and the `crm.zoho.import` audit carry
`outboundRecorded` (new send rows) and `contactsAdvanced` (stage moves).

The same stage rule runs for every outbound `POST /emails/link` that does not
name a flow or stage (the browser extension, and `send_email` logging in
`tools/forkmesh_mcp_server.py`); its response carries `stageAdvanced`. Inbound
links never move a stage. Tests: `tests/test_zoho_marks_contacted.py`.

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
`GET /outreach` returns access, templates, sender, the per-sender daily cap
(`OUTREACH_DAILY_LIMIT`, 100 in a rolling 24 hours, shared with automated
emails; it was 50 until 1.38.0, when the iPhone's auto-send was paced at 100 a
day) and recent sends; `POST /outreach/send` sends one email. Every attempt is written to
`crm_outreach_log` with recipient and subject sealed.

## Automated outreach email

**Send AI email** (the Reach out and Follow up cards and the contact detail
card, on the web and the desktop) writes and sends one email with no editing
step, the way the spider suggests outreach but sent for you. A confirmation
dialog names the recipient and the link first.
`POST /api/crm/<org>/outreach/auto {contactId, mode?, list?, reason?}`:

- **Who may send.** Owners, admins and the outreach team, from the outreach
  sender (`_outreach_sender`). Each send counts toward the per-sender
  `OUTREACH_DAILY_LIMIT` and lands in `crm_outreach_log` (template `auto`).
- **Refused** when the contact has no email (`contact_has_no_email`), sits at
  an ended stage (`contact_closed`), or got a CRM email in the last three days
  (`recently_emailed`, `AUTO_EMAIL_GAP_MS`), so a double click or two
  teammates never send twice.
- **The link.** A contact outside an investor pipeline gets one trackable
  link, the sender's referral link `https://forkmesh.com/r/<account>`, which
  counts visits and signups. A contact in an investor pipeline gets no link
  in the note: the whole deck is in the email (see "The investor email"
  below). Their deck link is still minted (the active one, else a new one,
  as `POST /decks` makes it) and returned as `link`, so the team can share it
  by hand.
- **The model.** `runtime.ai_json` (the host model, `docs/app/package-ai-failures.md`)
  gets the organization's brief (`messageBrief`, else a short ForkMesh brief
  for the `forkmesh` organization), the contact's fields and notes, the angle
  for the list or pipeline (journalists, investors, grants, partners, users),
  their recent activity with us, the last six messages with them, the link,
  and the lessons below.
- **Recent activity** (`_auto_email_activity`, 2026-10-19): when we last
  contacted them, when they last wrote to us, and how often and how long they
  read the investor deck. Each message we sent in the conversation shows
  whether they opened it, when they clicked and how long they read
  (`_auto_email_engagement`). The model is told to use it to judge interest
  and never to mention the tracking.
- **Never how we got them** (2026-10-19). Most investors arrived by CSV import,
  and drafts told them so. The prompt no longer carries when the record was
  added or updated and by whom, the contact's `source` or `owner`, custom
  fields named like import bookkeeping (`AUTO_EMAIL_PRIVATE_FIELD_RE`: import,
  source, sheet, rank, score, confidence, evidence, owner, added, batch), or
  the list or sheet name in the angle ("They are an investor: …"). The system
  rules (`AUTO_EMAIL_RULES`, also in the agent fallback prompt) forbid saying
  or hinting at an import, list, spreadsheet, database, CRM, record or
  ranking, and the deck note forbids calling the deck attached or linked: it
  is in the email body. The desktop and web confirm dialogs now say the whole
  investor deck is in the email, not "their tracked deck link". It answers
  `{subject, body}`. Drafts with `[placeholders]`, under 60 or over 2400
  characters, or with any link not in the prompt are refused and asked for
  again once (`AUTO_EMAIL_ATTEMPTS`); a missing referral link is appended. An
  investor note may carry no link at all, so any link in it is refused.
- **Learning from opens and clicks.** The prompt's "What our past emails
  taught us" reads the organization's last 120 sent Mailtrap emails (manual,
  automated and outreach) with their open and click results: overall rates,
  rates for first contacts, follow-ups, deck links and referral links (three
  or more each), up to three emails that got a click, two opened without a
  click, and three nobody opened after two days, with their subjects and
  openings. The model is told to follow what got clicks, avoid what the
  unopened ones share, and change one small thing each time so the team keeps
  learning. The response returns the same totals as `learning`.
- **Without a host model** the worker answers `503 ai_unavailable` (or `422
  draft_unusable`) with the full `prompt`. The desktop then asks the default
  agent (Claude Code or Codex) for a `Subject:` line and body and posts them
  back as `{subject, text, model}`; the same checks apply. iPhone auto-send
  does the same with its own writers and, last, the paired desktop's agent
  through `agents.write` (`docs/native/DESKTOP_AGENTS.md`). The web says the
  host has no model. `preview: true` returns the draft without sending.
- **Sent through Zoho Mail when the organization has it connected** (1.36.0).
  Before 1.36.0 every automated email went out through `runtime.send_mailtrap`
  (the server's Mailtrap key, from the outreach sender), so it never showed in
  the team's Zoho Sent folder; the Zoho connection could only read mail. Now
  `_zoho_send` refreshes the access token, reads the account
  (`GET /api/accounts/<id>`) and posts the tracked HTML to
  `POST /api/accounts/<id>/messages`, so Zoho files it in Sent like any mail
  written there. The sender is the outreach sender when Zoho lists it among
  the account's addresses (`sendMailDetails`, `emailAddress`), else the
  mailbox's primary address. The row is stored as `transport='external'`,
  status `accepted`, provider `zoho`, `tracked: true`, with Zoho's message id
  as `external_id`, so the next **Sync Zoho Mail** finds it already linked
  instead of logging the message a second time. The three-day guard and the
  learning history count these rows with the Mailtrap ones (`transport =
  'mailtrap' OR status <> 'external'`; learning keeps only tracked rows).
  A connected Zoho never falls back to Mailtrap: a token without the send
  scope answers `409 zoho_send_not_allowed` asking for a reconnect, a one-click
  connection whose host Zoho app is gone `409 send_failed`, any other Zoho
  refusal `502 send_failed`; both leave a `failed` row and do not touch
  the contact. The response carries `via` (`zoho` or `mailtrap`) and the
  `fromEmail` used.
- **Without Zoho** it is sent through `runtime.send_mailtrap` with category
  `CRM automated outreach` and `forkmesh_crm_send_id`, so Mailtrap's own
  delivery, open and click events also reach the row.
- Either way the contact's last-contacted date moves, `contact.emailed` is
  recorded, and the usual tracked-email celebration and "Automated outreach
  email sent" team ping go out. Founders outreach (`/outreach/send`) and
  one-off sends (`/emails/send`) still use Mailtrap.

### Open and click tracking

The worker builds the HTML itself (`_tracked_email_html`): each link in the
body points at `<origin>/api/crm/-/t/c/<send id>.<signature>?u=<url>&s=<url
signature>`, and a 20 px ForkMesh logo at the end loads
`<origin>/api/crm/-/t/o/<send id>.<signature>.png` and links to forkmesh.com
through the same click redirect. Signatures are the host's keyed blind index
of `crm-track:<send id>` and `crm-click:<send id>:<url>`, so a link cannot be
pointed anywhere else (`404 link_not_found`) and an open cannot be forged
for another send. `<origin>` is the `PACKAGE_CRM_TRACK_ORIGIN` package
secret, else the caller's https origin, else `https://app.forkmesh.com`.

- The routes need no session. An open answers `302` to the logo
  (`TRACK_LOGO`), a click `302` to the target, with `no-store`.
- Each hit adds to `crm_email_tracking` (`opens`, `clicks`, first
  `opened_at`/`clicked_at`, `last_at`) and moves the send's status to `open`
  or `click` (never backwards, `_advance_send_status`). Hits in the first 30 s
  after sending (`TRACK_QUIET_MS`) are ignored: mail gateways scan links and
  images as the message arrives.
- The first open and the first click ping the sender: "📬 <name> opened your
  email" and "🔗 <name> clicked the link in your email". Since 1.38.0 the
  ping's body adds, under the subject, who the reader's address belongs to
  and the client that loaded the logo or link:
  `Whois 66.249.84.10: Google LLC (GOOGLE, 66.249.64.0/19, US)` and
  `Agent: Gmail image proxy · <user agent, first 200 characters>`. The host
  hands the tracking route the request's `cf-connecting-ip` and `User-Agent`
  (`request_visitor()`, nothing else). A public address is looked up once, on
  that first hit, through RDAP (`https://rdap.org/ip/<ip>`, which redirects to
  the registry; the five registries are in `outboundOrigins`): the
  registrant's name, the network name, its CIDR (or start – end range) and
  country. Private, loopback and reserved addresses are never looked up; a
  failed lookup still pings with "no answer". `_mail_agent_label` names the
  Gmail and Yahoo image proxies, Outlook, Thunderbird, Zoho, Superhuman,
  Proton and Apple Mail (WebKit without Safari), else device and browser.
  Neither the address nor the agent is stored: they live only in the ping.
  Gmail, Yahoo and Apple Mail Privacy Protection load images through their
  own servers, so for those readers the whois names the provider, not the
  reader.
- Email rows (`GET /emails`, the contact's emails) carry `opens`, `clicks`,
  `openedAt`, `clickedAt`, `automated` and `linkKind`; the web detail card
  shows "Sent by AI · 2 opens · 1 click".
- Opens are a hint, not proof: Apple Mail Privacy Protection loads images for
  the reader, and clients that block images never report one. Clicks are the
  stronger signal, and the lessons weigh them first.

### Ratings

`POST /api/crm/<org>/outreach/rate {emailId, rating: 1-5, note?}` (1.38.0)
stores the sender's rating and a short note (300 characters) on an automated
send. Only automated sends can be rated (`409 email_not_automated`), by
their sender or an owner or admin (`403 rating_forbidden`); it is audited as
`crm.outreach.rate`. Email rows carry `rating` and `ratingNote`. The next
automated email's "What our past emails taught us" adds up to three emails
rated 4-5 (write like these) and three rated 1-2 (do not), and the five newest
notes, which the model is told outrank the open and click numbers. The
iPhone's auto-send review popup is the first client
(`docs/native/GENIE.md`, "Suggestion mode and pocket mode").

Tests: `tests/test_auto_email.py`.

### The investor email

Since 20 Oct 2026 an automated email to an investor carries the whole
investor deck in its body, under the short note the model writes. All
fourteen slides, from the cover to Sources & contact, are there, with the
same copy and order as `app/public/pitch-deck/deck.html`. There is no "Full
slide" or "Open the full deck" link: the founders asked for the deck to
arrive as an email, not as a link to click. Before that, the email carried a
condensed deck (cover, Problem, Product, Proof, Market, Team, The round) with
a link to each full slide, and the note was told to send the reader to the
deck link.

- **Why it is rebuilt, not copied.** `deck.html` cannot be mailed as it is.
  Mail clients drop its script, CSS variables, grid, `clip-path`, fixed bar
  and web font, and Gmail clips any message over about 102 KB, which would
  also cut off the reading beacons at the bottom. `_mail_deck_sections`
  rebuilds each slide from tables and inline styles, so it lays out in Gmail,
  Outlook and Apple Mail. `test_an_investor_email_carries_all_fourteen_slides_and_no_deck_links`
  checks the order, the light-and-dark declaration, the images, that the only
  links are the reply button and the tracked forkmesh.com ones, and that the
  page stays under 100 KB (80 KB on 21 Oct 2026).
- **Why it looks like the homepage.** The first version was a light page of
  white cards, and investors read it on phones in dark mode. Gmail's dark
  mode recoloured the cards and text but not the white-backed images, and
  every slide was a card with a blue dot, a "PROBLEM · 02" label and more
  cards inside it, so on a phone it read as generated rather than designed.
  A dark-only version fixed that, and on 21 Oct 2026 the founders asked for
  a light mode in the homepage's style too. The deck now follows
  `www/public/index.html`: a white page with left-aligned ABC Favorit
  headings whose second half is grey ("Code with agents. Own the forge."),
  a white badge pill above the cover title, the textured dark and white
  button pair, and dark notched "slabs" (`DECK_SLAB` `#0b0b0c`) for the
  cover image, Why now, Beyond the desk and The round. The notched corners
  are two transparent PNGs, `slab-top.png` and `slab-bottom.png`, above and
  below a dark cell, because mail clients drop `clip-path`.
- **How it survives each mail app's dark mode.** Every screenshot and GIF
  sits on a dark slab or a dark panel and is drawn on the same `#0b0b0c`,
  so no image ever shows as a white box. The page declares
  `color-scheme: light dark`. Apple Mail and Outlook for Mac then apply
  `_mail_night`, a `prefers-color-scheme: dark` block that recolours every
  light element through its `fm-ink`, `fm-body`, `fm-line`, `fm-tile`…
  class to `DECK_NIGHT` on a `#18181b` page, while the slabs stay as they
  are. Gmail ignores that query and inverts light colours itself; it leaves
  the dark slabs, panels and images alone, so the result is close to the
  Apple Mail one. A light element therefore always takes its colour from
  `DECK_TONES["light"]` plus the matching class, and an element on a slab
  from `DECK_TONES["slab"]` with no class.
- **Layout rules.** Light slides are full-width bands with generous padding
  (76 px, 56 px on a phone); nothing is a card inside a card. No slide
  carries a section label or number: the heading carries it. Lists are
  hairline rows or check-marked lines in one column, so nothing squeezes on
  a 360 px screen. Business model shows the three plans as columns, with
  Agent team on a dark panel like the homepage's Pro plan, and the columns
  stack on a phone (`fm-col`). Competition opens with a check matrix
  (`DECK_MAIL_MATRIX`: agent workflow, full forge, local-first) summarising
  the rows under it. Small grey labels are sentence case; "Live in
  production" and "In build" are green because they are status. The
  headline proof figure, 7,416 merged agent branches, is live text on a dark
  panel, not a counting GIF, because Outlook shows only a GIF's first frame.
  "Reply to the founders" is a `mailto:` to the sender with a "Re:" subject;
  it is the one untracked link, and "forkmesh.com" goes through the click
  tracker like the logo.
- **Size.** The deck helpers use a short font stack (`DECK_SANS`) because
  it repeats on every element, and the email is about 80 KB, under Gmail's
  102 KB clipping point that would cut off the reading beacons.
- **Copy lives in `DECK_MAIL_*`.** When a slide's wording changes in
  `deck.html`, change the matching constant too. Nothing copies it across.
- **Images** come from `https://forkmesh.com/assets/email/` and are drawn on
  the slabs' `#0b0b0c` by `tools/render_email_assets.py` (Pillow):
  `slab-top.png`, `slab-bottom.png`, `deck-cover.jpg`, `deck-flow.gif` (the
  mobile app screens, without a caption, since the step list sits under
  it, and cut between screens rather than crossfaded: the fade frames
  tripled it to 518 KB, too large to send as a pull request through the
  API), `xr.jpg` and the two avatars. `xr.jpg` is the "Beyond the desk" slide flattened for email: the
  desktop app and a Claude Code session seen through AR glasses, with the
  watch's agent list beside them. The deck draws that slide with CSS, which
  mail clients cannot. `python3 tools/render_email_assets.py` rewrites every
  file; to redraw one, import the module and call its function (for example
  `xr()`). The light images of the first version (`hero.jpg`, `mesh.gif`,
  `agent-flow.gif`, `count.gif`) stay in the folder because emails already
  sent load them, but nothing draws or uses them any more; the animated mesh
  banner under the logo was removed at the founders' request.
- **The note.** The prompt's "## The investor deck" section tells the model
  that the deck follows its note, so it should keep to a few sentences, add
  no link, and ask for a reply.

### Reading time, slide reach and the summary ping

Every tracked email (investor or not) reports how long it stayed open. An
investor email also reports the furthest deck slide the reader got to. Mail
clients run no script, so all of it comes from images that CSS loads:

- **Opened.** The forkmesh logo at the top of the email is the open pixel,
  `/api/crm/-/t/o/<token>.png`. It records the open, starts a view
  (`crm_email_reading.view_at`), stores the reader's mail app
  (`crm_email_report.client`, from the request's user agent) and redirects to
  the real logo. Hits in the first 30 s after sending are ignored
  (`TRACK_QUIET_MS`), so link scanners don't count.
- **Read time.** Twelve hidden cells (`READ_MARKS`: 5, 15, 30, 45, 60, 90,
  120, 180, 240, 300, 420 and 600 s) each run a 1 ms animation delayed by
  their number of seconds, which sets a background image
  `/t/r/<token>.<seconds>.png`. A mark counts only after a view has started,
  and only once at least 80% of its seconds have passed since then
  (`READ_LAG_SHARE`), so a proxy that fetches every image at once does not
  report ten minutes of reading.
- **Slide reach.** Each of the fourteen slides ends with a marker
  (`.fm-s1` … `.fm-s14`). A scroll-driven animation
  (`animation-timeline: view()`) on it loads `/t/s/<token>.<slide>.png` when
  it scrolls into view, at least 1.5 s after the view started
  (`DEPTH_MIN_LAG_MS`). The furthest slide is stored as a percentage in
  `crm_email_reading.depth`, so the table needed no new column. The email row
  turns it back into `slideCount`, `slideReached` and `slideTitle` (from
  `DECK_PAGE_TITLES`). Emails sent before 20 Oct 2026 carried 25/50/75/100%
  markers (`/t/d/…`), and those still count.
- **Pings.** The sender gets two pings per email at most, both straight to
  the phone (`runtime.notify` → the host's APNs push, in the same request
  as the beacon):
  1. **First open.** "📬 Ivy opened your email", with the network owner and
     mail app.
  2. **One summary.** For a deck, it is sent when the reader reaches the last
     slide or passes five minutes (`SUMMARY_DECK_SECONDS`), whichever comes
     first: "🏁 Ivy read your whole deck" or "📖 Ivy has spent 5 min+ on your
     deck". For a plain note it is sent at 30 seconds
     (`SUMMARY_NOTE_SECONDS`). The body reads, for example, "Read for 4 min+ ·
     reached slide 9 of 14 (Market) · iPhone · Apple Mail".

  The host gives packages no timer, so the summary rides on the reader's own
  beacons, and `crm_email_report.summary_at` keeps it to one. A reader who
  leaves before the last slide and before five minutes gets no summary
  ping; the CRM still shows how far they got.
- **Where it shows.** The email row returns `readSeconds`, `scrollDepth`, the
  slide fields, `mailClient` and `readingReported`. It shows on the web
  activity line (`crmReadingParts`), on the iOS contact screen
  (`CrmActivity.engagementLine` and the email's Reading section), and on the
  desktop contact card and Linked emails tooltip (`emailReadingLine`), for
  example "read 4 min+ · reached slide 9/14 · Market · iPhone · Apple Mail".
  Android does not show it yet.
- **Which mail apps report it.** Apple Mail runs CSS animations, so it
  reports read time. It also reports slide reach where its engine supports
  scroll-driven animations (WebKit in the 2025–26 releases). Gmail, Yahoo
  and Outlook do not run them (`SILENT_MAIL_CLIENTS`), so `readingReported`
  is `"no"` and the apps say "Gmail doesn't report read time" for an opened
  email. That way an empty line is not mistaken for "didn't read".

Tests: the reading, summary, Gmail and legacy-beacon tests in
`tests/test_auto_email.py`, `tests/test_web_reading_report.py`, the desktop
contract in `tests/test_qt_plugin_contracts.py`, and iOS
`CrmReadingReportTests`.

## Investor deck

Deck links (`POST /decks`) are minted only for contacts in an investor pipeline
(icon `investors` or flow id `investors`/`investment`); otherwise
`409 deck_needs_investor_pipeline` (audited). `POST /emails/send` and
`/outreach/send` refuse a body carrying a `/pitch-deck/` link or the
`investor` template for a contact outside an investor pipeline. Only **Send AI
email** sends the deck without a compose step, and only to a contact in an
investor pipeline; it sends the deck itself in the email body, not the link
("The investor email" above). Opens are recorded per visit and page
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

## Teammates on cc

`GET /api/crm/<org>` returns `memberEmails`: the verified addresses of the
organization's other members, for the viewer only (the host capability
`organization_member_emails` needs the `organization.members.email` permission,
the CRM installed for the organization, and the viewer to be a member; at most
50, active web accounts only). The phone's spider puts them on cc when it opens
a suggested email (`docs/native/GENIE.md`), so the rest of the team sees each
outreach. Without the capability the list is empty.

## Team leaderboard and activity history

- **Team leaderboard**: members ranked over 7 or 30 days by emails sent or
  linked, accepted outreach and contacts added (`activity` in
  `GET /api/crm/<org>`, days cut in the caller's `?tz=` offset).
- **List activity history** (`crm_list_activity`): opens (at most one per
  person per list every 30 minutes), list changes, contact changes (field
  names only, never values), notes (their first 140 characters) and
  imports. Actor and action are stored in the
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
- **Notes** ping everyone connected to the contact's lists instead (see
  [Notes](#notes)), each note once.

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
