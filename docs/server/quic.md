# QUIC, HTTP/3 and WebTransport (FM Media transport)

The transport under FM Media ([fm-media.md](fm-media.md)): our own QUIC v1,
a minimal HTTP/3 and WebTransport over HTTP/3, in Go, standard library only
(no quic-go, no x/crypto, no x/net). The TLS 1.3 handshake is `crypto/tls`'s
QUIC API (`tls.QUICServer`, `tls.QUICClient`, `QUICConn` events); everything
else, packet protection included, is ours.

| Package | Role | Spec |
| --- | --- | --- |
| `server/rtc/quic` | QUIC v1 endpoint, server and client, with multipath and a standby-core helper | RFC 9000, 9001, 9002, 9221, draft-ietf-quic-multipath-21 |
| `server/rtc/h3` | HTTP/3 framing, control streams, SETTINGS, QPACK (static table only), extended CONNECT | RFC 9114, 9204, 9220, 9297 |
| `server/rtc/webtransport` | WebTransport sessions, streams and datagrams, server and Go client | draft-ietf-webtrans-http3 (draft-02 and draft-07 wire format) |

Why: a phone joins in one round trip instead of ICE + DTLS + SDP, keeps its
call when it moves between Wi-Fi and cellular (connection migration), and
needs no ICE because every client talks to an SFU with a public address.
Browsers reach the same server through WebTransport.

## How a media layer uses it

```go
ep, _ := quic.Listen("udp", ":4433", &quic.Config{TLSConfig: tlsConf, MaxDatagramFrameSize: 1500})
conn, _ := ep.Accept(ctx)
control, _ := conn.AcceptStream(ctx)   // one bidirectional stream: join, tracks, receiver reports
conn.SendDatagram(frameFragment)       // encrypted media fragments, unreliable, never retransmitted
frag, _ := conn.ReceiveDatagram(ctx)
```

