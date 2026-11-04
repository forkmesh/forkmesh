# Report a bug

Bugs: report a bug in ForkMesh, follow what happened to your reports, and,
for platform administrators, triage what everyone reported. Reports and
attachments stay in ForkMesh's encrypted admin-only support inbox; they are
never filed as repository issues.

Surfaces: desktop, web, iPhone, Android.

- **Desktop.** A **Bugs** page in the left rail and on Home
  (`content/desktop`, module `bug-reports.so`): a Report form (message, how
  bad it is, optional contact), My reports, and Reported for administrators.
  The rail badge counts new reports for administrators. Test:
  `forkmesh-bug-reports-tests`.
- **Web.** The dashboard page `/dashboard/bugs` (`content/web`) in the
  sidebar, with the same three tabs.
- **Phones.** The Bug button and the Report a bug sheet (screenshots, videos
  and files) from Home or the menu.

Platform administrators (`is_admin`) also see a **Reported** tab in the Bug
sheet on iPhone and Android. It lists the reports everyone has sent, newest
first, filtered by status (New, Triaged, Closed, All), with the message,
reply contact, account, platform, build, screen and plugin. Each report has
New / Triaged / Closed buttons to triage it, and a button per attachment that
downloads the file and opens it (Quick Look on iPhone, the phone's viewer app
on Android). A report triaged out of the filtered status leaves the list.

Administrators get the same **Reported** tab on the desktop Bugs page and the
web page. All of it goes through `/api/bug-reports/inbox`, which answers only
a bearer session whose account is an administrator (403 otherwise), so on the
web it shows only where the page has a bearer session; the admin console's
Bug reports section covers cookie sign-ins:

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

Everyone who is signed in sees a **My reports** tab on the desktop Bugs page,
the web page and the Bug sheet on iPhone and Android. It lists only the reports they sent while signed
in, newest first, each with its status: Received (new), In progress
(triaged), Closed, or "Fixed, confirmed by you". A **Confirm it's fixed**
button on each report not yet confirmed marks it closed and records when the
reporter confirmed it; administrators see that time as "Reporter confirmed
fixed" in the console. Reports sent signed out cannot be tied to an account,
so they never appear there.

It goes through `/api/bug-reports/mine`, which answers a signed-in session,
bearer or the web's account cookie (401 otherwise; a cookie POST also needs
an allowed origin, like every cookie write), and only ever reads rows whose
account is the caller's:

- `GET /api/bug-reports/mine?offset=` lists the caller's reports and the count
  per status. A report carries its id, status, severity, platform, build,
  time, message, screen, plugin, attachment names and sizes, and
  `confirmedFixedAt` (0 until confirmed). The account and reply contact are
  left out, and attachments cannot be downloaded from here.
- `POST /api/bug-reports/mine` with `{"report": id, "fixed": true}` sets the
  report to closed and stores `confirmedFixedAt` inside the encrypted row, so
  no schema change was needed. It works from any status; a report that is not
  the caller's answers 404.

The install state follows your ForkMesh account: removing it on one device
removes it from every device, and installing it brings it back everywhere.

It is off in projects and accounts created from 2026-10-11; older ones keep it
installed until it is removed (`install.installedBefore`).

It became a plugin (with `content/`) in 1.2.0, when the desktop and web pages
were added; the phone sheets are still built into each app and gated by the
package. See `docs/marketplace/core-plugins.md`.
