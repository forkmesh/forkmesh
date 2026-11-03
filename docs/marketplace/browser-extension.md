# Browser extension internals

How the `browser-extension` Market package is laid out, how its parts talk to
each other, and how it is tested and released. What it does for users is on
the [browser extension guide](../browser-extension.md). Back to the
[Marketplace](README.md).

## Package layout

```text
  marketplace/packages/browser-extension/
  ├── manifest.json              Market manifest (not Chrome's)
  ├── WRONG-FOLDER-READ-THIS.txt reminder: never load this folder in Chrome
  ├── content/
  │   ├── extension/             the Manifest V3 extension Chrome loads
  │   │   ├── manifest.json      Chrome manifest, permissions, content scripts
  │   │   ├── background.js      service worker: sign-in, every ForkMesh request
  │   │   ├── vault*.js          vault background half and copied crypto
  │   │   ├── popup*.{html,js}   toolbar popup and its cards
  │   │   ├── content.js         Gmail and Zoho Mail reader, send tracking
  │   │   ├── mastodon.js x.js reddit.js forum.js   page readers
  │   │   └── refresh-*          side panel
  │   └── web/                   /dashboard/extension install and update page
  ├── preview/                   Market screenshot
  └── tests/                     contracts and fake-browser harnesses
```

The package has no worker module and stores nothing on the server; each
feature calls the API of the package it serves. One extension and one sign-in
serve the `vault`, `crm` and `marketing` packages so each can release without
the others.

## Message flow

```text
   page reader (content script)        popup · side panel · capture page
   reads only; no network access        extension pages, no tab
              │                                      │
              │ chrome.runtime.sendMessage           │
              ▼                                      ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │ background.js (service worker)                                  │
   │  sender check: vault and POPUP_ONLY messages are answered only  │
   │  for one of the extension's own pages with no tab               │
   │  session token in chrome.storage.local · vault keys in .session │
   └──────────────────────────────┬──────────────────────────────────┘
                                  │ fetch, Authorization: Bearer,
                                  │ credentials: "omit"
                                  ▼
          /api/accounts/login · /api/vault · /api/crm/<org>/…
          /api/marketing/<org>/… · /api/repository-imports
```

- **Sign-in.** The website's session is an HttpOnly cookie whose non-GET
  requests are origin-checked, so a `chrome-extension://` caller cannot use
  it. The extension calls `POST /api/accounts/login` itself, keeps the
  returned `sessionToken`, sends it as a bearer with `credentials: "omit"`,
  and revokes it with `POST /api/accounts/logout`.
- **Sender checks.** `background.js` refuses any message whose type contains
  `vault`, and every type in `POPUP_ONLY` (the repository and forum cards,
  most Reddit desk actions, toot tracking), unless the sender is one of the
  extension's own pages with no tab (`mayUseVault`). A content script on a
  hostile page can therefore never ask for a password or start a walk. Add a
  new popup-only message to `POPUP_ONLY`.
- **Vault fill.** Fill goes through `activeTab` and `scripting` after a host
  check against the tab Chrome reports: exact host and port (the scheme's
  default port counts as none, since another port can be a different service),
  https, top frame. The page repeats the host and port check when the fill
  runs, in case the tab navigated. The
  keyset lives in `chrome.storage.session` only, with a 15-minute idle lock on
  an `alarms` timer.
- **Marketing capture.** The dashboard page passes only the organization and
  channel id (`dashboard.js`); the worker looks up the saved link, opens it,
  injects `marketing-capture.js` into that tab with `scripting.executeScript`
  and posts to `/api/marketing/<org>/capture`. Channels in
  `CAPTURE_SERVER_CHANNELS` (Discord) are never opened in a tab: opening and
  closing them logged the user out, so they go to `/refresh` instead. Add any
  other site that misbehaves the same way to that set, provided Marketing has
  a server refresher for it.
- **Repositories.** `repo.js` parses the tab address the same way the
  server's `parse_provider_source` does (`app/src/repository_imports.py`) and
  derives the server's `external_repository_id` (`ext_` and the first 24 hex
  digits of SHA-256 of `<provider>\n<provider id>\n`), so the status is one
  `GET /api/repository-imports/<id>` with no list downloaded.

The full feature reference, with every endpoint and message, is
`content/extension/README.md` beside the code.

## Copied cryptography

`content/extension/vault-crypto.js` and `vault-client.js` are copies of the
vault package's files, so the extension never runs server-delivered crypto.
Never edit them in the extension: edit
`marketplace/packages/vault/content/crypto/` and run
`python3 marketplace/packages/vault/tools/sync_crypto.py`. Both packages' test
suites fail when a copy drifts.

## Tests

| Test | Covers |
| --- | --- |
| `tests/test_browser_extension.py` | Narrowest permissions, bearer-only requests, the mail, Mastodon, Zoho, X, Reddit, capture, refresh panel and repository flows, the CRM contract |
| `tests/test_extension_vault.py` | The vault background code under a fake browser against the real vault worker module |
| `tests/test_extension_forum.py` | Discourse detection and the mirror setup link |
| `tests/background_*_harness.js` | Node harnesses for capture, Mastodon, Reddit, refresh panel, repositories, vault, X posts, X and Zoho setup |
| `tests/zoho_tidy_harness.js` | Zoho Mail search, sender grouping and toolbar button matching |

The pytest wrappers run the harnesses with Node and stand up
`crm_stdio_server.py` and `marketing_stdio_server.py` as the server side.

## Releasing

```text
  manifest.json ─┐
  catalog.json  ─┼─ same version, held together by tests
  extension/    ─┤
   manifest.json │
  web/page.js   ─┘  EXTENSION_VERSION
         │
         ▼
  python3 app/tools/build_market_catalog.py
         │
         ▼
  deploy stage zips content/extension reproducibly to
  /dashboard-plugins/browser-extension/browser-extension-<version>.zip
  (one top-level browser-extension/ folder; app/tools/build_app_plugins.py)
```

- Bump the version in all four places, then rebuild the catalog.
- A version is out only once the **Deploy nodes** run for its merge passes
  ([auto-deploy](../operations/auto-deploy.md)): the zip and the CRM endpoints
  it calls ship together. 2.30.0 and CRM 1.50.0 sat on `main` unserved because
  every deploy failed its test gate, so the panel found no lists; 2.31.0 also
  takes in the open-tracking changes merged after 2.30.0 without a bump.
- Other packages must not pin this one's exact version. The CRM page keeps
  only the oldest build it works with (`CRM_EXTENSION_MINIMUM`) and links to
  `/dashboard/extension`.
- Chrome limits the manifest `description` to 132 characters;
  `minimum_chrome_version` is 137 for WebCrypto Ed25519 and X25519.
- In `popup.html`, dark-mode overrides must come after the base rules: they
  have equal specificity, and placed first they lose.
- A user who loaded the extension from a different folder gets a new Chrome
  extension id and must sign in again.

## Related

[Browser extension guide](../browser-extension.md) ·
[Package system](package-system.md) ·
[Repository imports](../repository-imports.md) ·
[Encryption and signatures](../encryption.md)
