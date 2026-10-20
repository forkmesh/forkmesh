# NEXUS package (desktop)

Part of [the Marketplace docs](README.md). Code:
`marketplace/packages/nexus/content/desktop/`. The iPhone has its own NEXUS
screen, built from the Swift package in `native/ios/Nexus`
([../native/NEXUS.md](../native/NEXUS.md)); this package is the desktop
counterpart, written in Qt with the same safety policy, port presets and
service table.

## Where it runs

`nexus` is a desktop-only `extension.native` package (`builtin:nexus`),
bundled but not installed by default, like Security and Router. Installing it
from the Market adds **NEXUS** to the rail (octicon `pulse`). On Linux it is a
package module (`build/packages/nexus.so`), loaded only once installed
([../desktop/package-modules.md](../desktop/package-modules.md)). There is no
iPhone or Android entrypoint: the iPhone NEXUS screen is always listed in the
You hub's Workspace group and is not a Market package.

## Code map

| File | Role |
| --- | --- |
| `NexusCore.{h,cpp}` | Pure logic, no sockets: the 129-row service table (generated from the Swift `ServiceCatalog`), port presets and port-list parsing, target parsing (host name, IPv4/IPv6 address, last-octet or full range, IPv4 CIDR), scope classification and `evaluate()` (the scan policy), banner identification, the mDNS query builder and DNS message parser, device merging, scan comparison, header findings, and the JSON/CSV formats. |
| `NexusNetwork.{h,cpp}` | The network side, with callbacks instead of signals so the package needs no moc: `TcpScanner`, `MdnsBrowser`, `lookupDns` (`QDnsLookup`) and `inspectHttp`. |
| `NexusFeature.{h,cpp}` | The page: Overview, Discover, Scan, Inspect, History and Settings sections, saving, top-search hits and the export offered on removal. |
| `tests/test_nexus.cpp` | `forkmesh-nexus-tests`: everything in `NexusCore`, plus a real scan of a loopback listener and a refused port, and cancelling. |

## Safety

The policy is the iPhone's `ScanAuthorizationPolicy`, ported rule for rule:

- Loopback and local networks are always allowed: 10/8, 172.16/12,
  192.168/16, 169.254/16, 100.64/10, fe80::/10 and fc00::/7 (IPv4-mapped IPv6
  addresses are classified as IPv4).
- Multicast, broadcast, 240/4 and unspecified addresses are refused.
- At most 256 hosts and 70,000 connection attempts per scan. A CIDR larger
  than the host limit is refused at parse time; IPv6 networks are refused
  (too large to scan), single IPv6 addresses are allowed.
- Internet addresses are refused unless **Allow internet targets** is on.
  Then a scan may include up to four single internet hosts, after a dialog
  where the user confirms "I own these targets or have written permission to
  test them". Ranges of internet addresses are refused.
- Addresses inside **Authorized networks** (Settings) count as the user's own:
  ranges there need no confirmation.

### Firewall

The desktop firewall lives in `BackoffNetworkAccessManager` and only sees
requests made through it. The web inspector uses `host.network`, so an unknown
destination prompts as usual. TCP probes and multicast DNS are raw sockets,
because a connect scan cannot be expressed as an HTTP request. Before a scan,
`NexusFeature::deniedByFirewall` asks the window's manager (when the firewall
is on) for each host's verdict on a `tcp://<address>` URL, and skips hosts a
deny rule covers. The status line says how many were skipped. Unlisted hosts
are not prompted for one by one: starting the scan is the user's decision for
the targets they typed. DNS lookups use the system resolver, as the Router
package does.

## Scanning

The Scan section fills an empty **Target** with the range of the network this
computer is on whenever it is shown, and **This network** refills it after it
was edited. `localNetwork()` lists the addresses of every running,
non-loopback interface and `suggestedLocalNetwork` takes the first private
IPv4 one (not link-local), with its prefix narrowed to at least /24, so a /16
network suggests the /24 around this computer's address and stays inside the
256-host limit. The iPhone does the same with `LocalNetwork` in `NexusCore`.

`TcpScanner` opens up to *Connections at once* (default 48, 1–256)
`QTcpSocket`s, each with *Connection timeout* (default 1.5 s, 0.2–30 s):

- **Open:** the connection was accepted. The scanner then waits up to 0.7 s
  for a greeting; for cleartext web ports it sends one
  `HEAD / HTTP/1.0` first. A recognised greeting (SSH, HTTP, RFB/VNC,
  POP3, IMAP, `220` for FTP or SMTP) marks the service **confirmed**;
  otherwise the name comes from the port table and the page says *inferred*.
- **Closed:** the connection was refused.
- **No response:** a timeout or any other error. A timeout cannot tell a
  firewall from a host that is off, so the page does not guess.

The scan runs on the GUI thread's event loop, so it stays responsive without
threads. Cancelling bumps a generation counter so late callbacks are dropped.
A host name is resolved with `QHostInfo` and its first address (IPv4
preferred) is scanned.

## Discovery

Qt has no mDNS API, and the window's local mesh beacon is ForkMesh-only, so
`MdnsBrowser` sends one-shot multicast DNS queries (RFC 6762 §5.1) from an
ephemeral UDP port to 224.0.0.251:5353. Responders answer such queries by
unicast to that port, which works next to a running Avahi or mDNSResponder
because nothing binds 5353. Every query goes out on each running,
multicast-capable IPv4 interface (`setMulticastInterface`), since a machine
with Docker bridges or a VPN would otherwise send only on whichever interface
the multicast route picks. Each second for eight seconds it asks for
`_services._dns-sd._udp.local`, the default service types (and any new type
an answer names, up to 64), then SRV for instances without one and A for
host names without an address. `devicesFromRecords` groups instances by their
SRV host into devices with addresses, service titles and ports. Discovered
devices are merged into the saved list by name (capped at 500).
Networks that filter multicast show no devices; scanning still works.

## Inspect

- **DNS:** A, AAAA, CNAME, MX, NS, TXT, PTR (an IP address is turned into its
  `in-addr.arpa` / `ip6.arpa` name) and SRV through `QDnsLookup`.
- **Web and TLS:** `inspectHttp` follows up to six redirects by hand
  (`ManualRedirectPolicy`) and aborts each response once its headers arrive.
  It records status, timing, the TLS version and cipher, and the peer
  certificate chain. Certificate errors are reported, never ignored, so an
  invalid certificate ends the inspection with the reason. Header findings
  mirror the iPhone's informational checks: plain HTTP, missing HSTS, CSP,
  `nosniff`, framing protection and Referrer-Policy, and a versioned `Server`
  or an `X-Powered-By` header. They describe what was observed and never claim
  a vulnerability.

## Storage

`forkmesh::paths::pluginDataDir("nexus")/nexus.json` (inside
`~/projects/<project>/data/nexus` on organized layouts) holds the settings,
the newest 50 scans and the discovered devices, written with `QSaveFile`.
Nothing is uploaded. **Clear history…** removes scans and devices after a
confirmation. Removing the package offers the same JSON as an export
(`forkmesh-nexus.json`). A scan exports as JSON or CSV; CSV cells that start
with `=`, `+`, `-` or `@` are prefixed with `'` so spreadsheets do not run them.

## Limits

- TCP connect scans only: no SYN, UDP, OS fingerprinting or traceroute, the
  same boundary as the iPhone package.
- Discovery covers multicast DNS only; devices that do not advertise services
  appear only in scans.
- IPv6 multicast DNS (ff02::fb) is not queried.
