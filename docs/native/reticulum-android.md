# Reticulum on Android

Part of [the native docs](README.md). The Android client of the
[Reticulum package](../marketplace/reticulum.md) (`reticulum`, entrypoint
`builtin:reticulum`); the iPhone client is in
[reticulum-ios.md](reticulum-ios.md). The wire protocol, LXMF messaging,
interface access codes, limits and safety rules are the desktop's
([Messaging](../marketplace/reticulum.md#messaging),
[Interface access codes](../marketplace/reticulum.md#interface-access-codes));
this page lists what Android does differently.

Route `Route.Reticulum` (`forkmesh://reticulum`, `/dashboard/reticulum`),
Home tile **Reticulum** in the Personal group, icon `hub`, with the unread
message count as its badge, wired like the OpenWrt Router
([openwrt.md](../marketplace/openwrt.md#phones)): `Route`, `RouteCatalog`,
`RouteCodec`, `RouteParser`, `PackageGate`,
`MarketPackage.NATIVE_DESTINATIONS`, `MarketScreens.routeFor`,
`HomeShell.hubBadges` and `Features.install`. The Market shows it on the phone
once the manifest lists an `android` entrypoint and the `mobile` place.

## Files

| Part | Files (`native/android/`) |
| --- | --- |
| Packets, HDLC and KISS framing, identities, announces, path requests, HKDF, msgpack reader, aspects, display names, discovery and stamp value | `core/.../reticulum/ReticulumProtocol.kt` |
| X25519 exchange, Ed25519, Token (AES-256-CBC + HMAC), identity encrypt and decrypt, msgpack writer, array items, IFAC, proofs | `core/.../reticulum/ReticulumCrypto.kt` |
| LXMF pack, unpack, verify, announce data, stamps, resource advertisements | `core/.../reticulum/ReticulumLxmf.kt` |
| Messages, conversations, the send queue, links, resources, proofs, retries, persistence | `core/.../reticulum/ReticulumMessenger.kt` |
| Interfaces and IFAC, reconnect backoff, local destinations, routed sending, destination table, dedupe, path lookups, counters, history, snapshot, config import | `core/.../reticulum/ReticulumNode.kt` |
| `ReticulumTransport`, `ReticulumTimers`, the socket transport (TCP and UDP on `java.net`) | `core/.../reticulum/ReticulumTransport.kt` |
| Map groups, number formats, tiles, detail rows, sorting, bubble and composer text | `core/.../reticulum/ReticulumFormat.kt` |
| Process-wide `ReticulumState.shared`: settings, sealed secrets, actions, Compose state | `app/.../feature/reticulum/ReticulumState.kt` |
| Screen, tabs, interface editor | `app/.../feature/reticulum/ReticulumScreen.kt` |
| Messages tab, bubbles, composer, New message | `app/.../feature/reticulum/ReticulumMessages.kt` |
| Stat tiles, traffic chart, network map (Compose `Canvas`) | `app/.../feature/reticulum/ReticulumGraphics.kt` |
| Incoming message notifications (channel `reticulum-messages`) | `app/.../feature/reticulum/ReticulumNotifier.kt` |
| Tests | `core/src/test/.../reticulum/` |

The code ports the desktop's `ReticulumProtocol`, `ReticulumLxmf`,
`ReticulumMessenger` and `ReticulumNode` function for function. SHA-256, HMAC
and AES-CBC come from the JDK, X25519 and Ed25519 from the app's pure Kotlin
`crypto/X25519.kt` and `crypto/Ed25519.kt`, so it runs on `minSdk` 26 and on
the JVM. The node and the messenger share one lock. Each TCP link has a reader
and a writer thread, each UDP link a receive and a send thread, so writing
never blocks the caller; timers run on one scheduled thread and stamps on
their own low-priority thread. The app calls the node on a single background
worker and shows the `ReticulumSnapshot` and message lists it publishes, at
most every 250 ms.

## Storage

`market.packages.reticulum` shared preferences hold `interfaces` (JSON, the
desktop's format without passphrases), `announceName`, `announceMinutes`,
`answerPathRequests`, `networkOn` and `messages` (the desktop's history
JSON). `identity` (the 64-byte private key, hex) and `passphrases` (interface
id to passphrase, JSON) are in the same file, sealed with AES-GCM by the
Android keystore through `SecureStore`. If the keystore cannot store the
identity, it lives in memory only and **Make a new identity** refuses.
Destinations, events and traffic history stay in memory with the desktop's
caps.

## Differences kept on purpose

- **One resource at a time per link.** Python RNS lets every incoming
  resource on a link see each part, and a part for the next resource marks a
  finished one as still transferring, so LXMF drops it. Android queues a
  second outgoing resource on the same link until the first is proved or
  fails (seen with stamp cost 8, where two stamped messages became resources
  at once).
- **The shared instance is TCP.** RNS on Android shares its instance on
  `127.0.0.1:37428`, never on a Unix socket, so the "on this phone" interface
  always connects there. A desktop setting with `sharedOverTcp = false` loads
  and connects over TCP; a refused connection says no app on the phone shares
  an instance (start Sideband). The default interface is this one, as on the
  desktop.
- **UDP** works as on the desktop. While a UDP interface is on, the app holds
  a Wi-Fi multicast lock so broadcasts reach it.
- **No request firewall.** Android has none to ask, so a connection opens when
  you add or switch on an interface; the node's `authorizer` hook stays for
  when it gets one, and **Blocked** is never shown.
- **Lifetime.** The node lives as long as the app process, not the screen, so
  messages keep arriving after you leave it. Android may cut the connections
  in the background; when the app comes back, every switched-on interface
  that is not online reconnects. Incoming messages notify unless that
  conversation is open on screen. The Home badge counts once the screen has
  been opened in this process.
- **Phone layout.** Messages shows the conversation list, or one conversation
  with a back button, instead of the desktop's side-by-side panes. The
  composer sends with its Send button.
- **Import** reads a file you pick (Sideband keeps its config inside its own
  app), up to 1 MB. Attachments are saved through the system file picker.
- **Touch.** A tap on the map selects a destination instead of hovering; press
  or drag across a traffic chart to read it; tables become rows with a sort
  menu; right-click menus become long-press menus. Ripples follow reduced
  motion.
- **Wording** says "this phone" where the desktop says "this desktop".
- **No search hits** in the app search yet.

## Tests

```sh
cd native/android
./gradlew :core:coreTest -Pfilter=Reticulum
```

`ReticulumTest` and `ReticulumMessagingTest` reuse the desktop's Python RNS
1.5.7 vectors and add Python-made ones for Token, IFAC, LXMF packing and the
stamp workblock. They cover framing, msgpack bounds, config import with IFAC,
routing (HEADER_2 to the next hop, dropped transported data, repeatable
contexts), a session against a fake node on a localhost `ServerSocket`, UDP,
timeouts on an injected clock, the caps, and messaging between two nodes
through a relay hub (single packet, link and resource, open and with IFAC,
where a node with the wrong passphrase hears nothing).

`ReticulumInteropTest` runs only with the desktop's interop variables
(`RETICULUM_INTEROP_TCP`, `RETICULUM_INTEROP_SHARED_PORT` for a shared
instance over TCP, `RETICULUM_INTEROP_TARGET`, `RETICULUM_INTEROP_OUT`, and
`RETICULUM_LXMF_TCP`, `_PEER`, `_OUT`, `_NETNAME`, `_PASSPHRASE`; see
[Tests](../marketplace/reticulum.md#tests)). Point the desktop's `interop.py`
and `lxmf_interop.py` at a script that runs
`java -cp core/build/classes/kotlin/main:core/build/classes/kotlin/test:<kotlin-stdlib.jar> com.forkmesh.nativeapp.testing.TestRunnerKt core/build/classes/kotlin/test ReticulumInteropTest`.
On 2026-11-04 against RNS 1.5.7 and LXMF, TCP and a TCP shared instance
passed (announce, path both ways), and LXMF passed with stamp cost 0, cost 8
and an IFAC network, all three sizes in both directions.
