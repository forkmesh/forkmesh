# Reticulum on the phones

Part of [the native docs](README.md). The phone clients of the
[Reticulum package](../marketplace/reticulum.md) (`reticulum`, entrypoint
`builtin:reticulum`). The wire protocol, limits and safety rules are the
desktop's; this page only lists what the phones do differently.

## iPhone

Route `.reticulum` (`forkmesh://reticulum`, Home tile **Reticulum** in the
Personal group, icon `antenna.radiowaves.left.and.right`), wired like the
OpenWrt Router ([openwrt.md](../marketplace/openwrt.md#phones)).

### Files

| Part | Files |
| --- | --- |
| Packets, framing, identities, announces, path requests, HKDF, msgpack reader, aspects, display names, discovery and stamp value | `ForkMeshCore/Sources/FMPackages/ReticulumProtocol.swift` |
| AES-256-CBC, Token, identity encryption, msgpack writer, IFAC, proofs, the SHA-256 midstate stamps use | `ReticulumCrypto.swift` |
| LXMF pack, unpack and verify, announce app data, stamps, resource advertisements | `ReticulumLxmf.swift` |
| Messaging: queue, links, resources, proofs, retries, persistence JSON | `ReticulumMessenger.swift` |
| Interfaces, reconnect backoff, local destinations, routed sending, destination table, dedupe, path lookups, counters, history, config import, the link and scheduler protocols | `ReticulumNode.swift` |
| Map groups, number formats, the radial map layout | `ReticulumFormat.swift` |
| `@MainActor @Observable` `ReticulumHub`: settings, actions, tiles and every list the screen shows | `ReticulumHub.swift` |
| `NWConnection` link, connector, the process-wide hub | `ForkMesh/Platform/ReticulumNetworkLink.swift` |
| Screen, tabs, tiles, traffic chart, network map, Messages tab and conversation | `ForkMesh/Features/Reticulum/` |
| Tests | `ForkMeshCore/Tests/FMPackagesTests/Reticulum*Tests.swift` |

The code ports `ReticulumProtocol`, `ReticulumLxmf`, `ReticulumMessenger` and
`ReticulumNode` line for line, with SHA-256, HMAC, X25519 and Ed25519 from
FMCrypto (pure Swift, so it runs on Linux). FMCrypto has no AES-CBC, so
`ReticulumCrypto.swift` carries a small AES-256 checked against FIPS-197 and
RNS Tokens. Stamps hash the 3000-round workblock once and then only the
candidate (a SHA-256 midstate), on a background queue through
`ReticulumScheduler.background`. The node is single-threaded: links and timers
come in through `ReticulumLink`, `ReticulumConnector` and `ReticulumScheduler`,
which the app fills with `NWConnection` and the main queue and the tests with
fakes and a virtual clock. Messaging behaves as
[Messaging](../marketplace/reticulum.md#messaging) and IFAC as
[Interface access codes](../marketplace/reticulum.md#interface-access-codes)
describe.

### Storage

The identity private key (hex, the RNS identity file layout) is in the
Keychain through `SecureStore`, under
`market/packages/reticulum/data/identity`. `interfaces`, `announceName`,
`announceMinutes`, `answerPathRequests` and `networkOn` are in the project's
`KeyValueStore` under the same prefix. An interface's IFAC passphrase is not in
that JSON: it is in the Keychain under `passphrase/<interface id>`, removed
with the interface. Messages (the desktop's `messages` JSON, 5,000 messages,
4 MB of attachments per message) are in `messages.json` in the project's
`Application Support/.../reticulum/` folder, saved 1.5 s after a change and
when the app goes to the background, since UserDefaults is no place for
megabytes. Destinations, events and history stay in memory with the desktop's
caps.

### Differences kept on purpose

- **TCP only.** There is no `rnsd` on iPhone, so there is no shared-instance
  interface. UDP broadcast needs Apple's multicast networking entitlement and
  a local-network prompt, so UDP is left out too. Imported shared-instance
  and UDP entries are listed as skipped; a stored one shows **Failed**.
- **No request firewall.** The phone app has no firewall to ask, so the
  connection opens when you add or switch on an interface (the node's
  authorizer hook stays for when it gets one). iOS may show its own
  local-network prompt for a node on your Wi-Fi.
- **No default interface.** The desktop starts with the shared instance; the
  phone starts empty with the onboarding card.
- **Import** reads a file you pick in Files, not `~/.reticulum/config`.
- **Touch.** A tap on the map selects a destination, instead of hovering for a
  tooltip; drag across a traffic chart to read it; tables become cards and rows
  with a sort menu; right-click menus become long-press menus. Ripples follow
  Reduce Motion.
- **Wording** says "this iPhone" (or iPad) where the desktop says "this
  desktop" in the page; events use the desktop's text.
- **Messages.** The conversation opens as its own screen with the composer
  pinned at the bottom (the desktop splits list and chat side by side).
  Attachments save through the share sheet. A message that arrives while you
  are not reading that conversation posts a local notification only when
  notifications are already allowed (the app never asks for them here), and
  the unread count shows on the Messages tab and tile, not on the Home tile.
- **Lifetime.** The node lives as long as the app process, not the screen. iOS
  suspends it in the background and the TCP connections drop; when the app
  comes back, every switched-on interface that is not online reconnects, as the
  desktop page does on refresh.
- **No search hits** in the app search yet.

### Tests

```sh
cd native/ios/ForkMeshCore
FM_ONLY=FMPackages swift build --build-tests --scratch-path <scratch>
FM_ONLY=FMPackages swift test --skip-build --scratch-path <scratch> --filter Reticulum
```

They reuse the desktop's Python RNS 1.5.7 vectors plus Python-made Token,
identity-encryption and IFAC vectors, and cover framing, msgpack bounds,
config import, a session against a fake peer on a virtual clock (announces,
dedupe, path lookups and answers, backoff 5 s doubling to 120 s, the 10 s
connect timeout, caps), routed HEADER_2 sending, one session over a real TCP
socket, and two nodes messaging through a relay by packet, link and resource,
open and with IFAC, while a node with the wrong passphrase hears nothing.
`testInteropWithPythonRNS` and `testLxmfInteropWithPython` read the desktop's
`RETICULUM_INTEROP_*` and `RETICULUM_LXMF_*` variables, so the desktop's
Python harnesses drive them (point the harness at a script that runs the test
binary with `FMPackagesTests.ReticulumSocketTests/<test>`). On 2026-11-04
against LXMF with RNS 1.5.7 both directions delivered and verified every
message by packet, link and resource with stamp cost 0, with 8 and over IFAC.
Python RNS loses a message when two resources run at once on one link (a part
of the second marks the finished first one as still transferring, so LXMF
drops it after proving it), which cost 8 caused by sending "medium" and "long"
together; the messenger therefore sends one resource per link at a time and
queues the rest ("Waiting for the previous transfer"), starting the next when
the current one is proved, refused or stalls. `ReticulumStampTests` compares
message ids, workblocks, the msgpack round salts and 342 stamp values with
LXMF's `LXStamper`.
