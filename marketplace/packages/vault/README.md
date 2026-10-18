# Password Vault package

Password Vault is an opt-in ForkMesh package. Each account creates one vault
for itself, protected by a master password that never leaves the device it is
typed on. Logins are encrypted before they are uploaded, so ForkMesh stores
ciphertext and cannot read a title, a URL, a username, or a password. A login
can be shared with a named ForkMesh user, or with an organization the owner
belongs to.

The same vault opens in four places, and each one is a full client (create,
unlock, recover, add, edit, delete, share, stop sharing, settings):

| Client | Where | Fills logins |
| --- | --- | --- |
| Web dashboard | `/dashboard/vault` | through the browser extension |
| Browser extension | the `browser-extension` package | on the login's own https site |
| Desktop | the Vault rail page (`content/desktop`) | copies to the clipboard |
| iOS and Android apps | Tools, Vault (`native/ios/ForkMeshCore/Sources/FMVault`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/vault`) | into other apps: the Android 8+ autofill service and the iOS AutoFill credential provider (`native/ios/ForkMeshAutoFill`) |

The package owns its cryptography, its clients' logic, its dashboard page, its
Worker API and data hooks, its manifest, preview, and tests. The host supplies
sessions, D1, and row sealing only. The phone clients live in `native/ios`
and `native/android` because that is where the apps are built; their tests
open this package's fixtures.

## What the server can and cannot see

It cannot see anything inside an item. There is no plaintext column for it:
the tables hold blind account indexes, account and organization names, ids,
revisions, timestamps, permissions, and one sealed `data` blob per row.

It can see who owns an item, who it is shared with and at which permission,
when it changed, and roughly how large it is (items are padded to 256-byte
steps). It also decides who is handed which ciphertext, so it enforces
permissions, organization membership, and revocation.

Every blob is additionally sealed with the host row key. A copy of the
database alone therefore does not even yield the wrapped keyset to guess a
master password against.

## Cryptography (suite `fmv1`)

| Purpose | Primitive |
| --- | --- |
| Master password stretch | Argon2id v1.3, 64 MiB, 3 passes (RFC 9106's second recommended setting). The client refuses anything below 19 MiB and 2 passes, so a server cannot downgrade it. |
| Key derivation | HKDF-SHA-256, one labelled context per use |
| Items, wrapped keys | AES-256-GCM, with the item id, account, and purpose bound as associated data |
| Shares | X25519 and ML-KEM-768 (FIPS 203) together, joined with the X-Wing combiner, so opening a share takes breaking both |
| Authenticity | Ed25519 signatures on every published key bundle and every share |
| Recovery | A 256-bit random code wrapping a second copy of the keyset |

Each account has a keyset: a vault key, an X25519 key, an ML-KEM-768 seed,
and an Ed25519 key. It is stored only wrapped, once under the stretched master
password and once under the recovery code. Changing the master password
re-wraps the keyset and leaves every item and share untouched.

Each item has its own random 256-bit key. The owner's copy is wrapped under
the vault key. A share seals the item key to one recipient's public keys and
is signed by the sender over the item id, both account names, the permission,
and the ciphertext. A share moved to another item, another recipient, or
another permission fails its signature.

**Post-quantum scope.** Confidentiality is what a recorded ciphertext puts at
risk later, so that is what ML-KEM-768 protects: AES-256 and Argon2id are
already outside a quantum computer's reach, and every share adds ML-KEM-768 to
X25519. Signatures stay Ed25519, because forging one needs a quantum computer
at the time of the forgery, not afterwards.

**Trust on first use.** The server hands out public keys, so it could hand out
the wrong ones. The first time a person's keys are seen, their fingerprint is
pinned in an encrypted record only the owner can open. The share dialog shows
the fingerprint so it can be compared out of band (Vault settings shows your
own). If the server later presents different keys for a pinned name, nothing is
sealed to them and nothing from them is opened until the owner confirms the new
fingerprint.

**Revocation.** Ending a share deletes its grants and then rotates: the item
is re-encrypted under a new key, sealed again to everyone who remains. A grant
made through an organization is served only while its reader is still a
member. Someone who already saw a password still knows it, so the page says to
change it on the website too.

**Organization shares.** An organization share is a set of per-member grants.
A member who joins, or creates a vault, later is listed as pending; the owner's
client seals to them the next time it unlocks, because only a holder of the
item key can.

## One suite, four implementations

Suite `fmv1` exists four times, because each client has to do its own
cryptography:

- **JavaScript** (`content/crypto/vault-crypto.js`) for the page and the
  extension. WebCrypto supplies AES-GCM, HKDF, X25519 and Ed25519; SHA-3,
  ML-KEM-768, BLAKE2b and Argon2id are written out and checked against
  OpenSSL 3.5 vectors.
- **C++** (`content/desktop/VaultCrypto.cpp`) for the desktop. Every primitive
  is OpenSSL 3.5's; nothing is reimplemented. A desktop that loads an older
  OpenSSL gets `argon2id_unavailable` or `mlkem_unavailable` from
  `cryptoAvailable()` before any stretch or key generation, and the page says
  so instead of unlocking.
- **Swift** (`native/ios/ForkMeshCore/Sources/FMVault/VaultCrypto.swift`) for
  the iOS app and **Kotlin**
  (`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/vault/VaultCrypto.kt`)
  for the Android app, on the apps' own crypto modules.

`tests/fixtures/interop-from-{js,cpp,dart}.json` hold a small vault made by
the browser, the desktop and the previous mobile app, with its test-only
secrets beside it. Each implementation's test opens them and refuses the same
tamperings, so a vault written by any client opens in every other. The web
and desktop clients' logic is likewise tested against one thing: this
package's real Worker module, over `tests/vault_stdio_server.py`.

A master password is NFKC-normalized before it is stretched: browsers and Qt
do that themselves, the iOS app uses Foundation and the Android app
`java.text.Normalizer`.

## Importing from Apple Passwords and other password managers

The dashboard page's **Import** button (next to New login) takes a CSV export
from Apple Passwords (on a Mac: Passwords, File, Export All Passwords), Chrome,
Firefox, DuckDuckGo, Bitwarden or 1Password, or the ZIP that Safari's Export
writes on iPhone. The iPhone app reads the same files (see On the phone).

- `vault-client.js` does the reading, so the page and the browser extension
  share it: `readLoginsFile(bytes)` opens a ZIP (stored or deflated, through
  `DecompressionStream`; `__MACOSX/` and dot files skipped), decodes UTF-8 or
  falls back to Latin-1, and `readLoginsCsv(text)` matches columns by header
  name. The only column it needs is a password one. Rows with neither a
  username nor a password (secure notes, cards) are counted and left out.
  Files over 16 MB are refused, and so is a ZIP entry that inflates past
  16 MB, whatever size it claims: the inflate stream is cut off there. A
  password-protected entry is refused as `zip_encrypted`, and an entry whose
  length or CRC-32 does not match its header as `zip_unreadable`.
- The header also names the app for the review: Bitwarden's `login_*`,
  Firefox's `guid`, 1Password's `Archived` (checked before `OTPAuth`, which
  1Password's export has too), Apple's `OTPAuth`, Chrome's `note`, then
  DuckDuckGo's plain `title,url,username,password,notes`.
- `importFields(login)` gives what `save()` takes: the title falls back to the
  site's host, then the username, then "Imported login"; an `otpauth://` code
  (the vault has no one-time-code field) goes at the end of the notes, which is
  where the iPhone app's export looks for it.
- `planImport(logins, items)` skips a login whose host (www. folded), username
  (case-folded) and password match a readable item or an earlier row. A
  changed password for the same account is imported beside the old item, never
  over it.
- `client.importLogins(logins, { onProgress })` saves the new logins one at a
  time, each encrypted in the browser exactly as a typed-in login is, and stops
  at the first error (`vault_full`, `vault_locked`, a network failure),
  reporting `added`, `duplicates`, `remaining` and `stoppedBy`.
- The page shows how many logins are new, already there or left out before
  anything is saved, and afterwards reminds the person to delete the export:
  it holds every password in plain text. The file is read in memory and never
  uploaded.

`tests/transfer_check.js` reads each app's format, Latin-1 and ZIP files;
`tests/e2e_client.js` imports an Apple export against the real Worker module.

The desktop rail page has the same import (**Import…** in its toolbar), with
C++ twins in `VaultClient`: `readImportCsv(bytes)` (the same columns, app
detection and 16 MB cap, UTF-8 with a Latin-1 fallback), `planImport`,
`importFields`, and `importLogins`, which saves one login at a time and stops
at the first error with the same report. It reads CSV files only: a Safari ZIP
is unzipped first and its `Passwords.csv` chosen. The page zeroes its copy of
the file once parsed. `tests/test_vault_client_flow.cpp` imports an Apple
export against the real Worker module.

## On the desktop

The rail page keeps every secret in memory: no settings key, no log line, no
temporary file. It locks after an idle period, clears the clipboard 30 seconds
after a copy if it still holds what was copied, hands only https addresses to
the system browser, and answers the window's top search box with titles and
usernames only, and only while unlocked. Requests are signed by the host as
this device's account (`REQUEST_PROOF`), through the window's firewalled
network manager; `desktop/resources/network/endpoint-catalog.json` lists them.
The list distinguishes an empty vault from a search with no matches, and gives
the matching count to accessibility clients.

**Saving another package's password.** The page is the desktop's login store
(`storesLogins()`, permission `desktop.logins.store`; the host side is
**Login store** in `docs/marketplace/README.md`). Today the Router uses it to
keep the router password. `saveLogin` and `findLogins` first unlock: a locked
vault shows its own master-password dialog over the asking page, naming the
package and the address, asks again after a wrong password, and reports
`cancelled` when dismissed; an account without a vault is told to create one.
`VaultClient::rememberLogin(fields)` reloads the items, then updates the
password of the writable login with the same origin and username (keeping its
title and notes), does nothing when the password is unchanged, or saves a new
login; either way it is sealed like any other item. `loginsFor(url)` returns
the readable logins whose origin (`loginOrigin`: scheme, host and port, `http`
when no scheme is given) matches, so a password is handed back only for the
address it was saved for. The save reply never carries the password back.
`tests/test_vault_client_flow.cpp` checks the matching and the updates against
the real Worker module.

## On the phone

The vault locks when idle, after a minute in the background, and on sign-out.

**Saving the Router's password.** `VaultService.saveLogin` and `findLogins`
(both apps) let the Router keep its password here, with the same rules as the
desktop's login store: matched by origin, an unchanged password is not saved
again, a new one updates the login and keeps its title. A locked vault answers
`locked`, and the Router shows this package's unlock sheet (`VaultUnlockSheet`,
`VaultUnlockDialog`) rather than asking for the master password itself.
`VaultClientFlowTests` / `VaultClientFlowTest` and
`VaultScreenFlowTests.testAnotherFeatureSavesAndFindsALoginThroughTheService`
check it against the real Worker module.

