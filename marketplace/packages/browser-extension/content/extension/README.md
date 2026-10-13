# ForkMesh for Chrome: CRM and Password Vault

One extension, one sign-in, two jobs, plus two readers for Marketing (the
organization's X profile and the Reddit desk, below):

- **Password Vault.** Fills and saves logins from your end-to-end encrypted
  ForkMesh Password Vault (the `vault` Market package).
- **CRM.** Ties the email or Mastodon conversation you have open to a contact
  in your organization's ForkMesh CRM (the `crm` Market package). The CRM keeps
  the provider's message id on the contact's record; clicking it on the record
  reopens the message where it came from.

The CRM half works in Gmail (`mail.google.com`) and Zoho Mail
(`mail.zoho.com`, `.eu`, `.in`, `.com.au`, `.jp`, `mail.zohocloud.ca`) out of
the box, and in any Mastodon instance you add to it (see **Mastodon** below).
The vault half works on any https site, through the toolbar popup.

Needs Chrome 137 or newer (or another Chromium browser of that age): the vault
uses the browser's own X25519 and Ed25519.

## Install

1. Open **Browser extension** in the ForkMesh dashboard
   (`/dashboard/extension`), click **Download vX.Y.Z** and unzip it; it holds a
   `browser-extension` folder with this extension's `manifest.json` directly
   inside it.
2. Click **Open chrome://extensions**, turn on **Developer mode**, **Load
   unpacked**, and pick that unzipped `browser-extension` folder.

   From a checkout, pick `marketplace/packages/browser-extension/content/extension`
   instead. Not the package folder `marketplace/packages/browser-extension`
   itself: its `manifest.json` is the ForkMesh Market manifest, and Chrome
   refuses it with "Missing 'manifest_version' key".
3. Click the ForkMesh toolbar icon and sign in with your ForkMesh account
   (username or email, password, two-factor code if you use one).
4. For the CRM, pick the organization whose CRM should receive the links. The
   CRM package must be installed for that organization from the ForkMesh
   Market. The vault needs no organization.

Coming from the older **ForkMesh CRM** extension (1.x, loaded from a
`crm-extension` folder)? Remove it and load this one. It does everything the
old one did; you sign in once more because Chrome treats a new folder as a new
extension.

Self-hosted ForkMesh: expand **Self-hosted ForkMesh** on the sign-in form and
enter your server URL. Chrome asks once to let the extension reach it.

To update, unzip the newer download over the same folder and press Reload
on the extension's card; the extension's dashboard page says when the
installed copy is older. With the extension installed, **Open
chrome://extensions** opens it directly (websites cannot open it themselves,
so without the extension the address is copied instead).

## Password Vault

Create the vault on ForkMesh first (**Vault** in the dashboard). Then, in the
popup:

- **Unlock** with your master password. It is stretched with Argon2id inside
  the extension and never sent anywhere. The vault locks itself after 15 idle
  minutes, on **Lock**, on sign-out, and when the browser closes.
- On a site with a saved login, press **Fill**. The extension types the
  username and password into that tab.
- **User** and **Pass** copy one field. Search to copy from a login saved for
  another site; those cannot be filled here.
- **Save this page's login** reads the username and password you typed into
  the page and offers to save them, encrypted, to your vault.
- **Generate password** copies a new 24-character password.

How it stays safe:

- **No standing access to the sites you visit.** There is no content script on
  the open web and no `<all_urls>` permission. A page is touched only through
  `activeTab`, after you press a button in the popup.
- **Exact site match.** A login is filled only when the tab's host, as Chrome
  reports it, is exactly the login's host (`www.` aside), over https. A
  subdomain, a look-alike, or a plain-http page is refused in the background
  worker, whatever the popup or the page says. Only the top frame is filled,
  never an embedded one.
- **Keys in memory only.** The unlocked keyset lives in the service worker and
  in `chrome.storage.session`, which Chrome keeps in memory, never on disk, and
  which this extension closes to its own content scripts. Nothing about the
  vault is written to `chrome.storage.local`.
- **The popup gets no passwords.** Lists carry titles and usernames. A password
  leaves the background worker only into the page you pressed Fill on, or to
  the popup for one explicit Copy.
- **Its own cryptography.** `vault-crypto.js` and `vault-client.js` are packaged
  copies of the vault package's files (`marketplace/packages/vault/tools/
  sync_crypto.py` keeps them identical), so the extension does not depend on
  JavaScript a server delivers.

## Mastodon

A fediverse instance can be any host, so the extension ships with none and
asks for each: open the toolbar popup, expand **Mastodon instances**, type the
instance you read Mastodon on (`mastodon.social`, or your whole handle), and
press **Add**. Chrome asks once to let the extension read that site, and the
reader is registered for it until you press **Remove** (or take the permission
back in `chrome://extensions`).

Or skip the typing: open a post on an instance that is not added yet, click
the toolbar icon, and press **Record this toot in the CRM**. That asks Chrome
for the instance, adds it, and starts the reader on the open tab, so the panel
is ready to link the toot right away.

