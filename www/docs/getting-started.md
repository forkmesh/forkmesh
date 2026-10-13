# Getting started with ForkMesh

This guide takes you from a new machine to a running desktop node, a repository
on the mesh, working chat, and your first change.

## Choose how to install

The fastest path is the published, SHA-256-verified installer:

```sh
curl -fsSL https://forkmesh.com/install.sh | bash
```

If you want to change ForkMesh itself, clone the source and run the desktop app (Qt):

```sh
git clone https://forkmesh.com/forkmesh/forkmesh
cd forkmesh
./desktop/run.sh
```

The source build needs Qt 6.4 or newer with Widgets, Network, SVG, Concurrent,
and, on Linux, D-Bus; CMake 3.16 or newer; OpenSSL development headers; Git; a
C++17 compiler; and the `openssl` command. Exact package commands and manual
build instructions are in the [desktop app guide](qt-client.md).

Which platforms have signed prebuilt packages, and how to upgrade, roll back,
or uninstall, is listed in the
[installation support matrix](install-support-matrix.md).

## Your first five minutes

1. Launch ForkMesh. A local Ed25519 node identity and a fun temporary node name
   are created automatically. The client connects to the default mainnode; no
   server URL is required.
2. Sign in or create an account from **Settings** if you want a portable public
   username. The node identity remains on this device.
3. Wait for the initial ForkMesh project mirror to finish syncing. A fresh
   install opens it automatically.
4. Open **Chat**, choose a conversation or repository channel, type in the
   composer at the bottom, and press Enter. Signed-in users can also reply in
   mirrored World office channels; the client obtains the scoped room access
   and sends the message over its encrypted office socket.
5. Open **Repos** and choose one of these paths:

   - **New repository** creates a Git repository, optional README, local mirror,
     and signed catalog entry.
   - **Add local repo** selects an existing local `git init`/`git clone`
     directory and asks whether it is public or private.
   - **Repos → Fork** makes an independent writable copy of a repository you
     found on the mesh.

Public repositories appear in the network catalog. Private repositories remain
hidden from public browse and clone routes unless you explicitly share them.
ForkMesh publishes signed metadata; it does not publish a local filesystem path.

## Keyboard and accessible browsing

The responsive dashboard supports the same repository journeys with a keyboard,
pointer, or touch screen. Press **Control+K** (or **Command+K** on macOS) to open
the command palette from any dashboard page. Type to filter its actions, use
Up/Down to select, Enter to run, and Escape to close and return focus. Press
**/** outside a text field to search and **T** on a repository to find a file.

The first Tab stop skips directly to main content. Focus stays visibly outlined;
light and dark themes meet the dashboard contrast target; browser zoom up to
200% reflows to the compact layout; and operating-system reduced-motion and
forced-colors preferences are honored. Clone commands remain selectable beside
their Copy buttons, and copy results are announced. If repository discovery is
offline, use **Retry repository list** after reconnecting; the page does not
discard your local theme or repository cache.

## Make a change

You can use normal Git commands in the repository folder:

```sh
git switch -c docs/my-change
# edit files
git add -A
git commit -m "docs: explain my change"
```

Or stay in the desktop application:

1. Open the repository, then **Code → Branches → New branch**.
2. Edit files in your editor or ForkMesh's file view.
3. Open **Code → Changes**, enter a commit message, then choose **Commit** or
   **Commit & push**.
4. For a PR within a repository you own, return to **Code → Branches**, select
   the feature branch, and choose **Create PR**.

For an upstream project, fork it first. In the writable fork, open
**Pulls → New pull request**, select the upstream owner as **Target node**, and
choose `main` as the base and your feature branch as the head. That sends the
signed PR to the owner's inbox; the relay queues it even when the owning node is
offline.
See [Contributing](../../CONTRIBUTING.md#opening-a-pull-request) for the complete
review checklist.

## Where ForkMesh stores data

Use **Settings → Data** for the authoritative paths on your machine. That page
can open each directory and export or restore configuration data.

Everything ForkMesh keeps on disk lives in one place, the projects folder:

```text
~/projects/forkmesh/                the ForkMesh project
  src/                              the source checkout it builds and updates from
  data/                             identity, chat, agents, mirrors, local stores
  config/ForkMesh/ForkMesh.conf     preferences
  ssh/                              keys and known hosts for hosts and mirrors
  logs/diagnostics/                 action and UI-stall journals
  state/  cache/                    lock file; disposable cache
  toolchain/  builds/  worktrees/   shared build tooling
~/projects/<project>/               any other project, fully isolated:
  data/ config/ ssh/ logs/ state/ cache/   its own files, nothing shared
```

Pick or add a project with the logo at the far left of the top bar. A project
can start with no repository, no relay and only the plugins you choose; each
one runs as its own process with its own identity folder, settings and logs.

An install from before this layout kept these in `~/.local/share/ForkMesh`,
`~/.config/ForkMesh`, `~/.cache/ForkMesh` and `~/.forkmesh`. ForkMesh moves them
into `~/projects/forkmesh` the first time it starts (a rename, so it is instant)
and leaves a link at each old path so other tools keep working; **Settings →
Data** lists those links and can remove them. If you run ForkMesh from a source
checkout at `~/projects/forkmesh`, run `python3 tools/forkmesh_layout.py` first:
it shows, and with `apply` performs, the move of the checkout into `src/`.
Setting `FORKMESH_HOME` or `FORKMESH_PROJECTS_DIR` places the layout elsewhere;
a non-default `XDG_DATA_HOME` keeps a run on the XDG paths it names.

Do not delete application data casually: it contains the private identity key
for this node. Prefer **Settings → Data → Export configuration** before moving
machines or testing recovery.

Upgrading, rolling back, and uninstalling keep these directories.
`curl -fsSL https://forkmesh.com/uninstall.sh | bash` removes only the program.
Add `-s -- --purge` to erase your data as well; it asks for confirmation first.

## If something does not work

- Build failure: check the dependency and clean-build sections in the
  [desktop app guide](qt-client.md#build-and-run-from-source).
- Cannot type or send in chat: select a conversation first. World office replies
  also require a signed-in account on this node; reconnect after signing in so
  the client can obtain room access.
- Repository is read-only: use **Fork** to create a local working copy.
- A branch has no **Create PR** action: it must differ from the default branch
  and the repository must have a writable working tree. For an upstream PR, use
  **Pulls → New pull request** and select the upstream target node.
- Network or UI problem: use the built-in diagnostics and logs described in
  [desktop app diagnostics](qt-client.md#logs-and-diagnostics).

Next: [learn the desktop app](qt-client.md), [open a pull
request](../../CONTRIBUTING.md#opening-a-pull-request), or browse the
[documentation index](README.md).
