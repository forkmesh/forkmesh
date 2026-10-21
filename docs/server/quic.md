# QUIC, HTTP/3 and WebTransport (FM Media transport)

The transport under FM Media ([fm-media.md](fm-media.md)): our own QUIC v1,
a minimal HTTP/3 and WebTransport over HTTP/3, in Go, standard library only
(no quic-go, no x/crypto, no x/net). The TLS 1.3 handshake is `crypto/tls`'s
QUIC API (`tls.QUICServer`, `tls.QUICClient`, `QUICConn` events); everything
else, packet protection included, is ours.

| Package | Role | Spec |
| --- | --- | --- |
| `server/rtc/quic` | QUIC v1 endpoint, server and client | RFC 9000, 9001, 9002, 9221 |
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
| Paths per connection / outstanding challenges / pending responses | 4 / 4 / 4 | |
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
    `GODEBUG=cpu.aes=off` so Go prefers ChaCha20.

  Fuzzers: frames, packet headers, transport parameters, frames fed into a
  live connection, and raw datagrams fed into a listening endpoint.
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
