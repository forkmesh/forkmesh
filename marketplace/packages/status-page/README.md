# Status Page

ForkMesh's live status at a glance. Free, bundled and installed by default.
It follows your account: removing it from the Market on any device removes it
from the desktop, the phone and the web together, and installing it again
brings it back on all of them.

- Desktop: one dot per system in the debug bar, plus this desktop's own probes
  of the website and the Status page. Clicking a dot rechecks it and opens the
  Status page.
- iPhone, iPad and Android: a row of dots under the version number at the foot
  of the shell. Tapping them opens the Status page.
- Web: the **Status** page in the dashboard's Plugins column
  (`/dashboard/status-monitors`), with the dots, the checks, the settings and
  your monitors.

Each check is its own package (`status-check-*`, for example Status: Website,
Status: Database, Status: Core Nodes) that depends on this one. Remove a check
to hide its dots everywhere; installing a check while Status Page is removed
installs Status Page too.

Settings: **Show only systems that need attention** hides green dots;
**Recheck healthy systems every** 2, 5, 15 or 30 minutes.

Your monitors: add up to 20 of your own addresses (a name and an http or https
URL). Each device loads them itself and shows a green dot when they answer
below HTTP 400, red otherwise. The browser can only tell whether the address
answered at all. Your monitors are stored encrypted on your account and are
deleted when you remove Status Page.

See `docs/marketplace/status-page.md`.
