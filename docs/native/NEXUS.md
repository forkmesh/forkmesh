# NEXUS iOS network intelligence

The desktop counterpart is the `nexus` Market package, written in Qt with the same safety policy and service table ([../marketplace/nexus.md](../marketplace/nexus.md)).

NEXUS is the native, Nmap-inspired network inspection package at `native/ios/Nexus`. It uses supported iOS APIs only: `Network.framework` for TCP connections, UDP DNS queries and Bonjour browsing; `URLSession` for HTTP and TLS-validated web requests; and SwiftData for the optional local history. It does not use raw sockets, ARP tables, ICMP, a bundled Nmap executable, background scans or fabricated MAC/vendor data.

## Package layout

| Target | Responsibility |
| --- | --- |
| `NexusCore` | Addresses, CIDR/target parsing, safety policy, shared models, transport protocols and Apple Network.framework transport |
| `NexusDNS` | Standards-compliant UDP DNS parser and A, AAAA, CNAME, MX, TXT, NS and PTR queries |
| `NexusScan` | Bounded-concurrency TCP connect scans and safe HTTP/SSH/FTP/SMTP service identification |
| `NexusDiscovery` | Bonjour discovery, inventory merging and discovery-session comparisons |
| `NexusConnectivity` | TCP reachability, latency, hostname resolution and foreground monitoring |
| `NexusWeb` | HTTP status, redirects, response headers, timing, TLS trust/chain observations and informational header findings |
| `NexusRemote` | Authenticated remote-agent request contract for approved advanced scan profiles |
| `NexusKit` | Public dependency-injected API |
| `NexusStore` | SwiftData archive and JSON/CSV report encoding |
| `NexusUI` | Embeddable SwiftUI overview, discovery, scan, inspection, history and settings interface |

`NexusCore` deliberately labels port-table guesses as **inferred**. A protocol banner or HTTP response is labelled **confirmed**. Security observations only describe what NEXUS observed; they never claim an unverified vulnerability.

## Safety and authorization

The default policy permits loopback and local-network hosts only. Internet targets require both the user setting and a per-scan `AuthorizationAttestation`. Internet ranges are refused unless the range is in `authorizedNetworks`. Host, probe, timeout and concurrency limits are clamped before a scan begins.

The Scan screen fills an empty target with the range of the network the phone is on. `LocalNetwork.current()` reads the interface addresses with `getifaddrs` and `LocalNetwork.suggested(from:)` picks one: Wi-Fi (`en0`) first, skipping cellular (`pdp_ip`), VPN tunnels (`utun`, `ipsec`), AirDrop links (`awdl`, `llw`), container bridges, loopback, link-local and internet addresses. The prefix comes from the netmask but is narrowed to at least /24, so a /16 home network suggests the /24 around the phone's own address and stays inside the 256-host limit. **Use this network** refills the target after it was edited. Reading interface addresses needs no Local Network permission.

NEXUS uses TCP connect scans, so a closed port means the endpoint refused a connection. A timeout cannot distinguish filtering from an offline endpoint; the UI reports it as “No response.” Scans are foreground work and every stream is cancellation-aware.

## Host integration

Add `native/ios/Nexus` as a local package to the app target, link the `NexusUI` product, and present the root view from a feature route:

```swift
import NexusUI

NexusRootView()
```

The ForkMesh iPhone app lists NEXUS in the You hub's Workspace group (`Route.nexus`, `/dashboard/nexus`). It is not a Market package, so `PackageGate` never hides it. `Route.nexus` is modal: `HomeShell` presents it as a full-screen cover rather than pushing it, because `NexusRootView` is a `TabView` whose tabs each own a `NavigationStack`, and nesting those inside the app's stack or the generic route sheet doubles the navigation bars. The host passes `onClose`, which puts a Done button on each tab's root screen; without `onClose` there is no Done button, so a host that embeds the view directly is unchanged.

For a custom host experience, link `NexusKit` and inject transports or test doubles into `NexusClient`:

```swift
import NexusKit

let client = NexusClient(configuration: NexusConfiguration())
let updates = try await client.scan(target: "192.168.1.10", ports: .init([22, 80, 443]))
for try await update in updates {
    consume(update)
}
```

