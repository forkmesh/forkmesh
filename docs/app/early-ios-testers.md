# Early iOS testers

An administrator marks an account as an early iOS tester in the admin
console, and every ad hoc iPhone build from the office Mac is emailed to all
of them ([MAC_BUILD.md](../native/MAC_BUILD.md), "Publisher"). Until
2026-09-29 the publisher sent each build to one fixed address.

## The flag

- `users.early_ios_tester`, `0` or `1` (migration 0224). It is a plaintext
  column beside `is_admin`, not a key in the encrypted `data` blob, so the
  tester list is one indexed read (`idx_users_early_ios_tester`, partial on
  `early_ios_tester=1`) instead of decrypting every account.
- A column on the `users` row goes away with the account. A separate table
  keyed by `user_bi` would outlive a deleted account and pass the flag to
  whoever registers the same name next.
- The column is added before `SCHEMA_STATEMENTS` runs
  (`SCHEMA_PRE_CREATE_ALTER_STATEMENTS` in `entry.py`), because the index
  refers to it.
- Quarantine copies (`quarantined_users`) do not carry the flag. A
  quarantined signup is not a tester, and one that is released is flagged
  again by hand.

## Admin console

- Users → an account: the "Early iOS tester" switch sits next to
  "Administrator". Saving writes the column and names `early_ios_tester` in
  the banner and the sensitive audit record (`earlyIosTesterGranted` /
  `earlyIosTesterRevoked`), as `is_admin` does.
- The Users list has an "iOS tester" column, sortable, showing an
  "early iOS" pill.

## Who gets the email

`fmnext-app-ops early-ios-testers all` (`forkmesh_host.ops`) runs inside the
unsealed app on a core, because only the app holds the key that opens the
account records. It prints `{"ok": true, "testers": [{"account", "email"}],
"skipped": [{"account", "reason"}]}`. The trailing `all` is there because the
vault socket takes `ops <action> <account>`, three words.

A flagged account is only listed when it is active and its email is
verified. Otherwise it appears under `skipped` with `not_active`, `no_email`
or `email_not_verified`, and the publisher prints that line, so an
unconfirmed address never gets a build.

## Publisher

`tools/mac_adhoc_publish.py --execute` asks the first core for the list over
the fleet SSH key before it sends:

- Each tester gets a separate email. One message with every address in `To`
  would show all the testers each other's addresses.
- Addresses are lowercased and duplicates dropped.
- With nobody flagged (or nobody verified) the build still goes to
  `jett@forkmesh.com`, so a build is never silently dropped.
- `--to ADDRESS` (repeatable) skips the lookup and sends to those addresses
  only.
- If the lookup fails (core sealed, SSH down), nothing is sent and the next
  timer tick tries again.
- `published.json` records each address as it is sent, with
  `"complete": false` until the last one. If a `sendmail` fails partway, the
  next tick sends only to the addresses still missing, so nobody gets the
  same build twice. Entries written before 2026-09-29 have no `complete` key
  and count as done.

## The ping

Asked for on 2026-10-07: a ready build should also raise a ping with a link
to download it, not only an email. After the last email,
`tools/mac_adhoc_publish.py` runs
`fmnext-app-ops ios-build-ping <manifest-sha256>:<version>:<build>:<commit>`
on the same core. The argument is one word because the vault socket takes
`ops <action> <account>`. The publisher keeps only `0-9A-Za-z.-` in the
version and build, and a commit that is not hex is left empty, so the core
refuses nothing it built itself (`bad_spec` otherwise).

- Every flagged, active account gets one ping. Unlike the email it does not
  need a verified address, because the ping goes to the account's own inbox.
- Kind `release_published`, so it uses the Releases ping setting and the
  rocket icon. A new kind would need an icon and a label in every client (see
  the pings vocabulary). The ping names no repository, so the iOS ping screen
  opens the absolute `href` (`NotificationTapTarget.web`) instead of a
  repository's Releases tab.
- `href` is
  `https://forkmesh.com/ios-install.html?build=<build>&v=<first 12 of the manifest-sha256>#<manifest-sha256>`
  (the query busts caches and gives the page its build number), the
  same page the email's button opens, and the body repeats it. The desktop
  only follows a relative `href`, so on the desktop the link is the text in
  the body.
- The dedupe key is `ios-build:<manifest-sha256>`. Running the command again
  for the same build updates the one row.
- An account that switched Releases pings off reports `not_filed`, and a
  suspended one reports `not_active`. The publisher prints both.
- A failed ping (core sealed, SSH down) is printed and does not hold the
  build back. The emails have already gone out, so the entry is still marked
  complete and the ping is not retried. `published.json` records who was
  pinged under `pinged`.
- `--to` is for sending to particular people, so it sends no ping.
