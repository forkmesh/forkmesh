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
| Android | Signed APK, `forkmesh-android-universal.apk`, from the downloads page | The iPhone app's features and look; a few newer chat and call changes are still being ported |

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
and use the Market's phone tools. Guides:
[Siri](mobile/siri.md), [Apple Health](mobile/apple-health.md),
[editing files](mobile/mobile-file-editing.md),
[agent access](mobile/mobile-agent-access.md).

## Apple Watch

The watch app is new and still growing. Today it works through your iPhone,
which holds your session and keys:

- **Agent list.** Every active and recent agent with its model, repository,
  status, turns and tokens; running agents first.
- **Live view.** Watch the agent work, with the last reply on screen.
- **Controls.** Stop, Continue, Retry, and send a prompt by dictation or
  Scribble.
- **Review changes.** Read an agent's diff file by file and mark files as
  reviewed.
- **Listen.** Each agent plays as sound and haptics, so you can tell what it
  is doing without looking. The optional Announcer also says short lines
  such as "run done" or "is waiting for you".
- **Siri.** Say "Start a new ForkMesh agent" to start one. Name a
  repository, or it uses the repository and model you last used on the
  phone.
- **Complications and Smart Stack** show the current run at a glance.

Next: a standalone mode that signs in on the watch itself, so it works
without the phone nearby.

## Android

The Android app is built from the same design as the iPhone app (the same
Liquid Glass theme) and talks to the same endpoints, so the server cannot
tell them apart. New phone features land on iPhone first; each one is
tracked until Android matches it, and a few recent chat and call changes
(including group calls) are still on that list. It is distributed as a signed APK from the
downloads page.

Developer documentation for the native apps is in
[docs/native/README.md](../../docs/native/README.md).
