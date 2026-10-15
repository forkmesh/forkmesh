# Service plug-ins: background services in forkmesh-core

A service plug-in is a marketplace package whose manifest carries a `service`
block. `forkmesh-core` runs it as a separate, sandboxed process, so it keeps
working while the desktop window is closed, and the desktop, the CLI and (for
the methods it opts in) the owner's phones call it over the core's IPC.
Desktop UI plug-ins are a different thing: they are in-process Qt modules
shipped with the release ([Desktop package modules](README.md#desktop-package-modules)).

| | UI plug-in (package module) | Service plug-in |
| --- | --- | --- |
| Runs in | the `forkmesh` process | its own process, started by `forkmesh-core` |
| Needs the window | yes | no |
| Code | C++ against the host ABI, built with the release | any executable; `python3`, `sh`, `bash` or `node` scripts |
| Trust | ships inside the signed release only | Ed25519 `SIGNATURE` from a trusted signer, plus the user's consent |
| Loaded | when the package is installed | lazily on first call (`lazy`) or when the core starts (`boot`) |

Example: [`examples/service-plugins/repo-pulse`](../examples/service-plugins/repo-pulse)
(a Python service that watches local repositories and shows a phone card).

## Manifest

```json
"service": {
  "api": 1,
  "exec": "content/service/repo_pulse.py",
  "interpreter": "python3",
  "start": "boot",
  "methods": ["card", "status", "poll", "watch", "unwatch"],
  "events": ["changed"],
  "remoteMethods": ["card", "status", "poll"],
  "remoteEvents": ["changed"],
  "idleExitSeconds": 300,
  "memoryMB": 128
}
```

| Field | Rule |
| --- | --- |
| `api` | `1`. A core that speaks another version lists the package as `invalid` |
| `exec` | Clean relative path to a regular file inside the package (`{os}` and `{arch}` expand to `GOOS`/`GOARCH`); the marketplace also requires it under `content/` |
| `interpreter` | Optional: `python3`, `sh`, `bash` or `node`, looked up in `/usr/bin`, `/bin`, `/usr/local/bin`, `/opt/homebrew/bin`, then `PATH` |
| `start` | `lazy` (default: started by the first call, stopped after `idleExitSeconds` idle) or `boot` (started with the core, restarted after a crash) |
| `methods`, `events` | Names matching `^[a-z][A-Za-z0-9_]{0,63}$`; 1–64 methods, up to 64 events. Undeclared methods are refused and undeclared events dropped |
| `remoteMethods`, `remoteEvents` | Subsets of the above that the owner's phones may call and receive |
| `idleExitSeconds` | 10–86400, default 300 |
| `memoryMB` | 64–4096, default 512: the data-segment limit on Linux |

The only permission the core enforces itself is `service.network`; any other
`service.*` permission id is refused by the marketplace validator. All
permissions are shown to the user before consent.

## Trust, consent and updates

1. **Signature.** `SIGNATURE` (JSON, `schemaVersion` 1, `algorithm`
   `ed25519`) holds the signer's public key, the SHA-256 of every file except
   `SIGNATURE` and `.forkmesh-install.json`, and an Ed25519 signature over
   `forkmesh-package-signature-v1\n<id>\n<version>\n` followed by one
   `<sha256>  <path>\n` line per file in byte order. Symlinks, extra files,
   missing files or a changed byte make the package `invalid`.
2. **Trusted signers.** The node's own identity key (packages you sign
   yourself) and the keys in `config/marketplace/trusted-signers.json`
   (`{"signers":[{"key":…,"name":…}]}`). Anything else is `untrusted` and can
   not be enabled.
3. **Consent.** Nothing runs until the user enables it (Settings → General →
   STARTUP, or `core.plugins.enable`). The consent in `data/core/plugins.json`
   records the signer and the granted permission ids. An update keeps running
   only when it has the same signer and asks for no new permission; otherwise it
   becomes `needs_consent` and stops.
4. **Updates and removal.** The core rescans `data/marketplace/packages/`
   every 15 s (a stat of each manifest and `SIGNATURE`). A changed digest
   restarts the process on the new files; a removed package is stopped.

Tools: `forkmesh-core plugins sign <dir>` (signs with this node's key),
`plugins verify <dir>`, `plugins trust <key> [name]`, `plugins list`.

## Sandbox

| Measure | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Separate process, own process group, killed with the core | yes (`Pdeathsig`) | yes | process only |
| Environment reduced to `PATH`, `HOME`/`TMPDIR` in the plug-in's data folder, `LANG` and `FORKMESH_PLUGIN_*` | yes | yes | yes (+ system variables Windows needs) |
| `no_new_privs`, 256 open files, no core dumps, memory limit | yes (through `forkmesh-core plugin-shim`) | open files and core dumps | no |
| No network unless `service.network` is granted | yes: a private user and network namespace; when the kernel refuses one, the plug-in starts without it and `networkIsolation` is `false` in `core.plugins.list` | no | no |

Working directory: `data/core/plugins/<id>/`. Stderr goes to
`logs/core/plugins/<id>.log` (1 MiB, one rotation). A stdout line over 4 MiB
stops the plug-in; more than 200 events in a second are dropped. The sandbox
does not hide the user's files: a plug-in can read what the user can read.

## Protocol (newline-delimited JSON on stdin and stdout)

| Direction | Message |
| --- | --- |
| core → plug-in | `{"id":1,"method":"plugin.init","params":{"api":1,"id","version","dataDir","grants","networkIsolated"}}`; answer within 10 s |
| core → plug-in | `{"id":N,"method":"<method>","params":{…}}` for each call |
| core → plug-in | `{"method":"$/cancel","params":{"id":N}}` when the caller cancels or the call times out (2 min) |
| core → plug-in | `{"method":"plugin.shutdown"}`, then stdin closes; SIGTERM after 3 s and SIGKILL 2 s later |
| plug-in → core | `{"id":N,"ok":true,"result":{…}}` or `{"id":N,"ok":false,"error":"<code>"}` |
| plug-in → core | `{"event":"<event>","data":{…}}`, broadcast as `plugin.<id>.<event>` |
| plug-in → core | `{"event":"$/progress","data":{"id":N,…}}`, forwarded to the caller as `core.progress` |
| plug-in → core | `{"id":N,"method":"host.log","params":{"text"}}`, `host.info`; every other method answers `forbidden` |

## Core methods

| Method | Who | What |
| --- | --- | --- |
| `core.plugins.list` | desktop, CLI | Every service package with `state` (`invalid`, `untrusted`, `disabled`, `needs_consent`, `stopped`, `running`, `failed`), permissions, signer, pid, restarts, `networkIsolation` |
| `core.plugins.enable {id, grants?}` | desktop, CLI | Consent; `grants` defaults to every requested permission and must cover them all |
| `core.plugins.disable`, `.restart {id}`, `.rescan` | desktop, CLI | |
| `plugin.<id>.<method>` | desktop, CLI; phones for `remoteMethods`, owner only (`not_owner`) | Starts a lazy plug-in, then calls it |
| `plugins.list` | owner's phones | Enabled plug-ins that expose `remoteMethods` |
| Event `core.plugins.changed` | desktop | The list after every change |

A `boot` plug-in that exits is restarted after 1, 2, 4… s (at most a minute);
five exits in ten minutes mark it `failed` until `core.plugins.restart`. While a
call is in flight the plug-in host reports busy, so a lingering core does not
exit under it. A `boot` plug-in only keeps running without the window while
the core does: turn on background mode or login start for that.

## Phone mini-apps

A plug-in becomes a mini-app on iOS by exposing a `card` method in
`remoteMethods`. The card is data, not code (App Store rule 2.5.2), rendered by
the app's own SwiftUI:

```json
{"title": "Repository pulse", "subtitle": "2 watched",
 "rows": [{"id": "src", "label": "src", "value": "main @ 1a2b3c", "tone": "neutral"}],
 "actions": [{"id": "poll", "title": "Check now", "method": "poll", "destructive": false, "confirm": ""}],
 "refreshSeconds": 300}
```

Tones are `neutral`, `good`, `attention`, `warning`. The phone keeps at most
50 rows and 6 actions, drops actions whose method is not a remote method, and
caches the last card. See [native/ios/docs/mini-apps.md](../../native/ios/docs/mini-apps.md).