`ForkMesh-Info.plist` declares the Local Network reason and the Bonjour service types NEXUS browses. The app must retain normal App Transport Security settings: the HTTP inspector does not bypass certificate validation. If the host app changes the default Bonjour types, its `NSBonjourServices` list must match.

Use `NexusStore` when retained local history is desired. Its reports are encoded in memory with `NexusReportExporter.json` and `NexusReportExporter.csv`, ready for a host-owned `ShareLink` or file exporter. No scan history or inventory is uploaded by the native package.

## Remote-agent boundary

The optional `RemoteScannerClient` posts a signed-in user's bearer token, explicit target, selected approved profile and attestation to a configured agent. The server must enforce authorization, audit logging, target ownership, rate limits and its own allow-list. Native NEXUS remains useful without a remote agent; capabilities such as SYN scanning, OS fingerprinting and hop-by-hop traceroute belong behind that boundary.

## Verification

On a host with the ForkMesh Swift toolchain:

```sh
cd native/ios/Nexus
swift test
```

The Linux test suite covers target parsing, safety decisions, DNS packets, banner classification, inventory merging, report contracts and public defaults. Apple-only SwiftUI and SwiftData targets are parsed in repository checks and compile when the package is linked by the iOS target on macOS. Linux does not compile the `Network.framework` transport either, so build the package for iOS on the office Mac after changing it (`docs/native/MAC_BUILD.md`, "Swift rules the Mac archive enforces"). `NexusStore` and `NexusUI` have no test targets.

`NetworkTransport.swift` shares one `startConnection` for TCP and UDP. Its state handler clears itself after the first `ready`, `failed` or `cancelled`, so a later failure or `close()` cannot resume the continuation twice. Each wait sits in `withTaskCancellationHandler`, which cancels the `NWConnection` when `withTimeout` gives up; without it a connection stuck in `waiting` kept the timeout from returning. A cancelled connection reports `ConnectionFailure.cancelled`.

`HTTPInspector` reads certificate validity dates with `SecCertificateCopyNotValidBeforeDate`/`AfterDate` on iOS 18 and macOS 15, and with `SecCertificateCopyValues` on older macOS. iOS 17 has no public API for them, so the summary there carries open-ended dates.

## Android

The Android app has the same package in Kotlin: logic in `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/nexus` (tests in `core/src/test/.../nexus/NexusTest.kt`, run with `./gradlew :core:coreTest -Pfilter=Nexus`) and screens in `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/nexus`. `Route.Nexus` is in the You hub's Workspace group and opens from `/dashboard/nexus`; like iOS it is never hidden by `PackageGate`.

The safety policy, limits, port presets, service catalog, DNS wire format, banner rules and report formats are line-for-line ports, so a target refused on iPhone is refused on Android with the same message. Platform differences:

- Service discovery uses `NsdManager` (mDNS/DNS-SD, the protocol Bonjour speaks) for the same eight service types. Android asks for no Local Network permission; the app already declares `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE` and `CHANGE_WIFI_MULTICAST_STATE`.
- The suggested scan range reads `java.net.NetworkInterface`, prefers `wlan0`, and skips mobile data (`rmnet`, `ccmni`), VPN (`tun`, `ppp`, `ipsec`) and Wi-Fi Direct/Aware (`p2p`, `aware`) interfaces, with the same /24 floor.
- The HTTP inspector uses `HttpURLConnection` with redirects followed by hand (so each hop is recorded) and an `SSLSocket` handshake for the certificate chain, protocol and cipher suite. Like App Transport Security on iOS, the app's network security config blocks plain `http://` to anything but localhost, and the inspector says so.
- Scan targets that are hostnames resolve through the system resolver; the DNS explorer queries 1.1.1.1 like iOS.
- History is kept on the phone (`filesDir/nexus/history.json`) unless turned off in NEXUS Settings, and scan reports share as JSON or CSV.

The reasons behind these choices are in [ANDROID.md](ANDROID.md#28-nexus-network-inspection-sync-ledger-s046).
