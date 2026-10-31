# FM Media call server (phase 4)

Phase 4 of [FM Media](fm-media.md): the SFU forwarding core of
[fmm-protocol.md](fmm-protocol.md) on real connections. It runs inside
`forkmesh-calls`, beside the WebRTC SFU of [rtc.md](rtc.md), with the same
room model: the same tickets and room ids, at most 4 places per account in a
room, one server-wide cap for both engines, and the same browser origin
list. Built 2026-10-21.

| Package | Role |
| --- | --- |
| `server/rtc/fmmserver` | The server: QUIC listener, WebTransport sessions, admission, standby parking, per-room workers, certificate rotation |
| `server/rtc/fmmcall` | The client side the phones and the Go test clients use: pinned TLS, a session binding `fmm.Client` to a QUIC connection, and the move to the standby core |
| `server/rtc/signal` | Tickets (two new optional fields) and `RoomEngines`, the room registry both engines share |
| `server/cmd/forkmesh-calls` | Starts the listener when `FORKMESH_CALLS_FMM` names an address, and adds the FM Media block to `/calls/health` |

Deploy settings: [../deploy/calls.md](../deploy/calls.md#fm-media). The app's
side (the ticket response): [../app/chat-group-calls.md](../app/chat-group-calls.md#fm-media-tickets).

## One UDP port, two ways in

One `quic.Endpoint` on one UDP port (4433 on the cores) serves both kinds of
client. The TLS ALPN decides which:

- **`fmm/1`: phones and Go clients** over raw QUIC. The client opens one
  bidirectional stream, the fmm control stream, and sends media as QUIC
  datagrams.
- **`h3`: browsers** over WebTransport at `https://<core>:4433/calls/fmm`.
  `webtransport.Server.Check` accepts only that path and an allowed
  `Origin` (`FORKMESH_CALLS_ORIGINS`, the WebRTC list). Then it is the same
  as above: one bidirectional stream for control, and session datagrams for
  media. One session per connection.

Both run the same code from there on (`serve` over a small `link`
interface). On every connection:

1. The control stream must open within 10 s, and its first message must be
   `Join` with a ticket. The server reads exactly that one message itself,
   before any room exists. It checks the ticket with `signal.VerifyTicket`
   (HMAC under `FORKMESH_CALLS_SECRET`, 60 s lifetime). The ticket must say
   `"engine":"fmm"`; a WebRTC ticket is refused here, and an FM Media
   ticket is refused by the WebRTC SFU.
2. Admission (`enter`, under the server lock):
   - `RoomEngines.Join(room, "fmm")` enforces the server cap (400 people over
     both engines) and the one-engine-per-room rule;
   - the room's per-account count enforces at most 4 places per account;
   - the room's `sfu.Room` is created on first use, with its own worker.
3. The server creates the hop state (`sfu.Room.Connect`, with the
   connection's transport) and replays the `Join` into it. `Admit` returns
   the ticket's peer id, which the server registered under that exact ticket
   a moment before. The sfu core then refuses a peer id already in the room.
4. Two goroutines per connection: the control reader, which feeds
   `Peer.HandleControl`, and the datagram reader, which feeds
   `Peer.HandleDatagram` within a budget. The room's worker calls
   `Room.Tick` every 5 ms; this drives pacing, feedback, allocation and
   hints. Nothing runs per packet beyond the forwarding itself.
5. When the control stream ends, the core closes the peer (a `Leave`, a
   broken control stream, a refused join), or the connection dies, the peer
   leaves. The account place and the engine claim are released at once, and
   the room and its worker go when the last peer leaves.

Errors go back as fmm `Error` messages before the connection closes. The
server waits up to 1 s for the client to close first, so the message
arrives. The codes are 401 (ticket refused, not a join, peer id in use), 503
(room, account, standby or server full) and 409, which is new: the room runs
on the other engine, and the text names it (`engine webrtc`).

### Per-connection plumbing

- **Control writes never block the room.** The sfu core sends control
  messages while it holds the room lock, and a QUIC stream write blocks when
  its send buffer is full. Each connection therefore has a writer goroutine
  with a queue of at most 1 MiB. A peer that lets it fill is closed (code
  0x12, "control backlog"); it is not allowed to stall the room.
- **Datagram budget per connection:** 1,500 datagrams and 1 MB per second
  (about 8 Mbps), and none over 1,500 bytes. The rest are dropped before
  the room lock.
- **Audio over every path.** The transport learns which downstream stream
  ids carry audio from the `TrackAdded` messages it relays. When the
  connection negotiated multipath and has two usable paths (checked at most
  every 200 ms), audio media and repair datagrams go `quic.Redundant`, a
  copy on each path. Everything else goes `Fastest`. The client does the
  same with the audio streams it publishes.
- **Duplicates** from redundant sending are dropped by the transport
  (`Config.DatagramDedupKey = wire.DedupKey`: kind and transport sequence of
  media, repair and padding datagrams).

### Limits

| What | Limit |
| --- | --- |
| Connections, including ones not yet joined | 1,000 |
| Connections between accept and join (not parked) | 200 |
| Join timeout | 10 s |
| People on the server (both engines) | 400 (`sfu.MaxServerPeople`) |
| Places per account in a room | 4 |
| Peers per room / tracks per peer | 16 / 4 (sfu core defaults) |
| Parked standby connections / per account | 400 / 8 |
| Parked lifetime | 12 h |
| Control backlog per connection | 1 MiB |
| Datagrams per connection per second | 1,500 and 1 MB |
| QUIC idle timeout / connections per IP / streams per direction | 30 s / 32 / 8 |
| Downstream bitrate per subscriber (start, min, max) | 600 kbps, 50 kbps, 2.5 Mbps |

## Certificates

Browsers connect with WebTransport's `serverCertificateHashes`, which
Chromium accepts only for an ECDSA P-256 certificate valid for at most 14
days. Phones pin the same hashes. The core makes its own certificates
(`CertStore`):

- Each certificate: a new P-256 key, self-signed, valid from 1 h in the past
  to 13 days ahead (13 days 1 hour in all, under the 14-day ceiling).
- Two at a time: **current**, the one the server presents, and **next**,
  published but not yet presented. A new certificate is made every 6 days.
  It is published as next for 6 days, then presented as current for 6 days,
  and it still has a day of validity left when it is retired. A check runs
  every hour.
- `/calls/health` publishes `[current, next]` as base64 SHA-256 of the
  certificate DER. The app copies them into the ticket response. Any hash
  list a client received in the last 6 days therefore contains the
  certificate the core presents now, so a rotation never fails a client
  holding a fresh ticket. Clients refetch tickets far more often than that.
- The private keys never leave the core. They are kept as PEM (0600, written
  through a temporary file and a rename) in the unit's `StateDirectory`
  (`/var/lib/fmnext-calls`). A restart therefore keeps the hashes, and
  tickets handed out just before it still work. A missing, broken or expired
  file is replaced; a broken current certificate is replaced by the
  published next one.
- Clients (`fmmcall.PinnedTLS`) accept a leaf only when its SHA-256 is in the
  list, its key is ECDSA P-256 and it is within its validity, at most 14
  days long. They skip chain and name checks: the pin is the whole trust
  decision.

**Why not a CA certificate.** A browser with a CA certificate would need no
hashes, but:

- ACME for this port would be another moving part on every core. The
  HTTP-01 and TLS-ALPN-01 challenges are TCP, so the UDP listener cannot
  answer them itself; it would depend on Caddy's issuance and on copying its
  key across.
- Sharing Caddy's certificate would put the key of the web front door (every
  `nN.next` name) into the calls process, so a bug in our QUIC code could
  leak the key that serves the app.
- The hashes travel inside the ticket response, which already comes over the
  app's own HTTPS and is already the trust anchor for the call (it carries
  the ticket). Pinning adds no new trust, and an expired or revoked public
  CA cannot take calls down.

**Downsides.**

- Only clients that got a ticket response can connect: no plain-URL access.
  A browser without `serverCertificateHashes` support cannot use FM Media
  and stays on WebRTC.
- The cores' clocks matter: a core whose clock is days off could present a
  certificate a client thinks expired. The cores keep NTP time (UDP 123 and
  323 are in their IDS listener list); a clock alarm would be worth adding.
- A core restored from an old snapshot presents certificates nobody has
  published. The app learns the new hashes within 10 s (the health cache),
  and until then that core's calls fail and clients fall back to WebRTC.

## Multipath, standby and failover

### Multipath

The endpoint enables `quic.Config.Multipath` (4 paths). Native clients open
a second path (`Conn.OpenPath`, cellular beside Wi-Fi) and audio goes
redundant as described above. Chromium does not negotiate multipath, so
browsers stay single-path (and still get migration).

### Datagrams and QUIC's own congestion control

The first three-person run over 5% random loss collapsed the lossy
participant's link: RTT 1.87 s, almost nothing delivered. The cause was
QUIC's NewReno. It halves the window on every loss event, and at 5% random
loss there is one every round trip, so the window sat at its 2-packet
minimum. About 26 kB/s got through, and the media queued behind it in the
datagram send queue for seconds. The engine's own controller (delay
gradient plus loss, with FEC and DRED, [fmm-protocol.md](fmm-protocol.md))
never got a chance to act.

