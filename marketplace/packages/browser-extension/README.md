# Browser Extension package

> **⚠️ To load this extension in Chrome:** Use `content/extension/` directory, NOT this folder.  
> See `WRONG-FOLDER-READ-THIS.txt` if Chrome says "Missing 'manifest_version' key".

The ForkMesh extension for Chrome, as its own Market package. It used to ship
inside the CRM package; it moved here when the Password Vault needed it too,
because one extension with one sign-in is what people should install, and
because an extension release should not need a CRM release.

What it does is documented in `content/extension/README.md`:

- fills and saves logins from the Password Vault (`vault` package);
- links Gmail, Zoho Mail and Mastodon conversations to CRM contacts, links mail
  you send to the recipient's record on its own, and files pages under a
  contact (`crm` package);
- reads an organization's X profile counts and its posts' stats and images for the Marketing package, adding each post to its posted links;
- walks an owner or admin through X's developer console to connect X for posting, reading the four keys off the page (added in 2.12.0);
- fills the Marketing package's Reddit desk from the watched subreddits,
  searches, threads and rules pages you open on Reddit (added in 2.3.0,
  because ForkMesh's servers get 403 from Reddit).
- lists, in Chrome's side panel, the popup and a badge on its icon, the
  pages of the current site that feed Marketing, with a button for each and
  one that syncs X, sends the Reddit desk or has Marketing refresh the
  channel from its server, without any script in the channel's site (added
  in 2.13.0 as an in-page bar, moved out of the page in 2.14.0).
- walks the Reddit desk's watch list in one tab when you press **Walk the
  watch list**, pacing each page with reader-speed scrolling and pointer
  movement so Reddit renders what it loads lazily, then sends what it read
  to the desk (added in 2.15.0). It never clicks, types, votes or posts.
- tells Marketing, in one line, when it reads a watched page after it loads,
  runs the refresh panel or updates a channel, so the desktop's Marketing
  page reloads and shows it under **From your browser** (added in 2.16.0).

The package has no Worker module and stores nothing on the server. Each
feature talks to the API of the package it belongs to, and works only when
that package is in use: the vault per account, the CRM per organization.

## Layout

- `content/extension/` — the Manifest V3 extension. The deploy stage zips it
  to `/dashboard-plugins/browser-extension/browser-extension-<version>.zip`
  (`app/tools/build_app_plugins.py`), reproducibly, under one top-level
  `browser-extension` folder. To load it from a checkout, pick
  `content/extension/` in Chrome's **Load unpacked**, never this package
  folder: the `manifest.json` here is the Market manifest, which Chrome
  rejects for having no `manifest_version`. The READMEs alone did not stop
  that mistake (2026-09-25: Chrome reported *Missing 'manifest_version' key*
  on this folder), so the dashboard page's install card and the desktop CRM's
  **Browser extension** status name the checkout folder too, and
  `WRONG-FOLDER-READ-THIS.txt` sits beside the Market manifest, where Chrome's
  folder picker shows it to anyone about to pick this folder.
- `content/web/` — the `/dashboard/extension` page: install, update, and what
  the extension does with each package.
- `tests/` — the extension's own contracts, its contract with the CRM package,
  and the vault background code driven under a fake browser against the real
  vault Worker module.

`content/extension/vault-crypto.js` and `vault-client.js` are copies of the
vault package's files. Never edit them here: edit
`marketplace/packages/vault/content/crypto/` and run
`python3 marketplace/packages/vault/tools/sync_crypto.py`. The test suites of
both packages fail when a copy has drifted.

## Releasing

The extension's version lives in four places that tests hold together:
`manifest.json`, the row in `marketplace/catalog.json`,
`content/extension/manifest.json`, and `EXTENSION_VERSION` in
`content/web/page.js`. Then run `python3 app/tools/build_market_catalog.py`.

Other packages must not pin this one's exact version. The CRM page keeps only
the oldest build it works with (`CRM_EXTENSION_MINIMUM`).
