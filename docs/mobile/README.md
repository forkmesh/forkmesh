# Phone apps

ForkMesh on iPhone, iPad, Apple Watch and Android. A phone is a node of your
account: it holds its own keys, clones and commits with its own Git engine,
reads your end-to-end encrypted agent sessions, and can drive your desktop
from the couch or the other side of the world.

How the apps are built is on the [phone app internals](../native/README.md)
home page; every ForkMesh app side by side is on [ForkMesh apps](../apps.md).

```text
            ┌──────────────────────── your phone ─────────────────────────┐
            │  own Ed25519 key (Keychain / Keystore)  ·  own Git engine   │
            │  Agents · Inbox · Repos · Chat          ·  widgets, Siri    │
            └──────┬──────────────────────┬───────────────────────┬───────┘
                   │ same Wi-Fi           │ HTTPS                 │ paired
                   │ (direct, LAN)        │                       │ (watch)
                   ▼                      ▼                       ▼
         ┌──────────────────┐   ┌───────────────────┐    ┌────────────────┐
         │  your desktop    │   │  ForkMesh cores   │    │  Apple Watch   │
         │  agents, repos,  │◄─►│  relay sees only  │    │  follows the   │
         │  worktrees       │   │  ciphertext and   │    │  phone's keys  │
         └──────────────────┘   │  routing          │    └────────────────┘
                                └───────────────────┘
```

## Get the app

| | Platform | Get it | Requires |
| --- | --- | --- | --- |
| 📱 | iPhone and iPad | [App Store](https://apps.apple.com/app/forkmesh/id6803060735) | iOS 17 or later |
| ⌚ | Apple Watch | Installs with the iPhone app | watchOS 10 or later, with the iPhone nearby |
| 🤖 | Android | Signed APK `forkmesh-android-universal.apk` from [forkmesh.com/#downloads](https://forkmesh.com/#downloads) | Android 8.0 or later |

Then follow [getting started on a phone](getting-started.md).

## The app at a glance

| | Tab | What lives there |
| --- | --- | --- |
| 🤖 | **Agents** | Every agent you run, on your desktop, a cloud runner or the phone itself; the default tab |
| 📥 | **Inbox** | Pings, review requests, assigned issues and tasks, mentions and approvals |
| 📦 | **Repos** | Your repositories: code, commits, pull requests, issues, releases and Actions |
| 💬 | **Chat** | Channels, direct messages, `#mesh` and office rooms |

Always on screen: the **new agent** button (tap for the composer, hold to
speak a prompt). Your avatar opens the **You** hub; **Home** holds the tiles
of the Market packages you turned on, and **Search** finds anything.

## What you can do

| | Area | Highlights | Guide |
| --- | --- | --- | --- |
| 🤖 | Agents | Start, steer, stop, continue, retry and merge agents on your desktop or cloud runners; answer approvals; review diffs file by file; run an agent on the phone with your own API key | [Agent access](mobile-agent-access.md) |
| 📦 | Repositories | Browse code, commits and releases; review and merge pull requests; unified or side-by-side diffs; clone to the phone, commit and publish | [Owned Git engine](owned-git-engine-spec.md) |
| ✏️ | Quick edits | Edit one file and open a pull request, with no clone or fork | [File editing](mobile-file-editing.md) |
| 💬 | Chat and calls | End-to-end encrypted direct messages with safety numbers; voice and video calls to a person or a private channel; `#mesh` over the local network and Bluetooth | [Calls](../calls.md) · [Safety numbers](../chat-safety-numbers.md) |
| 🔔 | Notifications | Pings, pushes, direct messages and chat; act from the banner (Approve, Deny, Continue, Merge, Reply); per-kind switches, pause and quiet hours | [Getting started](getting-started.md#notifications) |
| 🧩 | iPhone system features | A Live Activity in the Dynamic Island, Home and Lock Screen widgets, Control Center controls and a Focus filter | [Getting started](getting-started.md#widgets-and-system-features) |
| 🗣️ | Siri and Shortcuts | Start, check, approve, message, stop and merge agents by voice | [Siri](siri.md) |
| 🛍️ | Market tools | Turn on the packages you want as Home tiles: Password Vault with AutoFill, RSS Reader, Notes, Tasks, phone security checkup and more | [Apple Health](apple-health.md) · [Phone security](../marketplace/phone-security.md) |
| ⌚ | Apple Watch | Follow, steer and hear your agents from your wrist | [Apple Watch](apple-watch.md) |

## Guides

| | Page | Covers |
| --- | --- | --- |
| 🚀 | [Getting started on a phone](getting-started.md) | Install, sign in, first run, connecting to your desktop, notifications and widgets |
| 🔐 | [Privacy and security on the phone](privacy-and-security.md) | Where your keys live, what is encrypted, what the server sees, and what to do if you lose your phone |
| 🤖 | [Android](android.md) | Installing and updating the APK, and how the Android app differs on purpose |
| ⌚ | [Apple Watch](apple-watch.md) | What the watch app does today and what it needs |
| 🎛️ | [Agent access](mobile-agent-access.md) | Reading encrypted agent sessions, queueing prompts and controlling a desktop from the phone |
| ✏️ | [File editing](mobile-file-editing.md) | Editing one file and opening a pull request from the phone, with no clone or fork |
| 🗣️ | [Siri](siri.md) | Siri and Shortcuts commands for agents on iPhone |
| ❤️ | [Apple Health](apple-health.md) | The seven optional tools that read or write Apple Health, and what reaches your account |

## Contracts

For people building clients or checking the design:

| Page | Covers |
| --- | --- |
| [Mobile node contract](mobile-node-contract.md) | The wire and policy contract that makes a phone a node of your account |
| [Owned Git engine](owned-git-engine-spec.md) | The Swift and Kotlin Git engine the phones carry, and its security boundary |

Core pages: [architecture](../architecture.md),
[encryption](../encryption.md), [mirroring](../mirroring.md),
[censorship resilience](../censorship-resilience.md). Back to the
[documentation index](../README.md).
