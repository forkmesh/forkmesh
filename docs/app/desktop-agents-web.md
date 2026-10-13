# Desktop agents on the web Agents tab

Part of [the App docs](README.md). When the owner's ForkMesh desktop is
running and signed in, a repository's Agents tab on app.forkmesh.com lists
that desktop's own agents for the repository, next to the sessions started
from the website, and manages them fully: live transcript, steer or continue
with a prompt, stop, view the diff, merge and clean up, and delete.

Before 2026-09-28 the tab listed only website-queued sessions
([web-composer.md](web-composer.md), "Gaps"); agents started on the desktop
were visible only in the desktop app and the phone apps.

## How the browser reaches the desktop

The browser speaks the same end-to-end encrypted control protocol the phone
apps use away from home ([desktop networking](../desktop/networking.md),
"Phone control over the LAN", and [native PARITY.md](../native/PARITY.md)
WS7). The relay only forwards `{"type":"control"}` chunks between the
browser's account event socket and the desktop's node socket
(`app/src/node_control.py`); it cannot read or forge them.

| Piece | Code |
| --- | --- |
| Browser protocol (hello/welcome/auth, HKDF keys, AES-256-GCM frames, relay chunking, one shared socket for every desktop) | `app/public/desktop-control.js` |
| Browser key registration | `POST /api/accounts/devices/browser-control`, `_account_browser_control_bind` in `app/src/entry.py` |
| Desktop approval | `_account_device_check` also answers `owned` for an enabled `browser_control` device |
| Agents tab UI | `connectRepoDesktops`, `renderDesktopAgentDetail`, `runDesktopAgentAction` in `app/public/dashboard/js/06-repo-content.js` |
| Desktop methods | `agents.list|transcript|prompt|stop|continue` (`MainWindowLocalControl.cpp`), `agentops.prepare|merge|delete|diff` (`MainWindowLocalControlAgentOps.cpp`) |

### Browser identity

- Each browser generates a non-extractable Ed25519 key in WebCrypto and keeps
  it in IndexedDB (`forkmesh-desktop-control`, one per account). Without
  IndexedDB the key lives for the page only.
- It registers the key with the signed-in session: body
  `{pubkey, ts, sig, label}`, `sig` over
  `forkmesh-browser-control-v1\n<account>\n<pubkey>\n<ts>`. The server stores
  an `account_devices` row of kind `browser_control` with the single
  capability `desktop_control`. It is not an `owner_sign` key, so it cannot
  sign catalog writes, drains or anything else the owner signs.
- The row shows in Settings → Devices as "Browser desktop control" and is
  revoked there like any other device. A revoked key is refused
  (`device_revoked`) and the browser makes a new one.
- At most `BROWSER_CONTROL_MAX_DEVICES` (20) are enabled per account; binding
  a new one revokes the least recently seen.
- A key already bound to the account as another device kind (the desktop's or
  a phone's) is refused with `device_key_conflict`, so a browser can never
  overwrite a node key's capabilities.
- The answer lists the account's desktops: `desktops: [{key, node}]` from the
  enabled `desktop_node` rows (their label is the desktop's machine name,
  `machineNodeName()`, sent at login) plus the account's primary key. The
  browser accepts a welcome only from one of these keys.

### Which desktops the tab tries

The union of the owner route's `targets` with `runtimeMode: "desktop"` and the
`desktops` from the bind answer. On an organization repository (the
`agent-bots` route) the route's targets are other members' nodes, so the tab
tries only the bind answer's desktops: the signed-in account's own.
Since 2026-09-28 the "On desktop" section appears there too, so an
organization admin sees what their desktop is running for the repository; it
also shows when the member may not read the website sessions. Desktop catalog records may carry the
account name rather than the machine name, and the desktop answers only to
frames addressed to its machine name, so the bind list is what normally finds
it. Desktops that do not answer within 20 s are grouped on one "Not
reachable" line with a Retry button.

### Desktop side

- A connection whose hello says `platform: "web"` is never written to
  `paired-devices.json`. It is checked against the account
  (`/api/accounts/devices/check`) on every connect, so revoking the browser
  on the website locks it out at once. Phones keep the old behaviour.
- The agentops family is owner-only: the hello's `user` must be the desktop's
  account, which the browser sends.
- Every change is written to the desktop log with the browser's label.

## What the tab does

| Action | Method | Confirmation |
| --- | --- | --- |
| List (this repository only) | `agents.list`, re-read on each `agents.changed` push (debounced 400 ms) | — |
| Transcript | `agents.transcript` with `since` for deltas; kept to the last 200,000 characters | — |
| Send (steer a running agent, or resume a finished one) | `agents.prompt` | — |
| Stop | `agents.stop` | browser confirm |
| Continue | `agents.continue` | — |
| Diff | `agentops.diff`: `git diff <base>...<branch>` with `--numstat`, the worktree's uncommitted `git diff HEAD`, and untracked files; at most 400,000 characters in total (`kMaxDiffChars`), then `truncated` | — |
| Merge & clean up | `agentops.prepare {action:"merge"}` then `agentops.merge {token, typed}` | none: runs on click (since 2026-09-30) |
| Delete | `agentops.prepare {action:"delete"}` then `agentops.delete {token}`; the desktop runs `deleteAgentSessionAndCleanup` (task, worktree, branch) | browser confirm with the desktop's summary |

Merge and delete tokens are one-use and last a minute (the desktop's
`ConfirmationStore`). Delete refuses a running agent (`agent_running`).
Merge runs on the click with no confirmation: the desktop no longer asks for the branch
name to be typed back (`typeToConfirm` is not sent and `typed` is not
checked). Clients still echo an older desktop's `typeToConfirm` as `typed` so
the same one click works against it.

## Limits

- The browser needs Ed25519 and X25519 in WebCrypto (current Chrome, Edge,
  Firefox and Safari have both). Otherwise the tab says so and shows website
  sessions only.
- A relay socket reconnect ends every open channel (the frame counters cannot
  resume); the tab reconnects once by itself, then offers Retry.
- Starting a new agent still goes through the website queue (the start form);
  the desktop claims it as before.
- The home page's "Active agent sessions" panel does not use this yet.
- Settings → Agents & runners uses the same channel for the desktop's
  Claude Code and Codex accounts, usage and terminals
  ([settings-agents-and-runners.md](settings-agents-and-runners.md)).
- The phone apps reach the same desktops the same way, with the same
  actions ([native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md));
  `GET /api/accounts/desktops` gives them the desktops' keys and machine names.

## Tests

- `app/tests/test_desktop_control_js.py`: the key derivation and sealing
  vectors pinned in `desktop/tests/test_local_control.cpp`, a full handshake
  against a simulated desktop, a wrong desktop key, a refused browser, key
  rotation after revocation, and one shared socket for two desktops.
- `app/tests/test_mobile_node_devices.py`: the bind route and the device
  check.
- `desktop/tests/test_local_control.cpp`: a web browser is approved over the
  relay, re-checked on every connect, never remembered, and refused once
  revoked. `desktop/tests/test_local_control_agentops.cpp`: the delete action
  and the diff helpers.
- `app/browser_tests/tests/dashboard-desktop-agents.spec.js`: the Agents tab
  against a fake `/desktop-control.js` (list, transcript deltas, prompt, stop,
  diff, merge, delete). Set `DESKTOP_AGENTS_SCREENSHOT_DIR` to save
  screenshots.
