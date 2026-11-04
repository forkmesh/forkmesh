# Reticulum package

`marketplace/packages/reticulum` (Market id `reticulum`, entrypoint
`builtin:reticulum`) connects the desktop to the
[Reticulum](https://reticulum.network/) network (rail label **Reticulum**, icon
`pulse`). It is bundled and off by default, desktop only, and on Linux a lazily
loaded package module ([../desktop/package-modules.md](../desktop/package-modules.md)).
The user-facing description is the package
[README](../../marketplace/packages/reticulum/README.md). Part of
[Marketplace](README.md).

Before this package ForkMesh had no Reticulum code. Everything below lives in
the package, and the host gained one neutral call (`authorizeConnection`, see
[package-system.md](package-system.md#desktop-page-contract)).

## What it is on the network

The desktop speaks the Reticulum wire protocol natively (C++, OpenSSL). It
runs as a leaf node: it never forwards other nodes' packets. It does:

- receive every packet on its interfaces, validate each announce (Ed25519
  signature, and the destination hash against the announced key) and keep a
  table of destinations with hops, interface, next hop and app data;
- announce its own `forkmesh.node` destination when asked (on a timer the
  user picks, or a click), with the user's name as app data;
- send path requests (`rnstransport.path.request`) and time how long the
  answer takes;
- answer path requests for its own destination with a path-response announce,
  at most one every 10 seconds;
- decode interface-discovery announces (`rnstransport.discovery.interface`) and
  check their LXMF proof-of-work stamp (16 bits required, as in RNS).

It does not open links, decrypt data packets, or carry LXMF messages. Those
need the Token cipher, ratchets and links, and are the next step if the
package grows messaging. Interface access codes (IFAC) are not implemented:
packets with the IFAC bit are dropped, and IFAC-protected interfaces are
skipped on import.

## Code map

| File | Responsibility |
| --- | --- |
| `ReticulumProtocol.{h,cpp}` | Pure protocol: header flags, packet pack/parse and hash, HDLC and KISS framing, identities (X25519 and Ed25519 via OpenSSL), announce build/parse/validate, path requests, HKDF, a bounded msgpack reader, known aspects, LXMF and Nomad Network display names, discovery data and stamp value. |
| `ReticulumNode.{h,cpp}` | `Node` and `InterfaceRuntime`: TCP (`QTcpSocket`), shared instance (`QLocalSocket` in the abstract namespace `rns/<instance>`, or TCP 37428) and UDP (`QUdpSocket`); reconnect backoff, destination table, dedupe, path lookups, counters, per-second traffic history; config import. |
| `ReticulumGraphics.{h,cpp}` | `StatTile`, `TrafficChart` (crosshair tooltip), `NetworkMap` (radial hop rings, interface sectors, ripples on announce). |
| `ReticulumFeature.{h,cpp}` | The page: header, onboarding, tiles, the five tabs, the interface editor dialog, settings, search hits. |
| `tests/test_reticulum.cpp` | `forkmesh-reticulum-tests`: framing, vectors generated with Python RNS 1.5.7 and LXMF, config import, a live session against a fake TCP node, firewall denial, and the page. Optional interop and listen modes (below). |

## Wire details worth knowing

Checked against RNS 1.5.7 (`RNS/Packet.py`, `Identity.py`, `Destination.py`,
`Transport.py`, `Interfaces/TCPInterface.py`, `LocalInterface.py`):

- Hops: RNS adds one to every received packet, then takes one off again for
  packets that came through the shared instance. The node does the same, so a
  direct TCP peer is one hop away whichever way you are connected.
- The packet hash covers `flags & 0x0F` plus everything after the hops byte
  (after the transport id for header type 2), so the same announce arriving
  by two routes dedupes. A later copy with fewer hops still updates the route.
- The last five bytes of an announce's random hash are its emission time;
  older announces for a known destination are ignored, and a known
  destination announced with a different key is refused.
- Linux `rnsd` shares on an abstract Unix socket, `\0rns/default`. Other
  systems, and `shared_instance_type = tcp`, use TCP 37428. The page's shared
  instance interface sends the same empty HDLC keepalive as RNS clients.

## Settings and data

Package settings (removed with the package): `interfaces` (JSON),
`identity` (the 64-byte private key, hex, the same layout as an RNS identity
file), `announceName`, `announceMinutes`, `answerPathRequests` and
`networkOn`. The destination table, events and traffic history are kept in
memory only, capped at 20,000 destinations, 400 events and 180 seconds.

## Safety

- Every TCP and UDP connection goes through `NativePackageHost::authorizeConnection`
  (permission `network.socket.connect`), which asks the request firewall exactly
  as a WebSocket does. A denial leaves the interface **Blocked** with no retry.
- Inputs from the network are bounded: frames over 256 KiB, hop counts over
  127, short packets, IFAC packets and malformed announces are counted and
  dropped. The msgpack reader limits depth to 8 and items to 4096, and checks
  every length against the bytes left.
- Nothing is announced by default. Public entry points are only added when the
  user picks one. The defaults are the RMAP backbones (`rmap.world:4242` and
  `:4343`) because the old Dublin and Amsterdam testnet hosts were
  decommissioned. Hosts announced on the network are offered only when their
  stamp is valid.

## Tests

```sh
cmake --build desktop/build --target forkmesh-reticulum-tests
QT_QPA_PLATFORM=offscreen desktop/build/forkmesh-reticulum-tests
```

`RETICULUM_SCREENSHOT_DIR=<dir>` saves a PNG of each tab. Two opt-in modes
reach real Reticulum:

- **Interop**: run a Python RNS transport node with a `TCPServerInterface`
  (or as a shared instance) announcing an `lxmf.delivery` destination named
  `Python peer`, then start the test binary with
  `RETICULUM_INTEROP_TCP=127.0.0.1:<port>` (or `RETICULUM_INTEROP_SHARED=<instance>`),
  `RETICULUM_INTEROP_TARGET=<that destination hash>` and
  `RETICULUM_INTEROP_OUT=<file>`. The binary checks it receives and validates the
  announce and gets a path answer. It then writes its own destination hash to
  the file, so the Python side can check it heard our announce and that its
  `request_path` gets answered. Both modes passed against RNS 1.5.7 on
  2026-11-04.
- **Listen**: `RETICULUM_LISTEN_TCP=rmap.world:4242 RETICULUM_LISTEN_SECONDS=60`
  connects without sending anything and prints what it heard. On 2026-11-04,
  75 seconds gave 61 destinations: LXMF, LXST calls, Nomad Network,
  propagation nodes, rncp and one discovery announce, with 0 rejected and 0
  unreadable frames.