Both ends now set `quic.Config.DatagramLossExempt`
([quic.md](quic.md#datagram-loss-and-the-media-layer)). When a lost packet
carried only DATAGRAM, ACK or PING frames, the window is not reduced: the
media layer's controller answers media loss. Stream data (the control
stream) still backs off as usual, and persistent congestion still collapses
the window. Same run afterwards: RTT 92 to 117 ms, and 83 to 97% of the
audio played.

### Standby core: rejoin on the second rendezvous choice

Rooms stay per core. That leaves two ways to survive a core's death:

1. Rooms that span cores, with a core-to-core relay (cascade).
2. Every client keeps a warm connection to the next core, and everyone moves
   there together.

We built option 2. It needs no inter-core protocol and no shared room state.
It works because rendezvous ordering is the same for every participant: the
app ranks the cores for a room by SHA-256(room + node) over the healthy ones.
The first is the core for the room (the same core the WebRTC ticket names),
and the second is the standby. With highest-random-weight hashing, removing
the first core makes the second one the first, so:

- every client of a room holds a standby connection to the same core;
- when the primary dies, all of them land on that core together;
- a client that asks the app for a new ticket after the death gets that same
  core too.

How the standby works:

- The ticket response carries a second ticket for the standby core, with
  `"standby":true` and the same peer id. The client dials the standby core
  at once (`quic.NewStandby`), opens the control stream and sends `Join`
  with the standby ticket.
- The server checks that ticket immediately, while it is fresh, and
  **parks** the connection. It is in no room, holds no account place and
  claims no engine, and the server sends it nothing. The client keeps it
  alive with a PING every second.
- The server accepts a parked connection's next message only if it is a
  `Join` with the identical ticket string. That activates it: the normal
  admission of step 2 above, without the 60 s expiry, since the ticket was
  checked when it arrived. Anything else is refused. The parked lifetime is
  12 h.
- The client watches the primary (`Standby.PrimaryDead`: closed, two probe
  timeouts on every path, or acknowledgements stopped). It then promotes the
  standby connection, starts a new `fmm.Client` on the already open stream,
  sends the `Join` again (the activation, half a round trip), and publishes
  its tracks again (`fmmcall.Call`, `OnSession(s, moved=true)`).
  Subscriptions follow from the `TrackAdded` messages, as on any join. No
  handshake, no ticket fetch, no ICE.

Measured with `TestCoreDeathMovesTheCallToTheStandbyCore`: three in-process
cores, each behind a netsim relay with 20 ms (±2 ms) one way to every
client, three clients sending 50 audio frames a second. All three hold a
standby on the second rendezvous core, and the third core must see no
joins. Two ways to kill the primary: **silent**, where its relay drops
everything both ways (power loss, network partition), and **closed**, where
the process stops and sends CONNECTION_CLOSE (crash, restart, `systemctl
stop`). The gap is the time from the death to the first audio frame the
listener's jitter buffer plays from that speaker on the new core. Runs on
2026-10-21 on the i9-10980XE (load 5), plain and `-race`, 3 to 4 runs each:

| Death | Judged dead after | Audio back after (all 6 listener-speaker pairs) | Longest silence |
| --- | --- | --- | --- |
| Silent | 120 to 145 ms (no acknowledgement) | 303 to 331 ms | 260 to 301 ms |
| Closed | 22 to 27 ms (closed) | 174 to 210 ms | 160 to 201 ms |

Both are under the 500 ms target of [fm-media.md](fm-media.md#beyond-facetime).
Where the silent case's ~310 ms goes:

- about 125 ms to judge the primary dead, which is the standby helper's rule
  of 2 × (srtt + 4 rttvar + max_ack_delay);
- about 20 ms for the activating `Join` to arrive;
- about 40 ms for `TrackAdded` and `Subscribe`;
- about 20 ms for the first forwarded frame;
- the rest is the new jitter buffer filling to its starting depth.

Older frames already queued were played until the death showed, which is
why the longest silence is shorter than the time to recovery.

**Partial failures.** If only one client loses its primary (its own network
path to that core breaks), that client moves alone and the call splits
across two cores. A client that finds itself alone on the standby core
should ask the app for a new ticket. The app's health probe still sees the
primary, so the ticket names it, and the client rejoins there. This is the
same convergence the WebRTC client relies on. When a dead core comes back,
new joiners rank it first again while the moved call is on the standby.
WebRTC has the same gap today. A per-room "where is it now" lookup between
cores would close it for both engines; not built.

## WebRTC and FM Media in one room: later

Decided 2026-10-21: no bridge in this phase. A room runs on one engine at a
time, and **the first joiner's engine holds it until the room empties**.
`signal.RoomEngines` is shared by both servers in the process:

- each join claims the room for its engine;
- a join from the other engine is refused (WebRTC: error `room_engine` with
  the engine; FM Media: Error 409 `engine webrtc`);
- the claim is released with the last leave.

The core decides, not the app. The app cannot see joins and leaves, and two
ticket requests at the same moment could otherwise send the first two
people to different engines. A client that speaks both asks the app for both
(`"engines": ["fmm", "webrtc"]`). It tries FM Media first, and on 409 joins
with the WebRTC ticket from the same response, with no second request. Old
clients ask for nothing and get the old response unchanged.

The cost: an old WebRTC-only client cannot join a call that started on FM
Media. FM Media ships disabled ([../deploy/calls.md](../deploy/calls.md#fm-media)),
so this cannot happen until it is switched on. A bridge before then would
need, for every mixed pair:

- translating SRTP and RTP into fmm datagrams, both ways;
- feedback translation, from transport-wide CC to fmm reports and back;
- a keyframe-only recovery path for WebRTC receivers.

That is the size of another SFU. It waits until a bench shows mixed rooms
are needed during the switch (phase 8).

## Datagram size across both kinds of client

The SFU forwards a publisher's datagrams unchanged in size, so a datagram
must fit every subscriber's transport:

- a native QUIC connection at the default 1,200-byte packets carries 1,156
  bytes of datagram;
- a WebTransport session carries one byte less (its quarter-stream-id
  prefix).

`fmmcall.Session` therefore reports the QUIC maximum minus 4 to the engine,
and the browser engine must do the same (phase 6).

## Tests

```
cd server
GOTOOLCHAIN=local go vet ./rtc/... ./cmd/forkmesh-calls/...
GOTOOLCHAIN=local go test -race ./rtc/fmmserver ./rtc/fmmcall ./rtc/signal ./rtc/sfu ./cmd/forkmesh-calls
GOTOOLCHAIN=local go test -run CoreDeath -v ./rtc/fmmserver      # the failover table above
FORKMESH_TEST_CHROMIUM=/path/to/chrome-headless-shell go test -run Chromium -v ./rtc/fmmserver
```

- `TestJoinAdmitsOnlyFMMTicketsAndEnforcesCaps`: a WebRTC ticket, a forged
  ticket, a message before `Join` and a duplicate peer id are refused (401);
  a fifth place for one account is refused (503); a room another engine
  holds is refused (409 `engine webrtc`), and so is a WebRTC claim on an FM
  Media room.
- `TestRoomEmptiesAndReleasesItsEngine`, `TestStandbyParksUntilItsTicketComesAgain`
  (parked, invisible, activated by the same ticket, refused by any other
  message), `TestPinnedTLSRefusesOtherCertificates` (foreign hashes, and the
  next certificate's hash alone: it is not presented yet).
- `TestCertificatesRotateWithAnOverlapAndPersist`: 40 days on a fake clock.
  Every day the presented certificate is among the hashes published the day
  before, the next one takes over every 6 days, and each is ECDSA P-256 and
  at most 14 days long. The files are 0600 and survive a restart; a broken
  file is replaced by the next one; after a 30-day outage nothing expired is
  kept.
- `TestThreePeopleHoldACallThroughImpairedLinks`: three Go clients
  (`fmm.Client` over `fmmcall` over our QUIC), each with audio and a video
  track whose synthetic encoder follows `VideoBitrate`, marks long-term
  frames and answers `EventRecover`. The links through netsim:
  - 20 ms ± 3 ms;
  - 30 ms ± 5 ms with 3% loss;
  - 45 ms ± 8 ms with 5% loss at 3 Mbps.

  Over 3 s every participant must play at least 70% of each other's audio
  frames. Each must decode video frames too: at least 20 on the first two
  links, and at least 5 on the 5% link. Typical runs:
  - audio 90 to 99% on the clean pairs, 83 to 97% to and from the 5% link;
  - video 90 to 150 frames on the first two links, 17 to 105 on the 5% one.

  The rest is engine work: audio concealment at 5% loss (DRED is off in the
  Opus build, [fmm-protocol.md](fmm-protocol.md#not-done-yet)), and video
  recovery for a subscriber whose own link loses 5%.
- `TestCoreDeathMovesTheCallToTheStandbyCore`: the table above.
- `TestChromiumJoinsOverWebTransport`: headless Chromium opens a
  WebTransport session to `/calls/fmm` with both published hashes. A
  minimal page joins with a ticket, publishes an audio stream, subscribes to
  the `TrackAdded` it gets for a native Go peer in the same room, and sends
  120 media datagrams. Both sides must receive: in the first run the
  browser got 137 media datagrams and 48 feedback datagrams, and the native
  peer played 70 of the browser's frames. The origin check saw the page's
  origin. It skips when no Chromium is found.
- `cmd/forkmesh-calls`: `/calls/health` without and with the listener (port,
  path, ALPN, two hashes, `no-store`), the state directory from
  `STATE_DIRECTORY`, and the refusal to start without one.
- `rtc/signal`: the registry (cap across engines, release, nil registry);
  the WebRTC ticket bytes unchanged by the new fields; the standby ticket
  vector shared with `app/tests/test_chat_call_fm_media.py`.
- `rtc/sfu`: `TestAWebRTCRoomRefusesTheOtherEngineUntilItEmpties`.
- `rtc/quic`: `TestDatagramLossExemptKeepsTheWindowOpen` (10% loss: the
  window stays 12,000 bytes instead of 2,400) and
  `TestDatagramLossExemptStillBacksOffForStreams`.

## Not done yet

- The bridge to WebRTC (above).
- A client that ends up alone on the standby core rejoins through the app;
  `fmmcall` reports the move but leaves that decision to the engine.
- After a move, the next standby is the third rendezvous core. Reaching it
  needs a new ticket response, which the client fetches as on any rejoin.
  The standby is not re-armed by itself.
- Redialing a lost standby connection reuses its ticket, which only works
  within 60 s of the ticket response; after that the client needs a new
  response.
- No per-track packet cache shared across subscribers (as in
  [fmm-protocol.md](fmm-protocol.md#not-done-yet)).
