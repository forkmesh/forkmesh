# ForkMesh for Chrome: CRM and Password Vault

One extension, one sign-in:

- **Password Vault.** Fills and saves logins from your end-to-end encrypted
  ForkMesh Password Vault (the `vault` Market package).
- **CRM.** Ties the email or Mastodon conversation you have open to a contact
  in your organization's ForkMesh CRM (the `crm` Market package), and tracks
  mail you send.
- **Marketing readers.** Reads the counts that channel pages show (X profile,
  Reddit, any configured channel) for the `marketing` package, only when you
  ask.

The CRM half works in Gmail (`mail.google.com`), Zoho Mail (`mail.zoho.com`,
`.eu`, `.in`, `.com.au`, `.jp`, `mail.zohocloud.ca`) and any Mastodon instance
you add. The vault half works on any https site through the toolbar popup.
Requires Chrome 137 or newer (or a Chromium browser of that age) for native
X25519 and Ed25519.

## Install

1. Open **Browser extension** in the ForkMesh dashboard
   (`/dashboard/extension`), click **Download vX.Y.Z** and unzip it; it holds a
   `browser-extension` folder with this extension's `manifest.json` inside.
2. Click **Open chrome://extensions**, turn on **Developer mode**, **Load
   unpacked**, and pick that folder. From a checkout, pick
   `marketplace/packages/browser-extension/content/extension`, not the package
   folder (whose `manifest.json` is the Market manifest; Chrome refuses it with
   "Missing 'manifest_version' key").
3. Click the toolbar icon and sign in with your ForkMesh account.
4. For the CRM, pick the organization whose CRM receives the links (the CRM
   package must be installed there). The vault needs no organization.

Self-hosted ForkMesh: expand **Self-hosted ForkMesh** on the sign-in form and
enter your server URL; Chrome asks once for access to it. To update, unzip the
newer download over the same folder and press Reload on the extension's card;
the dashboard page says when the installed copy is older.

## Password Vault

Create the vault on ForkMesh first (**Vault** in the dashboard). In the popup:

- **Unlock** with your master password, stretched with Argon2id inside the
  extension and never sent. The vault locks after 15 idle minutes, on
  **Lock**, on sign-out and when the browser closes.
- **Fill** types the saved username and password into the current tab.
  **User** and **Pass** copy one field. **Save this page's login** offers to
  save what you typed. **Generate password** copies a new 24-character
  password.

Safety properties:

- **No standing access.** No content script on the open web and no
  `<all_urls>` permission; a page is touched only through `activeTab` after
  you press a button.
- **Exact site match.** A login fills only when the tab's host is exactly the
  login's host (`www.` aside) over https, checked in the background worker;
  only the top frame is filled.
- **Keys in memory only.** The unlocked keyset lives in the service worker and
  `chrome.storage.session` (memory only, closed to content scripts). Nothing
  about the vault is written to `chrome.storage.local`.
- **The popup gets no passwords.** A password leaves the background worker
  only into the page you pressed Fill on, or to the popup for one explicit
  Copy.
- **Bundled cryptography.** `vault-crypto.js` and `vault-client.js` are copies
  of the vault package's files (`marketplace/packages/vault/tools/sync_crypto.py`
  keeps them identical), so the extension runs no server-delivered crypto.

## CRM

Open an email. A compact **CRM header** above the message names the other
party and offers **Look up**; nothing is requested from ForkMesh until you press
it. **Look up** shows the record (name, pipeline, stage, owner, last email,
linked emails, company, title, deal value, next step, tags). The **FM** button
(or the toolbar popup) opens the panel, prefilled from the message (contact,
name, company, website, signature details, date and subject, all editable):

- **Add to pipeline** picks a flow and stage, creates the contact if needed
  and links the email.
- **Link only** records the message without changing the pipeline.
- **Link to an existing contact…** files the message under another record
  (searching by name, address or company); the address joins that record so
  the next message is found automatically.
- **Last contacted** sets the date on the record; **Unlink** removes the link.

Records are matched by address (including custom columns), Mastodon handle,
LinkedIn/GitHub/X profile, phone, or name when the address or company
supports it. A name that contradicts the record's or fits two records never
matches on its own, and the panel says how a record was found.

