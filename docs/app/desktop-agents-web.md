# Desktop agents on the web Agents tab

Part of [the App docs](README.md). When the owner's desktop is running, a
repository's Agents tab lists that desktop's agents and manages them: live
transcript, prompt, stop, continue, diff, merge and clean up, delete.

## Transport

The browser uses the same end-to-end encrypted control protocol as the phone
apps ([desktop networking](../desktop/networking.md)); the relay only forwards
`{"type":"control"}` chunks (`app/src/node_control.py`) and cannot read or
forge them. Browser side: `app/public/desktop-control.js`, UI in
`06-repo-content.js`. Desktop side: `agents.*` (`MainWindowLocalControl.cpp`)
and `agentops.*` (`MainWindowLocalControlAgentOps.cpp`).

## Security rules

- Each browser holds a non-extractable Ed25519 key (IndexedDB) registered via
  `POST /api/accounts/devices/browser-control`, signed over
  `forkmesh-browser-control-v1\n<account>\n<pubkey>\n<ts>`. It becomes an
  `account_devices` row of kind `browser_control` with only `desktop_control`;
  it can never sign owner writes.
- Revocable in Settings → Devices (`device_revoked`); at most
  `BROWSER_CONTROL_MAX_DEVICES` (20); a key bound as another kind is refused
  (`device_key_conflict`). The browser accepts a welcome only from the
  account's listed desktop keys.
- The desktop never persists a web peer, re-checks it on every connect, and
  allows `agentops.*` only to the owner.

## Actions

`agents.list` (re-read on `agents.changed`), `agents.transcript` (deltas via
`since`), `agents.prompt`, `agents.stop`, `agents.continue`, `agentops.diff`
(capped at `kMaxDiffChars`), and `agentops.prepare` followed by
`agentops.merge` / `agentops.delete` with a one-use, one-minute token. Delete
refuses a running agent. New agents still start through the website queue.
[Settings → Agents & runners](settings-agents-and-runners.md) and the phones
([native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)) use the same channel.

Requires Ed25519 and X25519 in WebCrypto. A relay reconnect ends all channels.

Tests: `app/tests/test_desktop_control_js.py`, `app/tests/test_mobile_node_devices.py`,
`desktop/tests/test_local_control*.cpp`,
`app/browser_tests/tests/dashboard-desktop-agents.spec.js`.
