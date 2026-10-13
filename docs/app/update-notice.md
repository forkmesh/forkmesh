# Dashboard update notice

Part of [the App docs](README.md). An open dashboard tab keeps running the
JavaScript it loaded, while `main` deploys several times a day. Since
2026-10-07 the dashboard tells you when a new version is live and updates in
one click, as the World already did ([deploy notice](../deploy/auto-deploy.md#world-deploy-notice)).

## How it works

`app/public/dashboard/js/09-update-notice.js` (core bundle, not a lazy chunk)
asks `GET /api/version` for `rev`, the `BUILD_REV` the deploy stamps. The
first answer after the page boots is the running build; any later answer with
a different `rev` shows the notice.

| Trigger | Check |
| --- | --- |
| Boot | Records the running `rev` |
| Window focus, tab becomes visible | At most once a minute |
| Every 5 minutes | Only while the tab is visible |
| The account event socket reconnects | At once: a deploy restarts the App, which drops the socket |

The notice (`data-update-notice` in `partials/main.html`, `.fm-update-notice`
in `shell.html`) is a strip at the top of the main column, in normal layout
flow: it pushes the page down instead of floating over it. Until 2026-10-09 it
was a card fixed under the header in `partials/modals.html`; the user asked
for no toasts, with information shown inline, so it now sits in the page.

- **Update** reloads the page. The dashboard's static scripts are content
  addressed (`?v=<hash>`), so the reload fetches the new build; the URL keeps
  the page and repository.
- **×** hides the notice for that `rev` for the rest of the browser session
  (`sessionStorage` `forkmesh.dashboard.updateDismissed`). A newer deploy
  shows it again.

## Traps

- The App and the dashboard's static files come from one release on the core
  nodes, so the App's `rev` stands for the dashboard build. A page served from
  somewhere without `/api/version` never shows the notice.
- The baseline is the first answer after boot, not a value stamped into the
  HTML. A deploy that lands in the second between the HTML and that answer is
  missed until the next one.
- Once shown, the notice stays until Update or ×, even if a later answer
  returns the old `rev` (a node still restarting during a rolling deploy).
