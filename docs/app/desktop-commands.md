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
Checks: `ownerAccount` is the serving account after the org-alias rewrite, or
holds write access in an organization that links the repo (`_org_write_allowed`,
the org push gate), else 403 `not_authorized`; allow-listed command and target
(400), `ts` within 5 minutes (`stale_request`), valid signature by one of
`ownerAccount`'s keys (`invalid_signature`), no replay (`replayed_request`), at
most 50 pending (429). The request is queued for `ownerAccount`'s own desktop
(the serving account's queue, or the member's), and success rings that
account's node socket (topic `desktop-commands`).

The `<owner>` signed is the name the phone addressed: the serving account, or an
organization alias (`_public_repository_owner`). The relay keeps it as
`repoOwner` and the drain's `canonical` uses it.

| Commands | Target |
| --- | --- |
| `pull.merge`, `pull.merge_delete`, `pull.fix_agent`, `pull.close`, `pull.reopen`, `pull.delete` | `pull:<n>` |
| `action.rerun` | `run:<id>` |
| `release.draft`, `release.notes`, `release.create`, `release.publish` | `release:<tag>` |

`payload` is unsigned, so the relay discards it. The drain returns `sig`, `ts`,
`signer` and `canonical` so the desktop verifies the phone itself. Acked rows
are kept a day for replay protection.

### Organization repos

Phones used to refuse locally unless the signed-in account equalled the repo
owner name, so a member (or the serving account itself, when the row named the
org or a core node such as `fmnext-n1`) saw "Only the repo owner can request
desktop control for this repo." (2026-10-14). Now the phones sign as the
signed-in account and name the repo by its serving account when that is the
signed-in one, else by its public name (`Repository.displayOwner`). The relay
decides who may ask. Screens that only hold a route `owner/name` (Actions rerun,
Releases) sign that owner as given: when it is another node's name, the
member's desktop cannot match it to a local repo and reports "repository is not
open on this desktop".

## Desktop consumer

`desktop/src/MainWindowDesktopCommands.cpp` verifies (`DesktopCommands.*`),
asks for approval, runs and acks. Release create/publish from a known key may
run unattended (a Pings setting disables it;
[../tools/mcp-releases.md](../tools/mcp-releases.md)). The queue is keyed by
the signed-in account; the desktop checks `ownerAccount` is that account,
verifies the signature over `repoOwner` (falling back to the queue owner for
rows queued before it existed) and runs the command on the local repo whose
owner is `repoOwner` (`desktopCommandRepoOwner`).