**Quick unlock** (opt-in) keeps the unlocked keyset in the platform's secure
storage and releases it after the device's own biometric or passcode prompt.
It trades the master password's strength for the device lock's: anyone who can
unlock the phone can then open the vault. There is no in-app stand-in for the
prompt; where the platform cannot ask, quick unlock is not offered.

**Autofill into other apps** (opt-in, Android 8+):

- The service only offers locked suggestions. Choosing one opens a prompt for
  a fingerprint, face, or the screen lock, and only then is a password
  decrypted.
- Passwords are sealed to an RSA key in the Android Keystore that the hardware
  releases for 30 seconds after that prompt. The app can write the cache at any
  time (the public half needs no prompt), and nothing, the app included, can
  read a password back without it. Titles, usernames and hosts sit in a
  second file under a Keystore key that needs no prompt, so suggestions can
  be listed.
- In a browser, a login is suggested only on its own https host, and an app's
  claim of a web domain is believed only from a known browser. In any other
  app, only logins the person picked for that app before are suggested.
  Picking a login saved for a different site says so first.
- A login saved from another app is held, sealed, until the vault is next
  unlocked, because only the unlocked vault client can encrypt into it.
- The cache is a copy of the logins, not of the vault's keys. Turning autofill
  off, signing out, or deleting the vault erases it. It is never backed up. A
  phone without a screen lock cannot turn autofill on.

