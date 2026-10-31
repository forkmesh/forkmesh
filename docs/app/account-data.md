# Your data: saved phone settings, the data inventory and the export

Settings > Data on the iPhone does three things for the signed-in account:

1. **Save to ForkMesh**: the home screen and bar layout, the news reader
   subscriptions and the app settings are saved to the account, encrypted, so
   a new phone or a reinstall can restore them.
2. **Everything ForkMesh has about you**: every row in the database tied to
   the account, its linked nodes, its email, the repositories it owns and each
   organization it belongs to, browsable row by row, with per-item delete
   where removing one row on its own is safe.
3. **Download your data**: the same inventory as one JSON file
   (`GET /api/accounts/export`), which the phone already asked for before the
   server had the route.

| Piece | Code |
| --- | --- |
| Server module (routes, store, inventory, export) | `app/src/account_data.py` |
| Runtime adapter | `_AccountDataRuntime`, `account_data_handler` in `app/src/entry.py` |
| Routing | `app/src/app_runtime.py`, before the `/api/accounts/` catch-all |
| Tables | `account_store`, `account_store_keys` (module DDL `STORE_SCHEMA`, migration `0287_account_store.sql`) |
| Account deletion | `_delete_account_namespace` deletes both store tables |
| Tests | `app/tests/test_account_data.py` |
| iPhone core | `FMAccount/AccountSync.swift`, `FMAccount/Identity/AccountDataInventory.swift`, `FMPackages/RSSReaderStore.swift` (`syncLibrary`, `restoreLibrary`) |
| iPhone screens | `ForkMesh/Features/Account/Data/AccountDataScreen.swift`, `AccountDataBrowserViews.swift`, `AccountSyncSetup.swift` |
| iPhone tests | `FMAccountTests/AccountDataInventoryTests.swift`, `FMPackagesTests/RSSReaderStoreTests.swift` |

## API

Every route needs a signed-in session (bearer token or same-origin cookie)
and only ever reads or changes the caller's own rows.

| Route | Does |
| --- | --- |
| `GET /api/accounts/data[?refresh=1]` | Inventory: `subjects`, `groups` (one per table and subject with a row count), `store` (saved items, no values) |
| `GET /api/accounts/data/rows?subject=&table=&after=` | Up to 50 rendered rows of one group; `next` is the cursor (rowid, or offset for a `WITHOUT ROWID` table) |
| `DELETE /api/accounts/data/rows/<table>/<rowid>` | Removes one row when the table is in `DELETABLE` and the row's owning column is the caller |
| `GET /api/accounts/data/store` | Saved items with metadata |
| `GET`, `PUT`, `DELETE /api/accounts/data/store/<key>` | One saved item; `PUT {kind, label?, device?, value, baseRevision?}` |
| `DELETE /api/accounts/data/store` | Every saved item and the account's store key |
| `GET /api/accounts/export` | The full export (`forkmesh-account-export-v1`), at most once per 30 seconds per account |

Keys match `^[a-z0-9][a-z0-9._-]{0,79}$`; kinds are `layout`,
`subscriptions`, `settings` and `app`. One value is at most 256 KiB of JSON,
an account keeps at most 200 items and 4 MiB in total. A `PUT` with a stale
`baseRevision` gets `409 revision_conflict` and the current revision.

## How the saved items are encrypted

No secret is shared by all of an account's devices (the identity seed and the
owner keys are generated per device), so the phone cannot encrypt for its
sibling devices without the vault's master password. The store therefore uses
the same per-account key scheme as account-scoped packages:

- Each account gets 256 bits of random key material in
  `account_store_keys.wrapped`, itself sealed with `DATA_KEY`.
- Each row's `data` is AES-GCM sealed with that material (`seal_with`), so a
  database dump without the App's secret opens nothing, and one account's key
  opens no other account's rows.
- The item key and kind stay plaintext columns so the inventory can list them;
  the label, device and value are inside the sealed blob.

This is encryption at rest, not end-to-end: the App can open the rows, which is
what lets the inventory and the export show them. End-to-end sealing would
need the vault keyset (`VaultCrypto.sealPrivate`) and would leave a phone
without an unlocked vault unable to restore.

## How the inventory finds everything

The inventory does not keep a list of tables. On each request (memoized for a
minute per account, the table catalogue for five minutes) it reads
`sqlite_master` and `pragma_table_info` and scans every table that has a
`*_bi` column or one of the plaintext name columns in `PLAIN_COLUMNS`. A new
table keyed by account therefore shows up without any change here, and
`test_every_table_with_a_hashed_identity_column_is_scanned` checks the whole
runtime schema.

