# Report a bug

The Bug button: report a bug in ForkMesh with screenshots, videos or other files, from Home or the menu. Reports and attachments stay in ForkMesh's encrypted admin-only support inbox; they are never filed as repository issues.

Surfaces: desktop, iPhone, Android.

Platform administrators (`is_admin`) also see a **Reported** tab in the Bug
sheet on iPhone and Android. It lists the reports everyone has sent, newest
first, filtered by status (New, Triaged, Closed, All), with the message,
reply contact, account, platform, build, screen and plugin. Each report has
New / Triaged / Closed buttons to triage it, and a button per attachment that
downloads the file and opens it (Quick Look on iPhone, the phone's viewer app
on Android). A report triaged out of the filtered status leaves the list.

All of it goes through `/api/bug-reports/inbox`, which answers only a bearer
session whose account is an administrator (403 otherwise):

- `GET /api/bug-reports/inbox?status=&offset=` lists reports and the count
  per status.
- `POST /api/bug-reports/inbox` with `{"report": id, "status": "triaged"}`
  sets a status and is written to the audit log as
  `admin.app_set_bug_status`, like the console's change.
- `GET /api/bug-reports/inbox/attachment?report=&attachment=` returns the
  decrypted file with `no-store`, `nosniff`, `content-security-policy:
  sandbox` and an attachment disposition.

Downloaded attachments are kept only in the app's cache folder
(`bug-report-attachments/`). The admin console's Bug reports section keeps
working the same way.

The install state follows your ForkMesh account: removing it on one device
removes it from every device, and installing it brings it back everywhere.

It is off in projects and accounts created from 2026-10-11; older ones keep it
installed until it is removed (`install.installedBefore`).

The feature itself is built into each app; the package is its on/off
switch. See `docs/marketplace/core-plugins.md`.
