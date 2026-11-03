# Browser extension

The ForkMesh extension for Chrome brings your Password Vault, your
organization's CRM and Marketing, and one-click repository mirroring into the
browser, all behind one sign-in. It acts only when you press a button, on the
sites listed below, and reads nothing else.

Part of the [ForkMesh apps](apps.md). How it is built and released:
[browser extension internals](marketplace/browser-extension.md).

## At a glance

| | Feature | Works on | Needs | What reaches ForkMesh |
| --- | --- | --- | --- | --- |
| 🔑 | [Password Vault](#password-vault) | Any https site, from the toolbar popup | Your vault (per account) | Ciphertext only |
| 📇 | [CRM](#crm) | Gmail, Zoho Mail, Mastodon | The `crm` package in your organization | Message ids, subject, addresses, date; never the body |
| 📥 | [Zoho Mail inbox tools](#zoho-mail-inbox-tools) | Zoho Mail | Nothing, not even a sign-in | Nothing |
| 📣 | [Marketing](#marketing) | X, Reddit, Google results, any channel you configure | The `marketing` package in your organization | The counts a channel page shows |
| 📦 | [Repositories](#repositories) | GitHub, GitLab, Codeberg, Gitea | A ForkMesh account | The repository's address, when you press Mirror |
| 💬 | [Discourse forums](#discourse-forums) | Any Discourse forum | A ForkMesh account | The forum's address, when you press Mirror |

Requirements: Chrome 137 or newer, or a Chromium browser of the same age (for
native X25519 and Ed25519). The current version is shown on the
**Browser extension** page of the dashboard (`/dashboard/extension`).

## How it works

```text
  ┌──────────────────────────── your browser ─────────────────────────────┐
  │                                                                       │
  │   toolbar popup · side panel            page readers                  │
  │   (you press a button)                  Gmail, Zoho Mail, Mastodon,   │
  │            │                            X, Reddit, Discourse          │
  │            ▼                                     │                    │
  │   ┌─────────────────────────────────┐            │                    │
  │   │ service worker                  │◄───────────┘                    │
  │   │  session token (local storage)  │   readers only look; they never │
  │   │  vault keys (memory only)       │   click, post or send to sites  │
  │   └───────────────┬─────────────────┘                                 │
  └───────────────────┼───────────────────────────────────────────────────┘
                      │ HTTPS · Authorization: Bearer · browser cookies never sent
                      ▼
       your ForkMesh server: /api/vault · /api/crm · /api/marketing
                             /api/repository-imports
```

Every request to ForkMesh goes from the extension's background service worker
with the session token it received when you signed in. Website cookies are
never used, so being signed in to ForkMesh in a tab does not sign the
extension in, and the reverse.

## Install

The extension is installed from the dashboard, not from the Chrome Web Store.

1. Open **Browser extension** in the ForkMesh dashboard
   (`/dashboard/extension`), click **Download vX.Y.Z** and unzip it. You get
   a folder named `browser-extension`.
2. Click **Open chrome://extensions**, turn on **Developer mode**, press
   **Load unpacked** and pick that `browser-extension` folder.
3. Pin the ForkMesh icon to the toolbar, click it and sign in with your
   ForkMesh account (password, and your authenticator code if you use one).
4. For the CRM and Marketing, pick the organization the extension works for.
   The vault and repositories need no organization.

```text
  dashboard ──► Download ──► unzip ──► chrome://extensions ──► Load unpacked
                                         (Developer mode on)        │
                                                                    ▼
                         ForkMesh icon ──► Sign in ──► pick organization
```

**Self-hosted ForkMesh.** Expand **Self-hosted ForkMesh** on the sign-in form
and enter your server's address; Chrome asks once for access to it.

**Updating.** The dashboard page says when the installed copy is older than
the current one. Unzip the new download over the same folder and press
**Reload** on the extension's card in `chrome://extensions`. Keeping the same
folder keeps the same extension id, so you stay signed in.

**Signing out.** **Sign out** in the popup ends the session on the server,
forgets the token and locks the vault.

## Password Vault

Create your vault on ForkMesh first (**Vault** in the dashboard), then use it
from the toolbar popup on any https site:

| Action | What it does |
| --- | --- |
| **Unlock** | Opens the vault with your master password, stretched with Argon2id inside the extension and never sent |
| **Fill** | Types the saved username and password into the page you are on |
| **User** / **Pass** | Copies one field |
| **Save this page's login** | Offers to save what you typed on this page |
| **Generate password** | Copies a new 24-character password |
| **Lock** | Locks at once; the vault also locks after 15 idle minutes, on sign-out and when the browser closes |

Why it is safe to use:

- 🚫 **No standing access.** There is no content script on the open web and no
  access to all sites; a page is touched only after you press a button.
- 🎯 **Exact site match.** A login fills only when the tab's host is exactly
  the saved host (`www.` aside), over https, in the top frame. The check runs
  in the background worker, not in the page.
- 🧠 **Keys in memory only.** The unlocked keys live in the background worker
  and Chrome's session storage, which content scripts cannot read and which
  is cleared when the browser closes.
- 🔒 **Ciphertext on the server.** The vault sends only encrypted items,
  wrapped keys and signed shares to ForkMesh; the extension carries its own
  copy of the vault's cryptography rather than loading it from the server.

## CRM

The CRM half links the conversations you have open to contacts in your
organization's ForkMesh CRM. It works in Gmail (`mail.google.com`), Zoho Mail
(`mail.zoho.com`, `.eu`, `.in`, `.com.au`, `.jp`, `mail.zohocloud.ca`) and any
Mastodon instance you add.

**An email.** A compact **CRM header** above the message names the other party,
their address and how many other people are on it. With **Look up and link
emails I open** on (the default), opening the email looks the address up, with
a spinner while it waits, and links the email to the contact that has exactly
that address. A match by name, profile or phone is shown but not linked on its
own, and a failed or slow lookup offers **Retry**. With it off, nothing is
asked of ForkMesh until you press **Look up**. The record shows name, pipeline
and stage, owner, last email, linked emails, company, title, deal value, next
step and tags. The **FM** button (or
the toolbar popup) opens the panel, prefilled from the message:

| Button | What it does |
| --- | --- |
| **Add to list** | Picks one of the CRM's lists, creates the contact if needed, puts them on it and links the email |
| **Link only** | Records the message without changing any list |
| **Link to an existing contact…** | Files the message under another record; the address joins that record, so the next message is found by itself |
| **Last contacted** | Sets the date on the record |
| **Unlink** | Removes the link |

Records are matched by address, Mastodon handle, LinkedIn, GitHub or X
profile, phone, or by name when the address or company supports it. A name
that contradicts the record or fits two records never matches on its own, and
the panel says how a record was found.

**A new email in Zoho Mail.** While you write, the header follows the **To**
address instead of the message open behind the compose tab, and looks it up
straight away. When the CRM has that person, the header shows **On send ·
Links here** and the email is linked to them when you send it.

**Mail you send.** With **Track emails I send** on (the default), a message you
send from Gmail or Zoho Mail is linked to the first recipient that already has
a record, To before Cc, people outside your domain first, and moves **Last
contacted**. With **Tell me when they are opened** on (the default), the
extension adds a 1-pixel image to the end of each email as you press Send, and
the first time the recipient's mail app loads it you get a ping ("📬 Ada opened
your email"). A send to someone the CRM does not know is still recorded so its
open can be reported, without creating a contact. Opens from your own network
(viewing your Sent copy) are not counted; Gmail and Apple Mail load images
through their own servers, so an open there can be early, late or missing. Gmail
sends are linked after its 10-second undo window. No contact is created for a
send to unknown people.

**Mastodon.** Add each instance under **Mastodon instances** in the popup, or
press **Record this toot in the CRM** on one you have not added; Chrome asks
for that site once. With **Add people I mention on Mastodon** on (the
default), every post you send, private mentions included, links everyone it
mentions to the CRM as soon as Mastodon accepts it, up to ten people, creating
contacts for new ones. Opening one of your own posts on its permalink does the
same for posts sent elsewhere. Timelines, profiles and other people's posts
send nothing.

**Save this page to a contact** adds a dated line with the page's title and
address to a contact's notes.

## Zoho Mail inbox tools

Two strips sit above Zoho Mail's message list. They need no sign-in and send
nothing anywhere; everything lasts until the tab is reloaded.

- **Sort** by **Sender**, **Subject** (ignoring `Re:` and `Fwd:`) or
  **Date**; click again to reverse.
- **Search** as you type: every word must match the subject or the sender;
  `from:acme` and `subject:invoice` narrow it, `"two words"` keeps a phrase.
- **Group by sender** puts the biggest senders first and lets you pick one.
- **Select**, **Mark read**, **Archive** and **Delete** act on exactly the
  messages shown. The extension ticks Zoho's own checkboxes, checks Zoho
  selected that exact set, then presses Zoho's own button. **Delete** asks
  first and moves to Trash; permanent delete is never pressed.

## Marketing

For organizations using the Marketing package, the extension reads the
numbers that channel pages show, so the Marketing dashboard stays current
without anyone copying figures by hand. It reads only when asked and never
posts, clicks or sends anything to the channel sites.

| Feature | What happens |
| --- | --- |
| **Update** on a channel | Opens the channel's saved page in a new tab, reads its counts, sends them to Marketing, closes the tab. The first time, Chrome asks for that one site; **Allow channel sites** grants all of them at once. Discord is never opened in a tab; Marketing asks Discord's public API instead. |
| **Side panel** | Lists, per site, the pages that feed Marketing, marks those read in the last 12 hours and suggests the next one to update. The toolbar badge counts the unread pages. |
| **X profile stats** | On your tracked X profile, **Sync now** records followers, following, posts and the counts of posts on screen. **Update X stats from this page** keeps sending changes until you stop it. |
| **Connect X for posting** | Started from Marketing by an owner or admin: a guide card walks X's developer console and reads the four keys, which go once to Marketing to be checked with X. Signing in, accepting X's terms and buying credits stay with you. |
| **Reddit desk** | Reads the watched subreddits, searches, threads and rules pages you open, and your own accounts' posts and views. Nothing reaches the desk until you press **Send to Reddit desk**, except fresh numbers for threads already in the log. **Walk the watch list** visits up to 24 pages in one tab at a reader's pace, scrolling only. |

## Repositories

Open the popup on any page of a repository on `github.com`, `gitlab.com`,
`codeberg.org` or `gitea.com`. A card shows its size, stars, forks and open
issues, its language, licence and default branch, when it last changed, and
whether it is already on ForkMesh.

**Mirror to my ForkMesh account** lists the repository under your account;
the mirror nodes copy its Git history on their next pass (about every 30
minutes), and **Open on ForkMesh** takes you to it. A repository someone else
already listed is not listed twice. Private repositories get no card, because
the provider's public API does not show them: import them from
**Repositories** on ForkMesh with a read token
([repository imports](repository-imports.md)).

Nothing runs inside GitHub or GitLab: the popup reads only the tab's address,
and details come from the provider's public API without your provider
sign-in.

## Discourse forums

On any Discourse forum the popup shows **Mirror this forum**. It opens your
ForkMesh server's mirror setup with the forum's address filled in: on an
instance where you are an admin it sets up the mirror; elsewhere it explains
how to start an instance of your own. The extension reads only the page's
generator tag and site name, and only when you open the popup.

## Permissions

| Chrome permission | Why the extension needs it |
| --- | --- |
| `storage` | Keeps your session token, organization and settings |
| `activeTab`, `scripting` | Fills a login or reads a page only in the tab where you pressed a button |
| `alarms` | Locks the vault after 15 idle minutes; runs the optional hourly Reddit thread update |
| `sidePanel` | Shows the Marketing refresh panel |
| `tabs` | Reads the current tab's address for the repository card and the side panel; the address stays in the browser |
| ForkMesh sites | Talks to app.forkmesh.com and forkmesh.com |
| Reddit | Lets the Reddit desk read the pages it is asked to |
| Optional: other https sites | Asked for one site at a time: your self-hosted server, a Mastodon instance, a Marketing channel |

The extension also reads pages on Gmail, Zoho Mail, X, X's developer console
and Zoho's API console, each for the feature named above.

## What leaves the browser

| Feature | Sent to ForkMesh | Never sent |
| --- | --- | --- |
| Vault | Encrypted items, wrapped keys, signed shares | Master password, keys, decrypted logins |
| CRM | Message and thread ids, subject, addresses or handles, direction, date, the details shown in the panel, the chosen list; with open tracking, the address of an unknown recipient, and the recipient's mail app loads the open image from ForkMesh | The email body (read only to prefill the panel) |
| Marketing | A channel's counts, title, description, image and up to five recent posts; Reddit desk fields; X keys once | Anything to X, Reddit or the channel sites |
| Repositories | The derived import id when you open the popup; the address when you press Mirror | Your provider sign-in |
| Zoho Mail tools | Nothing | Everything |

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| Chrome says **Missing 'manifest_version' key** | You picked the wrong folder. Pick the unzipped `browser-extension` folder, or from a checkout `marketplace/packages/browser-extension/content/extension` |
| The dashboard says the extension is out of date | Unzip the new download over the same folder and press **Reload** on its card |
| You were asked to sign in again after updating | The new copy went to a different folder, which Chrome treats as a new extension; sign in once |
| Chrome refuses to load it | Update Chrome to 137 or newer |
| CRM buttons do nothing | Pick an organization in the popup; the `crm` package must be installed there |
| A repository's size shows "–" | GitLab tells only a project's members its size |
| The repository card says the provider is rate-limiting | GitHub allows 60 public requests an hour per address; Mirror still works, because the server reads the provider itself |
| A Marketing update asks for a site | Allow it once, or press **Allow channel sites** to grant all channels |

## Related

[ForkMesh apps](apps.md) ·
[Repository imports](repository-imports.md) ·
[Encryption and signatures](encryption.md) ·
[Browser extension internals](marketplace/browser-extension.md) ·
[Documentation index](README.md)
