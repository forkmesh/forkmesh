# Sentry preview

Sentry contributes a **Sentry** tab to the desktop's Log page and a **SENTRY**
lane to the log's activity lights, filter chips, and badge column. Once a
Sentry organization is connected with an API key, the package reads that
organization's unresolved issues once a minute: a new issue, or an issue whose
event count grew, is written to the log as a `Sentry:` line and lights the
Sentry lane. The tab aggregates the same read — issues, events, and affected
users in total, by project, and by level — with every issue linked to Sentry.

The package requests only its declared capabilities: the read-only connection
it makes as the token you provide, the Log page contribution, the device-local
token and snapshot it keeps until the package is removed, and opening an issue
in the browser. Removing the package stops the reads and clears the token,
connection details, and cached issues.

`market.png` is the required Market listing screenshot. It shows the Sentry
tab the installed contribution adds, with the aggregated issue list and the
lit Sentry lane; the live read remains the source of truth for behavior, and
the image is a bounded preview rather than executable package content.