**AutoFill on iOS** (opt-in, the `ForkMeshAutoFill` credential provider):

- After each unlock the app seals the readable logins under a fresh AES-GCM
  key. The key sits in the keychain behind user presence (Face ID, Touch ID or
  the passcode); the sealed copy sits beside it in the keychain group the app
  shares with its extensions. Nothing is backed up or synced.
- Safari and apps with a matching associated domain get the logins saved for
  that exact host above the keyboard. Anywhere else the person taps Passwords
  and picks a login; the extension then links it to that app's domain (or, on
  iOS 26.2 and later, its bundle id) so it is suggested there next time.
- On iOS 18 and later the AutoFill menu of any text field can insert a
  username or a password from the vault.

**Moving passwords in and out** (Vault settings, Move passwords):

- Import reads a CSV export from Apple Passwords, DuckDuckGo, Chrome, Firefox,
  Bitwarden or 1Password, or the ZIP that Safari's Export writes on iPhone.
  Logins already in the vault (same host, username and password) are
  skipped; everything else is encrypted on the phone before it is uploaded.
- Export writes Apple's CSV format (`Title,URL,Username,Password,Notes,OTPAuth`),
  which Safari's Import on iPhone and DuckDuckGo's Import Passwords both read.
  The file is plaintext and the app says so before writing it.
- On iOS 26, Credential Exchange moves logins without a file: Apple Passwords
  (Export Data to Another App) can send them to ForkMesh, and ForkMesh can send
  them to Passwords, DuckDuckGo or any other app that accepts imports.
  One-time-code secrets travel as `otpauth://` links in the notes.

## Limits of this implementation

- It has not been audited by a third party.
- The JavaScript suite writes out the cryptography WebCrypto lacks. It is
  checked against OpenSSL 3.5 vectors on every test run, and against the local
  OpenSSL when it has ML-KEM. JavaScript cannot guarantee constant-time
  arithmetic.
