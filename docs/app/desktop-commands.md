# Desktop commands (phone to desktop, through the relay)

Part of [the App docs](README.md). Phone apps post signed "Request … on
desktop" commands; the owner's desktop drains, verifies, asks for approval and
runs them. Code: `app/src/desktop_commands.py`, table `desktop_commands`
(migration `0204_desktop_commands.sql`), tests `app/tests/test_desktop_commands.py`.

## Routes

| Method and path | Caller | Auth |
| --- | --- | --- |
| `POST /api/repo/<owner>/<repo>/desktop-commands` | phone | Ed25519 signature by an owner signing key |
| `GET /api/repo/<owner>/<repo>/desktop-commands` | owner's desktop | node-signed `ts`/`sig` |
| `POST /api/repo/<owner>/<repo>/desktop-commands/ack` | owner's desktop | same; `{"ids": [..]}` |

Body `{ownerAccount, command, target, ts, sig, payload}`, signed as
`forkmesh-desktop-command-v1\n<owner>\n<repo>\n<command>\n<target>\n<ts>`.
Checks: owner matches URL (403), allow-listed command and target (400), `ts`
within 5 minutes (`stale_request`), valid signature (`invalid_signature`), no
replay (`replayed_request`), at most 50 pending (429). Success rings the
owner's node socket (topic `desktop-commands`).

| Commands | Target |
| --- | --- |
| `pull.merge`, `pull.merge_delete`, `pull.fix_agent`, `pull.close`, `pull.reopen`, `pull.delete` | `pull:<n>` |
| `action.rerun` | `run:<id>` |
| `release.draft`, `release.notes`, `release.create`, `release.publish` | `release:<tag>` |

`payload` is unsigned, so the relay discards it. The drain returns `sig`, `ts`,
`signer` and `canonical` so the desktop verifies the phone itself. Acked rows
are kept a day for replay protection.

## Desktop consumer

`desktop/src/MainWindowDesktopCommands.cpp` verifies (`DesktopCommands.*`),
asks for approval, runs and acks. Release create/publish from a known key may
run unattended (a Pings setting disables it;
[../tools/mcp-releases.md](../tools/mcp-releases.md)). The queue is keyed by
the account backing the repository; only that account can sign.