Open a conversation there and the same CRM header sits under the post's
author, above its text (where an email's sits above the body), with the same
panel under the FM button. Mastodon knows people by handle rather
than by address, so the conversation is linked with the contact's
`@name@instance`: the CRM matches the record carrying that handle, creates one
when no record has it yet, and a contact whose handle is already on their
record keeps their emails and their threads in one history. The panel's
**Contact email** field is optional there and ties the two together when you
know the address.

## Use

Open an email. A compact **CRM header** sits above the message body, naming
the other party and offering **Look up**. Nothing is asked of ForkMesh until
you press it: opening or clicking through a mailbox sends no request at all.

**Look up** fills the header with what the CRM knows about that person: name,
pipeline, stage, owner, the date of the last email on the record, how many
emails are linked (and whether this one is), plus company, title, deal value,
next step, source, location, and tags when the record has them. Clicking the
name opens the CRM; a person the CRM does not know yet gets an **Add to
pipeline** button.

The header's **FM** button opens the panel right under it. The same panel is
in the toolbar popup: click the ForkMesh CRM icon while an email or a Mastodon
conversation is open.

The panel prefills what the email says about the contact, all editable:

- who the contact is: the recipient of mail you sent (Zoho's Sent folder and
  "Me" sender included), the sender of mail you received, and people outside
  your own domain before teammates on copy; a picker switches between everyone
  on the message;
- their name: the display name, else the greeting of mail you sent
  ("Hi Gracie,") or the signature of mail they sent;
- company and website from a work address's domain; title, company, phone,
  LinkedIn, X/Twitter, GitHub, and website from their signature (quoted
  replies are ignored);
- the date the message was sent;
- the subject, editable before linking. In Zoho Mail it is read from the
  reading pane, then the selected message row, and never from a folder name
  ("Inbox", "Sent", "Spam") that Zoho puts in the tab title. **Update link**
  saves a corrected subject on a message already linked.

- **Add to pipeline** picks one of the organization's flows (Investment,
  Partnerships, Customers, ... as configured on the CRM page) and a stage,
  puts the contact there (creating it if needed), and links the email. On a
  contact already in that flow it reads **Move to _stage_**.
- **Link only** records the message on the contact without touching its
  pipeline. A contact that does not exist yet is created from the address and
  name on the email.
- **Link to an existing contact…** (also **Link to contact** in the header
  when the address is not in the CRM) files the message under a record you
  pick instead: a reply from a new or plus-tagged address, a colleague writing
  for them, and so on. It searches the organization's contacts by name,
  address or company, and before you type it suggests the ones sharing the
  address's domain, local part, company or name. **Link** and **Add to
  pipeline** then send that contact's id, and the address joins the record
  (as its email when it has none, else in **Other emails**) so the next
  message from it is found by itself. An address another record already
  holds stays on that record. On a contact that was found, the same picker
  reads **Link to another contact instead…**.
- **Last contacted** is a date field on the record: linking a message moves
  it forward on its own, and this sets it to whenever you actually last spoke.
  **Now** beside it records this minute.
  The CRM page edits the same date in its **Last contacted** column.
- **Unlink** removes the record again.

Every person has one record, their box, named after them. Looking up or
linking finds the box however the message names the person: the address,
also when it is kept in a custom column; the Mastodon handle; a LinkedIn,
GitHub, or X profile read from the signature; a phone number; or the name,
when the address spells it ("robin@", "robin.moss@") or the company matches.
So someone already in a pipeline under a name alone, or under another
address, keeps their pipeline and gains the new address (as the record's
email when it has none, else in its **Other emails** column) instead of
turning up as a second record. A name that contradicts the record's, or that
fits two records equally well, never matches on its own; the panel says when
a record was found by name, profile, or phone rather than by the address.
- **Open CRM** opens the organization's CRM page. Linked messages show as
  chips under the contact's name; each chip reopens the email or the
  conversation where it came from.

Changes made from the toolbar popup redraw the header on the page.

## Mail you send, tracked on its own

Send a message from Gmail or Zoho Mail (the **Send** button or Ctrl/Cmd+Enter)
and the extension links it to the CRM by itself, the way **Link only** would:
this includes the compose windows the CRM opens when you click a contact's
address. The recipients are tried in order, people outside your own domain
first, and the message lands on the first one who already has a record; a
send to nobody in the CRM is left alone and no contact is created for it.
The record's **Last contacted** moves to the time you sent it.

Gmail holds a sent message for its undo period, so the link is made ten
seconds after **Send**, or when the tab closes; pressing **Undo** first
records nothing. Gmail's "View message" notice gives the message's id, so
the chip on the record reopens it; when there is no such notice (Zoho Mail,
or a tab closed at once) the chip opens a search of the Sent folder instead.

The popup's **Track emails I send** turns this off and on, and says what the
last send was linked to or why it was not. It is on from install; it does
nothing until you are signed in and have picked an organization.

## This page: the buttons for the site you are on

On Reddit or on a Marketing channel's site, the toolbar popup opens with a
**This page** card at the top (2.7.0). It names what the tab is and offers
the buttons that work on it, so nobody has to dig through the widgets below:

