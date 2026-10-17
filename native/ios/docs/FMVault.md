# FMVault

The Password Vault: suite "fmv1" (byte-compatible with the browser's vault-crypto.js, the
desktop's VaultCrypto.cpp and the Android app), the /api/vault client, the app-level service,
and the view models of the vault screens. `import FMVault` (re-import `FMBase` for JSON).

## Wiring (app target)

```swift
let vault = VaultService(
    request: { method, path, body in
        // FMApi's accountApiCall (20 s timeout, bearer session)
        do { return try await api.accountApiCall(method, path, body) }
        catch let error as OrgApiException { throw VaultError(error.code) }
    },
    accountProvider: { auth.session?.nodeName ?? "" },
    store: keychainStore,                 // FMBase.SecureStore
    platform: VaultAutofillBridge(),      // VaultPlatform
    deviceAuth: LocalAuthDeviceAuth())    // VaultDeviceAuth
```
The request must throw `VaultError(<server "error" code>)` for an error answer (the copy table
keys on those codes, e.g. `revision_conflict`, `recipient_not_found`, `invalid_session`); any
other error shows as "Something went wrong. Check your connection and try again."
Session changes: account became "" -> `await vault.signedOut()`; another account ->
`try await vault.refresh()`. Scene phase: `.background` ->
`vault.appPaused()`, `.active` -> `vault.appResumed()` (locks after 60 s away).

## Protocols the app implements (VaultPlatform.swift, all `@MainActor`, class-bound)

- `VaultPlatform` (the `forkmesh/vault` channel): `autofillStatus() async -> VaultAutofillStatus`
  (`supported, enabled, deviceSecure, logins`), `openAutofillSettings()`,
  `syncAutofill(account:logins: [VaultAutofillLogin]) -> Bool` (replace what the credential
  provider may offer; `VaultAutofillLogin {id,title,username,password,host}`, `.dictionary` is
  the same fields as a dictionary), `clearAutofill()`, `takePendingSaves() -> [VaultPendingSave]` (`title, host,
  username, password, app`; returned once). Default `NoVaultPlatform()` (nothing supported).
- `VaultDeviceAuth`: `available() async -> Bool`, `authenticate(reason:) async -> Bool`
  (LAContext `.deviceOwnerAuthentication`; never throws, failure is false). Default
  `NoVaultDeviceAuth()` (quick unlock hidden).
- `VaultClipboard`: `copy(_ text, expiresAfter: TimeInterval?)`. With an expiry (30 s for
  passwords and usernames) use UIPasteboard `setItems(_, options: [.expirationDate: ...])`;
  `nil` (recovery code, export) never expires.
- NFKC is not a platform call any more: `VaultCrypto.nfkc` (Foundation) is the default normalizer.

## VaultService (`@Observable @MainActor`)

`phase: VaultPhase` (`.loading .signedOut .notSetUp .locked .unlocked`), `items`, `account`,
`find(id)`, `autoLockMinutes` (1/5/15, `VaultService.autoLockChoices`), `quickUnlockEnabled`,
`quickUnlockAvailable`, `autofillEnabled`, `autofillStatus`, `client` (the VaultClient).
Async throwing: `refresh`, `create(pw) -> recoveryCode`, `unlock(pw)`, `unlockWithDevice() ->
Bool`, `recover(code, newMasterPassword:) -> newCode`, `reload`, `save(fields, id:)`,
`remove(id)`, `previewShare(kind:name:)`, `share(id, kind:, name:, permission:, accepted:)`,
`unshare`, `organizations`, `myFingerprint`, `exportData`, `changePassword`,
`renewRecoveryCode`, `destroy`. Plain: `lock()`, `touch()` (restart the idle timer; call on any
touch), `appPaused/appResumed`, `setAutoLockMinutes`, `dispose()`. Async non-throwing:
`setQuickUnlock(Bool) -> Bool`, `setAutofill(Bool)`, `openAutofillSettings`,
`refreshAutofillStatus`, `signedOut`. Secure-store keys:
`vault/<account>/{autoLockMinutes,quickUnlock,autofill}` (quickUnlock holds the keyset JSON).

## View models (bind with `@Bindable`; user-facing copy kept verbatim)

- `VaultScreenModel(vault:clipboard:)`: `start()` on appear; `phase`, `showsUnlockedActions`
  (lock/settings/+ buttons), `busy` (progress bar), `error` / `notice` (banners); setup form
  `setupPassword, setupConfirm, setupProblem, submitSetup()`; unlock form `unlockPassword,
  submitUnlock(), quickUnlockOffered, quickUnlock()`; `recover(code:newPassword:)` after a
  `VaultRecoverForm` (`code, password, confirm, problem, submit() -> (code, password)?`);
  `recoveryPrompt: VaultRecoveryCodePrompt?` (present while set: `code, saved, canFinish,
  copy()`; close with `finishRecoveryPrompt()`, which refuses until `saved`); list `search`,
  `filter: VaultFilter` (`.all .mine .shared`, `.label`), `visibleItems`, `rows: [VaultRow]`
  (`title, subtitle, icon, readable`; unreadable rows get no copy button and no tap),
  `reload()`; item `actionsHeader(item)`, `actions(for:) -> [VaultItemActionEntry]` (`action:
  .username .password .open .edit .share .delete`, `label, icon, destructive`), `copyPassword`,
  `copyUsername`, `siteURL(item) -> URL?` (open with `openURL`), `draft(for: item?) ->
  VaultItemDraft`, `save(draft) -> Bool`, `deleteConfirmation(item) -> VaultConfirmation`,
  `delete(item)`, `touch()`, `lock()`.
- `VaultItemDraft`: `title, url, username, password, notes, reveal` (bindable), `readOnly`,
  `sheetTitle`, `sharedByLine`, `revealLabel`, `generate()`, `VaultItemDraft.maxLength(field)`.
- `VaultShareModel(vault:itemId:)`: `start()`; `item` (nil = locked: show
  `VaultShareModel.lockedTitle`), `screenTitle`; current shares via `item.shares` +
  `shareTitle/shareSubtitle/stopConfirmation/stop(share)`; `kind` ("user"/"organization",
  organization segment enabled when `organizationsAvailable`), `name`, `organization`,
  `organizations`, `permission` ("read"/"write"); changing any of them clears `preview`.
  `canCheck`, `check()`, `preview: [VaultRecipient]?` with `stateLabel(state)`,
  `isAlarming(state)`, `fingerprintShown(member)`, `advice`, `changed`, `confirmedChanged`
  (checkbox, shown when `changed` is non-empty), `canShare`, `share()`, `busy`, `message`.
- `VaultSettingsModel(vault:clipboard:)`: `start()` on appear, `appResumed()` on return to the
  foreground; `shouldClose` (pop when locked underneath); autofill `autofill`,
  `autofillToggleEnabled`, `autofillSubtitle`, `autofillState` (nil while off),
  `showsOpenAutofillSettings`, `setAutofill`, `openAutofillSettings`; locking `autoLockChoices`,
  `autoLockLabel(m)`, `setAutoLock(m)`, `showsQuickUnlock`, `setQuickUnlock`; `fingerprint`;
  `password, confirm, changePassword()`; `renewRecoveryCode()` -> `recoveryPrompt` +
  `finishRecoveryPrompt()`; `copyExport()`; `destroyConfirmation` then `destroy()`; `message`.
- `VaultCopy`: every other string (`VaultCopy.title`, `setupMessage`, ...), `errorText(error)`,
  `newPasswordProblem(pw, confirm)`. Icon names are Material names, for `FmIcon`.

## Moving passwords in and out (VaultTransfer.swift, VaultTransferModel.swift)

- `VaultTransferLogin` (`title, url, username, password, notes, otpauth`) is the neutral shape
  every route uses. `.fields` is what `VaultClient.save` takes: the title falls back to the host,
  then the username, then "Imported login"; an `otpauth://` code (the vault has no one-time-code
  field) is appended to the notes. `init(item:)` splits it out again, so a code survives
  ForkMesh -> Apple Passwords -> ForkMesh.
- `VaultTransfer.readCSV` reads Apple Passwords (`Title,URL,Username,Password,Notes,OTPAuth`),
  DuckDuckGo, Chrome, Firefox, Bitwarden and 1Password exports by matching header names (the
  only required column is a password one); `source(forHeader:)` names the app for the review.
  Rows with neither username nor password (secure notes, cards) are counted and left out.
  `writeCSV` writes Apple's header, which DuckDuckGo and Apple Passwords both import. No
  spreadsheet-formula escaping: a leading `=` or `-` is part of a password and must survive.
- `VaultTransfer.plan` skips a login whose host (www. folded), username (case-folded) and
  password all match a readable item or an earlier row; a changed password for the same account
  is imported beside the old item rather than overwriting it.
- `VaultService.importLogins` saves the plan's new logins one by one and stops at the first
  error (`VaultImportReport.remaining`, `stoppedBy`), then reloads once; `exportLogins` lists
  readable items. `receiveImport` / `takeIncomingImport` hold logins another app handed over
  (Credential Exchange) in memory until the vault is unlocked; sign-out and `dispose` drop them.
- `VaultTransferModel(vault:)`: `prepareFile(data)` / `prepare(source:logins:)` /
  `takeIncoming()` -> `review: VaultImportReview?` (`title, message, canImport`), then
  `confirmImport()` or `cancelImport()`; `exportCSV()` / `exportLogins()` (nil with a
  `message` when locked or empty), `exported(count, to:)`, `message`. Copy is on the type.

## ZIP and one-time codes

- `VaultZip.firstCSV(in:maximumBytes:)` pulls the first `.csv` out of a ZIP (stored or deflated,
  CRC-checked, `__MACOSX/` and dot files skipped) with the system zlib, which is why FMVault
  depends on `CZlib`. Safari's Export on iPhone writes such a ZIP; `readCSV(Data)` opens it.
- `VaultOTPAuth(uri:)` / `.uri` and `VaultBase32` convert `otpauth://totp/...` links to the
  raw secret, period, digits and algorithm Credential Exchange's TOTP credential carries.

## AutoFill links

`VaultAutofillLinks` maps a service host to login ids (newest first, 10 per host, 500 hosts).
When someone picks a login in the credential provider for an app or site whose host is not
the login's own, the extension links them, so `VaultAutofillCache.ranked(..., links:)` suggests
it there next time and `identities(_, links:)` gives iOS an extra QuickType identity for that
host. The app prunes links to the logins it syncs and clears them with the cache.

An app without a website arrives on iOS 26.2+ as an `.app` service identifier;
`VaultAutofillServices.encode` writes it as `app:<bundle id>` (case kept), which never matches a
login's own host but can be linked. `VaultAutofillServices` (iOS only) also builds the
`ASPasswordCredentialIdentity` list and the "For <site>" label for the extension and the app.

## Client and crypto (below the service; mostly for tests and other layers)

- `VaultClient(request:normalizer:)` (`@Observable @MainActor`): the vault client
  (`status, setUp, unlock, resume(savedKeyset), exportKeyset, changePassword,
  renewRecoveryCode, recover, load, matching(host), save, remove, previewShare, share, unshare,
  syncPending, organizations, myFingerprint, exportData, destroy, lock`). Crypto failures
  surface as `VaultError(code)`; errors of the request function pass through unchanged. `newMemoryKiB/newPasses` lower the stretch for tests.
- Models: `VaultItem` (`fields: JSONObject`, `title/username/password/url/notes/host`,
  `readable`, `canEdit`), `VaultShare`, `VaultRecipient` (`state`: new, pinned, changed,
  no_vault, invalid_keys), `VaultShareOutcome`, `VaultFields.clean/limits`.
- `VaultCrypto` (synchronous): `stretch` (Argon2id; seconds, never on the main actor),
  `wrapKeyset/unwrapKeyset`, `newRecoveryCode/recoverySecret`, `sealItem/openItem`,
  `wrapItemKey/unwrapItemKey`, `sealPrivate/openPrivate`, `sealGrant/openGrant` (X25519 +
  ML-KEM-768, X-Wing combiner, Ed25519-signed), `generatePassword`, `loginHost`, `loginHref`,
  `matchesHost`, `toBase64/fromBase64` (unpadded base64url), `toHex`. Types `VaultKdf` (floor
  19456 KiB / 2 passes; `init(json:)` refuses weaker), `VaultKeyset` (class; `wipe()`),
  `VaultBundle` (`of`, `verify`, `fingerprint()`), `VaultCryptoError(code)`.

## Surprises

- No crypto was added: every primitive is FMCrypto's. The tests carry the three committed
  cross-client fixtures and OpenSSL's Argon2id / ML-KEM-768 answers, and also open whatever
  `marketplace/packages/vault/tests/fixtures/interop-from-*.json` holds. `VAULT_WRITE_INTEROP=1
  swift test --filter VaultInteropTests` writes `interop-from-swift.json` there.
- The flow tests drive the real Worker module through `vault_stdio_server.py`; they skip without
  python3.
- Blocking work (Argon2id, opening a listing) runs on detached tasks over a copy of the keys, so
  `lock()` may be called at any time; a load that finishes after a lock throws `vault_locked`.
- Base64 with non-zero trailing bits is `invalid_encoding`;
  a broken quick-unlock copy counts as `invalid_keyset` and is dropped.
- The AutoFill cache (sealed blob, its key, the links) lives in the keychain group
  `<team>.com.forkmesh.forkmesh` the app already shares with its extensions, not in the App
  Group: the App Group needs a portal step the release pipeline cannot do, the keychain group
  does not. The key item needs user presence; the blob and links items do not (the blob is
  AES-GCM sealed under that key).