- Media frames are already end-to-end encrypted by the client
  ([rtc.md](rtc.md#frame-encryption)); QUIC adds hop encryption on top.
  `Conn.MaxDatagramSize()` is the largest payload that fits one packet
  (about 1150 bytes at the default 1200-byte packet size), so the media layer
  fragments frames to that size.
- Datagrams are congestion controlled but never retransmitted. A full send
  queue returns `ErrDatagramQueueFull` instead of growing; the media layer
  decides what to drop.
- Control messages go on one QUIC stream (reliable, ordered).
- Phones (Go engine core, `gomobile`) call `quic.Dial` or `Endpoint.Dial`.
  When the OS moves the phone to a new network, the engine opens a new UDP
  socket and calls `Endpoint.Rebind(newSocket)`; the connection carries on.
  With `Config.Multipath` on both sides it instead keeps Wi-Fi and cellular
  open together with `Conn.OpenPath` and picks a policy per datagram
  ([Multipath](#multipath)), and `quic.NewStandby` holds a warm connection
  to a second core.
- Browsers: `webtransport.Server` on the same kind of endpoint. Session
  datagrams carry the quarter-stream-id prefix; streams are tagged with the
  session id. `Server.Check(*h3.Request) int` is the path and origin hook: it
  returns the HTTP status (200 accepts). It must be set in production; nil
  accepts every session.

## What is implemented

QUIC (`server/rtc/quic`):

- Long and short headers, coalesced packets, packet-number encoding and
  decoding (RFC 9000 A.2/A.3).
- Packet and header protection for every TLS 1.3 suite crypto/tls can pick:
  TLS_AES_128_GCM_SHA256 and TLS_AES_256_GCM_SHA384 (crypto/aes,
  crypto/cipher), and TLS_CHACHA20_POLY1305_SHA256 with our own ChaCha20 and
  Poly1305 (`chacha.go`). Go's TLS 1.3 suite choice cannot be configured, and
  a Go server picks ChaCha20 for any client that lists it first (devices
  without AES hardware, our own Go client among them). That is why we wrote
  ChaCha20 rather than refuse those clients. The code is ARX plus
  `math/bits` 64-bit limb arithmetic with no secret-dependent branches. It
  is checked against RFC 8439 vectors and RFC 9001 A.5, and was compared
  offline with x/crypto on 200,000 random inputs.
- Initial secrets from the RFC 9001 v1 salt. Retry integrity tag. Key update
  in both directions: we start one after `KeyUpdateInterval` packets (default
  2^22, under the AES-GCM confidentiality limit) once the peer has acked the
  current phase. The next read keys are derived in advance and the old keys
  are kept for 3 PTO. We count failed decryptions against the AEAD integrity
  limit.
- Three packet-number spaces. ACK frames with up to 32 ranges; we stop
  reporting ranges the peer has acked an ACK for. Duplicate detection over 64
  tracked ranges; anything older counts as a duplicate.
- RFC 9002 loss detection (packet and time thresholds), PTO with backoff,
  client anti-deadlock probes, discarding Initial and Handshake state.
- NewReno congestion control (slow start, avoidance, one reduction per
  recovery period, persistent congestion) with a token-bucket pacer at 1.25 ×
  cwnd / srtt.
- Bidirectional and unidirectional streams; STREAM, RESET_STREAM,
  STOP_SENDING, MAX_DATA, MAX_STREAM_DATA, MAX_STREAMS and the *_BLOCKED
  frames. Stream and connection flow control. Final-size checks. Stream
  credit is returned when a peer stream is fully closed.
- DATAGRAM frames (RFC 9221) and `max_datagram_frame_size` in both directions.
- Transport parameters, with the RFC 9000 7.3 connection-id authentication
  checks (`original_destination_connection_id`,
  `initial_source_connection_id`, `retry_source_connection_id`).
- Connection ids: 8-byte local ids, NEW_CONNECTION_ID up to the smaller of
  the two `active_connection_id_limit`s, RETIRE_CONNECTION_ID,
  `retire_prior_to`.
- Stateless reset, sent and detected. The token is HMAC-SHA256(key, cid);
  `Config.StatelessResetKey` shares the key across a cluster. A reset is
  always smaller than the packet that triggered it, is only sent for packets
  over 42 bytes, and is rate-limited.
- Retry with AEAD-sealed address validation tokens. A token is bound to the
  client's IP and port, carries the original DCID and the Retry SCID, and
  lives 10 s. Retry is used for every connection with
  `RequireAddressValidation`, or automatically once more than
  `RetryThreshold` (128) handshakes are half open.
- Anti-amplification: before the address is validated (by Retry token or a
  Handshake packet), a server sends at most 3 × the bytes it has received,
  and the PTO timer stops while blocked. The same limit applies to every
  unvalidated path.
- Path validation and migration (below). Idle timeout (the smaller of the
  two, at least 3 PTO) with optional keep-alive PINGs. CONNECTION_CLOSE for
  transport and application errors, with the closing and draining periods.
  Version negotiation, sent and handled.
- Client side: Dial, Retry handling, version negotiation, and migration with
  `Endpoint.Rebind`.

### Migration: new paths are used only after validation

How a phone keeps a call when it changes networks.

- A server that receives an authenticated non-probing packet with the highest
  packet number from a new address starts a candidate path. It sends
  PATH_CHALLENGE there, padded to 1200 bytes when the 3× budget allows, and
  keeps sending application data on the old, validated path. It switches only
  when a matching PATH_RESPONSE arrives. (RFC 9000 allows sending on an
  unvalidated path within the 3× limit; we chose not to.) Validation gives up
  after 3 × max(PTO, initial PTO) and the old path stays.
- If a later non-probing packet with a higher packet number comes from the
  old path, the candidate's switch is cancelled (RFC 9000 9.3.3). A replayed
  packet is dropped as a duplicate, so a forged path is never adopted.
- When we switch to a path with a new IP address, congestion control and the
  RTT estimate are reset. When only the port changes, they are kept (RFC 9000
  9.4). The old path is forgotten after 3 PTO.
- A client that rebinds moves to an unused connection id from the server,
  when it has one, and retires the old id, so the two paths cannot be linked
  by id. It then resets congestion control and validates the new path. A
  server whose peer changed ids uses a fresh peer id on the new path when it
  has one.
- Packets from new addresses are dropped until the handshake is confirmed.

## Multipath

Idea 4 of [fm-media.md](fm-media.md#beyond-facetime): no audio dropout when
a phone moves between Wi-Fi and cellular or when one of them goes bad.
FaceTime and WebRTC switch one path at a time (migration, ICE restart), so
audio stops for at least a round trip while they notice and move. With
multipath both networks stay open and audio can travel on both.

### Draft and codepoints

We implement **draft-ietf-quic-multipath-21** (17 March 2026). It was the
newest revision on 2026-10-21 and is in the RFC Editor queue, so the wire
format is settled. We use the codepoints the draft asks IANA for, not the
provisional experiment values from draft-15 (`0x0f739bbc1b666d0d`,
`0x15228c00`...), so we interoperate only with peers on the final values.
Today those are our own Go client and server.

| Item | Codepoint |
| --- | --- |
| `initial_max_path_id` transport parameter | 0x3e |
| PATH_ACK / PATH_ACK with ECN | 0x3e / 0x3f |
| PATH_ABANDON | 0x3e75 |
| PATH_STATUS_BACKUP / PATH_STATUS_AVAILABLE | 0x3e76 / 0x3e77 |
| PATH_NEW_CONNECTION_ID / PATH_RETIRE_CONNECTION_ID | 0x3e78 / 0x3e79 |
| MAX_PATH_ID / PATHS_BLOCKED / PATH_CIDS_BLOCKED | 0x3e7a / 0x3e7b / 0x3e7c |
| Abandon codes APPLICATION_ABANDON_PATH, PATH_RESOURCE_LIMIT_REACHED, PATH_UNSTABLE_OR_POOR, NO_CID_AVAILABLE_FOR_PATH | 0x3e, 0x3e75, 0x3e76, 0x3e77 |

### Negotiation and fallback

- `Config.Multipath` turns it on. We then send `initial_max_path_id =
  MaxPaths - 1` (default 3, so 4 paths). It is negotiated only when both
  sides send the parameter. Otherwise nothing changes: no new frames, the
  RFC 9000 nonce, and single-path migration with `Endpoint.Rebind` exactly
  as above. Chromium does not send the parameter, so WebTransport from
  browsers stays single-path. A value above 2^32-1 is
  TRANSPORT_PARAMETER_ERROR. The parameter with a zero-length connection id
  is PROTOCOL_VIOLATION.
- Any multipath frame on a connection that did not negotiate it is
  PROTOCOL_VIOLATION. So is a multipath frame outside 1-RTT, or a path id
  above our advertised maximum.

### What is implemented

- **Per-path packet number spaces** for 1-RTT, keyed by path id. Initial and
  Handshake stay on path 0. The AEAD nonce is IV XOR (path id (32) || 00 ||
  packet number (62)), checked against the draft's example (path 3, packet
  54321). Path 0 gives exactly the RFC 9001 nonce.
- **Per-path loss recovery and congestion control**: each path has its own
  RTT estimator, NewReno window, pacer, PTO count and loss timer. Lost
  frames are retransmitted on whichever path the scheduler picks, so a PTO
  on one path resends its data on another (draft 5.7).
- **Per-path ACKs.** With multipath every 1-RTT acknowledgement is a
  PATH_ACK (ACK frames are still accepted and mean path 0). Each path's ACK
  rides on that path. If the path cannot send (abandoned, or not yet
  validated), its PATH_ACK goes on another path. An RTT sample counts only
  when the PATH_ACK came back on the path it acknowledges, except the first
  sample of a path; a later same-path sample replaces a cross-path start.
  Packets carrying a DATAGRAM, PING, PATH_CHALLENGE or PATH_RESPONSE are
  acknowledged at once, so idle paths and audio have exact RTTs and failures
  show within one round trip. Other packets keep the normal delayed ACK.
- **Connection ids per path**: two ids for every path id the peer may open
  (PATH_NEW_CONNECTION_ID). Retirement and `retire_prior_to` are per path.
  Path 0 keeps using NEW_CONNECTION_ID and RETIRE_CONNECTION_ID.
- **Path creation (client)**: `Conn.OpenPath(ctx, socket, PathOptions)`
  takes a new UDP socket. That is the second interface: on a phone, a socket
  bound to the cellular network while path 0 uses Wi-Fi. It picks the
  smallest unused path id for which both sides have connection ids, sends a
  PATH_CHALLENGE padded to 1200 bytes, and returns once the PATH_RESPONSE
  arrives. Validation that fails or times out (3 PTO, or the context)
  abandons the path id. Servers never open paths (draft 3).
- **Path validation (server)**: a packet that decrypts under an unused path
  id creates the path state. Until that path's PATH_CHALLENGE is answered,
  the server sends there only PATH_CHALLENGE and PATH_RESPONSE, within 3 ×
  the bytes received on it. A 4-tuple change on an existing path is RFC 9000
  migration within that path (validated, candidate cancelled by a later
  packet from the old address).
- **Path abandon**: `Conn.AbandonPath(id)` or `Path.Abandon()` sends
  PATH_ABANDON on another path. The peer's ids for that path are retired at
  once (reset tokens dropped, so a late stateless reset cannot kill the
  connection). Its unacknowledged packets are declared lost and their frames
  resent elsewhere. Receive state is kept 3 × the largest PTO so late
  packets are still acknowledged, then dropped. A received PATH_ABANDON is
  answered with one. Abandoning the last usable path is refused; a peer
  that abandons our last path makes us close.
- **Re-adding paths**: each abandoned path id raises our MAX_PATH_ID by one,
  so the number of open paths stays at `MaxPaths` while ids keep growing
  (the test opens and abandons six paths on a 4-path limit). PATHS_BLOCKED
  and PATH_CIDS_BLOCKED are sent when a new path is blocked, and checked on
  receipt.
- **Backup paths**: `PathOptions.Backup` or `SetPathBackup` sends
  PATH_STATUS_BACKUP / AVAILABLE with per-path sequence numbers. Out-of-order
  updates are ignored, and a status for a path not yet open is kept until it
  opens. Our scheduler treats a path as backup when either side marked it.
- **Keep-alives**: with two or more usable paths, an idle path gets a PING
  every `PathKeepAlivePeriod` (default 1 s; negative turns it off), so a
  backup path has a fresh RTT and liveness when the primary fails.
- **Key updates** work across paths. Old keys are kept 3 × the largest PTO,
  and with multipath a packet whose key phase differs from the current one
  is tried with the previous keys and then with the next ones, not decided
  by packet number (packet numbers are per path; draft 2.5).
- Closing and draining last 3 × the largest PTO of all paths. The idle
  timeout is for the whole connection.

### Scheduler API for the media layer

```go
conn.SendDatagramWith(audioFrame, quic.Redundant)   // every usable path
conn.SendDatagramWith(videoFragment, quic.Fastest)  // the primary path
conn.SendDatagramWith(bulkChunk, quic.Bulk)         // whichever path has window
for _, p := range conn.Paths() { ... }               // health per path
```

- `Fastest` (and plain `SendDatagram`): the primary path. The primary is the
  usable path ranked best by: healthy before unhealthy, non-backup before
  backup, then lowest smoothed RTT. A healthy current primary is kept unless
  another is 25% faster, which stops flapping. Among unhealthy paths, the
  one with the most recent acknowledgement wins. When the primary turns
  unhealthy and another path takes over, its unacknowledged `Fastest` and
  `Bulk` datagrams are sent again on the new primary (draft 5.7), and so are
  its unacknowledged stream frames. Stream data and control frames always
  use the primary.
- `Redundant`: a copy on every usable path, backup paths included, healthy
  or not. A copy waiting for congestion window on a path is dropped 100 ms
  after the first copy left, so a dead path's full window never stalls the
  queue. Use it for audio while any path shows loss, RTT growth or a recent
  switch, or during a handover.
- `Bulk`: any usable non-backup path with congestion window. The send loop
  goes round the paths one packet at a time, so traffic splits roughly by
  each path's window.
- Health signals, per path in `PathInfo`: state (validating, active,
  abandoned), primary, healthy, backup and peer backup, smoothed/min/latest
  RTT and variance, loss rate (EWMA, 1/16 per ack-eliciting packet), last
  heard, last acknowledged, congestion window, bytes in flight, PTO count,
  packets sent/lost/received. A path is unhealthy when its PTO count is
  above zero or its oldest unacknowledged packet is older than srtt +
  max(4 × rttvar, srtt/4, 4 ms), plus the peer's max_ack_delay if that
  packet carried no DATAGRAM (at least 15 ms). The PTO alone is not enough:
  while audio keeps flowing every send re-arms it (RFC 9002), so a dead path
  under continuous media never times out. The oldest-unacknowledged rule
  catches it within about 1.25 RTT.
- Duplicates at the receiver: with `Config.DatagramDedupKey` set, the
  transport calls it on every received datagram and drops repeats of a key
  among the last 4,096 (counted in `Stats.DatagramsDuplicate`). FM Media
  passes a function that reads its sequence number, so nothing changes on
  the wire. Without it, duplicates are delivered. `ReceiveDatagramPath`
  returns the path id each datagram came on.

### Standby core

`quic.NewStandby(primary, dial, StandbyOptions{})` keeps a second, mostly
idle connection, normally to another core, warm with a PING every
`KeepAlive` (1 s), and redials it with backoff if it dies. It watches the
primary every 5 ms and closes `PrimaryDead()` when one of these holds:

- the primary closed (CONNECTION_CLOSE, stateless reset, idle timeout);
- every usable path reached `DeadPTOs` (2) probe timeouts;
- the oldest unacknowledged packet is older than 2 × (srtt + 4 × rttvar +
  max_ack_delay), clamped to 60 to 250 ms, and nothing new was acknowledged
  for half that time. This catches a silent death during media, when PTO
  never fires.

The engine then calls `Promote`, which returns the warm connection as the
new primary and starts a fresh standby. The engine sends its join there: one
round trip, no handshake. `Health()` gives the same inputs to any caller.
`Close` closes only the standby connection; the primary belongs to the
caller.

### Security caps

- At most `MaxPaths` (default 4, hard cap 8) open paths, and twice that
  counting abandoned paths still in their 3-PTO wait. Raising MAX_PATH_ID
  after an abandon is deferred while too many abandoned paths are waiting.
- At most 4 addresses per path, 2 connection ids per path id we issue, and
  `ActiveConnectionIDLimit` per path id we accept (more is
  CONNECTION_ID_LIMIT_ERROR); 4,096 dedup keys; early PATH_STATUS for at
  most 2 × MaxPaths unopened ids.
- A path's state exists only after a packet decrypted under that path's id
  and nonce. A forged packet with a valid unused-path connection id is
  dropped silently, with no reply.
- Anti-amplification (3 ×) applies to every unvalidated address of every
  path. No path carries anything but probes before its validation.

### Measured

`TestMultipathPathFailureAudio`: client with two sockets through the netsim
relay, path A 15 ms each way (RTT 30 ms), path B 25 ms each way (RTT 50 ms),
a 50 pps flow of 120-byte datagrams for 2.5 s. At 1.2 s path A starts
dropping everything in both directions. The gap is the longest time between
two arrivals at the receiver; the normal spacing is 20 ms. Measured
2026-10-21 on an i9-10980XE at load 20 to 30, three runs:

| Case | Lost | Longest gap | Beyond 20 ms spacing |
| --- | --- | --- | --- |
| Redundant, uplink | 0 of 125 | 28 to 32 ms | 8 to 12 ms (B's extra one-way delay) |
| Redundant, downlink (server sends) | 0 of 125 | 27 to 32 ms | 7 to 12 ms |
| Fastest, path A blackholed | 0 of 125 (2 to 6 resent on B) | 70 to 72 ms | 50 to 52 ms |
| Fastest, A abandoned by the app (planned handover) | 0 of 125 | 30 to 31 ms | 10 to 11 ms |

- Redundant meets the target: no lost datagrams, and the only gap is the
  10 ms difference in one-way delay.
- Fastest after a planned switch (the OS reports Wi-Fi gone, the engine
  abandons it or marks it backup) moves at once: the extra gap is 10 ms,
  well under one RTT.
- Fastest when path A dies silently cannot beat one RTT of path A. The
  sender only learns of the loss when an acknowledgement is late. That is
  about 38 ms here (RTT 30 + 8 ms margin), plus 25 ms on B for the resent
  datagrams. Nothing is lost because unacknowledged datagrams are resent on
  B, but they arrive about 50 ms late. That is why the engine sends audio
  `Redundant` whenever a path looks doubtful.

`TestStandbyDetectsDeadPrimary`: primary core behind a 20 ms each-way relay,
standby core behind 25 ms, 50 pps media both ways, then the primary's link
drops everything:

| Case | Detected after | Moved (first echo from the standby) |
| --- | --- | --- |
| Both directions dead | 118 to 122 ms (oldest-unacked rule) | 51 to 53 ms (one 50 ms RTT) |
| Only the uplink dead (the core still sends) | 138 to 151 ms | 51 to 54 ms |
| 10% loss both ways for 2 s | not declared dead (no false alarm) | |
| Primary closed by the core | 2 to 5 ms | |

Detection stays under the 300 ms target. Audio is back at the standby core
170 to 205 ms after the primary died, against the 500 ms target in
[fm-media.md](fm-media.md#beyond-facetime).

`TestMultipathBulkSpreadsAndKeysUpdate`: 3,000 1000-byte Bulk datagrams on
two loopback paths, with a key update every 200 packets: both paths carry
traffic (824 and 2,255 packets in one run), 15 key updates on one side and
30 on the other, no errors.

### Deviations and choices

- Path 0 connection ids use NEW_CONNECTION_ID and RETIRE_CONNECTION_ID
  rather than the PATH_ variants (allowed; the draft says SHOULD use the
  PATH_ ones).
- Congestion control is NewReno per path, uncoupled (no LIA/OLIA, which the
  draft offers but does not require). On a shared bottleneck a Bulk sender
  can take more than one connection's share. FM Media's own rate control
  sits above it and counts all paths.
- A server that migrates within a path id (NAT rebinding on that path) keeps
  that path's peer connection id instead of switching to a fresh one.
- Path state is created when the first packet decrypts on a new path id,
  before validation (the draft says SHOULD allocate after validation). It is
  bounded by the path caps and can only be created with the connection's
  keys.
- Stream data never spreads over paths: it uses the primary, to avoid
  head-of-line blocking on the slower path.
- Immediate acknowledgements for DATAGRAM, PING and path-probe packets are
  our choice to keep RTT and failure detection tight. The draft leaves ACK
  policy open. The suspect timer assumes the peer does this, so a foreign
  multipath peer that delays those ACKs may see extra primary switches.
- A key update is still started after `KeyUpdateInterval` packets once the
  previous update is acknowledged, not after 3 × the largest PTO (draft 2.5
  SHOULD). At the default 2^22 packets this never comes early.

## What is not implemented

- 0-RTT and session resumption: the server sends no session tickets and
  drops 0-RTT packets. NEW_TOKEN is ignored, so a client returning later
  goes through Retry again if Retry is on.
- ECN: we do not mark packets, and ECN counts in received ACKs are ignored.
- Path MTU discovery: packets are at most `MaxPacketSize` (default 1200,
  configurable up to 1452). The larger size is used only after the handshake
  is confirmed.
- `preferred_address` (server migration), `disable_active_migration`
  enforcement, `grease_quic_bit`, QUIC v2, compatible version negotiation
  (RFC 9368), ACK frequency, `reset_stream_at`.
- Cubic. NewReno only; FM Media's own delay-based rate control sits above it
  ([fm-media.md](fm-media.md), layer 3).
- HTTP/3: no dynamic QPACK table (we advertise capacity 0 and open no
  encoder or decoder streams, which RFC 9204 4.2 allows). No server push,
  no GOAWAY sending, no priorities. Regular requests get 404 unless a
  `Handler` is set; the package exists for extended CONNECT.
- WebTransport: no flow-control capsules (draft-07+ WT_MAX_* are not
  sent), DRAIN_WEBTRANSPORT_SESSION is accepted and ignored, no
  `wt-available-protocols` subprotocol negotiation, no HTTP/2 fallback.

## Limits and caps (defaults; all in `quic.Config`)

| What | Default | Hard cap |
| --- | --- | --- |
| Connections per endpoint / per source IP | 10,000 / 64 | |
| Half-open handshakes before Retry is forced | 128 | |
| Handshake timeout / idle timeout | 10 s / 30 s | |
| Incoming streams per direction | 100 | 65,536 |
| Stream receive window / connection window | 512 KiB / 4 MiB | 16 MiB / 64 MiB |
| Out-of-order gaps per stream | 1,024 (more is PROTOCOL_VIOLATION) | |
| Unsent plus unacked bytes per stream (Write blocks above it) | 1 MiB | |
| CRYPTO data buffered ahead per space | 64 KiB (CRYPTO_BUFFER_EXCEEDED) | |
| Pending datagrams, each direction | 128 (send: error; receive: dropped) | |
| Peer connection ids kept / pending retirements | 4 / 32 | 8 |
| Addresses per path / outstanding challenges / pending responses | 4 / 4 / 4 | |
| Multipath paths open (`MaxPaths`) / including abandoned paths waiting 3 PTO | 4 / 8 | 8 / 16 |
| Connection ids we issue per extra path id / dedup keys (`DatagramDedupKey`) | 2 / 4,096 | |
| Redundant copy waiting for window after the first copy left | 100 ms | |
| Datagrams queued from socket to connection | 256 (dropped above) | |
| ACK ranges parsed / sent / tracked | 256 / 32 / 64 | |
| Stateless replies (reset, Retry, version negotiation) | 200 per second per endpoint | |
| HTTP/3 field section / unknown frame / control frame | 16 KiB / 64 KiB / 16 KiB | |
| WebTransport sessions per connection / queued streams / queued datagrams per session | 4 / 32 / 256 | |
| WebTransport streams for not-yet-known sessions | 8, for at most 5 s | |
| WebTransport capsule size | 64 KiB | |

## Security choices

- All attacker input is length-checked before use. Frame parsing works on
  slices and never allocates by a peer-chosen length. Varints are bounded,
  frame types must use the minimal encoding, stream offsets are capped at
  2^62, `retire_prior_to` must not exceed the sequence number, and CID
  lengths must be 1 to 20. Frames not allowed in a packet type are
  PROTOCOL_VIOLATION.
- Receive memory is bounded by flow control: each stream buffers up to its
  highest received offset, and connection flow control counts highest
  offsets, so a sparse sender pays for the memory it causes.
- An Initial from an unknown client creates connection state only if it is
  at least 1200 bytes, has a DCID of at least 8 bytes, decrypts, and passes
  the per-IP and total caps.
- Reserved header bits are checked after decryption. A failed decryption
  drops the packet silently and only counts toward the AEAD integrity limit.
  Retry tags, reset tokens and Retry tokens are compared in constant time.
- Version negotiation without v1 aborts the client (no downgrade). A VN
  packet is ignored after any other packet from the server.
- The client accepts one Retry, only before any server Initial, only with a
  valid tag, and only with an SCID that is not its original DCID.

## Deviations from the RFCs

- New paths carry only probing frames until they are validated (stricter
  than RFC 9000 9.4).
- A Retry token that is invalid or from another address is dropped without
  sending INVALID_TOKEN (RFC 9000 8.1.3 allows either).
- Persistent congestion is computed over the packets declared lost in one
  pass, and cwnd grows only when at least half the window was in flight
  (simpler forms of RFC 9002 7.6 and 7.8).
- ACK ranges older than the 64 most recent are forgotten. Packets below that
  point count as duplicates and are dropped unacknowledged; the peer resends
  their frames.
- A client that rebinds with no spare connection id keeps its current id on
  the new path (RFC 9000 9.5 recommends against this; it can only happen if
  the server issues a single id).

## Chromium WebTransport

Measured with headless Chromium 149 and 151 (Chrome for Testing, Playwright's
`chromium_headless_shell`):

- Chromium sends SETTINGS `QPACK_MAX_TABLE_CAPACITY=65536`,
  `MAX_FIELD_SECTION_SIZE=16384`, `QPACK_BLOCKED_STREAMS=100`,
  `H3_DATAGRAM (0x33)=1`, `H3_DATAGRAM draft-04 (0xffd277)=1`,
  `ENABLE_WEBTRANSPORT (0x2b603742)=1` and a GREASE id. It does not send
  draft-07's `WEBTRANSPORT_MAX_SESSIONS (0xc671706a)`, so it negotiates
  WebTransport draft-02. It sends `sec-webtransport-http3-draft02: 1` on the
  CONNECT and expects `sec-webtransport-http3-draft: draft02` back.
- Its client needs from the server: `ENABLE_CONNECT_PROTOCOL=1`,
  `H3_DATAGRAM=1`, a WebTransport version setting, and the QUIC transport
  parameter `max_datagram_frame_size`. Our server sends all of them plus the
  draft-07 setting, so newer clients get draft-07. The stream and datagram
  wire format is the same in both drafts.
- With `serverCertificateHashes`, Chromium wants an ECDSA P-256 certificate
  valid for at most 14 days; the test makes one for 10 days. The page is
  served from `http://127.0.0.1`, which counts as a secure context.
- Chromium negotiated TLS_AES_128_GCM_SHA256 with X25519. Its closing
  `CLOSE_WEBTRANSPORT_SESSION` capsule reaches the server with its code and
  reason.

## Tests

```
cd server
go vet ./rtc/quic ./rtc/h3 ./rtc/webtransport
go test -race ./rtc/quic ./rtc/h3 ./rtc/webtransport
FORKMESH_TEST_CHROMIUM=/path/to/chrome-headless-shell go test -run Chromium -v ./rtc/webtransport
go test -run '^$' -fuzz FuzzConnFrames -fuzztime 60s ./rtc/quic
go test -run '^$' -fuzz FuzzMultipathConnFrames -fuzztime 60s ./rtc/quic
go test -run 'Multipath|Standby' -v ./rtc/quic
go test -run '^$' -bench . ./rtc/quic
```

- `quic`: RFC 9001 Appendix A vectors (A.1 keys, A.2 client Initial, A.3
  server Initial, A.4 Retry, A.5 ChaCha20 short header), RFC 8439 ChaCha20,
  Poly1305 and AEAD vectors, and RFC 9000 packet-number and varint examples.
  Loopback tests cover:
  - handshake, streams and datagrams;
  - handshake flights lost, 8% loss on a 3 × 1 MiB echo, 20% datagram loss;
  - migration to a new local port mid-transfer, NAT rebinding through a
    relay, and a spoofed-address race;
  - Retry, version negotiation, 3× amplification with a 12 KB certificate,
    and stateless reset;
  - key updates, idle timeout and keep-alive, application close;
  - stream limits and credit, small flow-control windows, RESET_STREAM and
    STOP_SENDING, unidirectional streams;
  - per-IP caps and connection-id retirement rules;
  - an end-to-end ChaCha20 handshake, run in a child process with
    `GODEBUG=cpu.aes=off` so Go prefers ChaCha20;
  - multipath: negotiation and a second path that carries a stream after
    path 0 is abandoned; path A failing under 50 pps Redundant and Fastest
    audio, both directions, with independent netsim impairment per path; a
    planned handover; six abandon and re-add rounds past the initial path
    limit; a replayed packet from a spoofed address and a forged packet for
    an unused path id; 3× amplification and validation timeout on a path
    whose client never answers; backup status; Bulk spreading with key
    updates; fallback when either side lacks multipath, including
    single-path migration; the draft's nonce example and frame round trips;
  - the standby helper: a dead core (both directions, uplink only), a lossy
    core that must not be declared dead, and a closed core.

  Fuzzers: frames (multipath frames included), packet headers, transport
  parameters, frames fed into a live connection, frames fed into a live
  multipath connection on either side, and raw datagrams fed into a
  listening endpoint.
- `h3`: RFC 7541 Huffman examples, RFC 9204 B.1, QPACK round trips and
  rejection of dynamic-table references, SETTINGS rules, request validation,
  QPACK and SETTINGS fuzzers.
- `webtransport`: a Go client against our server covering datagrams, bidi
  and uni streams both ways, close codes, path and origin rejection, and
  error-code mapping. Headless Chromium then opens a session with
  `serverCertificateHashes` three ways (direct, through Retry, and with 5%
  packet loss) and checks:
  - 10 datagrams are echoed;
  - a 200 KB bidirectional stream comes back intact;
  - a unidirectional stream gets a reply;
  - Chromium accepts a server-opened stream and replies on it;
  - its close code and reason reach the server.
  It skips when no Chromium is found.

Loopback throughput is about 90 MB/s on one stream at 1200-byte packets, and
a datagram round trip takes about 50 µs (i9-10980XE).
