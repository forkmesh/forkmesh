# App Store Status

App Store Status shows your iOS app as App Store Connect sees it, laid out
the same way on the web dashboard (`/dashboard/app-store-status`), the
desktop rail page (**App Store**), the iPhone app and the Android app.

## What it shows

- **Needs action.** One red banner when Apple wants something from you: a
  rejected version, an App Review submission with unresolved issues, or a
  TestFlight build that failed beta review.
- **App Store versions** with their build and review state (waiting for
  review, in review, rejected, ready for distribution).
- **App Review submissions** and the items in each.
- **Downloads**: downloads, redownloads and updates since the first version,
  and for the last 7 and 30 days, from App Store Connect Sales reports.
- **TestFlight builds**: processing, internal and external testing, beta
  review, and which groups get each build.
- **TestFlight groups**: internal or external, testers, every or chosen
  builds, the public link, and whether feedback is on.
- **Tester feedback** (screenshots and crashes) and **customer reviews**.
  The screenshots testers attached show as thumbnails; tap or click one to
  see it full size.

## How it stays up to date

Your App Store Connect API key stays on your desktop. ForkMesh's servers
never see it and never call Apple. When the **iOS App Store status** shortcut
runs, from the desktop's Shortcuts page or from your phone, the desktop
gathers the report, prints it, and publishes a snapshot to your account.
Every client then shows that snapshot and when it was published. Run the
shortcut again to refresh it.

The snapshot is private to your account and encrypted at rest. Uninstalling
the package, or deleting your account, removes it. The desktop uploads the
screenshots testers attached with the snapshot, and they are kept and removed
the same way.

Developer notes: [docs/marketplace/app-store-status.md](../../../docs/marketplace/app-store-status.md).
