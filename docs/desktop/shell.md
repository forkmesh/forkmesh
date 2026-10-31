# The empty shell

Part of [the desktop docs](README.md). Code: `desktop/src/CorePackageServices.*`,
`desktop/src/MainWindowCorePackageServices.cpp`, `desktop/src/WebEngine.*`,
`desktop/src/WebEngineQt.cpp`, `projects::StarterPack::Empty`
(`desktop/src/ProjectRegistry.*`, `MainWindowProjectPicker.cpp`). Tests:
`forkmesh-core-package-services-tests`, `forkmesh-web-engine-module-tests`,
`forkmesh-app-paths-tests`.

The goal is a completely modular ForkMesh: every feature a plugin, and an
empty shell that takes almost no memory, with everything added from the
Market. This page is where that stands on the desktop, what the shell still
does, and what is left. Plugin packaging is in
[package-modules.md](package-modules.md).

## Starting empty

The new-project dialog offers a fourth plugin pack, **Empty**: "Only the shell,
the Market and Settings. Nothing runs until you add it from the Market."
`projects::starterPackages(StarterPack::Empty, ...)` returns nothing, and the
dialog seeds every default-installed package (`install.default` and
`installedBefore`) as removed, as it does for the other packs. Everything else
is added from the Market, one plugin at a time.

The home project always counts as a legacy project
([core-plugins](../marketplace/core-plugins.md#projects)), so it keeps every
feature it had; an empty shell is a new project (`forkmesh --project <slug>`,
or the project switcher).

## Core services follow their packages

Removing a switch package stops its timers, sockets and feeds, not just its
buttons. `CorePackageServices` is a small registry of the core's background services. Each service names the
packages that need it and has a start and a stop hook; it runs while at least
one of those packages is shown on this desktop (`packageSurfaceShown`: installed,
with its dependencies, and placed here). `syncPackageRuntime`, which runs on
every install, removal and account sync, calls `syncCorePackageServices`, so a
service starts the moment its package is added and stops the moment the last
one is removed, without a restart. The log says which:
`[core services] started: …; stopped: …; running: …`.

| Service | Packages | Runs |
| --- | --- | --- |
| `agent-watchers` | `agents`, `desktops` | the 2 s machine-agent scan and the 1 s cloud-agent watch |
| `agent-housekeeping` | `agents` | the hourly recycle-bin sweep and the agent/branch/worktree parity watch |
| `local-mesh` | `chat`, `desktops` | the local mesh room (UDP beacons, TCP links, Bluetooth) and with it the phone's LAN control channel |
| `organization-tasks` | `tasks`, `agents` | the one task-catalog read 25 s after start |
| `host-log-feeds` | `hosts`, `logs` | a log stream from every saved host, 15 s after start |
| `fleet-log-feeds` | `logs` | the fleet nodes' log feeds, the fleet inventory and the deploy log follower, 15 s after start |

`startLocalMesh` refuses to start the mesh while its service is off, so the
reachability watch and the session start cannot bring it back behind the
package's back. Stopping it is the same silenced stop the window uses when it
closes. A phone can still reach the desktop through the relay socket
(`onNodeControlFrame`); only the LAN path needs Chat or Desktops.

The World voice bridge is created the first time someone asks for a pairing
code (`createWorldSpeechPairing`), not at start.

### Left running on purpose

- **Chat retention.** Expired chat history is still pruned every five minutes
  with Chat removed: removing a plugin must not keep messages past their
  expiry.
- **Repositories, mirrors, relay sync and auto-update.** Repos is core, so the
  mirror safety sync, the relay inbox sync and the update check stay.
- **Diagnostics.** The stall watchdog and the footer's diagnostics tick stay;
  they are how a freeze or crash gets reported.

## Qt WebEngine loads on first use

Qt WebEngine is a module on Linux (`packages/web-engine.so`), loaded the first
time the Browser tab or an agent opens a page. Linking it cost about 36 MB of
resident memory in every process. See
[browser](browser.md#the-engine-is-a-module).

## Measurements

Release build, offscreen, a fresh sandboxed `HOME` and `XDG_*_HOME`
(`env -i`), resident memory (`VmRSS`, `smaps_rollup`) 10 s after start, medians
of three runs:

| Profile | Services always on | Services follow packages | + Qt WebEngine as a module |
| --- | --- | --- | --- |
| Home project (legacy: every feature installed) | 207 MB (PSS 154) | 207 MB (PSS 154) | 179 MB (PSS 142) |
| New project, Standard pack | | 185 MB (PSS 132) | 158 MB (PSS 121) |
| New project, Empty pack | | 183 MB (PSS 130) | 155 MB (PSS 117) |

Gating the services saves work (no 1 s and 2 s agent timers, no mesh beacons,
no log feeds or task reads) more than memory. Before the WebEngine split the
Empty project held 66 MB heap and anonymous memory, 48 MB of the `forkmesh`
binary, 18 MB of `libQt6WebEngineCore` plus Qt Quick and QML, and the rest Qt,
OpenSSL and system libraries. A Qt Widgets program that only shows a window is
24 MB, about the floor for a Qt shell.

## What is still core

The shell is not empty yet. In order of what it would save:

1. **The session.** `startSession` (5 s after start) builds the whole Chat page
   and connects the relay `ServerNode` even without Chat, because the chat
   models (`m_channelList`, `m_dmList`) are widgets on that page and the relay
   socket also carries presence, pings and agent prompts. Splitting the chat
   models out of the page and starting the relay room only with `chat` is the
   next step; the session start adds about 22 MB.
2. **Feature code in the binary.** About 48 MB of the `forkmesh` binary is
   resident at start. The switch packages (Agents, Chat, Issues, Pull requests,
   Network, Hosts, Stats and the rest) are still window code in `MainWindow*.cpp`;
   each moves out as a module page the way Tasks, Notes and Calendar did
   ([package-modules](package-modules.md#plugins-built-apart-from-the-window)).
3. **Core-linked plugins.** `capture`, `discord`, `ledger` and `tour` are still
   linked into the app ([plugins](../marketplace/plugins.md#plugins-the-core-still-links)).
4. **macOS and Windows** link every plugin and the web engine into the app;
   modules there need a shared plugin SDK library.
5. **Phones** compile every package into the app and hide what is not
   installed ([plugins](../marketplace/plugins.md#phones)).