**Zoho Mail header with names only**: Zoho often shows the sender as a name
with no address in the open message (a Gmail reaction such as "Ada a réagi
depuis Gmail" is one), which before 2.23.1 left the header at "No contact
address found on this email". The reader now looks for the address in three
more places, in order: any attribute of the message header (tooltips,
`aria-label`, avatar URLs, URL-encoded `%40` included), the header's text, and
the open message's row in the list (found by the message id from the URL,
else the selected row). The list row gives the sender of received mail and
the recipient of sent mail.

**Mastodon**: add each instance in the popup (**Mastodon instances**), or
press **Record this toot in the CRM** on an instance not added yet; Chrome asks
for that host once. Conversations are linked by `@name@instance`.

**Your Mastodon posts** (on by default, **Add people I mention on Mastodon**):
open one of your own posts on its permalink, like
`https://mastodon.social/@forkmesh/113489…`, and after the page settles
(1.5 s) everyone the post mentions is linked to the CRM, one record each
(hashtags and your own accounts are skipped, at most ten people). Someone the
CRM does not know becomes a contact named from the page, or `@name@instance`.
The CRM pings you for each person ("🎉 New CRM contact from Mastodon"). The
post is yours when its author is the account signed in to Mastodon in that tab
or the Mastodon account saved in the CRM's settings (read once per ten
minutes). A post is logged once: the extension remembers the last 500, and the
popup shows who was logged last. Timelines, profiles and other people's posts
send nothing.

**Mail you send** (on by default, **Track emails I send**): sends from Gmail or
Zoho Mail are linked to the first recipient that already has a record (people
outside your domain first) and move **Last contacted**. Gmail sends are linked
after the 10-second undo window or when the tab closes. No contact is created
for a send to unknown recipients.

**Sort a Zoho Mail list**: a header strip sits above Zoho Mail's message
list with **Sender**, **Subject** and **Date** over their columns (or side by
side when the list is too narrow for columns). Click one to sort; click it
again to reverse it. Subjects sort without their `Re:`/`Fwd:` prefixes.
**Date** is Zoho's own order, newest first, with its Today/Yesterday
headings; any other sort hides those headings. Sorting only reorders the rows
already loaded, on screen (CSS `order`, never moving Zoho's nodes), and is
redone as Zoho loads more while you scroll; the choice lasts until the tab is
reloaded. Keyboard navigation and shift-click selection still follow Zoho's
order. Nothing is read from or sent to ForkMesh, and it needs no sign-in
(`zoho-sort.js`).

**Inbox zero in Zoho Mail** (2.24.0): a second strip under the sort header
holds the bulk tools. They work on the rows Zoho has loaded, like the sort, and
the count beside them says how many are shown (`12 of 340 loaded`).

- **Search** filters the list as you type. Every word must match the subject
  or the sender; `from:acme` looks only in the sender, `subject:invoice` only
  in the subject, and `"two words"` keeps a phrase together. Case, accents and
  `Re:`/`Fwd:` prefixes are ignored. Escape clears it.
- **Group by sender** orders the list by sender, largest group first, Zoho's
  order (or the chosen sort) inside each group, and opens a list of senders
  with their counts. Picking one shows only that sender's messages, marked by
  a `From:` chip; its × shows every sender again.
- **Select N**, **Mark read N**, **Archive N** and **Delete N** act on exactly
  the messages shown, or every loaded message when nothing filters the list.
  The extension ticks those rows with Zoho's own checkboxes (and unticks any
  other), checks that Zoho now selects exactly that set, then presses Zoho's
  own toolbar button with that name, the one nearest the list. If Zoho
  selected a different set, nothing else is done; if the button is not on
  screen (for example inside a More menu), the messages stay selected for
  Zoho's toolbar. **Delete** first asks `Delete N messages? Zoho Mail moves
  them to Trash.`, and is refused if the shown count changed before you
  confirmed. Zoho's permanent delete is never pressed.

Nothing is read from or sent to ForkMesh, there is no request to Zoho's API,
and the search, group and filter last until the tab is reloaded. Keys typed in
the search box are kept from Zoho's keyboard shortcuts. The matching, grouping
and button-name logic is `zoho-tidy.js`, tested by
`tests/zoho_tidy_harness.js`; `zoho-sort.js` reads the rows and drives Zoho's
page. Zoho's markup is not documented, so the checkbox, selection and toolbar
lookups go by role, ARIA state and labels (`Delete`, `Move to Trash`,
`Archive`, `Mark as read`) rather than Zoho class names.

**Save this page to a contact** appends a dated line with the current page's
title and address to a contact's notes; only the address bar is read.

## Marketing

### Channel updates

Every Marketing channel has **Update**. The Marketing page passes the
organization and channel id (never an address) to the service worker, which
looks up the channel's saved link, opens it in a new tab, injects
`marketing-capture.js` into that tab only, reads the counts the page shows and
posts them to `/api/marketing/<org>/capture`, then closes the tab. The first
update on a site asks Chrome for that one site (`capture.html`); **Allow
channel sites** grants all configured channels at once. Exceptions:

- **Discord**: never opened in a tab; Update asks Marketing's server refresh,
  which uses Discord's public invite API.
- **Google** results pages: only the result count (`#result-stats`, or the
  number of result links on a single page) as `indexedPages`.
