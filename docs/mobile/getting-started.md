# Getting started on a phone

From install to your first agent run on iPhone, iPad or Android in a few
minutes. Back to the [phone apps](README.md).

```text
   1 Install ──► 2 Sign in ──► 3 Permissions ──► 4 Starter pack ──► 5 Tour
                     │            asked only        Minimal ·         Essentials ·
                     │            when needed       Standard ·        Advanced ·
                     ▼                              Everything        whole shebang
         the phone makes its own key
         and joins your account as a node
```

## 1. Install

| Platform | Where | Requires |
| --- | --- | --- |
| iPhone and iPad | [App Store](https://apps.apple.com/app/forkmesh/id6803060735) | iOS 17 or later |
| Android | `forkmesh-android-universal.apk` from [forkmesh.com/#downloads](https://forkmesh.com/#downloads) ([Android guide](android.md)) | Android 8.0 or later |

The Apple Watch app comes with the iPhone app ([Apple Watch](apple-watch.md)).

## 2. Sign in

- **Log in** with your ForkMesh account, and your authenticator code if you
  turned on two-factor sign-in.
- **Sign up** opens the sign-up page in a sheet; confirm your email, then log
  in on the form the app returns you to.
- **Forgot password?** opens the reset page.
- **Explore as guest** lets you look around first; a guest can post only in
  `#general` and `#mesh`.

Signing in makes the phone a node of your account. It creates its own
Ed25519 key on the device and registers it, with no QR code and no second
step. Your other devices get a "new node added" notice, so a sign-in you did
not make is easy to spot ([privacy and security](privacy-and-security.md)).

**Your own instance.** To use a self-hosted ForkMesh, add a project with your
instance's address and press **Check instance**, or pick one from
**Instances on ForkMesh**, the public list of instances. A project can also be
local only, with no server.

## 3. Permissions, only when they matter

The first screen after sign-in explains that the app asks for each permission
(notifications, microphone, camera, photos, contacts, local network and the
rest) only the first time a feature needs it. A fresh install shows no system
prompts.

## 4. Pick a starter pack

Choose how much the Home screen starts with: **Minimal**, **Standard** (the
default) or **Everything**. Each Home tile is a Market package, so you can add
or remove any of them later. Then a short tour (**Essentials**, **Advanced**
or **The whole shebang**) shows the way around.

## 5. Connect to your desktop

There is nothing to pair. When your desktop runs ForkMesh under the same
account, the phone finds it and the desktop admits it after checking the
phone's key against your account:

```text
   phone ──── same Wi-Fi: direct, encrypted channel ────► desktop
     │                                                       ▲
     └── away from home: through the relay ──► cores ────────┘
         (end to end encrypted; the relay sees only routing)
```

- On the same network the phone talks to the desktop directly.
- Away from home it goes through the relay, encrypted end to end. The desktop
  must be online.
- A phone signed in to a **different** account pairs on the local network
  with a six-digit code shown on the desktop.

Then open **Agents**, tap the new agent button, pick a repository and write a
prompt. Details: [agent access](mobile-agent-access.md).

## Notifications

| What pings you | You can act from the banner |
| --- | --- |
| An agent waiting for approval, done, or stuck | **Approve**, **Deny**, **Continue**, **Merge & clean up** |
| A direct message or a chat message sent with notify | **Reply** |
| Pushes to your repositories, review requests, mentions | Open the item |

The ping settings screen has a switch per kind,
**Pause**, **quiet hours**, email and a test push. Quiet hours do not apply to
`#mesh`, `#general` or channel pushes. Chat text in a notification is
decrypted on the phone. Android has no push service: see
[Android](android.md#notifications).

## Widgets and system features

On iPhone:

- 🏝️ **Live Activity**: one summary of your running agents in the Dynamic
  Island and on the Lock Screen.
- 🧱 **Widgets**: running agents, Inbox, a pinned repository's pull requests,
  a timer, and Lock Screen widgets.
- 🎚️ **Controls**: **New agent** and **Approvals** in Control Center.
- 🌙 **Focus filter**: choose what ForkMesh shows in each Focus.
- 🗣️ **Siri and Shortcuts**: [Siri](siri.md).

## When something does not work

| Symptom | What is going on |
| --- | --- |
| Lists show but nothing can be changed | You are offline. Cached lists stay readable; signed changes wait in an outbox and go when you reconnect |
| The desktop is not found | Away from home the desktop must be online; on the same Wi-Fi allow **Local network** access for ForkMesh |
| Live agent updates stop when the app is in the background | iOS suspends the live connection in the background; a push wakes it, and it resumes when you open the app |
| File transfer to the desktop fails away from home | File transfer works only on the local network |
| A file will not open in the editor | Files over 512 KB, binary files and files that are not UTF-8 are refused ([file editing](mobile-file-editing.md)) |
| Siri does not run a command | See the troubleshooting table in [Siri](siri.md) |

Related: [privacy and security](privacy-and-security.md) ·
[agent access](mobile-agent-access.md) · [phone apps](README.md)