Rows are matched against three kinds of subject, in this order, and each row
is counted once:

- **account**: `blind_index(name)`, every linked node (`nodes.user_bi` and the
  record's `nodes`), `blind_index(email)`, and the plaintext names.
- **repos**: the `key_bi` of every repository the account or its nodes own
  (`repositories`, `hosted_repos`), which equals `blind_index(owner/repo)`, the
  `repo_bi` used everywhere else.
- **org:<name>**: each organization from `org_members`, its `org_bi` plus the
  `repo_bi` of its linked repositories (`org_repos`).

Blind indexes are keyed HMACs of different inputs (organizations under an
`org:` prefix), so matching any `*_bi` column by equality cannot pick up an
unrelated row. Values are passed as one JSON array per column
(`IN (SELECT value FROM json_each(?))`) so a long repository list does not
multiply the statement, and a scan stops at 5000 rows per table and reports
`truncated`.

Up to 40 tables (and 400 bound values) go into one `UNION ALL` statement that
returns each matching row's identity columns as a `json_array`, plus one
sentinel row. On the cores a local read that finds nothing is re-asked of the
rqlite leader (`_confirm_miss` in `d1_rqlite.py`, with its own budget); one
statement per table would have spent roughly 400 of those per inventory. With
the batches an inventory is about ten statements, none of them empty. A batch
that fails (a table dropped since the catalogue was read) is scanned again one
table at a time.

## What is shown, withheld and removable

- `*_bi` values are shown as "(hashed identifier)".
- Plaintext columns whose names look like credentials (`token`, `secret`,
  `digest`, `_hash`, `wrapped`, `seed`, …) are withheld. Sealed `data`
  columns are opened with `DATA_KEY` and secret-looking keys inside are
  withheld (`pass_hash`, `totp_secret`, `sessionToken`, `key`, …). A value
  sealed with a package's own key cannot be opened and is shown as encrypted.
- `WITHHELD_TABLES` and every `*_keys` table except `account_ssh_keys` and
  `owner_encryption_keys` (public keys only) are counted but never shown or
  exported: action secrets, workflow credentials, bot and app tokens, push
  tokens, chat webhooks, sign-up referral and verification codes, wallet
  intents, moderation and audit records.
- Organization rows are opened only for the organization's owners and admins;
  members see the counts. A row that matched the account only through a
  creator column (`created_by_bi`, `added_by_bi`, `installed_by_bi`,
  `invited_by_bi`) in an organization the account does not manage keeps its
  sealed data closed.
- A row that matched only because the account acted on someone else's record
  (`TOUCH_COLUMNS`: `approved_by_bi`, `reviewed_by_bi`, `last_used_by_bi`,
  `updated_by_bi`, …) keeps its sealed data closed for everyone: an admin who
  last used a sign-up code, or reviewed a report, does not get the record.
- `STAFF_NOTE_TABLES` (`moderation_blocks`, `repo_terms_flags`) keep their
  rows and plain columns (scope, category, whether content is hidden), but
  their sealed `data` is shown as a fixed placeholder: it holds the moderator
  or administrator who acted and their private reason or note, which the
  blocked or flagged account must not read
  (`test_moderator_notes_about_you_stay_with_the_moderators`).
- `DELETABLE` lists the tables where one row can go on its own without
  breaking anything else (notifications, stars, follows, reactions, read
  marks, activity days, presence, lobby links, package use, sign-in attempts).
  Everything else is removed by its own feature (devices, sessions, SSH keys,
  leaving an organization) or by deleting the account.

## The phone side

`AccountSyncService` (FMAccount) owns three sources, registered by
`AccountSyncSetup.install()` from `AccountInstaller`:

| Key | Kind | Source |
| --- | --- | --- |
| `ios.home` | layout | `shell/*` and the Insights layout preferences |
| `ios.rss` | subscriptions | the RSS reader library without articles, activity or fetch state |
| `ios.settings` | settings | every other preference `DataFolder.exportSettings` sees |

Preferences under `AccountSyncKeys.skippedPrefixes` never leave the phone:
project-scoped keys (`p/`), sign-in, identity and chat keys, server and relay
addresses, caches, read state and recents. A single value over 32 KiB is
skipped too.

Saving is off until the person switches on "Save my settings to ForkMesh".
While it is on, the service saves when the app goes to the background and
when it comes back, but only sources whose SHA-256 differs from the last copy
saved for that account (`accountSync/hash/<account>/<key>`). Restore is
explicit and asks first; it replaces the layout, subscriptions (local feeds
keep their articles and fetch state when the address matches) and settings.
Android has the same screens (`feature/account/settings/AccountDataScreens.kt`).
