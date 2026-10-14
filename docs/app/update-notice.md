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
column, built from the shared flash component
([dashboard components](dashboard-components.md)): a full-width neutral
`fm-flash fm-flash-neutral fm-flash-full` with the one blue sparkle icon
(`fm-flash-icon-accent`), a bold title and a muted line, a small green
`fm-btn fm-btn-sm fm-btn-cta` **Update** and an invisible icon button to close.
It is neutral rather than blue because a new deploy is news, not a problem; the
blue icon is the only colour so the strip reads as a hint, like GitHub's own
update banner. **Update** reloads (static scripts are content-addressed with
`?v=<hash>`); **×** hides it for that `rev` for the session
(`sessionStorage` `forkmesh.dashboard.updateDismissed`). Once shown it stays,
even if a restarting node still answers with the old `rev`.
