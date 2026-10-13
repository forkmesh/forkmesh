# Qt desktop client

The Qt client is a workspace application: it provides chat and repository UI,
keeps local Git data, signs collaboration records, runs actions, and can host
coding agents. It does not serve repositories or publish the catalog; the
managed Go mirror service owns those responsibilities.

## Install a release

For normal use, install the published binary:

```sh
curl -fsSL https://forkmesh.com/install.sh | bash
```

The installer verifies the signed release metadata and artifact checksum and
creates a desktop launcher. It never compiles as an implicit fallback. For a
native `.exe`, `.dmg`, or `.AppImage`, use the
[desktop downloads](https://forkmesh.com/desktop). To develop the client, build
it from source instead.

## Build and run from source

Requirements:

- Qt 6.4+ components: Widgets, Network, SVG, Concurrent, and D-Bus on Linux.
- CMake 3.16+ and a C++17 compiler.
- OpenSSL 3.0 or newer, with development headers, and the `openssl` command.
  Private mirrors and the Password Vault need OpenSSL 3.5 (ML-KEM-768 and
  Argon2id); built against an older OpenSSL they stay unavailable and say so,
  and everything else works. Ubuntu 24.04 and Debian 12 ship OpenSSL 3.0: add
  `-DFORKMESH_BUNDLED_OPENSSL=ON` to the first configure and the build fetches
  a pinned OpenSSL 3.5.8, checks its SHA-256 and links it (a few minutes, once),
  or pass an OpenSSL 3.5 you installed with `-DOPENSSL_ROOT_DIR=...`.
- Python 3 (the build generates sources from `marketplace/`).
- Git.
- Optional: `libnghttp3` (with OpenSSL's QUIC support) for the built-in HTTP/3
  client used by host log feeds; without it those feeds use SSH.
- Optional: Qt 6 Multimedia. With it, a video shared in chat plays in an in-app
  player; without it the same video opens in whatever player the desktop uses
  for that file type.

Common dependency commands:

```sh
# Ubuntu / Debian
sudo apt update
sudo apt install build-essential cmake git libssl-dev openssl \
  qt6-base-dev qt6-svg-dev qt6-base-dev-tools pkg-config

# Fedora
sudo dnf install cmake gcc-c++ git openssl openssl-devel \
  qt6-qtbase-devel qt6-qtsvg-devel

# Arch Linux
sudo pacman -S --needed base-devel cmake git openssl qt6-base qt6-svg

# macOS (after installing Xcode Command Line Tools and Homebrew)
brew install cmake git openssl@3 qt
```

Optional, for the in-app chat video player (Homebrew's `qt` already includes
it):

```sh
sudo apt install qt6-multimedia-dev        # Ubuntu / Debian
sudo dnf install qt6-qtmultimedia-devel    # Fedora
sudo pacman -S --needed qt6-multimedia     # Arch Linux
```

From the repository root (the helper preserves this as ForkMesh's launch
directory):

```sh
./desktop/run.sh
```

`run.sh` configures a Release build (with the test programs switched off),
builds incrementally with memory-safe parallelism, and launches the correct
Linux or macOS binary. It also detects a
CMake cache copied from another path and safely regenerates it.

Available commands:

```sh
./desktop/run.sh run       # incremental build and launch; "run" is optional
./desktop/run.sh test      # build and run the sub-minute critical contracts
./desktop/run.sh clean     # delete desktop/build
./desktop/run.sh rebuild   # clean, build, and launch
```

Manual CMake commands are useful for IDEs and focused tests:

```sh
cmake -S desktop -B desktop/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFORKMESH_BUILD_TESTS=ON
cmake --build desktop/build --parallel 12
ctest --test-dir desktop/build --output-on-failure
```

Prefer the Ninja generator: the CMake file declares a job pool that caps how
many of the giant `MainWindow*.cpp` units compile at once (six GiB reserved,
two GiB per compile, at most twelve), so a high `--parallel` is safe with
Ninja while Make ignores pools and needs the smaller `-j` that `run.sh`
computes from free memory. `run.sh`, `install.sh` and the desktop's own
*Rebuild & restart* all pick Ninja when it is installed and migrate an
existing Make tree in place. Install `mold` (a copy under `~/.local/bin` is
enough) and the link step drops from ~14 s to ~3 s for the RelWithDebInfo
binary; CMake only uses it after a successful test link.

*Rebuild & restart* compiles the checkout the app itself merges agent
branches into, so a session that finishes mid-build can rewrite a header the
precompiled header was built from (GCC then reports every symbol defined
twice). The rebuild therefore checks the sources after each build pass and
runs the pass again — incrementally — when anything changed, up to three
passes, installing only a build that finished on an unchanged tree. It builds
`RelWithDebInfo` so stall reports can be symbolized, and it waits for running
agents before it starts.

Developer notes on the build (precompiled header, test targets, CI, known
failures) are in
[docs/desktop/build-and-test.md](../../docs/desktop/build-and-test.md).

If CMake cannot find Qt on macOS, pass Homebrew's prefix:

```sh
cmake -S desktop -B desktop/build \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
```

Never run ForkMesh as root. The node runs Git commands and repository workflows
with the current user's permissions.

## Check a change against the oldest supported Qt

Qt 6.4 is a floor rather than an aspiration. Ubuntu 24.04 LTS and Debian 12 both
ship Qt 6.4.2, so `find_package(Qt6 6.4)` configures on them and an API added in
6.5 or later surfaces only as a compile error partway through their build —
never on a development box carrying a newer Qt. Reach for such an API behind
`#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)` with a 6.4 fallback.

To confirm a change on those distributions without root, Docker, or a second
machine:

```sh
tools/qt64-build-check.sh                 # both distributions
tools/qt64-build-check.sh ubuntu-24.04    # just one
```

Each profile unpacks that distribution's own Qt 6.4.2 and GCC into a private
sysroot under the ForkMesh toolchain directory
(`~/projects/forkmesh/toolchain/qt64` in the default layout), then runs the same configure and
build `install.sh` runs for a source install. The first run downloads roughly
250 MB per profile and caches it; later runs only pay for the build.

## KVM-isolated coding workspace

On Linux, ForkMesh can keep its desktop UI on the host while running coding
agents and their subprocesses behind a separate Linux kernel in a Lima
QEMU/KVM guest. The launch directory is mounted writable at the same path in
the guest; it is not copied. ForkMesh also mounts linked-worktree Git metadata
needed by that directory and a dedicated temporary agent-worktree root. The
host home directory, desktop sockets, SSH agent, and unrelated environment
variables are not shared.

This mode requires [Lima 2.0 or newer](https://lima-vm.io/docs/installation/),
QEMU, and read/write access to `/dev/kvm`. ForkMesh does not fall back to
software emulation. To use it:

1. Start ForkMesh from the narrowest directory all intended repositories live
   under. For a source build, run `./desktop/run.sh` from that directory. A
   desktop launch normally starts in the home directory, which ForkMesh refuses
   to expose because it is too broad.
2. Open **Settings → General → KVM workspace**, turn on **Run coding agents in
   an isolated KVM virtual machine**, and choose **Prepare / start VM**. The
   first base-image download can take several minutes.
3. Choose **Open VM shell** and install/sign in to Claude Code, Codex, or any
   configured agent inside the guest. Host CLI login files are deliberately not
   mounted.
4. Choose **Restart ForkMesh now**. The status changes to **Active** after the
   relaunch.

Turning the setting off also requires a relaunch so an already-running agent
cannot cross the host/guest boundary mid-session. Repositories outside the
recorded launch directory are rejected while KVM mode is active. The
per-directory VM is retained when the setting is off so its installed tools and
guest login survive; stop it manually with `limactl stop <instance>` if desired.

## First run and navigation

A first launch creates the node identity, connects to the default mainnode, and
starts syncing the ForkMesh project. The left rail holds Repo, Repos, Market,
Ledger, Tasks, Calendar, Notes, Pings, Agents, Chat, Settings, Tour, Log,
Control, Network, Users, Team, Files, Stats, Health, Memory and Restart
(`desktop/src/MainWindowNavigation.cpp`). The main areas are:

- **Chat** — direct conversations, repository channels, mirrored World office
  channels, and optional Discord channels. Compose in the input at the bottom.
  Office replies require a signed-in account so the client can obtain scoped
  encrypted-room access.
- **Repos / Network** — create or add local repositories, browse the mesh, fork
  a project, and control public/private visibility.
- **Code** — browse and edit files, view changes, commit, inspect branches and
  worktrees, and create a PR from a branch.
- **Issues / Pulls / Actions** (inside a repository) and **Agents** — signed collaboration records,
  reviews, isolated agent worktrees, and workflow runs. The Agents page has
  two tabs: **Sessions** (the session list and transcript) and **Tool
  efficiency**, a live board that folds every runtime tool call (Claude Code,
  Codex, MCP) into one provider-neutral tool family and shows, per family,
  calls, errors, latency, the context it injected, and the tokens and
  list-price cost it saved against the naive alternative (whole-file reads,
  whole-file rewrites, reading every matched file). Savings are only claimed
  where that baseline is measurable from local files; token counts are
  byte-based estimates computed on this device and never sent anywhere. The
  ledger lives beside the agent store as `tool-efficiency.json`.
  Claude Code sessions you start yourself in the repository's folder show in
  the session list inside an **External Claude Code** folder.
- **Network** — the relays, nodes, and hosts this client knows about, each as a
  tab with its own count, alongside live HTTP/WebSocket state and recent request
  metadata. The rail's Network badge shows the three counts added together.
- **Settings** — one scrolling page with a section list: account, node,
  agent, Pings, network, phone, storage, and backup controls.
- **Pings** — the app's notifications; each ping card has a gear that turns
  its kind off.
- **Health** and **Memory** — per-file code health of the checkout and a live
  breakdown of the app's own memory.

Under **Network → Hosts → Create a Vultr mirror**, the desired healthy mirror
count can be set before opting in to automatic fleet sizing. While enabled, the
client checks the public ForkMesh catalog every five minutes, counting down to
the next check beside the checkbox. It creates one managed Vultr mirror at a
time until the target is healthy, and permanently destroys one excess managed
instance at a time when the target is lowered. A check that cannot reach the
catalog — the shared relay answers HTTP 429 under a faster cadence — makes no
fleet change and is simply retried on the next countdown.
Only Vultr instances created and tracked by this client participate; manually
added hosts and other providers are never destroyed by this setting. A mirror
counts as healthy only when its public catalog row is online, fresh,
integrity-approved, clone eligible, and serving healthy endpoint traffic.

To add an existing project, choose **Repos → Add local repo** and select the
working-tree folder. To contribute to someone else's repository, find it under
**Repos**, choose **Fork**, and select a parent folder for the new working copy.

### Team and teammate run modes

The **Team** area loads repository-owned teammate definitions from
`.forkmesh/teammates/`. Each teammate can remain on demand, use a bounded cron
cadence (hourly, daily, weekly, weekdays, or selected days/times), react to
selected repository events, or run in **Continual** mode. Automatic work is
opt-in and runs only while the desktop client is open.

A cron invocation completes one run and waits for the next scheduled time. A
continual teammate instead keeps one agent conversation: it completes a bounded
cycle, presents an interactive decision card, and waits. When there is one
recommended next action the card asks for Yes or No (typing `OK` means Yes);
when the work has real alternatives it offers exactly three concrete next
steps with the recommendation first. The selected answer becomes the next
cycle, which returns to another decision gate when it finishes. No polling
interval starts work between those decisions.

Choose **Stop work** to stop the live agent and pause a continual loop. Starting
that mode again resumes automatic cycles. Permission mode, repository grants,
agent concurrency, worktree separation, and any optional VM/jail boundary still
apply to every cycle; continual operation adds no authority. In particular, a
Git worktree separates branches but is not an operating-system sandbox.

A narrow **teammates rail** on the right edge of the window lists the
teammates with a status light, when each last ran, a **Run** button and a
**⋯** menu with every other option. **Create needed teammate** below them
starts an agent that reads the repository's commits, docs, logs, errors, chats
and past agent runs and proposes the teammate the repository is missing; the
desktop adds it with its schedule paused. Turn the rail off from its
right-click menu or in Settings > Appearance. Teammates run on the model you
used most recently, and their runs stay out of the Agents list until you pick
them with the **Teammates** filter.

Every teammate works in its own agent session. Running the whole team, a
daily briefing and an `@all` mention in the Team chat start or continue one
session per teammate, and each teammate replies in the thread for itself.
Their sessions are filed in a **Special Agents** folder in the Agents list.
**Run all in one session** runs every available special agent together in a
single agent session instead: it works through each one in turn, updates a
special agent's definition when its work shows it is out of date, and records
each one's part on that special agent. That session only has the permissions
all of them share.

### Multiple relay instances

The relay selector in the top-left can retain any number of independently
named instances. Use **Add relay…** to register one, and the pencil or trash
controls beside a relay to rename or remove it. Each instance remembers its
last selected account. The avatar menu lists accounts by both username and
instance, so the same username on two relays remains two distinct identities.

The active relay is a security boundary. Repository catalogs and actions,
account/admin requests, cached bearer capabilities, private-channel membership,
chat history, avatars, `#team` replies, and newly queued agent work are scoped
to its stable local instance id and exact HTTPS/WebSocket origin. Switching
first clears active credentials, then revalidates a cached destination session;
the chat transport never silently fails over to another configured relay.
Removing an instance scrubs its in-memory bearers and remembered account
bindings. Bearer tokens are never written to `QSettings`.

Each relay stores its own repository list, and the desktop pulls it from
`GET /api/repositories` when a session starts, when the relay changes, and
whenever the Repositories page loads. A repository record carries the origin of
the relay it was added under and is only ever served, synced, or registered with
that relay: the mirror gateway configuration, the background sync pass, the pull
ledger sweep, and private mirror route registration all skip records that belong
to another relay. Records from before relays were tracked are assigned from the
pulled lists — the one relay that lists a repository owns it — and fall back to
the default relay only when no list settles it.

Public-channel encryption remains controller-readable so the selected relay's
approved bots and integrations can participate. Private channels (including
`#team`) and direct messages retain their additional device-to-device layer;
changing instances does not make those memberships or keys available to the
new relay. See [Chat encryption and integrations](security/sealed-chat.md) for
the complete boundary.

### Chat sidebar

Drag the divider between the channel list and the transcript to make the
sidebar wider or narrower; the width is remembered across launches. Channels
can be organised into collapsible groups: choose **New channel → New group…**
(or right-click empty space in the list), then drag channels onto a group
header or between its members. Right-click a channel for **Move to group**, and
a group header to rename, collapse, or delete it — deleting a group keeps its
channels in the sidebar, ungrouped. Connected services place their channels in
an automatic group named after the source (for example **Discord · Forkmesh**),
where each channel shows only its short name; a channel dragged out of that
group stays where you put it.

### Discord in Chat

The Discord connector is a Market package (**Market → Extensions → Discord**),
installed by default. While it is installed, **Settings → General → Discord →
Connect Discord** places Discord server text channels, active threads, and bot
direct messages in the same sidebar and transcript as native ForkMesh chat,
grouped under a **Discord · <server>** header. ForkMesh connects through
Discord's supported bot API: create a bot in the linked Developer Portal,
enable **Message Content Intent**, copy its bot token, and install it in each
server where it should participate. A normal Discord user token is
deliberately not accepted because automating a user account as a self-bot
violates Discord policy.

The server's permissions remain authoritative. Grant only what the integration
needs—normally View Channels, Send Messages, Read Message History, Attach Files,
Add Reactions, and Send Messages in Threads. Messages sent from ForkMesh appear
under the bot's Discord identity. History, live messages, replies, typing,
reactions, edits/deletes, avatars, and attachments render in the ordinary Chat
area; right-click a Discord channel to open it in Discord or refresh its recent
history.

The bot token is stored in device-local app settings owned by the package, not
in action variables, process arguments, logs, or chat history. **Disconnect**
removes the token and cached Discord conversations from this device. Removing
the package from **Market** goes further and leaves no trace: it stops the
client, hides the connector card in Settings, and clears the token, cached
history, cached Discord avatars, and the automatic sidebar group; reinstalling
it from Market starts fresh without restarting the app.

### Sentry in the Log

The Sentry connector is a Market package (**Market → Extensions → Sentry**),
installed by default. While it is installed, the **Log** page gains a
**Sentry** tab and the log's activity lights, filter chips, and badge column
gain a **SENTRY** lane. **Connect Sentry** on that tab takes the organization
slug, an optional project slug, the Sentry host (empty for sentry.io), and a
Sentry API key (auth token) with the `org:read` and `project:read` scopes;
ForkMesh then reads the organization's unresolved issues once a minute and
never writes to Sentry.

The first read logs one `Sentry:` summary line — issues, events, and affected
users for the selected period. After that, a new issue or an issue whose event
count grew lands in the **Activity** tab as its own `Sentry:` line and lights
the Sentry lane; unchanged and resolved issues stay quiet. The tab itself
aggregates the same read: totals, a by-project and a by-level breakdown, and
every issue with its events, share, users, and first/last-seen age, linked to
Sentry. A refused API key is logged once and stops the poll until **Refresh** or
a new API key; a rate limit waits the time Sentry asks for.

The API key is stored in device-local app settings owned by the package, never in
logs, action variables, process arguments, or chat history. **Disconnect**
removes it and the cached issues. Removing the package from **Market** stops
the reads, hides the tab, and clears the API key, connection details, and cached
issues; reinstalling it from Market starts fresh without restarting the app.

## Create a pull request

For a PR within a repository you own:

1. Open the writable repository and choose **Code → Branches → New branch**.
2. Make the change.
3. Choose **Code → Changes**, stage the files, enter a message, and commit.
4. Choose **Code → Branches**, select the branch, review its diff, and click
   **Create PR**.
5. Enter the title. ForkMesh records the commit series and a signed binary-safe
   patch, then opens the new PR.

The button is disabled when the branch is the default branch, has no diff, or
the repository has no writable working tree. Agents follow the same model: each
agent works in an isolated worktree and can produce a branch-backed PR. That
worktree lives inside the project, at
`.worktrees/agent-<session>-<short-description>`, so the tree a run is editing
sits beside the code it forked from. ForkMesh keeps the directory out of the
project's Git status and removes each worktree when its run is finished; agents
on a bare mirror (no working tree to nest one in) still use a temporary
directory, as does a run under KVM isolation, where the worktree has to land on
the host directory mounted into the guest.

For a PR to another owner:

1. Find the upstream repository under **Repos**, choose **Fork**, and select a
   folder for your writable checkout.
2. Create and commit the feature branch in that fork.
3. Open **Pulls → New pull request**.
4. Set **Target node** to the upstream owner, **Base** to its default branch
   (normally `main`), and **Head** to your feature branch.
5. Add the title and description, then submit. ForkMesh signs the patch and
   commit series and delivers it to the target node's inbox. The relay can hold
   it until the owner comes online.

For a very small one-file fix, a bare mirror offers a shortcut: edit the file
in ForkMesh and choose **Save as PR**. The client builds the signed commit in a
temporary worktree and sends it directly to the repository owner's inbox.

**Send to source of truth** is a recovery/re-delivery action shown while viewing
an open PR on a read-only mirror; it is not needed for the normal fork workflow.

For project-specific tests and the required PR evidence, see
[Contributing](../../CONTRIBUTING.md).

## Run a managed mirror

Do not run the desktop binary as a repository server. Operational deployments
use the dedicated Go mirror daemon and follow the
[headless mirror runbook](operations/headless-mirror-refresh.md). Packaged
mirror deployments may invoke the Qt binary in an internal, short-lived
offscreen intake mode; that worker materializes signed collaboration input only
and never serves repositories or publishes catalog records.

## Logs and diagnostics

Start in the UI:

- Click the CPU/RAM/stall indicator in the footer for UI-stall details.
- Open **Network diagnostics** for current connections and recent endpoint
  results.
- Enable **Settings → Log every network request in the network log** only while
  diagnosing a problem; it is intentionally opt-in.
- Open **Settings → Data** to see and open the exact application-data,
  preferences, mirror, and cache directories for the current OS.

Durable diagnostic journals are stored separately:

```text
~/projects/forkmesh/logs/diagnostics/actions.jsonl   bounded action lifecycle journal
~/projects/forkmesh/logs/diagnostics/stalls.log      UI freezes longer than 500 ms
~/projects/forkmesh/logs/diagnostics/stalls.log.1    previous rotated stall journal
```

These are the default-project paths (`desktop/src/AppPaths.cpp`,
`Layout::diagnosticsDir`). An install that has not migrated yet still uses the
legacy `~/.forkmesh/diagnostics/`.

Each `actions.jsonl` record, like the footer log's `Background ✓ …` outcome
lines, says who asked for the work and where it ran, not only what ran:
`source` (file:line that opened the ticket), `function`, `routine` (the timer
tick, user action or agent flow it ran for — for example
`home/repository stats timer (every 60s)` or
`agent files diff poll · agent #42`),
`cwd`, `queued_ms` (time spent waiting for a worker slot) and, on the
`finished` record, an `outcome` that names the exit code of a failed process.

The application data directory also contains the persistent network log
(`network_log.txt`) and crash records. On Linux it is normally
`~/projects/forkmesh/data/` (legacy installs: `~/.local/share/ForkMesh/ForkMesh/`);
use **Settings → Data** instead of assuming that path on macOS or Windows.
How the stall watchdog, crash repair and log retention work:
[docs/desktop/diagnostics.md](../../docs/desktop/diagnostics.md).

When reporting a problem, include:

1. `./desktop/build/forkmesh --version` output.
2. The operating system and display server (Wayland/X11 when relevant).
3. The exact action that triggered the problem.
4. A short relevant log excerpt with secrets, tokens, private repository names,
   and private message contents removed.

## Common fixes

- **CMake cannot find Qt6:** install the Qt development packages, not only the
  runtime; on macOS pass `CMAKE_PREFIX_PATH` as shown above.
- **Qt platform plugin error:** launch the desktop from a graphical session. For
  a server, install the managed mirror service instead. Linux screenshot capture under Wayland also needs
  the desktop portal and Qt D-Bus support.
- **A moved checkout reports a stale CMake cache:** `run.sh` fixes this
  automatically; otherwise run `./desktop/run.sh clean`.
- **A normal build is unexpectedly slow:** use the incremental
  `./desktop/run.sh`; install `ccache` to let objects survive clean builds and
  branch changes, and give it room (`ccache -M 30G`): the app's objects alone
  are ~700 MB per build type, so the 5 GB default thrashes as soon as a few
  worktrees or build types share it. Install `ninja` and `mold` for the
  generator and linker described above. Every edit to `MainWindow.h` or
  `MainWindowInternal.h` recompiles every `MainWindow*.cpp` unit (about 80);
  that fan-out, not the toolchain, is the remaining cost of a rebuild. Put new
  helper bodies in `MainWindowInternal1-4.cpp`, not in the header.
- **Chat will not send:** select a conversation and confirm the node is
  connected. World office replies also require a signed-in account; reconnect
  after signing in so the client can obtain room access.
- **Repository actions are read-only:** fork the repository or add its local
  working tree.
- **Windows: "configuration_lock_unavailable" at every launch:** fixed in
  1.2.4. Update; the window now opens with a warning offering **Retry** and
  **Reset Actions configuration…** instead of stopping (see
  [docs/desktop/windows.md](../../docs/desktop/windows.md)).
- **Need a normal rolling backup:** use **Settings → Data → Export
  configuration** before deleting or importing anything. For a portable,
  secret-complete recovery/deployment file, use **Control → Deploy → Export
  complete system…** and protect the resulting clear-text JSON as carefully as
  the private keys it contains.

Back to [Getting started](getting-started.md) or the
[documentation index](README.md).
