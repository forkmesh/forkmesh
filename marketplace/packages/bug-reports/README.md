# Report a bug

The Bug button: report a bug in ForkMesh with screenshots, videos or other files, from Home or the menu. Reports and attachments stay in ForkMesh's encrypted admin-only support inbox; they are never filed as repository issues.

Surfaces: desktop, iPhone, Android.

Platform administrators (`is_admin`) also see a **Reported** tab in the Bug
sheet on iPhone and Android. It lists the reports everyone has sent, newest
first, filtered by status (New, Triaged, Closed, All), with the message,
reply contact, account, platform, build, screen, plugin and attachment names.
The list comes from `GET /api/bug-reports/inbox`, which answers only a bearer
session whose account is an administrator (403 otherwise). Triage (changing
a report's status) and attachment downloads stay in the admin console.

The install state follows your ForkMesh account: removing it on one device
removes it from every device, and installing it brings it back everywhere.

It is off in projects and accounts created from 2026-10-11; older ones keep it
installed until it is removed (`install.installedBefore`).

The feature itself is built into each app; the package is its on/off
switch. See `docs/marketplace/core-plugins.md`.