- **Reddit** subreddit pages: only the subreddit header's members, weekly
  visitors and weekly contributions, as `followers`, `reach` and
  `engagements`.

The popup's **This page** card names the current site and offers the buttons
that work on it (update this channel from the page, update from the saved page,
Reddit desk actions, X sync). The **refresh panel** (Chrome side panel and the
popup, rendered by `refresh-view.js` from `fm-refresh-plan`/`fm-refresh-run`,
accepted only from the extension's own pages) lists, per site, the pages that
feed Marketing, marks those read in the last 12 hours, and suggests the next
channel to update (the one updated longest ago, skipping the last hour). The
toolbar badge shows unread pages. These need the `sidePanel` and `tabs`
permissions; nothing is injected into channel sites for them, and the tab's
address never leaves the browser.

`refresh-view.js` flattens the card's parts and drops the empty ones before
`replaceChildren`: unlike its `h` helper, the DOM call turns an array into the
text `[object HTMLParagraphElement],…` and a `null` into `null`, which is how
the popup's page lines (the Reddit desk's notes) showed before 2.22.1.

### X profile stats

On the tracked X profile (and its Media tab, Mentions and an `@handle`
search), `x.js` reads the counts the page shows: following, followers, posts,
media, and the replies, reposts, likes and views of posts on screen (last 30
days). It sends nothing to X and adds nothing to the page. **Sync now** records
a snapshot (`POST /api/marketing/<org>/metrics`, `channel: "x"`,
`source: "x-profile"`) and adds the posts read since the last sync to
Marketing's posted links (up to 1000 kept, sent in batches of 250, with text,
time, counts and up to four `pbs.twimg.com` images). **Update X stats from this
page** also turns the tab live: new posts and changed counts are sent within
ten seconds and header changes at most once a minute, until **Stop live
updates**, a handle change, a send failure or a browser restart. The reader
handles both the signed-in and the signed-out X page layouts (post dates fall
back to the id's timestamp, `(id >> 22) + 1288834974657`).

### Connect X for posting

Started from Marketing (**Post to X → Connect → Guide me with the extension**,
or `?xsetup=1`; owners and admins only), `x-developer.js` shows a guide card on
X's developer console (`console.x.com`, `developer.x.com`,
`developer.twitter.com`) and presses the next control on each page: sign-in,
developer sign-up text, app creation, user authentication settings (**Read and
write**, web app/bot type, callback and website URLs), and regenerating the API
key and access token. It reads the four keys off the page (or takes pasted
ones), then the service worker sends them once to
`POST /api/marketing/<org>/x-connection`, which verifies them with X. Signing
in, accepting X's terms and buying API credits stay with the user.

Narrowing: the script does nothing unless a setup started from Marketing is
active; it sends nothing to X; the card is in a closed shadow root; keys are
held in `chrome.storage.session` for at most two hours and forgotten once
Marketing has them; replies to pages carry only each key's last four
characters.

### Reddit desk

ForkMesh's servers cannot read Reddit, so `reddit.js` reads Reddit pages you
open on `www.reddit.com` and `old.reddit.com`, and only these:

- listings of a watched subreddit (subreddit, id, title, link, author, comment
  count, score, time per post);
