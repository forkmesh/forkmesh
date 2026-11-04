# Reticulum package

`marketplace/packages/reticulum` (Market id `reticulum`, entrypoint
`builtin:reticulum`) connects the desktop, iPhone and Android to the
[Reticulum](https://reticulum.network/) network (desktop rail label
**Reticulum**, icon `pulse`; Home tile on the phones). It is bundled and off by
default, account-synced (`install.sync: "account"`, `places: ["mobile",
"desktop"]`), and on Linux a lazily loaded package module
([../desktop/package-modules.md](../desktop/package-modules.md)). The phone
ports and what they leave out on purpose are in
[../native/reticulum-ios.md](../native/reticulum-ios.md) and
[../native/reticulum-android.md](../native/reticulum-android.md).
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

- carry LXMF messages (see [Messaging](#messaging)) as an `lxmf.delivery`
  destination, so Sideband, MeshChat and Nomad Network users can write to it;
- join virtual networks protected by a network name and passphrase (IFAC,
  see [Interface access codes](#interface-access-codes)).

## Code map

| File | Responsibility |
| --- | --- |
| `ReticulumProtocol.{h,cpp}` | Pure protocol: header flags, packet pack/parse and hash, HDLC and KISS framing, identities (X25519 and Ed25519 via OpenSSL), announce build/parse/validate, path requests, HKDF, a bounded msgpack reader, known aspects, LXMF and Nomad Network display names, discovery data and stamp value. |
| `ReticulumLxmf.{h,cpp}` | LXMF message pack, unpack and signature check, announce app data, stamps (workblock, generation with a cancel flag, validation), resource advertisements and map hashes. |
| `ReticulumMessenger.{h,cpp}` | `Messenger`: outgoing queue (path lookup, stamp, opportunistic packet, link packet or resource), incoming packets, links (both ends), resources (both ends), delivery proofs, retries and timeouts, conversation store. |
| `ReticulumNode.{h,cpp}` | `Node` and `InterfaceRuntime`: TCP (`QTcpSocket`), shared instance (`QLocalSocket` in the abstract namespace `rns/<instance>`, or TCP 37428) and UDP (`QUdpSocket`); reconnect backoff, destination table, dedupe, path lookups, counters, per-second traffic history; config import. |
| `ReticulumGraphics.{h,cpp}` | `StatTile`, `TrafficChart` (crosshair tooltip), `NetworkMap` (radial hop rings, interface sectors, ripples on announce). |
| `ReticulumFeature.{h,cpp}` | The page: header, onboarding, tiles, the five tabs, the interface editor dialog, settings, search hits. |
| `tests/test_reticulum.cpp` | `forkmesh-reticulum-tests`: framing, vectors generated with Python RNS 1.5.7 and LXMF, crypto and IFAC, config import, a live session against a fake TCP node, firewall denial, two in-process nodes messaging through a relay hub (packet, link and resource, open and IFAC, a wrong passphrase hearing nothing), and the page including the Messages tab. Optional interop and listen modes (below). |

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

## Messaging

The Messages tab speaks LXMF 0.9 to the `lxmf.delivery` destination of this
desktop's identity (`lxmf.delivery` app data `[name, stamp cost, []]`; the
empty list says it does not take compressed resources, so senders never
bz2-compress for it and the desktop needs no bz2). A message goes the cheapest
way that fits, as LXMF itself chooses:

| Size of the packed payload | Method |
| --- | --- |
| up to 287 bytes | one packet, encrypted to the recipient's key (or its announced ratchet), up to two tries 15 s apart, then a link |
| up to 319 bytes | a packet over an encrypted link |
| larger, up to 1 MiB | a resource over the link (one segment, uncompressed) |

Only one resource is offered per link at a time; further large messages wait
("Waiting for the previous transfer"). RNS 1.5.7 mishandles two resources on one
link at once: a part of the second marks the finished first one as still
transferring, so LXMF drops a message it has already proved. Sending them one
by one moved the stamp-cost interop from 2 in 5 runs to 6 in 6.

Each step waits for the recipient's proof: the packet proof (signature of the
packet hash) marks a message **Delivered**; a resource is delivered when the
resource proof matches. With no path the desktop asks for one and waits 30 s.
When the recipient's announce asks for a stamp, the stamp is computed on a
worker thread (3000-round workblock, as LXMF); costs over 24 bits are refused.
Before the first message in 30 minutes the desktop announces its own
`lxmf.delivery`, so the recipient can check the signature and reply.

On the Messages tab with a conversation open, the window's footer prompt box
is the composer (`hostPromptTarget` / `submitHostPrompt`, "Send to <name>"),
so the page's own composer is hidden whenever the host offers one. A
conversation with unread messages shows on the rail badge
(`navigationBadge`), and an incoming message the user is not looking at posts a
desktop notification.

Incoming messages are accepted as a packet to `lxmf.delivery`, a packet over a
link the sender opened, or a resource on that link (advertisements that are
compressed, split, larger than 1 MiB or more than eight at once are refused
with a receiver cancel). Every message is proved, deduplicated by its hash,
and its signature checked against the sender's announced key; a sender not yet
heard shows "Signature not checked". Attachments in field 5 are listed with a
Save button.

Links follow RNS 1.5: link id from the request's hashable part, X25519 then
HKDF(64, salt = link id) into a Token key, the responder's proof signed with
the identity key over link id, its ephemeral key, the identity signing key and
the 3 signalling bytes (MTU and mode, AES-256-CBC only), RTT packet, keepalive
`0xFF`/`0xFE`, and `LINKCLOSE`. Links the desktop opened close after two idle
minutes; links others opened after 15 minutes without traffic. Packets to a
destination several hops away are sent with header type 2 and the next hop
from its announce, the way `Transport.outbound` does.

## Interface access codes

An interface with a network name or passphrase (IFAC) derives its key as RNS
does: HKDF(64) of SHA-256 of the SHA-256 hashes of name and passphrase, with
the RNS IFAC salt, and the Ed25519 key in the second half. Every outgoing
packet gets the last *n* bytes (default 16) of its signature as the access
code, the IFAC flag and an HKDF mask; incoming packets without a valid code are
counted as invalid frames and dropped, as are packets with a code on an open
interface. The passphrase is stored with the interface in the package
settings, like a Wi-Fi password.

## Settings and data

Package settings (removed with the package): `interfaces` (JSON),
`identity` (the 64-byte private key, hex, the same layout as an RNS identity
file), `announceName`, `announceMinutes`, `answerPathRequests` and
`networkOn`, and `messages` (the last 5,000 messages as JSON, with up to
4 MB of attachments per message; saved 1.5 s after a change). The manifest's
`localData` makes the desktop offer that JSON when the package is removed. The
destination table, events and traffic history are kept in memory only, capped
at 20,000 destinations, 400 events and 180 seconds.

## Safety

- Every TCP and UDP connection goes through `NativePackageHost::authorizeConnection`
  (permission `network.socket.connect`), which asks the request firewall exactly
  as a WebSocket does. A denial leaves the interface **Blocked** with no retry.
- Text from the network (names, message titles and bodies) is shown as plain
  text, never as rich text.
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

`RETICULUM_SCREENSHOT_DIR=<dir>` saves a PNG of each tab. In the window tests, `forkmesh-window-tests --reticulum-only` installs the package in a real `MainWindow`, opens every tab in Control Tower, Focus and Control Tower again (`FORKMESH_RETICULUM_SHOT=<prefix>` saves each one). Two opt-in modes
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
- **LXMF interop**: a Python LXMF router (`register_delivery_identity`, a
  `TCPServerInterface`) exchanges three messages each way with the test binary
  started with `RETICULUM_LXMF_TCP`, `RETICULUM_LXMF_PEER` and
  `RETICULUM_LXMF_OUT` (plus `RETICULUM_LXMF_NETNAME` and
  `RETICULUM_LXMF_PASSPHRASE` for IFAC). On 2026-11-04 against LXMF with RNS
  1.5.7 every message was delivered and signature-checked both ways by
  packet, link and resource, with the Python side enforcing an 8-bit stamp,
  and again over an IFAC interface.
- **Listen**: `RETICULUM_LISTEN_TCP=rmap.world:4242 RETICULUM_LISTEN_SECONDS=60`
  connects without sending anything and prints what it heard. On 2026-11-04,
  75 seconds gave 61 destinations: LXMF, LXST calls, Nomad Network,
  propagation nodes, rncp and one discovery announce, with 0 rejected and 0
  unreadable frames.
