# Dashboard update notice

Part of [the App docs](README.md). An open dashboard tab keeps running the
JavaScript it loaded, so the dashboard shows an inline notice when a new
version is deployed, like the World
([deploy notice](../deploy/auto-deploy.md#world-deploy-notice)).

`app/public/dashboard/js/09-update-notice.js` reads `rev` (the deploy's
`BUILD_REV`) from `GET /api/version`. The first answer after boot is the
baseline; a later different `rev` shows the notice. It checks on focus or
visibility (at most once a minute), every 5 minutes while visible, and
immediately when the account event socket reconnects.

The notice (`data-update-notice`) is an in-flow strip at the top of the main
column. **Update** reloads (static scripts are content-addressed with
`?v=<hash>`); **×** hides it for that `rev` for the session
(`sessionStorage` `forkmesh.dashboard.updateDismissed`). Once shown it stays,
even if a restarting node still answers with the old `rev`.
