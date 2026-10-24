# Getting started with ForkMesh

## Install

```sh
curl -fsSL https://forkmesh.com/install.sh | bash
```

To build from source, clone `https://forkmesh.com/forkmesh/forkmesh` and run
`./desktop/run.sh` (Qt 6.4+, CMake 3.16+, OpenSSL headers, Git, a C++17
compiler). See the [desktop app guide](qt-client.md) and the
[installation support matrix](install-support-matrix.md).

## First run

1. Launch ForkMesh. A local Ed25519 node identity is created; no server URL is needed.
2. Optionally sign in from **Settings** for a portable public username.
3. Open **Chat**, pick a conversation, and press Enter to send.
4. In **Repos**, choose **New repository**, **Add local repo**, or **Fork**.
   Public repositories appear in the catalog; private ones stay hidden unless shared.

## Make a change

Use normal Git (`git switch -c`, `git commit`, `git push`) or the desktop:
**Code → Branches → New branch**, edit, then **Code → Changes → Commit & push**.
For another owner's project, fork it and use **Pulls → New pull request**; the
relay queues the signed pull request while the owner is offline.

## Where data lives

```text
~/projects/forkmesh/   src/ data/ config/ ssh/ logs/ state/ cache/
~/projects/<project>/  data/ config/ ssh/ logs/ state/ cache/
```

`data/` holds the node's private identity key: export it from **Settings →
Data** before moving machines. `FORKMESH_HOME` or `FORKMESH_PROJECTS_DIR`
moves the layout. Uninstalling removes only the program unless `--purge` is
given.

## Phones and the watch

Get ForkMesh for iPhone and iPad on the App Store (the Apple Watch app comes
with it) or the Android APK from the downloads page. Sign in with the same
account; see [ForkMesh apps](apps.md).

## Your own instance

To run ForkMesh under your own name on your own server, open
[forkmesh.com/start](https://forkmesh.com/start), or hand the job to a coding
agent. See [Start your own instance](start-your-own-instance.md) and
[Working with coding agents](coding-agents.md).

Next: [the desktop app](qt-client.md) or the [documentation index](README.md).
