# Browser Extension package

> **⚠️ To load this extension in Chrome:** Use `content/extension/` directory, NOT this folder.  
> See `WRONG-FOLDER-READ-THIS.txt` if Chrome says "Missing 'manifest_version' key".

The ForkMesh extension for Chrome, as its own Market package, so one
extension with one sign-in serves several packages and can be released on its
own. What it does is documented in `content/extension/README.md`:

- fills and saves logins from the Password Vault (`vault` package);
- links Gmail, Zoho Mail and Mastodon conversations to CRM contacts, links
  mail you send to the recipient's record, logs the people your own Mastodon
  posts mention (adding the new ones as contacts), and files pages under a
  contact (`crm` package);
- sorts, searches and groups a Zoho Mail list by sender and selects, marks
  read, archives or deletes the shown messages in bulk through Zoho's own
  toolbar, for inbox zero (no package needed);
- reads channel counts, the organization's X profile and posts, and Reddit
  pages for the Marketing package, only when asked, and guides an owner or
  admin through X's developer console to connect X for posting;
- shows, in Chrome's side panel, the popup and a toolbar badge, which pages of
  the current site feed Marketing, with no script injected into channel sites;
- walks the Reddit desk's watch list in one tab at a reader's pace when you
  ask, without clicking, typing, voting or posting.

The package has no worker module and stores nothing on the server. Each
feature talks to the API of the package it belongs to and works only when that
package is in use: the vault per account, the CRM and Marketing per
organization.

## Layout

- `content/extension/`: the Manifest V3 extension. The deploy stage zips it
  reproducibly to
  `/dashboard-plugins/browser-extension/browser-extension-<version>.zip`
  (`app/tools/build_app_plugins.py`) under one top-level `browser-extension`
  folder. To load it from a checkout, pick `content/extension/` in Chrome's
  **Load unpacked**, never this package folder: the `manifest.json` here is
  the Market manifest, which Chrome rejects for having no `manifest_version`.
  `WRONG-FOLDER-READ-THIS.txt` sits beside it as a reminder, and the dashboard
  install card and the desktop CRM name the right folder.
- `content/web/`: the `/dashboard/extension` page (install, update, what the
  extension does with each package).
- `tests/`: the extension's own contracts, its contract with the CRM package,
  and the vault background code driven under a fake browser against the real
  vault worker module.

`content/extension/vault-crypto.js` and `vault-client.js` are copies of the
vault package's files. Never edit them here: edit
`marketplace/packages/vault/content/crypto/` and run
`python3 marketplace/packages/vault/tools/sync_crypto.py`. Both packages' test
suites fail when a copy has drifted.

## Releasing

The extension's version lives in four places that tests hold together:
`manifest.json`, the row in `marketplace/catalog.json`,
`content/extension/manifest.json`, and `EXTENSION_VERSION` in
`content/web/page.js`. Then run `python3 app/tools/build_market_catalog.py`.

Other packages must not pin this one's exact version. The CRM page keeps only
the oldest build it works with (`CRM_EXTENSION_MINIMUM`).
