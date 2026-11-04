# ForkMesh apps

ForkMesh runs on the web, the desktop, iPhone and iPad, Apple Watch and
Android. Every app signs in to the same account and instance, so a change
made on one shows up on the others. Downloads are at
[forkmesh.com/#downloads](https://forkmesh.com/#downloads); the platforms
and their release status are in the
[installation support matrix](install-support-matrix.md).

| App | Get it | What it is for |
| --- | --- | --- |
| Web | [app.forkmesh.com](https://app.forkmesh.com) or your own instance | Repositories, pull requests, issues, chat, the dashboard and the admin console |
| Desktop (Linux, Windows, macOS) | The install script in [Getting started](getting-started.md), or the installers on the downloads page | The full node: mirrors repositories, runs coding agents in their own worktrees, runs Actions, hosts and agents |
| iPhone and iPad | [App Store](https://apps.apple.com/app/forkmesh/id6803060735) | A mobile node: browse, edit and commit, run and steer agents, chat and calls, review diffs, Siri, widgets |
| Apple Watch | Installs with the iPhone app | Follow, steer and hear your agents from your wrist |
| Android | Signed APK, `forkmesh-android-universal.apk`, from the downloads page | The iPhone app's features and look ([Android guide](mobile/android.md)) |
| Chrome and Chromium browsers | The **Browser extension** page of the dashboard | Fill Password Vault logins, link mail to the CRM, update Marketing channels and mirror a repository in one click ([browser extension](browser-extension.md)) |

## Desktop

The desktop app is a ForkMesh node. It creates its own Ed25519 identity on
first run, mirrors repositories, runs agents in isolated worktrees with live
transcripts and diffs, and turns their work into pull requests. See
[Getting started](getting-started.md) and the
[desktop app guide](qt-client.md).

## iPhone and iPad

The phone is a node too: it holds its own key and has its own Git engine, so
it can clone and commit without the desktop. You can start, steer, stop and
merge agents running on your desktop or on cloud runners, review their diffs
file by file, chat (direct messages are end-to-end encrypted) and make calls,
and use the Market's phone tools. Start with
[getting started on a phone](mobile/getting-started.md); every phone guide is
on the [phone apps](mobile/README.md) page, including
[privacy and security](mobile/privacy-and-security.md),
[Siri](mobile/siri.md), [Apple Health](mobile/apple-health.md),
[editing files](mobile/mobile-file-editing.md) and
[agent access](mobile/mobile-agent-access.md).

## Apple Watch

The watch app works through your iPhone, which holds your session and keys:
see the agent list and a live view, Stop, Continue and Retry, send a prompt by
dictation, review diffs, hear each agent as sound and haptics, and start an
agent with Siri. Everything it does is on the [Apple Watch](mobile/apple-watch.md)
page.

## Android

The Android app is built from the same design as the iPhone app (the same
Liquid Glass theme) and talks to the same endpoints, so the server cannot
tell them apart. Since 2026-10-28 every phone change ships in both apps in the
same pull request; the few differences kept on purpose, such as a background
connection in place of push, are on the [Android](mobile/android.md) page. It
is distributed as a signed APK from the downloads page.

## Browser extension

The ForkMesh extension for Chrome serves the Password Vault, the CRM and
Marketing from one sign-in, and mirrors a GitHub, GitLab, Codeberg or Gitea
repository to your account from its page. Install it from the dashboard's
**Browser extension** page; see the [browser extension](browser-extension.md)
guide.

Developer documentation for the native apps is in
[docs/native/README.md](native/README.md).
