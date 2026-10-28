# Getting started with ForkMesh

## Install

```sh
curl -fsSL https://forkmesh.com/install.sh | bash
```

To build from source, clone `https://forkmesh.com/forkmesh/forkmesh` and run
`./desktop/run.sh` (Qt 6.4+, CMake 3.16+, OpenSSL headers, Git, a C++17
compiler). See the [desktop app guide](qt-client.md) and the
[installation support matrix](install-support-matrix.md).

### Opening ForkMesh again on Linux

The installer puts the program at `~/.local/bin/forkmesh` and starts it once.
After you close it:

- Open your applications menu (on GNOME press the Super key, on KDE click the
  launcher) and search for **ForkMesh**. ForkMesh adds itself there the first
  time it runs. To keep it handy, right-click its icon in the dock and choose
  **Pin to Dash** or **Add to Favorites**.
- Or open a terminal and type `forkmesh`. If the terminal says the command is
  not found, type `~/.local/bin/forkmesh` instead.

## Accounts and instances

- **One account everywhere.** The website, the desktop app and the phone apps
  use the same ForkMesh account. There is no separate desktop password: sign
  in with the email and password that work on the website.
- **Your username** is the name on your ForkMesh profile (it appears as
  `@name`), not your email. The desktop may suggest one when you first sign in;
  change it if it is not yours. If you get it wrong, the desktop tells you
  which username your email belongs to.
- **An instance** is the ForkMesh server that holds your account.
  `forkmesh.com` is the main one; leave the instance as it is unless someone
  gave you a different address. Choosing **Continue** on the sign-in screen
  only signs you in: it does not create an account or an instance. To create
  an account, use **Create account**, which opens the website.
- **The eye button** in the password box shows what you typed.
- **Too many wrong passwords** pause sign-in for a short time. The message
  says how long to wait; your password has not changed, so use the same one
  after the pause.

## First run

1. Launch ForkMesh. A local Ed25519 node identity is created; no server URL is needed.
2. Optionally sign in from **Settings** for a portable public username.
3. Open **Chat**, pick a conversation, and press Enter to send.
4. In **Repos**, choose **New repository**, **Add local repo**, or **Fork**.
   Public repositories appear in the catalog; private ones stay hidden unless shared.

To keep a safe copy of a repository you already have on GitHub, follow
[Back up a GitHub repository](back-up-a-github-repository.md).

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
