# Early iOS testers

An administrator marks accounts as early iOS testers; every ad hoc iPhone
build from the build Mac is emailed and pinged to them
([MAC_BUILD.md](../native/MAC_BUILD.md), "Publisher").

## The flag

- `users.early_ios_tester`, `0` or `1`: a plaintext column (with the partial
  index `idx_users_early_ios_tester`) so the list is one indexed read. Being
  on the `users` row, it disappears with the account and cannot pass to a
  later account with the same name.
- The column is added in `SCHEMA_PRE_CREATE_ALTER_STATEMENTS`, before the
  index that refers to it.
- Quarantined copies do not carry the flag.
- Admin console: an "Early iOS tester" switch on the user page (audited as
  `earlyIosTesterGranted` / `earlyIosTesterRevoked`) and a sortable column in
  the Users list.

## Recipient list

`fmnext-app-ops early-ios-testers all` (`forkmesh_host.ops`) runs inside the
unsealed app, which alone can open account records. It prints
`{"ok": true, "testers": [{"account", "email"}], "skipped": [{"account", "reason"}]}`.
Only active accounts with a verified email are listed; others are skipped
with `not_active`, `no_email` or `email_not_verified`.

## Publisher

`tools/mac_adhoc_publish.py --execute` fetches the list over the fleet SSH
key, then:

- sends each tester a separate email (no shared `To` list), lowercased and
  deduplicated;
- falls back to a configured operator address when nobody qualifies;
- `--to ADDRESS` (repeatable) skips the lookup and sends no ping;
- sends nothing if the lookup fails, and retries on the next tick;
- records each address in `published.json` with `"complete": false` until
  the last one, so a partial failure resumes without duplicates.

## Ping

After the emails, `fmnext-app-ops ios-build-ping <manifest-sha256>:<version>:<build>:<commit>`
files one ping per flagged active account: kind `release_published`, `href`
`https://forkmesh.com/ios-install.html?build=<build>&v=<first 12 of the manifest-sha256>#<manifest-sha256>`,
dedupe key `ios-build:<manifest-sha256>`. Accounts with Releases pings off
report `not_filed`. A failed ping is printed and not retried.