- a search whose text is a watched search;
- a thread in a watched subreddit, or any thread already in the thread log
  (views, points, comments, and a followed comment's points and views);
- a watched subreddit's rules page (rule titles and descriptions);
- the profile of an account on the desk's accounts list.

The reader only looks: it adds nothing to the page, sends nothing to Reddit,
and checks the page at most every three seconds while visible. Nothing is
sent to the desk until you press **Send to Reddit desk** (threads logged as
**To review** with `source: "extension"`, rules replacing that subreddit's
rules). The extension never ticks **Rules read** or **Disclosed** and never
records a reply. The watch list copy is refreshed from the popup, at most when
older than six hours.

Numbers read on a thread that is already in the log (the newest 200, monitored
ones first) are the exception: since 2.23.0 they are sent by themselves a few
seconds after the page is read, as the same `PATCH` that **Check now** sends,
so the log stays current without pressing anything.

- **Monitored threads**: **Monitor** a thread or comment link; **Check now**
  (or an opt-in six-hour Chrome alarm) opens each monitored thread (up to 20)
  in a background tab, reads its numbers (`fm-reddit-read`, answered only to
  the extension) and sends a `PATCH`.
- **Update logged threads** (2.23.0, `fm-reddit-threads-update`, from the
  dashboard or the popup only, the extension's organization must be the
  page's): Marketing's **Update** next to a thread, or **Update all**, sends
  the thread ids (none means every logged thread). The extension refreshes
  the log from Marketing, then opens each thread (up to 50, least recently
  read first) in one background tab at a time, reads it like **Check now**,
  sends a `PATCH` with `checkedAt` even when nothing changed, and closes the
  tab. Progress goes back to open Marketing pages as
  `fm-reddit-threads-progress`; `reddit_update_running` when one is already
  running, `reddit_threads_empty` when none of the ids is logged.
- **Your accounts' posts and views**: on a Reddit profile, **Update u/<name>
  posts and views** records the account's own posts and comments in
  Marketing's posted links (`channel: reddit`), then opens each one not read
  in 30 minutes (up to 25 per press) in a background tab to read its views,
  which Reddit shows only to the author.
- **Update from the extension** on Marketing's Reddit page
  (`fm-reddit-accounts-update`, from the dashboard or the popup only, the
  extension's organization must be the page's): for each account on the
  desk's accounts list it opens `https://www.reddit.com/user/<name>/submitted/`
  and then `/comments/` in one background tab at a time, reads them like
  **Update u/<name> posts and views**, closes the tab, then starts the views
  check for every account. Progress goes back to open Marketing pages as
  `fm-reddit-posts-progress`.
- **Walk the watch list**: started only by you, it opens one tab and visits up
  to 24 stops (watched subreddits' `/new/`, watched searches over the last
  week, unread rules pages). `reddit-walk.js` paces each page with scrolling
  and pointer movement only (no clicks, keys, form events, requests or page
  writes), `reddit.js` reads it, and the walk ends with **Send to Reddit desk**.
  **Stop the walk** ends it; two walks never run at once. A walk asked for
  with `rulesOnly` (2.21.0; Marketing's **Read missing rules**, relayed by
  `dashboard.js`) visits only the rules pages not yet on file and fails with
  `reddit_rules_on_file` when there are none. This is why
  `https://www.reddit.com/*` and `https://old.reddit.com/*` are in
  `host_permissions`.

**Automatic thread updates and several Reddit accounts** (2.25.0). Before
2.25.0 a thread logged from Marketing stayed at "not read yet" until someone
pressed **Update**, and a logged comment's views were often missed. Now:

- **Update the thread log when the browser starts and every hour** (popup,
  on by default, `fm-reddit-auto-update`): when the extension is installed,
  updated or Chrome starts (`onInstalled`/`onStartup`), and on an hourly
  `fm-reddit-auto-update` alarm, the extension refreshes the log and reads up
  to 10 logged threads never read or not read for 12 hours, one background
  tab at a time, at the comment's own link when one is logged. Runs are at
  least 20 minutes apart. Opening the popup never starts one. Marketing's web
  page also asks for a thread right after it is logged there.
- **Comment views**: Reddit shows a view count only to the account that
  posted, and new Reddit draws it in several ways. The reader takes a number
  from a views-like attribute, an `aria-label`/`title`, a short label split
  across elements ("7" + " views"), or a bare number next to a views, eye or
  insights icon (never the score beside the vote arrows). Nested replies are
  skipped. A comment is found by `thingid`, then by `permalink`, then on old
  Reddit by `data-fullname`. Each reading also carries the post's `author`,
  the comment's `commentAuthor` and `readAs`, the Reddit account it was read
  as. That account comes from old Reddit's header, or from the author of an
  item whose views were shown, since only the author sees them.
- **Several Reddit accounts**: one browser is signed in to one Reddit account
  at a time. When a page shows the extension that a different account is
  signed in, it adds that account to the desk's accounts (owners and admins;
  a member gets an error in the popup, retried after a day), then reads again
  the logged threads that account posted or commented in and its posts' views.
  So signing in to Reddit as each account in turn pulls in every account's
  numbers. The popup lists the accounts, marks the one signed in now, and
  adds or removes one (`fm-reddit-accounts-change`, the settings `PUT` the
  watch list uses).

### Browser activity

When a reader makes its first read of a page after it loads, or a refresh,
send or channel update finishes, the extension writes one line to
`POST /api/marketing/<org>/activity` (at most one line per page every five
seconds), so the desktop's Marketing page can react. Only pages the readers
already read are reported.

## What leaves the browser

- **CRM**: only when you press a button, send a message, or open your own
  Mastodon post on its permalink: provider message and thread ids, subject, addresses or handles, direction, date, permalink,
  the contact details shown in the panel, and the chosen flow and stage. The
  email body is read in the page only to prefill fields and is never sent. A
  Mastodon post's content warning or opening line stands in for a subject. A
  tracked send carries its subject, sending mailbox, up to five recipients and
  the time, never its body.
- **Marketing**: the counts, page title, description, image, handle, address
  and up to five recent posts of a channel update; Reddit desk fields; posted
  link fields; X keys once to `x-connection`. Nothing is sent to the channel
  sites, Reddit or X.
- **Vault**: ciphertext only, to `/api/vault` (encrypted items, wrapped keys,
  signed shares). The master password, keyset and decrypted logins stay in the
  extension.

Requests go to your ForkMesh server with the session token from signing in;
browser cookies are never sent. The session token and chosen organization are
kept in `chrome.storage.local`. **Sign out** revokes the session on the server,
clears it and locks the vault. Added Mastodon instances are kept in the
extension's storage with the host permission Chrome granted.

## Files

| File               | Role                                                          |
|--------------------|---------------------------------------------------------------|
| `manifest.json`    | Manifest V3; `storage`, `activeTab`, `scripting`, `alarms`, `sidePanel`, `tabs`; hosts: ForkMesh and reddit.com. |
| `background.js`    | Sign-in and every request to ForkMesh; the CRM handlers.      |
| `vault.js`         | The vault's background half: unlock, fill, capture, idle lock.|
| `vault-crypto.js`  | The vault's cryptography (copied from the `vault` package).   |
| `vault-client.js`  | The vault's client logic (copied from the `vault` package).   |
| `popup.html/js`    | Sign in, the organization, Mastodon instances, the CRM panel. |
| `popup-vault.js`   | The popup's Password Vault section.                           |
| `panel.js`         | The CRM panel and header, shared by every page and the popup. |
| `content.js`       | Reads the open email in Gmail and Zoho Mail; tracks sends.    |
| `zoho-sort.js`     | The sort header and inbox-zero tools above a Zoho Mail list. |
| `zoho-tidy.js`     | Search, sender grouping and Zoho button matching for `zoho-sort.js`. |
| `mastodon.js`      | Reads the open conversation on a Mastodon instance.           |
| `x.js`             | Reads the tracked X profile's counts for Marketing.           |
| `x-developer.js`   | Guides Connect X through X's developer console and reads the keys. |
| `reddit.js`        | Reads watched Reddit pages you open, and monitored threads' views, for the Reddit desk. |
| `reddit-walk.js`   | Paces one page of a walk: scrolls and moves the pointer, reads and sends nothing. |
| `dashboard.js`     | Tells the ForkMesh dashboard which version is installed; relays Marketing's Update and the Reddit walk. |
| `marketing-capture.js` | Reads a Marketing channel's page when asked; injected per tab. |
| `refresh-panel.html/js` | The side panel: the active tab's site, a button per page that feeds Marketing. |
| `refresh-view.js`  | Renders that plan for the side panel and the popup.            |
| `capture.html/js`  | Asks Chrome once for a channel site, then continues the update. |

Endpoints used:

- CRM (`crm` package, `content/worker/crm.py`):
  `POST /api/crm/<org>/contacts/lookup` (by `email` or `handle`; returns the
  organization's flows), `POST /api/crm/<org>/emails/link` (optional `handle`,
  `flow`, `stage`, `contact`), `GET`/`PATCH /api/crm/<org>/contacts[/<id>]`.
- Marketing: `POST /api/marketing/<org>/capture`, `/metrics`, `/refresh`,
  `/posts`, `/activity`, `/x-connection`; `GET`/`POST /api/marketing/<org>/reddit`,
  `PATCH /api/marketing/<org>/reddit/<id>`, `POST /api/marketing/<org>/reddit-rules`.
- Vault (`vault` package, `content/worker/vault.py`): `/api/vault`.
