# Qt desktop client

The Qt client is a workspace application: chat and repository UI, local Git
data, signed collaboration records, actions, and coding agents. It does not
serve repositories or publish the catalog; the Go mirror service does that.

## Install a release

```sh
curl -fsSL https://forkmesh.com/install.sh | bash
```

The installer verifies signed release metadata and the artifact checksum and
never compiles as a fallback. Native `.exe`, `.dmg`, and `.AppImage` packages
are on the [desktop downloads](https://forkmesh.com/desktop) page.

## Build and run from source

Requirements: Qt 6.4+ (Widgets, Network, SVG, Concurrent; D-Bus on Linux),
CMake 3.16+, a C++17 compiler, OpenSSL 3.0+ with headers, Python 3, and Git.
Private mirrors and the Password Vault need OpenSSL 3.5; on older systems pass
`-DFORKMESH_BUNDLED_OPENSSL=ON` (fetches and verifies a pinned OpenSSL) or
`-DOPENSSL_ROOT_DIR=...`. Optional: `libnghttp3` for the HTTP/3 log-feed client,
Qt 6 Multimedia for in-app video playback, and Qt 6 WebEngine
(`qt6-webengine-dev`) for the Browser tab and YouTube inside the app.

```sh
sudo apt install build-essential cmake git libssl-dev openssl \
  qt6-base-dev qt6-svg-dev qt6-base-dev-tools pkg-config \
  qt6-webengine-dev                                         # Debian/Ubuntu
brew install cmake git openssl@3 qt                         # macOS
```

From the repository root:

```sh
./desktop/run.sh           # incremental Release build and launch
./desktop/run.sh test      # build and run the critical contracts
./desktop/run.sh clean     # delete desktop/build
./desktop/run.sh rebuild   # clean, build, and launch
```

Manual CMake (prefer Ninja; its job pool caps concurrent heavy units):

```sh
cmake -S desktop -B desktop/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFORKMESH_BUILD_TESTS=ON
cmake --build desktop/build --parallel 12
ctest --test-dir desktop/build --output-on-failure
```

On macOS add `-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"` and
`-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`. `ccache`, `ninja`, and
`mold` speed up rebuilds. In-app *Rebuild & restart* builds `RelWithDebInfo`,
re-runs a pass if sources changed mid-build, and installs only a build of an
unchanged tree. Developer notes:
[docs/desktop/build-and-test.md](../../docs/desktop/build-and-test.md).

Qt 6.4 is the floor. Guard newer APIs with
`#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)` and verify with
`tools/qt64-build-check.sh [ubuntu-24.04]`, which builds against each
distribution's own Qt in a private sysroot.

Never run ForkMesh as root.

## KVM-isolated coding workspace

On Linux, coding agents can run in a Lima QEMU/KVM guest while the UI stays on
the host. Only the launch directory (mounted at the same path), the Git metadata
it needs, and a temporary agent-worktree root are shared; the home directory,
sockets, SSH agent, and unrelated environment are not. It needs Lima 2.0+,
QEMU, and `/dev/kvm`; there is no software-emulation fallback.

Launch ForkMesh from the narrowest directory holding your repositories (the
home directory is refused), enable **Settings → General → KVM workspace**,
choose **Prepare / start VM**, sign in to agents in **Open VM shell**, then
restart. Toggling the mode requires a restart; repositories outside the launch
directory are rejected while it is active.

## First run and navigation

A first launch creates the node identity, connects to the default relay, and
syncs the ForkMesh project. The left rail holds the main areas: Chat, Repos,
Code, Issues/Pulls/Actions, Agents, Network, Team, Pings, Health, Settings,
and others. Add a project with **Repos → Add local repo**; contribute elsewhere
with **Repos → Fork**.

**Network → Hosts** can create managed Vultr mirrors and optionally size the
fleet automatically to a target healthy count. Only instances this client
created are ever destroyed; an unreachable catalog makes no change.

### Team and teammate run modes

**Team** loads teammate definitions from `.forkmesh/teammates/`. Each teammate
runs on demand, on a bounded cron cadence, on repository events, or in
**Continual** mode, and only while the desktop is open. A continual teammate
runs one bounded cycle, then waits at a decision card (Yes/No, or three
options) before the next; **Stop work** pauses it. Permission mode, repository
grants, concurrency limits, and any VM boundary apply to every cycle. A Git
worktree separates branches but is not a sandbox.

### Multiple relay instances

The relay selector holds any number of named relay instances, each with its
own remembered account. The active relay is a security boundary: catalogs,
account requests, bearer capabilities, channel membership, chat history, and
queued agent work are scoped to its local instance id and exact origin.
Switching clears active credentials first; chat never fails over to another
relay. Bearer tokens are never written to `QSettings`. Each repository record
belongs to the relay it was added under and is only synced with that relay.
Private channels and direct messages keep their device-to-device layer; see
[Chat encryption and integrations](security/sealed-chat.md).

### Connectors

Discord and Sentry connectors are Market packages. Discord uses a bot token
(never a user token) and the server's permissions; Sentry reads unresolved
issues read-only with an `org:read`/`project:read` token. Tokens live in
device-local package settings, never in logs, arguments, or chat. Removing the
package clears its token and cached data.

## Create a pull request

Within your own repository: create a branch under **Code → Branches**, commit
in **Code → Changes**, then select the branch and click **Create PR**. ForkMesh
records the commit series and a signed binary-safe patch. Agents work the same
way in isolated worktrees under `.worktrees/` in the project.

For another owner's repository: **Fork** it, commit a feature branch, open
**Pulls → New pull request**, set the target node, base, and head, and submit.
The signed patch is delivered to the owner's inbox; the relay holds it until
the owner is online. See [Contributing](../../CONTRIBUTING.md).

## Run a managed mirror

Do not use the desktop binary as a repository server; run the Go mirror daemon
per the [headless mirror runbook](operations/headless-mirror-refresh.md).

## Logs and diagnostics

- The footer CPU/RAM/stall indicator shows UI-stall details.
- **Network diagnostics** shows connections and recent endpoint results;
  per-request logging is opt-in in Settings.
- **Settings → Data** shows the data, preferences, mirror, and cache
  directories for the current OS.

Diagnostic journals (default layout):

```text
~/projects/forkmesh/logs/diagnostics/actions.jsonl   action lifecycle journal
~/projects/forkmesh/logs/diagnostics/stalls.log      UI freezes longer than 500 ms
```

Each `actions.jsonl` record names `source`, `function`, `routine`, `cwd`,
`queued_ms`, and an `outcome`. The data directory also holds `network_log.txt`
and crash records. Details:
[docs/desktop/diagnostics.md](../../docs/desktop/diagnostics.md).

When reporting a problem include `forkmesh --version`, OS and display server,
the triggering action, and a log excerpt with secrets and private content
removed.

## Common fixes

- **CMake cannot find Qt6:** install the Qt development packages; on macOS
  pass `CMAKE_PREFIX_PATH`.
- **Qt platform plugin error:** launch from a graphical session.
- **Stale CMake cache after moving the checkout:** `./desktop/run.sh clean`.
- **Repository actions are read-only:** fork it or add its local working tree.
- **Backup:** **Settings → Data → Export configuration**; **Control → Deploy →
  Export complete system…** writes clear-text secrets, so protect it.

Back to [Getting started](getting-started.md) or the
[documentation index](README.md).
