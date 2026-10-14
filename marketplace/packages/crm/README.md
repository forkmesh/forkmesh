# CRM package

CRM is an opt-in ForkMesh package for organization-shared contacts, customizable
relationship flows, CSV import/export, grouped Zoho Mail boxes, provider send
links, one-off Mailtrap email, founders outreach, emails and Mastodon
conversations tied to contacts from the browser, and contact sheets shared
with other ForkMesh users.

The package owns its web fragments, Worker API and data hooks, native desktop
widget, manifest, preview, and tests. The Chrome extension that links mail to
contacts is its own package, `browser-extension`. Platform code only
exposes generic package sockets and generated registries.

## Sheets

Every CSV import lands in its own tab (a *sheet*) named after the file and
ordered like the file's header row; the **Import contacts** tab in the CRM
picks the file. A sheet can be renamed, its columns hidden or reordered, and
shared with the whole organization, with picked teammates, or with any
ForkMesh user by name. On the desktop, the **Outside the organization** field
of **Add members** suggests names from the ForkMesh user directory as you type,
leaving out the owner, people already on it and organization members (who are
in the roster above). It completes only the name after the last comma, so
several people can be added at once. Users outside the organization see it read-only under
**Shared with me**, with only the columns it shows, and can copy it into
their own organization as a new tab (`POST /api/crm/<org>/views/copy`).
`GET /api/crm/-/shared` lists the sheets shared with the signed-in account. While
at least one sheet is shared with them, the dashboard lists the CRM in their
Plugins column even though no organization of theirs installed it.

### Sheets shared for editing

A sheet's owner (or an organization owner or admin) can tick **They can add
contacts to it and move them along its pipeline's stages** in the sheet
dialog, which stores `shareAccess: "edit"` on the sheet. For the people it is
shared with outside the organization, the tab then works as their own
pipeline: **Add contact** creates the contact in the sheet
(`POST /api/crm/-/shared/<sheet>/contacts`), and board cards drag between the
pipeline's stages (`PATCH /api/crm/-/shared/<sheet>/contacts/<id>`).

- The shared view carries `canEdit` and the pipeline's `stages`, and shared
  rows carry `flow` and `stage`, so the reader's board draws the owning
  organization's stages.
- A contact they add lands in the sheet's pipeline (first stage unless they
  pick one), on the sheet, owned by them, with source `shared sheet (<name>)`.
- They may change only visible columns from `SHARED_EDIT_FIELDS` and visible
  custom columns, and the stage within the pipeline. Flow, owner, lists,
  deletes and merges stay with the organization, and a contact outside the
  sheet answers 404. An address already in the organization answers
  `contact_exists` without saying which contact holds it.