- Like any web application, the page trusts the server to deliver honest
  JavaScript. The browser extension, the desktop, and the phone apps do not:
  they run their own packaged code.
- The fingerprint pins are stored on the server, encrypted. A server that
  served an old copy could make a known person look new again, though never
  make changed keys look known. Comparing fingerprints out of band is what
  settles it.
- ForkMesh cannot reset a master password. Without it and without the recovery
  code, the vault cannot be opened by anyone.
- Android's browser list is a list of package names, not a signature check. A
  package name can only be reused by a build signed with the same key while
  the real browser is installed.

## Layout

- `content/crypto/vault-crypto.js` — primitives and the `fmv1` formats.
- `content/crypto/vault-client.js` — unlocked state, trust pins, sharing,
  rotation. No DOM and no fetch; the page and the extension share it.
- `content/desktop/` — the desktop rail page: `VaultCrypto` (suite fmv1 on
  OpenSSL), `VaultClient` (the C++ twin of `vault-client.js`), `VaultFeature`
  (the widgets), and their core test.
- `content/web/page.ui.js` — the dashboard page's screen logic.
- `content/web/page.js` — generated: the two files above plus `page.ui.js`,
  because a dashboard page may load one script.
- `content/worker/vault.py` — the `/api/vault` API.
- `tools/sync_crypto.py` — rebuilds `page.js` and the extension's copies;
  `--check` fails on drift and runs in the test suite.
- `tools/make_openssl_vectors.js` — regenerates the committed OpenSSL vectors.
- `tools/make_interop_vectors.js` — regenerates `interop-from-js.json`. The
  C++ test writes its own with `--write <file>`, the Swift test
  (`interop-from-swift.json`) with `VAULT_WRITE_INTEROP=1`.

After editing anything under `content/crypto` or `page.ui.js`:

```
python3 marketplace/packages/vault/tools/sync_crypto.py
python3 -m pytest marketplace/packages/vault/tests marketplace/packages/browser-extension/tests
```

## Page styling

The web page uses the dashboard's shared `fm-*` components from
`app/public/dashboard/shell.html` rather than its own button and badge styles,
so it reads like the rest of the dashboard:

- Errors and notices are `fm-flash` (danger and info) with one icon and a
  dismiss button; the message goes into `[data-vault-message-text]`.
- "Copied" feedback is an `fm-toast` opened as a manual popover, so it shows
  above an open modal such as the recovery-code dialog.
- Every dialog is a transparent `<dialog class="vault-dialog">` wrapping an
  `fm-dialog` with a header, body and right-aligned `fm-btn-sm` footer.
  `fm-dialog` sets `display: flex`, which would override a closed dialog's
  `display: none` if it sat on the `<dialog>` itself.
- Confirmations are an in-page `fm-dialog` (`ask()`), not `window.confirm`;
  destructive ones use `fm-btn-danger`.
- Badges are `fm-label`; key states colour only when they matter (known keys
  success, first share accent, changed or invalid keys danger).
- Empty and unsupported states are `fm-blankslate`.
- Green (`fm-btn-cta`) is kept for the submit of each form and dialog; the
  rest are neutral `fm-btn-sm`, with icons left muted by `fm-btn`.
- `[data-view="vault"] .hidden` restores `display: none` because the shell's
  `fm-*` rules load after `tailwind.css` and would otherwise beat `hidden`.

The contract test accepts `fm-*` classes when `shell.html` defines them.

## API

All routes need a ForkMesh session and answer `no-store`.

| Route | Purpose |
| --- | --- |
| `GET /api/vault` | Whether the account has a vault, and its wrapped keyset |
| `POST /api/vault/install` | Create the vault: KDF settings, wrapped keysets, public bundle |
| `PUT /api/vault/keys` | Re-wrap after a master password or recovery code change |
| `GET /api/vault/recovery` | The recovery-wrapped keyset (audited) |
| `PUT /api/vault/trust` | The encrypted fingerprint pins |
| `GET, POST /api/vault/items` | List owned and shared items; create one |
| `PUT, DELETE /api/vault/items/<id>` | Save (owner or writer), rotate (owner), delete or leave |
| `POST /api/vault/items/<id>/shares` | Share with a user or an organization |
| `POST /api/vault/items/<id>/unshare` | End a share |
| `GET /api/vault/items/<id>/recipients` | Who the remaining shares cover, for rotation |
| `POST /api/vault/items/<id>/grants` | Seal to pending organization members |
| `GET /api/vault/pending` | Organization members still waiting for a grant |
| `GET /api/vault/users/<name>` | A user's public bundle |
| `GET /api/vault/organizations[/<org>/members]` | The caller's organizations and their members' bundles |
| `GET /api/vault/export`, `POST /api/vault/uninstall` | Encrypted export; delete the vault |
