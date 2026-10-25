# Desktop: the TMP fast disk and slow USB links

Part of [the desktop docs](README.md).

## Why

A box that runs ForkMesh from an external USB SSD sends every agent build,
ccache write and Action sandbox through that cable. On 2026-10-24 that was what
froze a workstation: the system disk was a 10 Gbps SanDisk plugged into the
board's internal USB 2.0 hub, linked at 480 Mbps, and its link reset under the
write load of parallel C++ builds until the whole box stopped. A spare internal
disk gives those throwaway writes somewhere fast to go, and a drive on the
wrong port should be pointed out rather than found in a kernel log.

## The TMP disk

Any mounted, writable filesystem labelled `TMP` (`/dev/disk/by-label/TMP`,
matched against `/proc/self/mounts`) is used. ForkMesh writes only inside
`<mount>/ForkMesh`:

| Folder | What goes there |
| --- | --- |
| `scratch/<key>/.scratch/<worktree>` | an agent run's scratch folder and its `tmp/` (`FORKMESH_AGENT_SCRATCH`, `TMPDIR`) |
| `ccache` | `CCACHE_DIR` for agents, capped by `CCACHE_MAXSIZE=40G` |
| `actions/sandbox`, `actions/cache` | Action run trees and per-repository build caches |

`<key>` is the first 16 hex digits of the SHA-1 of the worktree root
(`<repo>/.worktrees`). Each key folder holds the same `.gitignore` marker as
`.worktrees`, so `trustedScratch()` and `isAgentRoot()` accept it, and a
`worktree-root` file naming the root it belongs to, which is how startup
cleanup knows what is still live.

Worktrees themselves stay in `<repo>/.worktrees`: Git, the access rules and
the parity check expect them there, and they hold the work an agent produces.
Projects, settings, history and the identity key never move.

Code: `desktop/src/FastScratchDisk.h` (probe, activation, cleanup, erase),
`AgentScratch.h` (`rootFolder()` picks the TMP disk when it is active;
`foldersFor()` returns both places so removal finds a folder made before the
disk appeared or after it left), `ActionStore::heavyRoot()`, and
`MainWindowFastDisk.cpp`.

### When it switches

`initFastScratchDisk()` probes synchronously before the Actions store is
created, so the first launch already uses it. A timer re-probes every minute
off the GUI thread; plugging the disk in, mounting it, or turning the setting
off takes effect for the next agent or Action that starts. A running agent
keeps the folder it launched with. Old scratch on the system disk is left for
its worktree's normal removal, and old Action sandboxes there are still
reclaimed at startup (`legacySandboxDir()`).

### Keeping it clean

At startup and then hourly, `cleanUnused()` removes:

- a key folder whose `worktree-root` is missing or no longer exists;
- a scratch folder whose worktree is gone (this also catches
  `.discarded-<ms>` leftovers from interrupted deletes);
- anything in a live scratch folder's `tmp/` not modified for a day.

The Action sandbox sweep (`ActionRunner::reclaimSandboxes`) already runs on
whatever `sandboxDir()` returns. The node deploy agent looks for Deploy nodes runs
in both sandbox folders ([auto-deploy](../deploy/auto-deploy.md#where-the-agent-finds-runs)). Every delete goes through
`safedelete::removeTree` with the folder's own parent.

### Erasing it

Settings > TMP disk has **Erase TMP disk…**. It re-probes before deleting and
refuses unless the same device is still labelled `TMP` and still mounted at
the same place, then deletes every top-level entry except `lost+found`, each
through `safedelete::removeTree` with the mount point as parent. The setting
`storage/useTmpDisk` (default on) turns the redirect off without touching the
disk.

## Debug panel tiles

The bottom debug bar has a quad with three tiles:

| Tile | Shows |
| --- | --- |
| DATA (top left) | how much is on the TMP disk, or EMPTY; colour by how full it is |
| USB (top right) | OK, or SLOW when a USB drive is on a slower link than it and the computer support |
| TMP (bottom left) | ON (in use), OFF (mounted but turned off), ERR (labelled but not mounted, or read-only), or a dash when no TMP disk exists |

Clicking any of them re-checks at once.

## Slow USB links

`desktop/src/UsbLinkCheck.h` reads `/sys/bus/usb/devices`. For each
mass-storage device (an interface of class `08`) it estimates what the drive
can do from its USB version (3.x means at least 5 Gbps), a UAS interface in its
descriptors (protocol `0x62`, also at least 5 Gbps), and its name (`Gen 2` or
`3.1`/`3.2` means 10 Gbps, `Gen 2x2`, USB4 or Thunderbolt 20 Gbps). It caps that
at the fastest root hub on the machine and reports the drive when its link is
slower. A drive on a USB 2.0 link reports version 2.10, so the UAS interface
and the name are what catch the common case. Each slow drive is logged and
shown as a notice once per session, and the USB tile keeps the full text in
its tooltip.