- This is how the hackathon partners (Ugeh, Cenzo) run their own pipelines
  inside the forkmesh CRM without joining the organization or seeing its other
  contacts (2026-09-30). On the web a partner opens their sheet from its link
  even with no organization of their own (shared-only mode, see "Links to a
  contact or a list"); the desktop still shows shared sheets read-only. Since
  2026-10-01 the forkmesh sheets **Ugeh** and **Cenzo** are shared for editing
  with `ugehchristopher` and `cenzo`.

## GitHub contributor import

On the desktop, **Import GitHub** in the CRM toolbar (beside **Import CSV**),
**Import GitHub contributors…** on the **Import CSV list** page (beside
**Import Mastodon followers…**) and the same item in that page's right-click
menu turn the people who worked on a public GitHub repository into a new list.
The toolbar button exists because the page sits under **More** at the bottom of
the list sidebar, where the import went unnoticed. It takes `owner/name`, a
`github.com` link or a `git@github.com:` clone address, and:

1. clones the repository into a temporary folder with
   `git clone --bare --filter=tree:0 --no-tags`, which fetches commits only (no
   trees or files), with no stored credentials and terminal prompts off, so a
   private or missing repository fails instead of asking for a login. The
   clone is stopped after 10 minutes, and the folder is deleted once the list
   is read;
2. reads `git log --all`, so every branch is included, with mailmap off to
   avoid fetching `.mailmap` from the partial clone. It takes each commit's
   author, committer and every `Co-authored-by:` trailer;
3. leaves out masked addresses: GitHub's `users.noreply.github.com`,
   `noreply@github.com`, and any address whose local part or domain is a
   `noreply`, `no-reply` or `donotreply` label (GitLab, Codeberg and
   ForkMesh's own `users.noreply.<host>` included). It also drops addresses no
   one can mail (no dotted domain, or `.local`, `.localdomain` and similar
   host names that git makes up) and `[bot]` accounts;
4. merges each address, compared case-insensitively, into one row: the name on
   its newest commit, how many commits it appears on, and the date of the
   newest one. Rows are ordered by commit count, and at most 2,000 (the CSV
   import's row limit) are kept.

The result goes through the ordinary CSV import as
`github-contributors-<owner>-<name>.csv` with the columns
`Name,Email,Commits,Last commit`, so the column mapping step, the tab name
(`Contributors of owner/name`) and split uploads all work as they do for any
file. Commits and Last commit become the sheet's own columns rather than
overwriting a contact's notes. Contacts are matched by email, so importing the
same repository again updates the contributors it added before. The web page
has no clone step: a browser cannot run git.

## ForkMesh user import

On the desktop, a ForkMesh administrator gets **Import users** in the CRM
toolbar (beside **Import GitHub**) and **Import ForkMesh users** on the
**Import CSV list** page and in its right-click menu. It reads the
administrator view of the user directory (`GET /api/accounts/users` with the
bearer session or the signed `kAccountUsersAdminProof`, through the host's
`userRecords`), because only that view carries email addresses, and imports it
as the list **ForkMesh users**:

1. only active user accounts with a valid email address are kept (the status
   line says how many were left out for having none), one row per address;
2. rows are ordered newest signup first, so when there are more than 2,000 (the
   CSV import's row limit) the newest are the ones imported;
3. the columns are `Name,Email,Signed up,Email notifications`, mapped to the
   contact name and email and two of the list's own columns, with no mapping
   step.

The list is marked by its file name, `forkmesh-users.csv`. Once it exists the
toolbar button reads **Refresh users**, the page button **Refresh ForkMesh
users**, and the list's tab menu offers **Refresh ForkMesh users**: each
re-imports into that same list, so new signups are added and the users already
on it are updated, matched by email. A refresh never removes contacts: a user
who closes their account stays on the list. Non-administrators do not see the
buttons.

The same directory read gives an administrator's desktop each user's profile
picture (`avatarPng`), so any contact whose email address belongs to a
ForkMesh user shows that user's own picture, on every list, not only
**ForkMesh users**. The CRM reads the directory when it loads, at most once
every fifteen minutes (an import or refresh of the list also counts), keeps the
pictures in memory, and never writes them to disk. Other members see the
contact's other pictures, since only an administrator can match a user to an
email address. The web page has no user import: it has no administrator proof to
read the directory with.

## Pipelines and lists

A *list* (a sheet) holds the contacts put on it. A *pipeline* is a set of
stages; each pipeline has its own. Binding a list to a **pipeline type** walks
its contacts through that pipeline's stages. Lists and pipelines are separate
things: several lists can use the same pipeline, and they never hold the same
contact. A contact is on a pipeline list when it is in that pipeline (its
`flow`) and the list is the first of that pipeline's lists, in the
organization's order, among the lists it was put on (`views`). A contact in the
pipeline that was put on none of its lists shows on the pipeline's first list,
so a pipeline with one list still shows everyone in it, as it did before lists
could share a pipeline. The worker (`_contact_in_view`, for shared sheets), the
web page (`crmContactInView`) and the desktop (`contactInSheet`) all apply this
rule.

The binding is on the list's own right-click menu under **Pipeline type**, is
stored on the sheet as `flow`, and only the organization's owners and admins
can change it - the worker drops the field for everyone else.

Pipelines themselves are added and edited from the same menus: **New
pipeline...** creates a flow and the list bound to it in one step, and **Manage
pipelines...** opens every pipeline the organization has, with its ordered
stages and how many contacts are in it, to rename, restage, reorder or remove.
A pipeline's id is what contacts and lists are stored against, so renaming one
never moves anybody; removing one leaves the contacts in it without a pipeline
and is confirmed with that count.

Right-clicking contacts (the selected rows on the desktop, the row under the
pointer on the web) moves them:

* **Move to pipeline list** puts them on that list and in its pipeline,
  keeping the stage when the pipeline has one by that name and starting at its
  first stage otherwise. It takes them off the pipeline's other lists, so
  lists that share a pipeline stay apart.
* **Set stage** walks them along the pipeline they are already in. It is
  offered only when the whole selection shares a flow, because a stage means
  nothing across two pipelines.
* **Move to list** puts them on that list and takes them off the others;
  **Add to list** leaves the others alone; **Remove from** takes them off the
  list that is open (a plain list, or a pipeline list that shares its pipeline).
  The worker's `views` field only ever adds, so these send `setViews` and
  `removeViews` instead.

**Merge into** on a list's right-click menu is a submenu of the other lists
the account may manage; picking one, after a confirmation, moves every contact
on the list onto it (into that list's pipeline too, when it has one), shows on
the target every field the merged list showed, and then deletes the merged
list. It was a picker dialog behind **Merge into...**; the submenu makes the
merge one right click and one choice.
It is one `POST /contacts/bulk`, one `PATCH /views/<target>` for the fields,
and one `DELETE /views/<merged>`. **New list...** (the **+** over the list
sidebar) creates a list and, for owners and admins, puts it on a pipeline.
Deleting one list of a pipeline that has several leaves the pipeline alone;
its contacts fall back to the pipeline's first list.

A move shows at once: the table or page changes its own copy of the contacts
first, then saves. The desktop sends a whole selection as one
`POST /api/crm/<org>/contacts/bulk` (`{"edits": [{"id", "flow", "stage",
"views", "setViews", "removeViews"}]}`), rather than a PATCH per contact, so a
large move is one request; the worker takes only those membership fields from
it and answers with the saved contacts plus any ids that no longer exist.

Each contact also carries a **Deal value** and a **Next step**, which are
columns like any other: hideable, sortable, editable in place, and carried
through CSV import and export along with the pipeline itself.

### Stages a flow has

Every write that places a contact checks its stage against the organization's
saved flows (`_flow_placement`): creating a contact, a PATCH or bulk edit that
sends `flow` or `stage`, a CSV import and a shared-sheet copy. An unknown flow
becomes the first flow and a stage the flow lacks becomes its first stage, so
no contact lands in a stage no pipeline column shows. An edit that sends
neither field leaves the stored placement alone. Before this, a customized
`customers` flow (`new`/`in-progress`/`complete`) received 315 imported
contacts at `lead` and showed none of them (2026-09-30).

### Owners and follow-up

`owner` holds an account name. The bulk edit (`POST /contacts/bulk`) carries
it, so a selection can be assigned in one request. The web toolbar filters by
owner (**My contacts** matches the signed-in account, **Unassigned** an empty
owner, or any member) and by follow-up: **Next step set**, **Quiet 30+ days**
(the same rule as the quiet tag: no contact for `CRM_QUIET_DAYS` outside a
closed stage) and **Never contacted**.

Three more follow-up choices turn the table into a work list (2026-10-01):

- **My queue**: contacts the signed-in account owns, outside a closed stage,
  that wait on us or have a next step. Ones waiting on us come first, then the
  longest untouched.
- **Waiting on us**: the newest email recorded against the contact is
  inbound, so the next move is ours.
- **Possible duplicates**: reads `GET /contacts/duplicates` and shows each
  group together. A group is contacts sharing a primary address, a GitHub,
  LinkedIn or X profile, a fediverse handle, or a full name of two or more
  words. Addresses kept in custom columns, phones and company websites are left
  out because a firm's partners share them: the investor import keeps a shared
  firm mailbox in each partner's `other_emails` on purpose, and counting it
  flagged 20 firm/partner pairs that are different people. A shared
  name alone is a hint, not proof, so right-click a row and **Merge into** the
  record to keep.

**More → Assign / Tag / Untag shown contacts…** applies one owner, or adds or
removes tags, on every contact the current tab and filters show, through the
bulk edit in chunks of 500. The bulk edit takes `addTags` and `removeTags`
beside the membership fields; neither touches tags it does not name.

## Desktop table

The CRM button on the app rail carries a count of the shown organization's
contacts that have never been contacted: no **Last contacted** date, which a
linked or sent message, **Mark contacted now** or the cell's date picker set.
The page loads its records once when the rail is built, drawing the saved copy
first, so the count is there before the page is opened; it follows every load,
edit and Refresh after that, and disappears at zero.

### Reach out and follow up (1.29.0)

The user asked for two people at the top of the CRM, "one as the new best
qualifying lead to reach out to and one as the person who we should follow up
with so we can be in two modes", on every platform (2026-10-13). The single
next-best-contact card (below) is now two cards side by side on the desktop
and the web, and two sections at the top of a list's contacts on iOS and
Android:

- **Reach out** picks the best new lead: a contact never contacted (no
  **Last contacted**), with no unanswered reply, outside a won or ended stage,
  that can be reached. Among them, in order: one with a **Next step** first,
  then the highest **Deal value**, then the lowest number in a custom column
  named `rank` or ending in `_rank` (the investor list's `rank` and
  `priority_rank`), then the highest in a column named `score` or ending in
  `_score` (`coding_fit_score`), then an email address over a handle alone,
  then the longest on the CRM. **Why them** names what qualified them ("deal
  value 5,000; ranked 2 (rank); an email address on file"). The count reads
  "N never contacted".
- **Follow up** picks someone already in conversation: an unanswered reply
  first (newest first, any stage), then someone last contacted 14 or more
  days ago with nothing back (longest silence first, open stages only). A
  next step goes first within each group. The count reads "N due".

A lead who wrote in before we ever contacted them is a follow-up, not a new
lead. Each card has its own buttons (write or message, **Mark contacted**,
**Skip**); a skip holds for the session and **Show skipped again** clears it.
Marking the reach-out lead contacted moves them out of that card; they come
back on the follow-up card 14 days later if nothing comes in.

Where each surface looks: the desktop and the phones rank the open list's
contacts, the web ranks the whole organization (as 1.27.0 did). Reachable is
an email or Mastodon handle on the desktop, and an email, Mastodon or X handle
on the web and the phones. The phone sections show over the list view, not
the board, and hide while selecting. On the phones **Email** opens the app's
compose screen and the name opens the contact.

Code: `CrmFeature::nextContact(NextMode)` and `nextContactCard` (desktop),
`crmNextContact(mode)` and `CRM_NEXT_MODES` (web), `CrmNextContact.pick`
(`native/ios/ForkMeshCore/Sources/FMPackages/CrmNextContact.swift`,
`native/android/core/.../market/CrmNextContact.kt`). Tests:
`tests/test_qt_plugin_contracts.py`, `tests/test_web_layout.py`,
`tests/contact_detail_checks.js`, `CrmParityTests.swift`, `CrmParityTest.kt`.

### Write with AI (1.30.0)

The user asked for a button on the reach-out and follow-up cards that
generates a message, email or toot with web ChatGPT or Claude, pasting in
everything about ForkMesh and its links, everything about the person (their
recent updates and links), and every past message and bit of context
(2026-10-13). **Write with AI** sits on both cards and in the contact detail
card, on the web and the desktop, and opens a dialog that builds one prompt:

- **What to write**: Email (with a `Subject:` line, under 150 words), Direct
  message (LinkedIn, X or Bluesky, under 80 words) or Toot (mentions their
  Mastodon handle, 500 characters, and replies in our existing Mastodon
  thread when the CRM has one). It starts on Email when there is an address,
  else Toot when there is a Mastodon handle.
- **The goal**: a first contact when nothing has gone out yet (or the
  **Reach out** card opened it), else a follow-up that picks up the
  conversation. The open list or the pipeline sets an angle, the same
  keywords the desktop's older **Copy prompt** uses: a journalist is pitched a
  story, an investor the opportunity, a partner a fit, a user one way it
  helps.
- **About us**: the organization's saved brief (setting `messageBrief`, up to
  8,000 characters). Until one is saved, the `forkmesh` organization starts
  from a built-in ForkMesh brief (what it is, its promises, and links to the
  site, app, World, docs, blog, changelog, press kit, status page and
  installer); other organizations start blank. The brief edits in the dialog,
  and an owner or admin can **Save for the organization**. The web
  (`CRM_FORKMESH_BRIEF`) and desktop (`kForkMeshBrief`) copies are held
  identical by a test.
- **About them**: every filled field (title, company, location, pipeline and
  stage, deal value, next step, owner, source, tags, last contacted, when
  added, the ForkMesh account check on the web, email, each profile as a
  link, phone, their investor deck link, up to 40 custom fields) and our
  notes (the newest 2,500 characters).
- **Their recent public posts**: the same Mastodon, Bluesky and GitHub reads
  the detail card makes, read when the dialog opens (the desktop reuses what
  the detail card already read this session). An account that could not be
  read says so, and until the reads finish the prompt says "Not read yet".
- **Our conversation so far**: the six newest linked messages (email and
  Mastodon), oldest first, each with who wrote it, how, the subject and its
  link, and the body (fetched one by one from `GET /emails/<id>`; quoted
  reply tails are cut, 900 characters each). An unsent draft is marked as
  not delivered.
- **Why now**: the card's **Why them** reason when a card opened it.

The prompt stays editable. **Open in ChatGPT** and **Open in Claude** open
`https://chatgpt.com/?q=…` or `https://claude.ai/new?q=…` in a new tab with
the prompt filled in and also copy it to the clipboard; past 14,000 characters
of link they open the bare site and the prompt is pasted from the clipboard.
**Copy prompt** only copies. Nothing is sent from the CRM, and nothing is
stored except a brief an admin saves. The desktop detail panel's **Copy
prompt** became **Write with AI**; the row button and menu item keep the short
toot prompt.

Code: `crmAiPrompt`, `openCrmAi`, `sendCrmAi` and `[data-crm-ai-dialog]` (web),
`CrmFeature::aiPrompt` and `showAiWriter` (desktop), `messageBrief` in
`_settings_data` (worker). Tests: `tests/test_ai_writer.py`,
`tests/contact_detail_checks.js`.

### Next best contact (1.26.0)

The user asked for the desktop CRM to show the next best person to contact,
with a card holding all of their info at the top, and an explanation of why
(2026-10-09). Every pipeline and list tab now opens with that card above the
stage strip and the table. Since 1.29.0 it is split into the **Reach out** and
**Follow up** cards above; the rest of this section is the 1.26.0 rule.

Who it picks follows the rule the Marketing Email channel's suggested emails
already use (`email_suggestions`, under "Sent mail and the Marketing Email channel"
below), worked out on the desktop from the contacts and linked
messages it already holds, so the card needs no new request and the two
surfaces agree. Among the open list's contacts that have an email address or a
Mastodon handle:

1. **Waiting on us**: the newest linked email or Mastodon message is inbound,
   came in within the last 60 days, and is newer than both our last outbound
   message (unsent drafts and failed sends do not count) and **Last
   contacted**. Any stage. Newest reply first.
2. **Follow-up due**: last contacted 14 or more days ago. Longest silence
   first.
3. **Never contacted**: no **Last contacted** date. Longest on the CRM first.
   Unlike the Email channel, which only suggests intros on the Investment and
   Partnerships pipelines, every pipeline counts, so a customers list gets a
   recommendation too.

Contacts at a won stage (`won`, `closed-won`, `customer`, `renewal`,
`committed`, `signed`, `paid`, `active`) or an ended one (`passed`,
`rejected`, `closed`, `inactive`, `hired`, `lost`, `closed-lost`, `churned`,
`declined`, `unsubscribed`, `do-not-contact`) are only picked for an
unanswered reply. A contact with a **Next step** goes ahead of the others in
its group. Someone contacted in the last 14 days who has not written back is
not due and is left out.

The card shows the picture, name, title, company, location, stage, pipeline
and last-contacted date; a **Why them** line that says which rule picked them
(when they wrote, the subject, how many days it has been, the next step); then
every other field the record fills in, its custom columns, how many emails and
Mastodon conversations are linked, when and by whom it was added, the lists it
is on and its notes. A muted line under it says how many people on the list
are due and the order they are taken in. Its buttons:

- **Write email** opens the organization's Zoho Mail or Gmail compose window,
  like the address cell (**Open Mastodon** for a contact with only a handle).
- **Open contact** opens the full detail card in the side panel.
- **Mark contacted** stamps **Last contacted** now, which takes them out of
  line and brings up the next person.
- **Skip** moves on to the next person for this session only; nothing is
  saved. Once everyone due has been skipped, **Show skipped again** puts
  them back.

The card looks at the whole list, whatever stage the strip has narrowed the
table to. When nobody on the list is due, the card says so and why. A list with no
contacts shows no card. The rule's stage sets, the 60 and 14 days and the
unsent statuses are checked against the worker's constants by
`tests/test_qt_plugin_contracts.py`.

### Best person to contact on the web (1.27.0)

The user asked for the web CRM page to show one contact person, the best to
contact from the CRM, with a button to email them with Zoho or message them on
Mastodon or X (2026-10-10). The Contacts tab now opens with a **Best person to
contact** card above the sheet tabs (since 1.29.0 the **Reach out** and
**Follow up** cards).

It picks with the desktop card's rule above (replies waiting on us, then
follow-ups 14+ days quiet, then never contacted; a **Next step** first within
a group; won and ended stages only for an unanswered reply), over every contact
in the organization rather than one list, since the web page asked for one
person from the whole CRM. A contact counts as reachable with an email address,
a Mastodon handle or an X handle. A shared sheet hides the card.

The card shows the name (which opens the detail card), title, company, stage,
last contacted and a **Why them** line, and a button per channel the record
names:

- **Email with Zoho** opens `https://<data center>/zm/comp.do?ct=mailto:<address>`
  in the organization's Zoho data center, the same handler the desktop and the
  Email channel use.
- **Message on Mastodon** opens the newest linked Mastodon conversation with
  them, or their profile on their own instance when none is linked.
- **Message on X** opens their X profile, where the Message button is; X has
  no link that opens a direct message from a handle alone.

**Mark contacted** stamps **Last contacted** now and **Skip** moves on for this
page load. The stage sets, days and unsent statuses are checked against the
worker by `tests/test_web_layout.py`, and the ranking runs for real in
`tests/contact_detail_checks.js`.

The header's actions are icon buttons with a short label under each: Add
contact, Import CSV, Import GitHub, Import users (administrators only; Refresh
users once the list exists), Export CSV, People, Pipelines, Sync Zoho Mail, Zoho
Mail, Gmail, Extension, Settings and Refresh. **Pipelines** opens
the same Manage pipelines dialog as the Import CSV list page and a list's
right-click menu, which were both out of the way; it is enabled for
organization owners and admins, who alone may change pipelines. The icons are the app's own octicons, tinted
to the theme.

The page has three columns. On the left, the lists run top to bottom instead
of as tabs: each pipeline and each list (a pipeline no list is bound to still
gets one, and **No pipeline** collects contacts whose pipeline was deleted),
then All activity, Zoho Mail boxes, Import CSV list and People with pictures
under **More**. Every entry, the **More** pages included, is its own card with
a border, and the open one is tinted and outlined in the theme's highlight
colour. The app stylesheet paints a background on every widget, so the list's
own selection colour was hidden under each row and nothing showed which list
was open; the card's `current` property now carries it. Each list shows its
contact count; the people who can open it (the owner first, then everyone it
is shared with) as up to three initials avatars with a `+N` for the rest,
which open the share menu on a click; the faces of up to six of its contacts
(pictures first, newest first) and, for a pipeline list, a bar of its stages
with a count for each (New 1,349 · In progress 0 · Complete 0). A right click on a list renames,
moves, retypes, merges or deletes it; a double click renames it. The table of
the open list sits in the middle, and a click on a contact opens it in the
third column (see below). There is no every-contact list. Sorting, cell text
and list membership are worked out on a background thread and the rows are put
in a chunk at a time, so a large organization keeps the window responsive.

**People** opens a grid of only the contacts whose picture has loaded, from
every list, sorted by most recent (when the contact was added), last contacted
or name. Each card shows the picture, the name, the lists the contact is on
and its pipeline and stage; a click opens the contact in the side panel. The
top search box filters the grid as it does the table. The pictures are read at
twice the grid's size and kept on disk for a week, like the rows' pictures.

Each tab's contacts are a spreadsheet-style table. A click into a cell edits
it in place (the change is saved on the contact); the email cell is a link
that opens a compose window in the organization's Zoho Mail data center, or in
Gmail when CRM settings → *Email addresses open in* says so (Zoho opens
`https://<data center>/zm/comp.do?ct=mailto:<address>`, the handler Zoho Mail
registers for mailto links; the older `/mail/compose.do` path now answers
*Page not found*). With the ForkMesh browser extension installed, the message
sent from that window is linked to the contact on its own and moves their
**Last contacted** (the extension's *Mail you send, tracked on its own*); and the name
cell, its picture included, opens the contact's detail card, with F2 still
editing the name where it stands. A click on a header sorts the column,
dragging a header reorders it, a right click hides or restores columns, and
the trailing **+** header adds a custom column, stored inside the contact's
`fields` like a CSV header the CRM has no built-in field for.

### Views

Which columns the table shows, and in what order, is a *view*, picked from the
**View** box above the table on the desktop and beside **Columns** on the web
page. Every list opens in **Default**: Name, Email, Mastodon, Bluesky, Twitter,
LinkedIn and GitHub. **All** shows every column the list carries: the built-in
ones, the list's own imported or added columns (a custom column another list
brought stays out), and on the desktop the computed ones. Neither can be
edited: hiding, showing or moving a column in either changes the table at once
without asking anything, marks the view *(modified)* in the picker and shows a
**Save view** button beside it. **Save view** stores the columns on screen as a
new view named *View 1*, *View 2*…, which **Rename view…** can rename. The
unsaved change is kept only on screen: picking another view drops it, and
undoing it by hand clears the mark. **New view…** (the **…** button on the
desktop, the **Columns** menu on the web) and the desktop header menu's
**Save as new view…** ask for a name and save the columns on screen.

A custom view is shared with the whole organization and applies to every
list. It is stored in the CRM settings as `tableViews`, a list of
`{"id", "name", "columns"}` where `columns` is the ordered field names it shows:
built-in fields, custom keys, and the desktop's `mastodon-latest`,
`linked-emails` and `linked-toots`, which the web page skips. Any member may
save `tableViews` with `PUT /api/crm/<org>/settings`; every other setting stays
with owners and admins. While a custom view is open, hiding or showing a
column, and dragging a header, save it at once; **Rename view…** and **Delete
view…** change it for everyone. A client keeps the view fields it has no column
for, so a web edit does not drop the desktop-only ones. Which view is open is
per person: the web page remembers it per organization in the browser, the
desktop for the session.

Adding a column, or restoring a deleted built-in one, puts it on the list's
own layout as before (which is also what a sheet shared outside the
organization shows) and, from Default, switches to All so it is on screen.

A right click on a header, on the desktop or the web page, also deletes that
column outright. A custom column's
values leave every contact and the column leaves every list. A built-in column
(any but Name, Email, Flow and Stage) is blanked on every contact, hidden on
every list, and recorded in the settings' `removedColumns`, so neither the
desktop nor the web page shows it again. Deleting or restoring a built-in
column is for owners and admins; **Restore deleted column** on the same menu, or
adding a column by that name, brings it back empty. It is the same
`POST /contacts/delete-column` as a custom column.

**Mastodon last update** shows the newest public post of each contact whose
Mastodon handle names an instance (`@name@instance` or a profile URL). The
column starts empty and fills a row at a time when the refresh button at the
right of that row's cell is pressed: the desktop then asks the instance's
public API directly, keeps the answers in memory for fifteen minutes, and never
stores them; a double click opens the post. No instance is reached just
because a row was drawn.

On the desktop each name carries the contact's picture. The initials show on
one of the web page's six tones until a picture loads, and they stay when no
picture does. Each load (opening the CRM or pressing **Refresh**) reads the
pictures not yet in memory, a few at a time, at the size a row shows; a detail
card reads its own larger one when it opens. It tries the ForkMesh user whose
email address the record carries (see ForkMesh user import above), then the
Mastodon account, then Bluesky (its public profile API), then GitHub
(`avatars.githubusercontent.com`), and last Gravatar
(`gravatar.com/avatar/<sha256 of the email>?d=404`), moving on to the next when
one has no picture. So a contact with no picture anywhere else gets their
Gravatar. The service pictures are kept in memory for the session and on disk
for a week; a service that answers 404 (no such GitHub user, no Gravatar) is
remembered on disk as an empty file for the same week, so a large list does not
ask Gravatar about every address on each load.

Nothing on the page re-reads itself. The organization's records load when the
CRM is opened, and **Refresh** in the header (the desktop's **Refresh**
button) is the only thing that reads them again, so a teammate's import, edit
or send shows up when you ask for it.

The window's top search box is the table's filter while the CRM is on screen,
and from anywhere in the app it lists matching contacts under a **CRM**
heading once the CRM page has been opened in this session; picking one opens
the CRM on that row. The **All activity** tab is
one log of email sends and imported Zoho messages, and **Import CSV list**
picks a CSV and shows the lists already imported. Its **Contact** column names
the contact the message is linked to (the other party's address when the
record has no name), and the **Email ID** is a link: one click opens the
message in Gmail, Zoho Mail or Mastodon.

Every click on a list or pipeline tab starts its stage filter on **New** (or,
when it has no stage by that name, its first stage in pipeline order); the
chevrons and the Stage filter switch from there, and a Refresh keeps the stage
picked. A contact picked from the top search box opens on its own stage.

## Zoho Mail connection

**Connect Zoho Mail** is one click when the host runs a ForkMesh Zoho app.
An owner or admin clicks it (in the CRM actions menu or in CRM settings),
`POST /api/crm/<org>/zoho/connect` returns Zoho's consent page, and after
**Accept** Zoho sends the browser to `GET /api/crm/-/zoho/callback`. The
callback exchanges the code for a refresh token in the data center Zoho
names (`accounts-server`), reads the first mail account from
`/api/accounts`, stores `{region, accountId, clientId, refreshToken,
managed: true}` sealed in the organization's settings, and returns to
`/dashboard/crm?org=<org>&zoho=connected`, where the page syncs once.

- The `state` parameter is the sealed `{org, account, redirect, expires}`
  record, base64url-encoded; it expires after ten minutes, and the callback
  checks again that the account is still an owner or admin and the package is
  installed. A state that does not open, has expired, or names a data center
  outside `ZOHO_REGIONS` returns to the page with an error instead.
- The app's client secret never enters organization settings: a `managed`
  connection takes the client from the host at every sync, so rotating the
  secret needs no reconnect.
- The host keeps the platform generic: `package_secret(name)` returns only
  environment variables named `PACKAGE_<PLUGIN>_<NAME>`, so the CRM reads
  `PACKAGE_CRM_ZOHO_OAUTH_CLIENT_ID`, `PACKAGE_CRM_ZOHO_OAUTH_CLIENT_SECRET`
  and the optional `PACKAGE_CRM_ZOHO_OAUTH_REDIRECT_URI`, and no package can
  read another's or the platform's secrets.
- To turn it on, register a server-based client at `api-console.zoho.com`
  with the redirect URI `https://app.forkmesh.com/api/crm/-/zoho/callback`,
  enable multi-DC, and set the two or three variables on the served stack.
  Until then `oneClick` is false and the button opens CRM settings, where an
  organization's own OAuth client can still be entered by hand.
- On the desktop, **Connect Zoho Mail** makes the same request (signed, so
  it has no browser origin: the host's `PACKAGE_CRM_ZOHO_OAUTH_REDIRECT_URI`
  supplies the callback) and opens Zoho's consent page in the browser; the
  browser lands on the web CRM, and the desktop picks the connection up on its
  next refresh.

### Connect with code: a Self Client without curl (1.23.0)

Setting up an organization's own Zoho Self Client used to end in two `curl`
calls: redeem the code at the right `accounts.zoho.*` host, then read
`accountId` from `/api/accounts`, and paste four values into settings. Codes
expire in minutes and work once, and a code sent to the wrong data center, to
the server-based client's id, or with a missing `redirect_uri` all answer the
same `invalid_code`, so the manual exchange failed often and gave no clue why.

`POST /api/crm/<org>/zoho/exchange` with `{clientId, clientSecret, code,
region}` now does that on the server (owners and admins only). It tries the
chosen data center first, then every other entry of `ZOHO_REGIONS`, without a
`redirect_uri` (a Self Client has none); on success it reads the first mail
account in that data center and saves `{region, accountId, clientId,
clientSecret, refreshToken}` sealed in settings, exactly what the manual form
saved. A blank client id or secret falls back to the stored self-client
values, so reconnecting needs only a new code. Failures answer
`zoho_code_incomplete`, `zoho_account_missing`, or `zoho_code_rejected` with
`zohoError`, Zoho's reason from the first data center tried.

On the web, CRM settings has a **Self Client code** field and **Connect with
code**; it closes settings, reports the account and data center, and runs the
first sync. On the desktop the same field sits in Settings and Save redeems
it after saving the other settings. Part B of the setup guide is now: create
the Self Client, generate a code, paste client id, secret and code. The
callback and the exchange share `_zoho_redeem` and `_zoho_mail_account`.

### Sent mail and the Marketing Email channel (1.22.0)

A sync reads the Sent folder (`folderType` Sent, or a folder named Sent,
Sent Items, Sent Mail or Sent Messages) after the other folders and keeps
half of the sync's limit for it, so a full Inbox no longer crowds sent mail
out. Each synced message records `sent` in its sealed data.

`email_channel_report(runtime, org_bi, since)` is what Marketing's Email
channel reads: outbound email since `since` (CRM sends and linked messages,
accepted outreach, Zoho Sent messages not already linked), linked replies,
and closed deals (contacts at a won stage: `won`, `closed-won`, `customer`,
`renewal`, `committed`, `signed`, `paid`, `active`) with their summed deal
value, plus whether Zoho Mail is connected. It returns
`{"status": "not_installed"}` when the CRM is not installed for the
organization. See the Marketing README's Email section.

`email_suggestions(runtime, org_bi, now)` (1.24.0) lists contacts to email
next for the Email channel's **Suggested emails**: unanswered replies,
follow-ups after 14 quiet days and intros to new Investment and Partnerships
contacts. Each is filled in from the outreach templates and carries a Zoho
Mail (or Gmail) compose URL. The rules are in the Marketing README's
"Suggested emails on the Email channel" section.

### Opens, per sender and per contact (1.25.0)

One-off sends and outreach now carry `forkmesh_sender` (the account that
sent them) next to the CRM's own custom variables. On the cores the host
keeps those variables with the send and hands them back with every Postfix
delivery, bounce, open and click, so `mailtrap_webhook_event` updates
`crm_email_sends` there as it always did for Mailtrap. Each open also posts
a note in the operator's #email-log that names the sender
(`docs/MAIL_LOG.md`, "Package mail and the tracked footer"). The mail ends
with a small ForkMesh logo footer whose image records the open.

A send's status no longer goes backwards: once it is `open` or `click`, a
later `accepted`, `delivery`, `delivered` or `soft bounce` event leaves it,
and `open` does not replace `click`.

Mail written in Zoho Mail itself never passes through ForkMesh. To see it
opened, each person adds the tracked footer from **Settings → Account →
Tracked email footer** to their Zoho signature. That footer is one per
account, so it counts opens of that person's mail without saying which
message or contact.

### Setup guide

**Zoho guide** on the desktop toolbar and **More → Zoho setup guide** on the
web open the same step-by-step guide, and **Connect Zoho Mail** opens it too
while one-click is off (it used to open CRM settings with no explanation of
where the client id and secret come from). The guide's first line says which
case applies: already connected, one-click on (skip to the last step), or
one-click off. Part A is the operator setup above (API Console server-based
client, the exact redirect URI, Multi-DC, the three `secrets.env` lines,
`optional_secrets` in `deploy/fleet.toml`, `tools/next_vault.py sync
--execute`). Part B is one organization's own Zoho Self Client: generate a
code for the three read scopes, and paste the client id, secret and code into
CRM settings' **Connect with code** (1.23.0; it used to be two `curl` calls
and four pasted values). Buttons open the API Console, copy the redirect URI or the
`secrets.env` lines, open CRM settings, and run Connect Zoho Mail (enabled
only once one-click is on). The values never pass through the guide: the
secret goes into `secrets.env` or the settings form.

## Unsent drafts

A follow-up written for a person to send lives in the contact's notes as a
paragraph that starts with `DRAFT, not sent.` The rest of that first line says
how to send it (a line mentioning Mastodon means a reply in the outreach
thread), an optional `Subject:` line follows, and the message runs to the next
blank line. **Drafts (N)** on the desktop toolbar and **More → Drafts (N)** on
the web count those paragraphs across the organization's contacts and list
them: contact, channel, recipient and message, each with one button.

- **Send** (email) opens the compose form filled in with the contact's
  address, the subject and the message. After Mailtrap accepts it the note's
  marker becomes `DRAFT, sent <date> by <name>.`, so the draft leaves the list
  and the text stays in the history.
- **Reply on Mastodon** copies the message, opens the outreach toot (the
  contact's newest linked Mastodon conversation, else the first status link in
  the notes) and asks whether the reply was posted; yes rewrites the marker the
  same way and stamps Last contacted.

Only the first unsent marker of a contact counts, and only at the start of a
line, so a later note that quotes the phrase does not add a draft.

## ForkMesh account evidence

"Is this signup a customer?" is answered from the platform's own records, not
from a CSV. **More → Check ForkMesh accounts** (owners and admins; the server
also requires a ForkMesh administrator) sends
`POST /api/crm/<org>/contacts/evidence` (optional `{"ids": [...]}`), which asks
the host's `account_evidence` for every address a contact holds (its email,
then any address in a custom column) and stores on the contact:

- `account.account` and `account.signedUpAt`: the ForkMesh user the address
  belongs to and when it signed up (empty when no active user account has it);
- `account.paid`: the best paid membership (Patreon writes these as platform
  `role_grants`), `{tier, since, until, active}`;
- `account.promotion` and `account.trial`: a hackathon prize or the 14-day
  Supporter trial, with the same dates;
- `account.suggestedStage`: in a flow with those stages, `customer` for an
  active paid membership and `trial` for an active trial or prize, unless the
  contact already sits there or later (a closed last stage does not count);
- `account.email`, the address that matched, and `account.recordedAt`, when
  this evidence last changed (a re-check that finds the same rewrites nothing).

It never moves a stage. The card and the row show a chip (**Pro member**,
**On trial**, **Membership ended …**, **Signed up …**; hover for the dates),
and when the evidence is ahead of the stage a **→ customer** button moves it
after a confirmation and appends a dated note naming the account and the
membership dates. The follow-up filter gains **Paying members** and **Stage
behind evidence**. The evidence never leaves the organization: it is not a
column, so CSV export and sheets shared outside the organization leave it out.
The agent tools read it in `account` and run the check with
`crm_check_accounts`.

Why the gate: matching an address to an account and its payments says who pays
ForkMesh, so the host answers only for a ForkMesh administrator, the same people
who already read the user directory with emails, and only for a package that
declares `platform.accounts.evidence`. On 2026-10-01 the only evidence for a
"customer" signup was a CSV signup date; the owner asked for code that proves it.

## Investor deck guard

The deck only goes to contacts in an investor pipeline (icon `investors`, or
the flow id `investors` or `investment`). The worker enforces it, so no client
can click through:

- `POST /decks` answers `409 deck_needs_investor_pipeline` (audited as denied)
  for a contact elsewhere; a link made earlier still opens for its stats.
- `POST /emails/send` and `POST /outreach/send` refuse a message whose body
  carries a `/pitch-deck/` link, or that uses the `investor` outreach template,
  when the recipient is a contact outside an investor pipeline. An address that
  is not a contact yet (a cold investor) is not blocked.
- The web **Pitch deck** button and the desktop **Write investor email** say
  why and stop, where both used to ask for a confirmation.

Nothing sends the deck on its own: signing up creates no deck link and no
email. On 2026-09-30 a signup in the customers pipeline was sent the deck by
hand; the confirmation prompt added that day could still be clicked through,
and on 2026-10-01 the owner asked that signups never receive it, so moving the
contact to an investor pipeline is now the only way.

## Agent-written investor email

On the desktop, the contact detail card has a **Write email with Claude Code**
(or **Codex**) button. It hands the current agent (the **Default agent** in
Settings, through the host's `askAgent`, see `docs/marketplace/README.md`) one
prompt that holds:

* an investor brief of ForkMesh (problem, product, proof, customers, business
  model, market, competition, team, the round, contact). It is copied from the
  deck. The proof figures are the deck's own, stamped on 13 September 2026, so
  refresh them in `investorEmailPrompt` whenever the deck's proof slide changes;
* every field on the record, its custom fields, its lists, when it was last
  contacted, the linked messages and Mastodon conversations, how the deck has
  been read so far, and any recent posts the card has already read;
* the sender's name and address, taken from the CRM settings;
* the contact's own deck link. This is the same one link per contact that
  **Pitch deck** mints: the button reuses the active link or mints it with
  `POST /decks`.

The agent answers with `{"subject", "body"}`. If the body leaves the link out,
the desktop adds it. The draft then opens in the one-off Mailtrap compose
window, and nothing is sent until you press **Send**. A sent email is linked to
the record and stamps it as contacted, like any other email from that window.
The button asks for confirmation first when the contact is not in an investor
pipeline, the same guard **Pitch deck** has.

Tracking: opens of the link are already recorded per visit and per page
(`crm_deck_sessions`, `crm_deck_page_views`). The card's Activity list now
shows the deck line (opens, pages read, reading time, last opened), which the
desktop takes from the summary's `decks`. When a visit sends its first beacon,
`app/src/pitch_deck_runtime.py` pings whoever minted the link: a
`plugin_activity` ping titled "<name> opened the investor deck", deduped per
visit. The first beacon is sent when the page loads, so the ping arrives
within seconds of the click. It is not sent when the page HTML is served:
mail scanners that prefetch links but run no script would otherwise ping on
every send.

## Contact detail card

A click on a contact's picture or name — a row of the table, a phone card, or
a card on the stage board — opens everything the organization knows about that
person. On the desktop a click anywhere on a row, or on a card in the People
grid, opens it in the panel to the right of the table rather than a window; it
stays open while you move from contact to contact, redraws when the record
changes, and closes with its **x**. It shows:

* Every field the record carries, the custom columns it fills in, the lists it
  is on, and who added it and who last edited it. On the web these are the same inline
  controls the table saves through, so the card is also where the name is
  edited; on the desktop they read as text and **Edit contact** opens the
  form.
* The messages and Mastodon conversations linked to the record, the deck
  link's opens (on the desktop too, as one line), and the record's own stamps,
  newest first.
* **Recent updates**: the newest public posts of each account the record
  names — the instance in its Mastodon handle, Bluesky's public API, and
  GitHub's public activity feed — up to five an account.

The picture is read at the card's own size, four times the circle on a row,
from the first of those sources that serves one, in the same order the rows
try them (the ForkMesh picture, the accounts, then Gravatar); the initials show until one loads and stay when none does.

Nothing the card reads is stored. The posts and the picture are read the first
time a card opens in a session, and again when **Read again** is pressed; they
live in memory for the session, so moving back to a contact shows what was
already read. A card replaced or closed mid-read drops whatever comes back, and
a record naming no Mastodon, Bluesky or GitHub account and no email address is
never read at all. The reads are public and unauthenticated, so those services see the reading device's IP
address and which account was asked about (see
`www/docs/external-dependency-map.md`).

A shared sheet is another organization's read-only copy of a few columns, so
its rows have no card of ours to open.

### Links to a contact or a list

**Copy link** on the web card, and on a contact's right-click menu on the web
and the desktop, copies `https://app.forkmesh.com/dashboard/crm?contact=<id>`,
and a list (or pipeline tab) copies `https://app.forkmesh.com/dashboard/crm?list=<id>`:
**Copy link** on a tab's menu and in **Rename or share…** on the web, and on a
list's right-click menu in the sidebar and on the **Import CSV list** page on
the desktop. Links carry no organization name (2026-10-01: the owner wanted a
clean link to hand outside partners). The web page keeps the same parameter in
the address while a card or tab is open, so the address bar can be shared too.

Opening a link asks `GET /api/crm/-/open/<id>` where it points for the
signed-in account, and the page goes there:

- a list or contact in one of the reader's organizations answers
  `{"kind", "organization"}`, and that organization opens with the list or card;
- a list shared with the reader from another organization answers
  `{"kind": "list", "shared": true}`. If the reader has an organization with the
  CRM installed, it opens under **Shared with me** there. If they have none, the
  page opens in shared-only mode: their shared sheets as the only tabs, without
  the organization picker, the **More** menu, the section tabs or the new
  sheet buttons, and with **Add contact** when the sheet is shared for editing;
- anything else, including a private list the reader is not on, answers 404,
  and the page says the link cannot be opened.

Copying a link shares nothing new; **Rename or share…** (web) or **Add
members…** (desktop) does that. Switching organizations drops `list`.

The desktop copies the `app.forkmesh.com` address because a desktop has no page
of its own to link to. Client-side pipeline tabs on the desktop (a pipeline no
list is bound to, and **No pipeline**) are not stored lists, so they have no
link.

## Founders outreach

The **Founders outreach** section of the CRM page sends one-off sponsorship,
investment, partnership, strategic-support, and follow-up asks from the
organization's default sender (CRM settings → Outgoing email; the package falls
back to `founders@forkmesh.com` / "ForkMesh Founders"). Sends go through the
host's Mailtrap capability with the server key, so no request key is needed.

Organization owners and admins can always send and manage the roster; any
other member must be added to the organization's outreach team first
(`POST /api/crm/<org>/outreach/team` with `{"action": "add"|"remove", "name"}`,
audited as `crm.outreach_team_grant` / `crm.outreach_team_revoke`).
`GET /api/crm/<org>/outreach` returns access, templates, the sender, today's
count against the per-sender daily cap, recent sends, and (for managers) the
roster; `POST /api/crm/<org>/outreach/send` sends one email. Every attempt is
written to `crm_outreach_log` with the recipient and subject sealed, and a
recipient who is already a contact gets their last-contacted stamp updated.
Members outside the roster see neither templates, roster, nor history.

## Team leaderboard

The **Team** tab on the web page and the **Team leaderboard** page on the
desktop rank the organization's members by what they did in the CRM over the
last 7 or 30 days, and chart it by day as stacked bars (emails, outreach,
contacts added). Pick a member, or click their row, to chart only their days;
the web chart has a **Table** view of the same numbers.

The leaderboard does not read the per-list history below, so the counts come
from the rows that already carry a member and a time:

- **Emails**: `crm_email_sends` rows the member sent or linked (drafts left
  out), dated by when they were recorded.
- **Outreach**: accepted founders outreach sends in `crm_outreach_log`.
- **Contacts added**: `crm_contacts` rows by `created_by_name`, so a CSV import
  counts every contact it created.

Stage moves and edits are not counted: a contact only keeps its latest
editor. *Touches* (emails plus outreach) set the rank so a large import does
not outrank outreach work; contacts added break ties.

The organization summary (`GET /api/crm/<org>`) carries this as `activity`:
`days` (30 dates, oldest first), `kinds`, `tzOffset`, and `members`, one entry
per organization member plus anyone else with activity, each holding a
30-long count array per kind. Days are cut in the caller's time zone: clients
send `?tz=` in minutes east of UTC (clamped to ±14 h, default UTC), so the
browser and the desktop agree with the viewer's calendar.

## List activity history

Every list keeps a history of who opened it and what they did in it, so a
team can see when a teammate last looked at a list and which contacts they
added, edited, moved or deleted. Open it from a list's right-click menu
(**Activity history…**) or the **History** toolbar button on the desktop, and
from a tab's ⋯ menu on the web page. The dialog lists When, Who and What,
newest first, and names everyone who opened the list.

What lands in a list's history (`crm_list_activity`, one row per list):

- **Opened**: the desktop and the web page post
  `POST /api/crm/<org>/views/activity {"view": <id>}` when a list tab is shown;
  opening a sheet shared from another organization records it as well. One
  open per person per list counts every 30 minutes, checked by the client and
  again by the worker, so tab switching does not flood the history.
- **List changes**: created, renamed, columns changed, sharing changed,
  pipeline type changed. Reordering tabs is not recorded.
- **Contact changes**: added to the list, taken off it, edited (with the
  field names that changed, never their values), deleted, merged, emailed
  (a Mailtrap send or a linked external send). Membership is worked out the
  same way the tabs are, so a contact moved into a pipeline shows on the list
  bound to it. A bulk move writes one entry per list with the count and the
  first five names.
- **Imports**: one entry when a CSV import finishes, with the file name and
  the new, updated and skipped counts.

The actor's account name and the action are stored in the clear, like the
outreach log's sender; contact names, field names and email subjects are
sealed. Rows older than 90 days are dropped by an insert trigger, and a
list's history goes when the list is deleted or the package is uninstalled.

`GET /api/crm/<org>/views/activity?view=<id>` returns up to 200 entries
(`at`, `actor`, `action`, `text`, `count`) and `more`; `?before=<ms>` pages
further back. It answers only for lists the caller can open, so a private
list's history stays with the people it is shared with. The `text` sentence
is written by the worker so both clients show the same words.

## Browser extension

The ForkMesh browser extension lives in its own Market package,
`marketplace/packages/browser-extension`, because it serves more than the CRM:
the same extension fills logins from the Password Vault. It is a Manifest V3
Chrome extension for Gmail, Zoho Mail and any Mastodon instance the user adds
to it. It signs in with a ForkMesh account, shows the contact's record in a
compact header above the open message, and records that message on the
matching contact (creating the contact when needed) through
`POST /api/crm/<org>/emails/link`. An email is matched by address and a
conversation by fediverse handle; the provider message id is kept on the
record and reopens the message from the CRM. Its toolbar icon also files any
page under a contact's notes and edits a contact's last-contacted date.

This package keeps the API the extension calls and pins only the oldest
extension build it works with (`CRM_EXTENSION_MINIMUM` in
`content/web/page.js`); the CRM page links to `/dashboard/extension` to
install or update it.

### Confetti on every device when an email is tracked (1.28.0)

The user asked that every email sent to the CRM and tracked there set off a
big confetti celebration on all of their devices. Each time an email is newly
recorded on a contact, the worker files one `plugin_activity` ping to the
member who tracked it, titled "🎉 Email tracked in the CRM", with the subject
and contact in the body and `meta.celebrate = "crm-email-tracked"`
(`_celebrate_tracked_email` in `content/worker/crm.py`). Three paths count:

- the browser extension's `POST /api/crm/<org>/emails/link`, the first time
  a message is linked (linking it again updates the row and stays quiet);
- `POST /api/crm/<org>/emails`, an external send recorded by hand;
- `POST /api/crm/<org>/emails/send`, once Mailtrap accepts the message.

Filing the ping rings the account's `pings` event on every connected client
and sends the phone banner (APNs). Each client re-reads its inbox and throws a
celebration for a ping that carries `meta.celebrate`, is unread, is no more
than five minutes old and has not already been celebrated on that device:

| Client | Where | What it shows |
| --- | --- | --- |
| Web dashboard (every `/dashboard` page, the CRM included) | `loadNotifications` in `app/public/dashboard/js/08-repo-detail-network.js` calls `celebrateFreshPings` in `app/public/member-welcome.js` | Twice the new-member confetti and fireworks, with the ping's title and body in a card; ids already celebrated are kept in `localStorage`, so moving between pages does not replay it |
| Desktop | `MainWindow::celebrateFreshWebAlerts` after `refreshWebAlerts`; the rule is `forkmesh::pingforward::isFreshCelebration` | `CelebrationOverlay` at double intensity with a headline card, plus the tray card |
| iOS | `NotificationService.celebrateFresh` (`PingCelebration` in FMApi) | `PingCelebrationOverlay` over the shell: falling confetti, a burst, the headline card and a success haptic |
| Android | `NotificationService.celebrateFresh` (`PingCelebration` in core) | `PingCelebrationOverlay` in `Features.Root`, the same show |

The five-minute window means a phone that opens from the push banner still
celebrates, while older or already-read pings never do. Paused pings stop the
show on the desktop and the web, and switching off "App package activity"
pings stops it everywhere, since nothing is filed. On the web, iOS and Android
reduced motion drops the confetti but keeps the card.

## Mobile

The native iOS and Android apps (`native/`) do what the desktop CRM does, under
Business in the You hub: the list and pipeline tabs, contacts, merge, bulk
actions, lists and pipelines, sharing, CSV import and export, email, Zoho
mail, outreach, deck links, settings and the team chart. They read
`GET /api/crm` for the organizations and `GET /api/crm/<org>?since=<revision>&tz=<minutes>`
for the summary. `tz` is the device's offset from UTC, as the desktop sends
it, so the team chart's days match.

Since 2026-10-13 a list opens with the **Reach out** and **Follow up**
sections (see "Reach out and follow up" above). Since 2026-10-07 they also:

- list unsent note drafts (a note paragraph starting "DRAFT, not sent.") and
  send or mark them sent;
- set a contact's last-contacted date;
- copy a contact link (`/dashboard/crm?org=<org>&contact=<id>`);
- restore deleted built-in columns;
- connect Zoho Mail with a Self Client code (`POST /zoho/exchange`).

Not on phones: the GitHub-contributor, Mastodon-follower and ForkMesh-user
imports, profile "Recent updates", and the desktop table's named column
presets. `docs/native/README.md` has the details.

## Install and remove

Install or remove CRM from the Market page. Removing it calls the package's
organization data-lifecycle hook before the install record is deleted, so CRM
contacts, imported mail, email events, and encrypted provider settings are
purged. Mailtrap keys are request-only and are never stored.

To remove CRM from a source distribution, delete this directory, remove the
`crm` entry from `marketplace/catalog.json`, and run the normal dashboard,
market-catalog, Worker, and CMake generators. Generated navigation, routes,
registries, static pages, resources, and native build sources then disappear.

## Package tests

```sh
python3 -m pytest -q marketplace/packages/crm/tests
```