- **Reddit** (`www.`, `old.` or `new.reddit.com`): whether this subreddit,
  thread, rules page or search is on the desk's watch list and so collected,
  how many threads and rule sets are waiting, and **Send to Reddit desk**,
  **Refresh watch list**, **Read r/_name_ rules** (opens its old Reddit rules
  page in this tab) and **Open Reddit desk**, plus the Reddit channel's
  buttons below.
- **Any page on a channel's site** (matched by the channel's saved link, else
  its sites, else the channel catalog): **Update _channel_ stats from this
  page** reads the numbers this page shows and records them as the channel's.
  The server accepts any page on the channel's sites. When this is not the
  page saved for the channel, the card says so, since the numbers recorded
  are this page's (another subreddit's members, another profile's
  followers), and adds **Update from saved page** (opens the saved page in a
  new tab, reads it and closes it) and **Go to _channel_ page**. A channel
  that is not in Marketing yet is added, with this page as its link, when an
  owner or admin updates from it. Discord's update is read on the server
  from its public invite.
- **X**: when the tracked profile's numbers have been read, **Sync X stats**
  too. Since 2.11.0 **Update X stats from this page** also adds the latest
  posts and turns on live updates for the tab, with **Stop live updates**
  while it is on (see [X profile stats for Marketing](#x-profile-stats-for-marketing)).

Every channel card has **Open Marketing**. On other sites the card shows only
the [next channel to update](#next-channel-to-update-2170). The card
reads only the tab's address, the Marketing settings and the extension's own
Reddit and X state; nothing on the page is touched until a button is pressed.

## Refresh panel (2.14.0)

The user asked for a refresh button for the site being viewed that leads to
the page that has to be read, with one button per page when refreshing a
site's data takes several. 2.13.0 drew that as a bar inside the page, which
meant injecting a script into X, Reddit and, after **Allow channel sites**,
every channel's site. The user then asked for no code in any channel site and
no **Allow channel sites** step, so 2.14.0 removed the bar (`refresh-bar.js`,
its manifest entry and the `fm-refresh-bar` registered script) and shows the
same plan in Chrome's side panel and the popup instead. Nothing new runs in
any page: the only scripts on X and Reddit are still the `x.js` and
`reddit.js` readers.

It shows only when the extension is signed in and an organization has
Marketing installed, on a site with pages that feed Marketing:

| Site | One button per page | The main button |
|------|---------------------|-----------------|
| X (`x.com`, `twitter.com`) | the tracked profile, its Media tab, Mentions, and a live search for `@handle`: the four pages `x.js` reads | **Sync X stats** (`xSyncNow`) on any of the four |
| Reddit (`www.`, `old.`) | each watched subreddit's `/new/` listing, each watched search, the old Reddit rules page of each watched subreddit whose rules Marketing has not got, each monitored thread, and **This thread** on a thread in a watched subreddit | **Send to Reddit desk (_n_ waiting)** (`redditSyncNow`) on any of them |
| A channel's own site (Mastodon server, YouTube, GitHub, a blog, …) | each Marketing channel whose saved link is on this site | **Refresh _channel_** anywhere on the site, for the channels Marketing refreshes from its server (`POST /api/marketing/<org>/refresh`, the `refreshableChannels` of the settings reply); the others only get their button, and the popup's **This page** card still updates them from their page |

Elsewhere on the site the main button becomes **Go to _next page_**: the
first page not read yet. A page button opens its page in the active tab
(`chrome.tabs.update`); the page in the tab is outlined. A page counts as
read (✓, and in the _n/m read_ count) when its reader saw it in the last 12
hours of this browser session, when Marketing recorded or refreshed the
channel in the last 12 hours, when a monitored thread was last checked in
that time, or, for a rules page, once its rules were read.

How it appears:

- **Badge.** On a tab of such a site the toolbar icon shows the number of
  unread pages in blue, or a green ✓ when all are read; it is cleared on
  other sites. It is redone when a tab is activated or loads, and when a
  reader marks a page read (from the cached channel list only).
- **Popup.** Since 2.17.0 the **This page** card is the refresh card too: the
  same buttons, its own, and **Open refresh panel**, which calls
  `chrome.sidePanel.open` inside the click.
- **Side panel** (`refresh-panel.html`). Once open it stays and follows the
  active tab of its window, re-rendering when the tab changes or loads and
  when a page is marked read (`fm-refresh-changed`).

The popup and the panel render with the same `refresh-view.js` and ask the
service worker for `fm-refresh-plan` and `fm-refresh-run` with the tab's
address. Both messages are taken only from the extension's own pages, never
from a content script, and a run only does what the plan offers for that
address. The channel list is fetched at most every five minutes and kept in
session storage.

The side panel needs the `sidePanel` permission, and reading the active
tab's address from the panel and the badge needs `tabs`. Chrome shows `tabs`
as **Read your browsing history**: updating from 2.13.x to 2.14.0 disables
the extension until the new permission is accepted on `chrome://extensions`
or from the toolbar's prompt.

## Next channel to update (2.17.0)

The user asked the extension to suggest the next channel to update: the one
updated furthest in the past and not within the last hour, a button that
brings it to the page, then a button that refreshes it, with the popup's two
cards for the page (the refresh card and **This page**) combined into one and
the repeated text gone.

`fm-refresh-plan` now carries `suggest`, worked out by `refreshNext` from the
cached Marketing channel list. A channel's last update is the latest of
Marketing's capture, its server refresh, and this browser's own read mark for
it (`channel:<id>` in session storage, also set by a successful **Sync X
stats**). Channels updated within the hour (`REFRESH_NEXT_MS`) are skipped;
of the rest, the oldest is suggested. A channel never updated comes first.

The card, in the popup and the side panel, says "Next to update: _channel_,
last updated 3 d ago." with **Go to _channel_**, which opens its saved page in
the active tab. On that page the button becomes **Update _channel_**
(`fm-refresh-channel`): Marketing refreshes it on its server when it can
(`refreshableChannels`), and otherwise the extension reads the tab, as
**Update _channel_ stats from this page** does. A channel Marketing cannot
refresh is never read away from its own page. After an update the plan is
redrawn, so the card moves on to the next channel.

To keep one button per job, the card leaves out:

- its own **Update** button when the page's main button already refreshes
  that channel (`suggest.covered`: Discord, Mastodon, …), or when the popup's
  **Update _channel_ stats from this page** does the same read;
- **Update _channel_ stats from this page** and Discord's "read from the
  public invite" line when the main button refreshes the channel
  (`refreshes` in the plan);
- **Send to Reddit desk** and **Sync X stats** from the page's buttons when the
  main button is that send or sync;
- the page list and the _n/m read_ count for a site with a single page, and
  the plan's hint when the popup has its own lines or buttons for the page.

## Your Reddit accounts' posts and views (2.18.0)

The user pressed **Update Reddit stats from this page** on their profile,
`https://www.reddit.com/user/ada_lovelace/`, and nothing updated
(2026-10-12). The Reddit reader only knew subreddits, threads, rules pages and
searches, and the channel **Update** reads a page's counts, which a profile
page does not show in a form it knows. They asked for each link they posted
with its views, and for more than one account (`ada_lovelace` and
`forkmesh`).

- The Reddit desk's watch list has an **accounts** list (up to 10, Marketing
  1.34.0). `fm-reddit-watch` passes it to `reddit.js`, which reads a listed
  account's profile (`/user/<name>/…` or `/u/<name>/…`, any tab) as you browse
  it, like a watched subreddit.
- On any Reddit profile the popup and the refresh panel offer **Update
  u/_name_ posts and views** (`fm-reddit-profile-update`, popup and panel
  only). It asks the reader in that tab for the profile's posts and comments
  (`fm-reddit-profile-read`; if the tab predates the extension, `reddit.js` is
  injected once), adds the account to the desk when it is not listed yet
  (owners and admins; a member is told, and the links are still recorded), and
  records each link in Marketing's posts (`POST /posts`, channel `reddit`)
  with its title, time, score as likes, comment count as replies and views.
- A post or comment counts as the account's only when Reddit names it as the
  author (`shreddit-post[author]`, old Reddit's `data-author`), or when it is a
  comment permalink, or a thread link on the profile's **Posts** tab
  (`/submitted/`). Posts by others on the page are left out.
- Reddit shows a post's views only to its author, and mostly on the post's own
  page. So after recording the links, **Update** opens each one not read in
  the last 30 minutes in a background tab, up to 25 per press, reads its views
  (`fm-reddit-read`, now with the comment id for a comment), closes the tab
  and sends the new counts at once. The popup shows the progress. The six-hour
  check (`Check monitored threads and your posts every 6 hours`) and **Check
  now** refresh them too.
- Post links are kept in the canonical form the desk uses
  (`/r/<sub>/comments/<id>/`, the comment's slug and id for a comment), so a
  link keeps its history when the title changes. A comment's title in
  Marketing is "Comment in r/_sub_: _thread title_".
- The channel **Update** is no longer offered on a profile page: it recorded
  nothing useful there.

## Browser activity on the desktop (2.16.0)

The user asked that refreshing Reddit and other pages with the extension make
the desktop app respond, with the activity of what they were doing in view.
Before, a read stayed in the browser and the desktop's Marketing page only
changed when it was reloaded by hand or by a timer.

Now the extension writes one line to `POST /api/marketing/<org>/activity`
(Marketing 1.33.0) when:

- `reddit.js` or `x.js` makes its first read of a page it reads after the
  page loads or the address changes (a reload counts; the readers mark that
  message `fresh`). The line names the page and what was read, for example
  "Read r/selfhosted: 25 posts. 2 waiting for the Reddit desk." The same page
  writes at most one line every five seconds, and later reads of the same
  load (scrolling, the three-second ticks) write none;
- the refresh panel or popup runs its main button: the line is the run's own
  message;
- a channel update from **This page** or the Marketing page finishes or
  fails.

Only pages the readers already read are reported: watched subreddits,
watched searches, monitored threads and the tracked X profile. Nothing is
sent for any other page. A fresh read also redraws the badge and the side
panel at once, even when the page was read in the last 12 hours.

The panel's **Send to Reddit desk** used to answer "Sent to the Reddit desk:
0 new threads, 0 rule sets, 0 view readings." when nothing qualified. It now
lists only what happened, including threads already on the desk and sends
the desk refused. When nothing was sent it says why: the posts on a listing
are not threads you opened and none has every word of a watched search in
its title (open a thread to log it), or the threads read so far are already
on the desk.

## Any page, filed under a contact

The toolbar icon works on every site, not only the ones with a reader. Open
the popup anywhere and expand **Save this page to a contact**: pick the
contact and **Add link to notes** appends a dated line with the page's title
and address to their CRM notes. Only what the address bar shows is read; the
page itself is never touched.

## X profile stats for Marketing

Open your organization's X profile (`x.com/forkmesh` unless you track another
handle) and the extension reads the numbers the page already shows: following,
followers, and posts from the header, photos & videos from the **Media** tab,
the replies, reposts, likes, and views under the profile's own posts as you
scroll them, and the posts mentioning the handle when you open a search for
`@forkmesh` or, signed in as that account, the **Mentions** tab of
notifications. Replies, reposts, likes, views, and mentions count posts from
the last 30 days that have been on screen.

It only looks: nothing is added to x.com, no request is sent to X, and nothing
is written to the page's console. Every other X page is left alone. The numbers
stay in the extension until you press **Sync now**, which records them as a
snapshot of the X channel in the organization's **Marketing** dashboard (the
CRM's organization when Marketing is installed there), keeping the reach and
clicks of the last X snapshot since the profile does not show them. Reading a
profile page records nothing by itself.

The toolbar popup's **X profile stats** widget shows the numbers, when they
were read and recorded, and any error from reading the page or recording the
snapshot; that widget is the only place errors appear. **Track** switches to
another handle (and forgets the previous one's numbers), **Open profile** opens
it, and **Sync now** records the snapshot.

Since 2.9.0 **Sync now** also adds the profile's posts to Marketing's
**Posted links**, so scrolling the profile is how X posts get there. Each post
read on screen is kept with the opening 200 characters of its text (up to 1000
posts), and Sync sends the ones read since the last sync, in batches of 250, as
`https://x.com/<handle>/status/<id>` links with the text as title, the posting
time, and the views, likes, replies and reposts. A link already in Marketing
gets the new counts; a title or date someone typed there is kept. The widget
says how many links were added and updated.

Since 2.10.0 each post is kept with the opening 280 characters of its text and
its images: up to four photos, video posters and link-card images from
`pbs.twimg.com` (`/media/` photos at X's `small` size), never the avatar,
emoji or a quoted post's pictures. A later read that finds no images keeps the
ones already seen. The widget lists the six most recent posts with their first
two images, date, views and likes, each linking to the post, and **Sync now**
sends the images with the posted links. The header's following count was
already read and is recorded with every snapshot.

Since 2.11.0 the popup's **Update X stats from this page** adds the latest
posts too (the user asked for the click to bring in the newest posts and keep
them up to date in real time, 2026-10-07). After the page's numbers are
recorded, the posts read on the tracked profile since the last sync go to
**Posted links** at once, and that tab turns live: while it stays open, every
new post or changed count the reader sees there is sent within ten seconds,
without another click, and a change in the header's following, followers,
posts or media count is recorded as a new X snapshot (at most one a minute,
so scrolling does not fill **Recorded numbers**). Only one tab is live at a
time; clicking Update on another tab moves it there. The popup's card says
the tab is live with the links added and updated so far and offers **Stop
live updates**; live updates also end when the tracked handle changes, a send
fails (the error goes to the X widget), or the browser restarts. While the
popup is open, its X widget and card redraw as new posts are read. **Update
from saved page** sends the latest posts once, since that tab closes when it
is done.

Since 2.12.1 the reader also understands the profile X shows when nobody is
signed in (2026-10-07: the user's Update said "0 new post links" under a post
an hour old). That page is a different build of X: its posts are plain
`<article>`s with no `data-testid`, the post's time is a link reading "1h" or
"Sep 26" with no `<time>`, the text is the post's first `div[dir="auto"]`, the
counts sit under `data-engagement-action="reply"`, `"retweet"`, `"like"` and
`"generic"` (views) as the visible number, and the "319 posts" header is a
bare `<div>` in `main`. The reader saw the following and followers links there
and nothing else, so no post was ever stored or sent. A post with no `<time>`
now takes its date from its id (X ids carry the millisecond they were made:
`(id >> 22) + 1288834974657`), a quoted post's nested `<article>` is skipped
along with its text, counts and pictures, and "Show more" is left out of the
text. The signed-in page is read exactly as before.

## Connect X for posting (2.12.0)

Marketing posts to X with the organization's own X app, which needs four keys
from X's developer console (`console.x.com`): the API key and secret and an
access token and secret made after the app has **Read and write** permission.
The user asked for the key entry to be guided by the extension, page by page,
with as much done for them as possible (2026-10-07), so the extension does it.

Start it from Marketing: **Post to X → Connect → Guide me with the extension**
on the web, or **Guide me in the browser** in the desktop's Connect X dialog
(which opens the web page with `?xsetup=1`, and the guide starts there). Only an
owner or admin can start it; the extension checks `canManageSettings` in the
Marketing settings first. It opens `console.x.com` in a new tab, and
`x-developer.js` shows a guide card in the page's corner with the four steps
and what to do on the page you are on. The control to press next is ringed,
and the card's button presses it:

| Page | What the guide does |
|------|---------------------|
| Sign-in | Says to sign in as the account Marketing should post as (the access token belongs to it) and presses **Sign in**. |
| Developer sign-up | **Fill in** writes a use description of ForkMesh Marketing into X's box. Ticking X's agreement boxes stays yours. |
| Apps | Opens your app, or presses **Create App**; on the new-app form **Fill in** names it "*org* Marketing". |
| App settings | Presses **Set up** (or **Edit**) under **User authentication settings**; when that section already says Read and write, the step is ticked by itself. |
| User authentication settings | **Fill in and save** picks **Read and write** (not Direct messages) and **Web App, Automated App or Bot**, puts the ForkMesh server in Callback URI and Website URL when they are empty, presses **Save** and confirms X's "changing permissions" question. |
| OAuth 2.0 keys | Says ForkMesh does not need the Client ID and secret and closes X's window. |
| Keys and tokens | **Make the API key** presses **Regenerate** under API Key and Secret (the card warns that this replaces the app's current key) and confirms; then, once Read and write is set, **Make the access token** does the same under Access Token and Secret, warning when X says the current token is read-only. |
| X's window with keys | Reads each key next to its label and ticks the step; then offers to close the window. |

What stays the user's own: signing in, accepting X's terms, buying API credits
(**Billing → Credits**; a refusal that mentions credits says so and offers
**Open Billing**), and anything X asks that the guide does not recognise. When
a key cannot be read off the page (X masks it, or the layout changed), the
card's **Paste the keys yourself** boxes take them; a pasted key goes to the
field its shape fits (`digits-…` is the access token; 25, 50 and 45 characters
are the API key, API key secret and access token secret), wherever it was
pasted.

When the fourth key arrives and Read and write was set, the service worker
sends the four to `POST /api/marketing/<org>/x-connection`, the same check and
save as the dialog's **Check and save** (X is asked `GET /2/users/me`). Keys
captured before Read and write was set skip that: ticking Read and write drops
an access token already read, since X keeps a token at the permission it was
made with. When X refuses the keys, the access token is dropped and the card
asks for a new one; the API key stays. On success the card says **Connected as
@handle** and **Back to Marketing** returns to the Marketing tab, which closes
its dialog and shows the account; the desktop polls `GET /x-connection` every
five seconds for twenty minutes after **Guide me in the browser** and shows it
too.

How it is kept narrow:

- `x-developer.js` runs on `console.x.com`, `developer.x.com` and
  `developer.twitter.com` (new site access in 2.12.0), and does nothing there
  until a setup started from Marketing is active: its first act is to ask the
  service worker, and with no setup it stops. It sends nothing to X and no
  request anywhere; the card lives in a closed shadow root.
- The setup, with the keys read so far, is kept in `chrome.storage.session`
  (memory only, closed to content scripts) for two hours at most, and the
  keys are forgotten once Marketing has them. Replies to the page and the
  Marketing tab carry the last four characters of each key, never a key.
- Only the Marketing page can start a setup, only an X developer console tab
  can hand over keys, and **Stop** (the card's ✕, or another start) ends it.

## Reddit desk for Marketing

The Marketing package's **Reddit desk** (`/dashboard/marketing`, **Reddit
desk** tab) is where a team logs the Reddit threads it might answer, with the
subreddit's rules, the disclosure check and the reply. ForkMesh's servers
cannot read Reddit (Reddit answers them 403), so the reader is your own
browser: the extension reads the Reddit pages you open yourself and, when you
press a button, sends what it read to the desk.

What it reads, on `www.reddit.com` and `old.reddit.com` only, and only on
these pages:

- a listing of a watched subreddit (`/r/<sub>/`, `/new/`, `/hot/`, `/top/`,
  `/rising/`, `/controversial/`, `/best/`): each post's subreddit, id, title,
  link, author, comment count, score and time;
- a Reddit search whose text is one of the watched searches (letter case and
  extra spaces aside), Reddit-wide or inside one subreddit: the same for each
  post listed;
- a thread in a watched subreddit (`/r/<sub>/comments/<id>/...`): the same for
  that thread's post;
- a watched subreddit's rules page (`/r/<sub>/about/rules`, best on
  `old.reddit.com`): each rule's title and description.

On new Reddit the post fields come from the `shreddit-post` element's
attributes, on old Reddit from the `div.thing` row's `data-` attributes, and
otherwise from the thread links on the page (title and link only). Rules come
from old Reddit's rule list; new Reddit's layout is read best-effort. An old
Reddit rules page with no rules is recorded as "no rules"; elsewhere an empty
read records nothing.

The watch list is the desk's own (owners and admins edit it in Marketing). The
extension keeps a copy and refreshes it only from the popup: **Refresh watch
list**, **Send to Reddit desk**, or opening the popup when the copy is more
than six hours old. Reading a page sends nothing to the desk; since 2.16.0
the first read after a page loads writes one line of browser activity
(see [Browser activity on the desktop](#browser-activity-on-the-desktop-2160)).

Version 2.3.0 added `www.reddit.com` and `old.reddit.com` to the sites the
extension runs a reader on; Chrome lists them under the extension's site
access. Every other Reddit page is left alone. `reddit.js` only looks: it adds nothing
to the page, sends nothing to Reddit, writes nothing to the page's console,
and checks the page at most every three seconds while the tab is visible,
passing it on only when it changed. Nothing is posted, voted, followed or
messaged, and the extension opens a Reddit page by itself only to check
monitored threads (below); the popup's rules links open one only when you
click them.

What is sent, and when: nothing until you press **Send to Reddit desk** in the
popup's **Reddit desk** widget. Then:

- each thread not sent before is logged on the desk as **To review**, with its
  link, title, author, comment count, score, posting time and
  `source: "extension"`, but only threads you opened in a watched subreddit,
  posts listed on a watched search, and posts in a watched subreddit's listing
  whose title holds every word of a watched search. A thread already on the
  desk counts as sent;
- each subreddit's rules read since the last send replace that subreddit's
  rules on the desk, which every member of the organization sees.

The extension never ticks **Rules read** or **Disclosed** on a thread and never
records a reply: those stay the person's own checks on the desk. The widget
shows the watch list's size and age, the threads and rule sets waiting, the
last send, errors, and links to the rules pages of watched subreddits whose
rules nobody has read yet; **Open Reddit desk** opens the desk.

### Monitored threads (2.8.0)

The widget's **Monitored threads** list follows threads on the desk that are
marked monitored, wherever they are (a monitored thread need not be in a
watched subreddit). Paste a thread or comment link and press **Monitor**
(the link of the Reddit tab you are on is filled in): the thread is logged on
the desk with `monitored: true`, or, if it is already there, switched to
monitored. A comment link makes that comment the one followed. **Stop**
switches monitoring off and leaves the thread on the desk.

On a monitored thread `reddit.js` reads the post's views, points and comment
count and the followed comment's points and views: points and counts from the
`shreddit-post` / `shreddit-comment` attributes (old Reddit: the `div.thing`
row), views from a `view-count`-style attribute or a short label such as
"1.2K views" or "Views 1,234" inside the post, the comment (not its replies)
or Reddit's insights panel. Reddit shows views only to the account that
posted, so open the thread from that account. Readings stay in the extension
until **Send to Reddit desk**, **Check now** or the six-hour check sends them
as a `PATCH` to the thread (with the title, if the desk has none).

**Check now** opens each monitored thread (up to 20, the comment's link when
one is followed) in a background tab, asks the reader in that tab for the
numbers (`fm-reddit-read`, answered only to the extension itself, never to a
page), closes the tab and sends what it read. **Check monitored threads every
6 hours** (off by default) does the same from a Chrome alarm. These are the
only times the extension opens a Reddit page itself; it still posts, votes
and sends nothing to Reddit.

### Walk the watch list (2.15.0)

A desk only fills when somebody browses Reddit. **Walk the watch list** (in
the widget, and on the Marketing Reddit desk page) has the extension do that
round itself, once, when you press it:

1. it refreshes the watch list from Marketing and builds the stops: each
   watched subreddit's `/new/`, each watched search over the last week, and
   the rules page of each watched subreddit with no rules on file yet; at
   most 24 stops;
2. it opens **one** tab and walks the stops in it, one after another;
3. on each page it injects `reddit-walk.js`, which only paces the page: it
   scrolls in bursts of five to eleven small steps 28–90 ms apart, waits
   0.3–0.9 s between bursts, sometimes drifts a little back up, and glides
   the pointer along a curved path onto one to three of the posts on screen,
   resting 0.3–1.1 s on each. Reddit's listings load lazily and parts of a
   card render on hover, so a page nobody scrolls holds almost nothing to
   read; the waits are also what keeps one walk's reading rate near a
   person's rather than hammering the site;
4. it asks `reddit.js` for the page (`fm-reddit-scan`), which reads it
   exactly as it reads a page you opened yourself;
5. after 2.5–6 s it moves to the next stop, and at the end it runs the same
   **Send to Reddit desk** and reports what was logged.

`reddit-walk.js` never clicks, types, submits, votes or posts: it dispatches
`pointermove`, `mousemove`, `pointerover`, `pointerout`, `mouseover` and
`mouseout` and nothing else, and it has no `chrome.*`, no network call and
no way to write to the page. It is not a way around Reddit's bot check: it
spoofs no header, hides nothing, runs in your own signed-in browser, and a
page Reddit refuses is simply one it reads nothing from.

The pacing lives in its own file so `reddit.js` keeps its promise never to
touch a Reddit page; the background injects it for one page of a running
walk. That injection is why `https://www.reddit.com/*` and
`https://old.reddit.com/*` are in `host_permissions` from 2.15.0 — both were
already `reddit.js` content-script matches, so Chrome's install warning is
unchanged.

Only you start a walk. **Stop the walk** ends it after the page it is on,
two walks never run at once, and the tab closes when the walk ends.

## Marketing channels

Every channel in Marketing has an **Update** button. On the Marketing page
`dashboard.js` passes the organization and the channel id (never an address)
to the service worker, which looks up the channel's saved link in Marketing,
opens it in a new tab, injects `marketing-capture.js` into that tab only,
reads the counts the page shows, and posts them to
`/api/marketing/<org>/capture`. It then closes the tab, returns to the
Marketing tab and tells it the result; a failure (a sign-in wall, a page with
no counts, a slow page) leaves the tab open and shows the reason.

Chrome must allow the extension to read the site. The first time, the new tab
shows `capture.html`, which asks for that one site and then continues; the
popup's **Allow channel sites** asks for all configured channels at once. The
reader changes nothing on the page and sends nothing to the site.

Discord is the exception: the extension never opens, reads or closes a
Discord tab. Its **Update** (and the popup's send on a Discord page) asks
Marketing's `POST /api/marketing/<org>/refresh` for the `discord` channel,
which reads the member and online counts from Discord's public invite API on
the server, and the popup does not ask for Discord site access. Opening the
Discord web app in a tab and closing it a second later signed people out of
Discord (2.4.1).

Google (2.5.0): on a Google results page (`/search` on `google.com` or a
country domain) the reader returns only the result count, from
`#result-stats`, or the number of result links when there is no next page,
as `indexedPages`. Marketing's Google channel links to
`site:<host>`, so its **Update** records how many pages Google lists for the
site.

Reddit (2.6.0): on a subreddit page (`/r/<name>/` on `www.`, `old.` or
`new.reddit.com`) the reader returns only the subreddit's own numbers:
members, weekly visitors and weekly contributions from the
`shreddit-subreddit-header` element's attributes or its visible labels, or the
reader count in old Reddit's sidebar (`.side .subscribers .number`), as
`followers`, `reach` and `engagements`. It does not scan the rest of the page,
where post scores and comment counts would be taken for the channel's likes
and replies. Dated posts on the listing still give the last post and the
30-day count. Marketing's Reddit channel shows these steps to people who have
not set it up yet.

The popup's **Marketing channels** section sends the page open in the current
tab to a channel (detected from its address, or picked), and lists the last
updates and their errors.

## What leaves the browser

Only what the CRM stores, and only when you press a button or send a
message: the provider message and thread ids, the subject,
the addresses or handles involved, the direction, the date, the permalink, the
contact details shown in the panel, and the chosen flow and stage. The body of
the email is read in the page only to prefill those fields and is never sent.
A Mastodon post has no subject line, so its content warning, or the opening of
its text when it has none, stands in for one: the panel shows exactly that
line before anything is linked, and nothing else from the thread is sent. On
a contact the CRM already has, those details fill empty fields only. A
message you send is tracked with its subject, the sending mailbox, up to five
recipient addresses and names (to find the one with a record), and the time;
never its body. Requests go to your ForkMesh server
with the session token from signing in; the browser's cookies are never sent.

Filing a page under a contact sends that page's address and title, and
nothing else. **Send to Reddit desk** sends the Reddit fields listed under
**Reddit desk for Marketing** to the Marketing package, and nothing to Reddit. **Update u/_name_ posts and
views** sends, for each post and comment of that account read on the page or
in its background tab, its link, title, time, score, comment count and views to
Marketing's posts, and adds the account name to the desk's accounts.
A channel **Update** sends the counts, the page's title, description, image,
handle and address, up to five recent post links and titles, and which labels
matched, to Marketing, and nothing to the channel's site. The refresh panel's buttons send what **Sync now**, **Send to Reddit desk** and Marketing's channel **Refresh** send; showing it or the badge asks ForkMesh only for the Marketing organization, its channel list and, on Reddit, the watch list when it is stale, and the tab's address never leaves the browser. **Connect X for posting** sends the four keys read on X's developer console
to Marketing's `x-connection`, once, and nothing to X. **Sync now** in
the X widget sends the counts above and, for each post of the tracked profile
read since the last sync, its link, the opening of its text, its time and its
four counts. The Mastodon instances you add are kept in the extension's own
storage with the host permission Chrome granted for each.

The session token and the chosen organization are kept in the extension's
own storage (`chrome.storage.local`). **Sign out** revokes the session on the
server, clears it, and locks the vault.

The vault sends ciphertext only, to `/api/vault`: encrypted items, wrapped
keys, and signed shares. The master password, the keyset, and every decrypted
login stay in the extension. See the vault package's README for the formats.

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

The CRM endpoints it uses live in the `crm` package's `content/worker/crm.py`:
`POST /api/crm/<org>/contacts/lookup` (by `email` or `handle`; returns the
organization's flows too), `POST /api/crm/<org>/emails/link` (takes optional
`handle`, `flow`, `stage`, and `contact` details), and
`GET`/`PATCH /api/crm/<org>/contacts[/<id>]` for the contact picker, the
last-contacted date, and the notes a saved page is appended to.
The X widget records snapshots with `POST /api/marketing/<org>/metrics`
(`channel: "x"`, `source: "x-profile"`) from the Marketing package.
The Reddit desk widget reads the watch list with
`GET /api/marketing/<org>/reddit` and sends threads with
`POST /api/marketing/<org>/reddit` and rules with
`POST /api/marketing/<org>/reddit-rules`; monitored threads are added with
the same `POST` (or `PATCH /api/marketing/<org>/reddit/<id>` when already
logged) and their readings sent with that `PATCH`.
The vault endpoints live in the `vault` package's `content/worker/vault.py`.
