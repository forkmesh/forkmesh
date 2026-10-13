# Status: Status Page API

A check for the [Status Page](../status-page/README.md) package: the Status page, its stylesheet and the /api/status snapshot are served, plus this device's own load of the Status page.

It shows the `status_page`, `desktop_status_page` rows of `/api/status` as
green, amber or red dots on the desktop debug bar, under the version on iPhone
and Android, and on the web's Status monitors page.

It depends on Status Page. Installing it installs Status Page too; removing
Status Page removes this check from every device on your account. Removing
only this check hides its dots everywhere and leaves the others. The package
carries no code: it declares its rows in a `status.monitor` contribution and
the hosts filter their dots by it. See `docs/marketplace/status-page.md`.
