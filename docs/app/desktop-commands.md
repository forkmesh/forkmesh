# Desktop commands (phone to desktop, through the relay)

Part of [the App docs](README.md). Code: `app/src/desktop_commands.py`,
table `desktop_commands` (migration `0204_desktop_commands.sql`), tests
`app/tests/test_desktop_commands.py`.

The phone apps (`native/android`, `native/ios`) show
"Request … on desktop" buttons on pull requests, Action runs and releases.
Each one posts a signed request that the owner's desktop picks up and asks the
owner to approve. Until 2026-09-24 no server implemented the route, so every
button failed with HTTP 404.

## Routes

| Method and path | Caller | Auth |
| --- | --- | --- |
| `POST /api/repo/<owner>/<repo>/desktop-commands` | phone | Ed25519 signature by one of the owner account's signing keys |
| `GET /api/repo/<owner>/<repo>/desktop-commands` | owner's desktop node | node-signed `ts`/`sig` query (`_authorize_owner`) |
| `POST /api/repo/<owner>/<repo>/desktop-commands/ack` | owner's desktop node | same; body `{"ids": [..]}` |

Queue request body: `{ownerAccount, command, target, ts, sig, payload}`. The
signed string is

```
forkmesh-desktop-command-v1\n<owner>\n<repo>\n<command>\n<target>\n<ts>
```

with `<owner>`/`<repo>` exactly as in the URL, so a request signed for one
repository fails verification on any other.

Checks, in order: `ownerAccount` equals the URL owner (403); `command` is on
the allow-list and `target` has its shape (400); `ts` within 5 minutes (401
`stale_request`); the signature verifies against the owner's primary key or an
enabled device key (`_owner_signing_pubkeys`, 401 `invalid_signature`); the
signature was not seen before (409 `replayed_request`); at most 50 pending per
repository (429). Success answers `{"ok": true, "queued": <pending>}` and rings
the owner's node socket with topic `desktop-commands` (`notify_repo_host`).

Commands and targets:

| Commands | Target | Stored payload |
| --- | --- | --- |
| `pull.merge`, `pull.merge_delete`, `pull.fix_agent`, `pull.close`, `pull.reopen`, `pull.delete` | `pull:<n>` | `{"pullNumber": n}` |
| `action.rerun` | `run:<id>` | `{"runId": "<id>"}` |
| `release.draft`, `release.notes`, `release.create`, `release.publish` | `release:<tag>` | `{"tag": "<tag>"}` |

The v1 string does not cover the client's `payload`, so the relay discards it
and stores only what the signed target implies. Clients' extra hints (`base`,
`head`, `workflowPath`, `channel`) never reach the desktop; it looks them up.

The drain returns each pending command with its `sig`, `ts`, the `signer` key
and the rebuilt `canonical` string, so the desktop can verify the phone's
signature itself instead of trusting the relay. Acked rows stay (marked done)
until they are a day old so a replay inside the retention window is still
refused; everything older than a day is deleted on the next queue write.

## The desktop consumer

`desktop/src/MainWindowDesktopCommands.cpp` drains the queue on the
`desktop-commands` node event, verifies each `canonical`/`sig` itself
(`desktop/src/DesktopCommands.*`), and asks the owner to approve before running
the matching action, then acks. Since 2026-10-01 `release.create` and
`release.publish` signed by a key the desktop already knows run without that
prompt, and `release.create` no longer opens the Draft a new release dialog: it
cuts the release with the dialog's defaults
([../tools/mcp-releases.md](../tools/mcp-releases.md)). Settings →
Pings → "Create and publish releases my phone asks for without asking
here" turns the unattended run off.

### Org-aliased repositories (2026-10-12)

The relay keys the queue by the account that backs the repository, so
`forkmesh/forkmesh` lives under `jett/forkmesh`. The desktop used to drain only
repositories whose own owner was the account, so every request for an
org-fronted repository sat in the queue until it expired. It now drains each
repository whose catalog owner is the account under that account
(`fetchDesktopCommands(account, name)`), verifies against the account as
before, and runs the command against the local record
(`desktopCommandRepoOwner`: the exact owner first, then the one whose catalog
owner matches). A terminal can send the same requests with
`tools/forkmesh_release_request.py` ([../tools/mcp-releases.md](../tools/mcp-releases.md)).

## Not built yet

- The phone shows Create release only on an existing release card, so it
  cannot yet name a new tag; an agent's `create_release` can.
- Organization repositories: requests must be signed by the account that owns
  the backing repository; org admins are not accepted.
- The native parity plan's relay stop command for agent runs
  ([native PARITY.md](../native/PARITY.md)) would be a new command on this
  allow-list.
