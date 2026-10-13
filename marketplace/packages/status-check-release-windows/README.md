# Status: Windows Release

A check for the [Status Page](../status-page/README.md) package: the latest Windows desktop version is published and downloadable.

It shows the `release_windows` row of `/api/status` as
green, amber or red dots on the desktop debug bar, under the version on iPhone
and Android, and on the web's Status monitors page.

It depends on Status Page. Installing it installs Status Page too; removing
Status Page removes this check from every device on your account. Removing
only this check hides its dots everywhere and leaves the others. The package
carries no code: it declares its rows in a `status.monitor` contribution and
the hosts filter their dots by it. See `docs/marketplace/status-page.md`.
