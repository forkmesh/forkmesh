# FM Media engine core: protocol and algorithms

This page specifies layers 2 to 4 of [FM Media](fm-media.md): the media
protocol (datagrams and the control stream), loss recovery (FEC and
retransmission), congestion control and pacing, the jitter buffers, the
session layer (`fmm.Client`) and the SFU forwarding core. It also records the
measurements that shaped them and the decisions taken on the way
(2026-10-21). The code is `server/rtc/fmm/`, standard library only, in the
`forkmesh.com/mirror-node` module.

## Packages

| Package | Role |
| --- | --- |
| `fmm` | `Client`, the session layer the phones bind with `gomobile` |
| `fmm/wire` | Datagram headers, control messages, the control stream reader |
| `fmm/frag` | Packetizer (frames to datagrams) and reassembler |
| `fmm/fec` | GF(256) Reed-Solomon (XOR as its first row), packet-level FEC encoder/decoder, the FEC policy |
| `fmm/cc` | Receiver feedback recorder, the sender's congestion controller, the pacer |
| `fmm/hop` | One hop's sender half (FEC, history, pacing, CC, probes) and receiver half (FEC recovery, feedback, NACK) |
| `fmm/jitter` | Audio and video jitter buffers |
| `fmm/sfu` | SFU forwarding core: rooms, per-subscriber forwarding, layer choice, publisher hints |
| `fmm/link` | The transport interface, a discrete-event simulator and an impaired in-memory link |
| `fmm/sim` | End-to-end call simulations (tests only) |
| `fmm/internal/seqnum` | 16- and 32-bit sequence unwrapping |

## Transport contract

The core never owns a socket or a goroutine. It is driven by calls:

```go
type Transport interface {
    SendDatagram(p []byte) error
    MaxDatagramSize() int
    SendControl(p []byte) error
}
```

`SendDatagram` is unreliable and unordered (QUIC DATAGRAM frames, RFC 9221);
`SendControl` writes bytes to one reliable ordered stream. The transport
delivers what it receives by calling `HandleDatagram(p, now)` and
`HandleControl(p, now)` on the `Client` or SFU `Peer`. Control bytes may
arrive in any chunking; `wire.Reader` reassembles messages. Every method takes
`now` in microseconds on any monotonic clock the caller chooses, and
`Tick(now)` must be called about every 5 ms (it returns the next wake time).
Received buffers are copied before the call returns, so a transport may reuse
them. One reader goroutine per connection is enough; nothing runs per packet.

The same `Client` runs in the phones through `gomobile bind`, so its exported
API uses only `int`, `int64`, `float64`, `bool`, `string`, `[]byte`, pointers
to structs of those, and the `Transport` interface. Nothing uses reflection.
The client is safe for concurrent calls (one mutex); the SFU room has one
mutex per room.

The engine uses at most 1200-byte datagrams (`hop.MaxPacketSize`) or the
transport's maximum when smaller. A media datagram leaves 22 bytes
(`wire.FECReserve`) unused so its repair symbol always fits in a datagram.

## Datagrams

All multi-byte fields are big endian. The first byte is `version << 4 | kind`
with version 1. A datagram longer than 1500 bytes, with another version or an
unknown kind, is dropped. Transport sequence numbers (bytes 2-3 in every kind)
count every datagram a hop sends, so congestion feedback covers media, repair,
retransmissions and padding alike.

### Media (kind 0), 16-byte header

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `0x10` |
| 1 | 1 | stream id (per connection and direction) |
| 2 | 2 | transport sequence |
| 4 | 2 | stream sequence |
| 6 | 2 | frame id |
| 8 | 1 | fragment index |
| 9 | 1 | fragment count (1..255) |
| 10 | 1 | flags: `0x80` keyframe, `0x40` FEC-protected, `0x38` reserved (must be 0), `0x07` temporal layer |
| 11 | 1 | reference delta: the frame this one depends on is `frame id - delta`; 0 means none |
| 12 | 4 | timestamp: the frame's send time in µs on the sender's clock, wrapping |
| 16 | n | payload: one fragment of the end-to-end encrypted frame |

Rules: `index < count`; a keyframe has layer 0 and delta 0; an audio frame
has delta 0 and no keyframe flag (every Opus frame stands alone); a video
non-keyframe has delta 1..255. The parser refuses anything else.

The stream sequence numbers are gap-free per stream as the receiver sees
them. The SFU rewrites them per subscriber (below), so a gap always means a
lost packet and never a layer the SFU chose not to forward.

### Repair (kind 1), 10-byte header plus member list

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `0x11` |
| 1 | 1 | stream id |
| 2 | 2 | transport sequence |
| 4 | 2 | base sequence: the lowest stream sequence in the group |
| 6 | 1 | scheme: 0 XOR (M must be 1), 1 Reed-Solomon |
| 7 | 1 | K, the number of protected packets (1..48) |
| 8 | 1 | M, the number of repair packets in the group (1..16) |
| 9 | 1 | index of this repair (0..M-1) |
| 10 | K | member offsets: each protected packet is `base + offset`; strictly increasing, first 0 |
| 10+K | n | repair symbol |

Each protected media packet contributes its recovery block: a 2-byte length
L, then its bytes from offset 6 to the end (frame id, fragment fields, flags,
reference, timestamp, payload). Blocks are zero-padded to the longest in the
group and coded; a recovered block rebuilds the datagram with stream id,
stream sequence (`base + offset`) and transport sequence 0.

The member list is explicit because groups do not have to be consecutive
ranges. An earlier layout (base plus K consecutive sequences) broke on the
SFU's downstream: every upstream gap and every out-of-order packet recovered
upstream closed the group, leaving groups of one or two packets. With explicit
members a group spans gaps and reordering, and the encoder keeps two groups
open per video stream (below).

### Padding (kind 2), 4-byte header

`0x12`, a zero byte, the transport sequence, then any bytes. Padding fills
bandwidth probes and is otherwise ignored.

## Fragmentation and reassembly

`frag.Packetizer` splits a frame into `ceil(len / maxPayload)` fragments of
nearly equal size (equal sizes keep FEC padding small), at most 255, so a frame
can be up to 255 × 1162 bytes (about 290 kB). Larger frames are refused
(`ErrTooLarge`). The packetizer assigns frame ids and references for temporal
layers: a keyframe resets the structure; a layer-0 frame references the
previous layer-0 frame; a layer-L frame references the most recent frame of
layer below L. That is the L1T2 and L1T3 structure the encoders produce, and
any structure whose frames reference the latest lower layer.

`frag.Reassembler` unwraps frame ids, sequences and timestamps, keeps at most
64 partial frames and 4 MiB, drops a frame whose fragments disagree on count,
flags, reference or first sequence, ignores duplicates (also of frames
already delivered, through a 512-entry ring), and expires partial frames after
1 s (`Expire` reports them, with their layer when known, to the video buffer).
A complete frame carries its first and last stream sequence and its
completion time.

## Control stream

Each message is `type (1) | length (2) | body (length)`, with the body at most
8192 bytes and fully consumed (no trailing bytes). Strings are a 2-byte length
and UTF-8 bytes. `wire.Reader` refuses an unknown type, an oversized length or
more than 1 MiB buffered, after which the stream is broken and the connection
should close.

| Type | Message | Direction | Body |
| --- | --- | --- | --- |
| 0x01 | Join | client → SFU | ticket (1..2048 bytes) |
| 0x02 | Joined | SFU → client | peer id (1..64) |
| 0x03 | Publish | client → SFU | stream (1), kind (1: 1 audio, 2 video), layers (1: audio 1, video 1..8), name (0..64) |
| 0x04 | Unpublish | client → SFU | stream |
| 0x05 | TrackAdded | SFU → client | downstream stream (1), kind, layers, publisher peer id (1..64), name (0..64) |
| 0x06 | TrackRemoved | SFU → client | downstream stream |
| 0x07 | Subscribe | client → SFU | downstream stream, max layer (0..7) |
| 0x08 | Unsubscribe | client → SFU | downstream stream |
| 0x09 | KeyframeRequest | either | stream (the receiver's downstream id, or the publisher's upstream id) |
| 0x0A | Report | receiver → sender of a hop | see below |
| 0x0B | BitrateHint | SFU → publisher, or client → SFU | stream (0xFF: the whole connection), bitrate (4, bps) |
| 0x0C | Leave | client → SFU | reason (0..256) |
| 0x0D | Error | SFU → client | code (2), text (0..256) |
| 0x0E | Nack | receiver → sender of a hop | stream (1), count (1..128), count × stream sequence (2) |

Error codes: 400 track refused, 401 ticket refused or not joined, 503 room
full. Any message other than Join before joining is refused and closes the
connection.

### Report

Transport-wide feedback for the hop, sent every 50 ms while packets arrive:

| Size | Field |
| --- | --- |
| 2 | base transport sequence |
| 2 | count N (1..2048) |
| 4 | reference time: arrival of the first received packet, in 250 µs ticks, wrapping |
| 2 | feedback delay: time since the newest packet arrived, 250 µs ticks |
| ceil(N/8) | received bitmap, most significant bit first, padding bits zero |
| … | for each received packet after the first, its arrival minus the previous received one: 1 byte for 0..254 ticks, else `0xFF` and a signed 2-byte delta |

At 100 packets with 10% loss a report is about 115 bytes, as compact as
RTP transport-wide feedback. Reordering produces negative deltas. The
receiver keeps reporting a missing packet for 100 ms (`cc.FinalizeHold`) before
the window moves past it, so a packet that arrives late is not counted lost,
and the sender treats packets below the window base as final (received or
lost). The sender computes RTT as `now - send time of the newest reported
packet - feedback delay`.

### Nack

The receiver of a hop asks for video packets that are missing 5 ms after a
later one arrived and that FEC has not rebuilt. It asks again after
1.5 × RTT (at least 20 ms), at most three times, and forgets a gap after
600 ms or a jump of more than 512. The sender keeps the last 512 video packets
per stream for 1 s, retransmits each at most once per RTT, and spends at most
30% of its target per second on retransmissions. Retransmissions keep their
stream sequence, get a new transport sequence, and leave the pacer ahead of
fresh video and behind audio. Audio is never retransmitted: a retransmission
lands after its playout slot, so audio relies on FEC and Opus concealment.

## Forward error correction

**Code.** Systematic Reed-Solomon over GF(2^8) (polynomial 0x11D) with a
Cauchy matrix `1 / (x_j ⊕ y_i)`, `x_j = j`, `y_i = M + i`, whose columns are
scaled so the first row is all ones. Repair 0 is therefore plain XOR parity,
and XOR (M = 1) and Reed-Solomon are one family; any K of the K + M symbols
recover the group. Decoding solves the e × e Cauchy submatrix for e erasures
by Gauss-Jordan. Multiplication uses a 64 KiB table.

Cost (i9-10980XE, 1200-byte packets, `go test -bench . ./rtc/fmm/fec`):

| Geometry | Encode per group |
| --- | --- |
| K 8, M 1 (XOR) | 8 µs (1.2 GB/s) |
| K 16, M 4 | 107 µs (180 MB/s) |
| K 32, M 8 | 445 µs |
| K 48, M 16 | 1.36 ms |
| K 16, M 4, decode four erasures | 112 µs |

That is cheap enough to run per subscriber on the SFU at our rates (a 2 Mbps
video stream is about 200 packets a second).

**Groups.** A group closes when it has K packets, when the oldest member is
60 ms (audio) or 50 ms (video) old, when its sequence span would pass 255, or
when its repair would exceed the datagram size. Video keeps two groups open:
one for keyframes and layer 0, one for the higher layers, because losing a
layer-0 packet costs a keyframe while losing a layer-2 frame costs one
skipped frame. Each class has its own parameters.

**Policy** (`fec.Choose`, `fec.ChooseVideo`, re-evaluated every 200 ms per
stream). The sender takes the hop's loss rate and mean loss-burst length from
its feedback and picks the (K, M) with the least overhead whose residual loss
meets the target. The residual model treats loss as events of `burst` packets
at rate `loss / burst`. Then `Residual(K, M, p, b)` is the binomial tail of more
than `M / b` events among `K + M`, times the share of data packets they hit.
K is limited to the packets the stream sends in one group delay.

| Class | FEC off below | Residual target | K candidates | Overhead cap |
| --- | --- | --- | --- | --- |
| Audio | 1% loss | 0.5% | 1, 2, 3 | 200% |
| Video keyframes and layer 0 | 0.5% | 0.1% | 2..48 | 100% |
| Video higher layers | 0.5% | 1% | 2..48 | 50% |

Partial groups (closed by time) round their repair count up.

**Budget.** On a hop with queueing evidence (RTT 40 ms above the minimum, or
an overuse in the last 2 s) the repair rate must fit in what the target leaves
after media and per-packet overhead; video repair is cut first, audio last.
Without that rule a congested downlink measured 20-50% loss from its own
tail drops, the policy answered with triple audio redundancy, and the repair
traffic alone exceeded the link (drop-recover concealment went from 13% to
8% with the rule). With random loss and no queueing the cap does not apply,
because the SFU's estimate is application-limited and would leave FEC no
room.

Recovery measured by `TestRecoveryRates` (20 000 packets, `FMM_SIM=1`):

| Code | Raw loss | After FEC | Overhead |
| --- | --- | --- | --- |
| XOR K 8, 2% random | 2.03% | 0.27% | 19% |
| XOR K 4, 5% random | 4.95% | 0.84% | 36% |
| RS 16+4, 5% random | 4.98% | 0.03% | 41% |
| RS 16+6, 10% random | 9.81% | 0.05% | 62% |
| RS 24+12, 20% random | 20.22% | 0.65% | 84% |
| RS 16+6, bursts (Gilbert-Elliott 0.02/0.25) | 7.87% | 3.60% | 62% |
| policy, bursts of 4 at 7.4% | 8.02% | 1.52% | 58% |
| policy, video 10% | 9.80% | 0.33% | 40% |
| policy, audio 20% | 20.08% | 0.40% | 180% |

Through a full hop with feedback-driven parameters (`hop` tests), audio at 10%
loss ends at 0.53% residual; video at 10% loss with retransmission only ends at
0.06%. Bursts are the weak case for FEC without interleaving; on video,
retransmission covers them.

## Congestion control

A sender-side estimator in the spirit of Google Congestion Control, one per
hop (`cc.Controller`):

1. **Arrival groups.** Packets sent within 5 ms form a group; per pair of
   groups the delay variation is `(arrival delta) - (send delta)`. Packets
   sent before the current group (reordered feedback) are ignored.
2. **Trendline.** The accumulated delay variation is smoothed (0.9) and a
   least-squares slope over the last 20 groups is scaled by
   `min(groups, 60) × 4`.
3. **Overuse detector** with an adaptive threshold γ (start 12.5, clamp
   6..600): γ moves toward |trend| at 0.0087/ms above it and 0.039/ms below,
   and does not adapt to outliers more than 15 above it. Overuse needs the
   trend above γ for 10 ms over two groups while not falling.
4. **AIMD rate control.** Overuse: decrease to β × acknowledged throughput
   (500 ms window), at most once per RTT, where β is 0.85 minus 0.5 × the
   queueing delay in seconds, floored at 0.5, so a 400 ms standing queue
   drains about twice as fast as GCC's fixed 0.85 would. Underuse: hold.
   Normal: increase, multiplicatively 8%/s while no link capacity is
   known, additively about one packet per (RTT + 100 ms) near the last
   decrease point. An increase never passes 1.5 × throughput + 10 kbps, but
   that cap never lowers the rate. Lowering the rate on it (the first version)
   deadlocked the SFU: an application-limited downstream estimate pulled the
   publisher's hint down, the encoder sent less, and the estimate fell again.
5. **Loss bound.** Each second (or 200 packets) of final outcomes gives a
   loss fraction and a mean burst. Above 10% (25% when there is no queueing
   evidence, so random radio loss does not collapse the rate) the bound
   becomes target × (1 - loss/2), at most once per 300 ms + RTT. Below 2%,
   or below the random-loss threshold with no queueing, it grows 8%/s.
6. **Probing.** At start, clusters at 3× and 6× the start rate; afterwards
   at 2× the target every 5 s when the hop has been quiet for 3 s, and again
   after 0.5 s when a probe found at least 70% of its rate. A cluster is 15 ms
   of the probe rate and at least five packets (padding of 250 to 1200 bytes,
   so a 2× probe on a 300 kbps link adds about 35 ms of queue, not 160 ms). The
   result is the minimum of send and receive rates over the cluster; when it
   beats the target it lifts both the AIMD rate and the loss bound.
7. **Target** = min(AIMD rate, loss bound), clamped to the configured range.
   Each datagram counts 48 bytes of transport overhead (UDP, IP, QUIC short
   header and AEAD tag), which the media budgets subtract again. At 300 kbps
   with 100 audio packets a second that overhead is about 40 kbps.

The pacer accrues budget at 2.5 × the target (at most 10 ms of it), raises its
rate to empty its queue within 0.5 s, drops video and retransmissions queued
for more than 400 ms (or past 2 MiB), and sends in order audio,
retransmissions, video (with their repairs). Probes temporarily raise the
rate and fill with padding.

### Measured curves

`go test -v ./rtc/fmm/cc` (one video-like source following the target through
the in-memory link: 25 ms each way, 400 ms drop-tail queue, 40 bytes of
per-packet overhead on the wire), every 2 s:

Convergence (mean target over 20-40 s; p95 of the link's queueing delay
sampled every 0.5 s):

| Cap | Mean target | Share of cap | p95 queue |
| --- | --- | --- | --- |
| 300 kbps | 284 kbps | 95% | 60 ms |
| 1 Mbps | 932 kbps | 93% | 33 ms |
| 3 Mbps | 2660 kbps | 89% | 15 ms |

1 Mbps, target in kbps by second: 848, 931, 1043, 906, 1019, 877, 989, 861,
959, 851, 934, 1046, 904, 1017, 875, 987, 864, 962, 850, 934 (queue mostly
0-9 ms, peaks of 23-39 ms at each probe of the ceiling).

Cap drop and rise (2.5 Mbps, 500 kbps from 20 s, 2.5 Mbps from 40 s):

| t (s) | 19 | 20 | 21 | 22 | 23 | 24 | 25 | 30 | 39 | 40 | 43 | 44 | 45 | 50 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cap (kbps) | 2500 | 500 | 500 | 500 | 500 | 500 | 500 | 500 | 500 | 2500 | 2500 | 2500 | 2500 | 2500 |
| target (kbps) | 2105 | 2148 | 366 | 250 | 278 | 323 | 510 | 484 | 501 | 432 | 574 | 2406 | 2464 | 2409 |
| queue (ms) | 0 | 0 | 293 | 0 | 0 | 0 | 49 | 2 | 5 | 46 | 0 | 2 | 3 | 5 |

The drop is handled within 2 s (one burst of tail drops); after the rise the
target is back above 1.5 Mbps in 4 s, through follow-up probes.

Random loss on a 2 Mbps link: mean target 1758 kbps at 5% loss and 1654 kbps
at 10% loss (no collapse; the loss bound sees no queueing and holds).

## Jitter buffers

### Audio (`jitter.Audio`)

Frames are 20 ms (`FrameUs`); the frame id gives the position. For every
arrival the buffer records `arrival - (send time)` and keeps the minimum over
10 s (`d_min`) and the delays of the last 3 s. The target playout delay is
the 98th percentile of those delays minus `d_min`, plus 5 ms, between 20 and
400 ms. It rises at once and falls at most 100 ms per second. Late packets
count too, since they are the ones the target must cover. With the 95th
percentile, 5% of the frames were late by design, too many for voice
(two-client jitter-100 concealment went from 5.7% to 1.6% with the 98th).

`Pull(now)` is called on the device's 20 ms clock and returns one of:

| Status | Meaning |
| --- | --- |
| `AudioWaiting` | not started yet: play silence |
| `AudioPlay` | the frame |
| `AudioConceal` | the frame is missing: run Opus concealment; when the next frame is already here its bytes come with the status, for Opus in-band FEC decoding |
| `AudioInsert` | the buffer is shorter than its target: play one concealment frame without consuming anything (deceleration) |

The lag of the frame due now is `now - (its send time + d_min)`. Above
target + 15 ms the buffer drops one frame at most every 100 ms (accelerate,
about 20%); above target + 200 ms it flushes back to the target at once;
below target - 15 ms it inserts one frame at most every 50 to 100 ms. A
packet older than the playout point is counted late and dropped; one more
than 3 s ahead resets the buffer.

`go test -v -run Jitter ./rtc/fmm/jitter` (3000 frames each):

| Profile | Concealment | Mean target |
| --- | --- | --- |
| constant 40 ms | 0.00% | 20 ms |
| 100 ± 30 ms uniform, reordering | 0.00% | 63 ms |
| Wi-Fi 10 ms with 5% spikes to 150 ms | 0.30% | 153 ms |
| 5% loss, 20 ± 5 ms | 4.67% (the losses) | 20 ms |
| 30 ms, a 100 ms step for 20 s, back | 0.63% | 40 ms |

### Video (`jitter.Video`)

Complete frames are released in frame-id (decode) order. The next frame is
released when it is a keyframe, or when its reference was released and either
its first stream sequence follows the last accounted sequence or it has
waited `wait` since it completed. Because the SFU rewrites sequences, no
missing sequence means nothing was lost, so a frame after a dropped layer
goes out at once. `wait` is the 95th percentile of
`completion - (timestamp + d_min)` over 3 s plus 10 ms, between 30 and 300 ms,
which covers FEC and retransmission latency.

A frame whose reference is missing waits for it for `wait`. For a layer-0
frame it waits up to 1 s, and once `wait` has passed it also asks for a
keyframe, so a late repair and a new keyframe race and the first one wins.
A frame whose reference was dropped or skipped is discarded, and its id
is remembered as broken so frames that depend on it are discarded at once. Only
an undecodable layer-0 frame (the chain is broken) or a delta frame before
any keyframe asks for a keyframe. Requests are limited to one per 500 ms at
the client and one per 300 ms per track at the SFU. Discarded frames do not move
the release point, so a layer-0 frame repaired after its dependants timed out
is still decoded (this removed most keyframe requests at 10% loss).
Sequence ranges of discarded frames are kept and absorbed once contiguous.

## Session layer (`fmm.Client`)

```go
func NewClient(t Transport, cfg *Config) *Client   // Config{StartBitrate, MinBitrate, MaxBitrate}
func (c *Client) Join(ticket string, now int64) error
func (c *Client) PublishAudio(name string, now int64) (int, error)
func (c *Client) PublishVideo(name string, layers int, now int64) (int, error)
func (c *Client) SendAudio(stream int, frame []byte, now int64) error
func (c *Client) SendVideo(stream int, frame []byte, keyframe bool, layer int, now int64) error
func (c *Client) VideoBitrate(stream int) int          // give this to the encoder
func (c *Client) Subscribe(stream, maxLayer int, now int64) error
func (c *Client) PullAudio(stream int, now int64) *AudioFrame   // every 20 ms
func (c *Client) PollVideo(stream int, now int64) *VideoFrame   // nil when nothing is ready
func (c *Client) PollEvent() *Event                    // joined, track added/removed, keyframe request, bitrate hint, error
func (c *Client) HandleDatagram(p []byte, now int64)
func (c *Client) HandleControl(p []byte, now int64)
func (c *Client) Tick(now int64) int64
```

plus `Leave`, `Unpublish`, `Unsubscribe`, `SetReceiveLimit`, `TargetBitrate`,
`PeerID` and `Stats`. Frames go in already end-to-end encrypted (the frame
format of [chat-group-calls.md](../app/chat-group-calls.md)); the engine never
looks inside them. `SendVideo` takes the encoder's own layer assignment; the
packetizer derives references from it.

The video bitrate is the hop target minus each audio stream's media, repair
and overhead rate, minus video packet overhead, shared between the published
video streams, divided by (1 + FEC ratio), times 0.97 for headers, and capped
by the SFU's `BitrateHint` for that stream.

## SFU forwarding core (`fmm/sfu`)

Every hop is terminated at the SFU: congestion control, FEC, NACK and receiver
reports run separately between publisher and SFU and between SFU and each
subscriber, so one subscriber's loss or bandwidth never reaches the
publisher or the other subscribers. The SFU forwards end-to-end encrypted
payloads without reading them; it reads only the datagram header.

- **Rooms and admission.** `NewRoom(Config{Admit: func(ticket) (peerID, error)})`;
  `Connect(transport)` returns a `Peer` that handles that connection's
  datagrams and control. Wiring `Admit` to the ticket check of
  [rtc.md](rtc.md#signal) and choosing the room from the ticket is left for
  the SFU-over-QUIC phase. Limits: 16 peers, 4 tracks per peer, peer ids
  unique in the room.
- **Tracks.** A Publish announces a TrackAdded to every other peer with a
  downstream stream id chosen per subscriber (1..254); a subscription starts
  forwarding. A new video subscription waits for a keyframe and asks the
  publisher for one.
- **Per-subscriber sequence rewriting.** The forwarder decides per frame
  (cached by frame id): forward, or drop because the frame's layer is above
  the subscriber's current layer. A dropped frame whose sequences are beyond
  everything forwarded so far shifts the subscriber's offset by its fragment
  count, so the subscriber sees no gap. A dropped frame that arrives after
  later packets were already forwarded is left as a gap (it looks like loss).
  Frame ids are not rewritten; references keep working because only whole
  higher layers are dropped.
- **Layer switching.** Going down takes effect at the next frame, going up
  at the next layer-0 frame or keyframe.
- **Allocation** (every 200 ms per subscriber). The budget is the
  subscriber's downstream target minus overhead and minus the audio it
  receives (with its FEC). Every video gets layer 0, then layers are added
  round-robin while the next layer's measured rate (times 1 + FEC ratio)
  fits; upgrades need 10% headroom.
- **Shedding.** When even the layer-0 rates do not fit, video forwards are
  paused, the last first, so audio keeps the link; a paused forward resumes
  behind a keyframe when 1.5 × its layer-0 rate fits, with a 2 s hold between
  changes. Before this rule cap-300k with three clients concealed 52% of its
  audio; after it, 2%.
- **Publisher hints** (every 500 ms per video track): the publisher is asked
  for min(lowest subscriber share / layer-0 share, highest subscriber share)
  × 0.9, at least 60 kbps, so the weakest subscriber can still take layer 0
  and nobody gets more than the best subscriber can use. Without simulcast a
  weak subscriber lowers the stream for everybody to about twice its share.

## Simulations

`server/rtc/fmm/sim` runs Go clients through the SFU core over the in-memory
link in virtual time (deterministic, single goroutine, a 30 s three-person
call takes about 0.3 s). Each client publishes Opus-like audio (80-byte
frames every 20 ms, 32 kbps) and L1T3 video at 30 fps following
`VideoBitrate` (layer weights 1.6 / 0.9 / 0.75, keyframes five times a mean
frame, a rate-control balance that repays keyframes, a keyframe on every
request at most every 300 ms), subscribes to everything, pulls audio every
20 ms and polls video every 5 ms. Frames carry their capture time, so delay is
measured end to end (capture to playout or release, through both hops and the
jitter buffers). The profiles match the bench's ([call-bench.md](call-bench.md),
`rtc/bench/profiles.go`) and apply to every client's uplink and downlink; all
have 20 ms of one-way delay and 40 bytes of per-packet overhead. The first 5 s
are excluded from the metrics. A freeze is an interval between released
frames longer than max(3 × mean interval, mean + 150 ms).

- `go test ./rtc/fmm/sim`: two clients, 12 s, clean, loss-10, jitter-100 and
  cap-300k, with pass limits (with `-short`: clean and loss-10).
- `FMM_SIM=1 go test -v -run Full ./rtc/fmm/sim`: three clients, 30 s, every
  profile.

Three clients, 30 s (`FMM_SIM=1`), per received stream averaged:

| Profile | Audio concealment | Audio delay mean / p95 | Video fps | Freezes (total) | Video delay mean / p95 | Video received | Encoder target |
| --- | --- | --- | --- | --- | --- | --- | --- |
| clean | 0.00% | 70 / 73 ms | 30.0 | 0 | 49 / 60 ms | 2226 kbps | 2197 kbps |
| loss-5 | 1.88% | 77 / 92 ms | 28.3 | 1 (0.2 s) | 56 / 97 ms | 1364 kbps | 1392 kbps |
| loss-10 | 1.81% | 90 / 93 ms | 28.9 | 4 (0.8 s) | 58 / 100 ms | 966 kbps | 985 kbps |
| loss-20 | 2.48% | 93 / 112 ms | 27.8 | 103 (23.8 s) | 87 / 200 ms | 323 kbps | 351 kbps |
| burst | 4.31% | 80 / 93 ms | 29.4 | 28 (6.0 s) | 74 / 147 ms | 1364 kbps | 1387 kbps |
| jitter-100 | 1.65% | 263 / 272 ms | 22.7 | 0 | 253 / 268 ms | 1471 kbps | 1821 kbps |
| cap-300k | 2.37% | 122 / 333 ms | 21.3 | 2 (4.8 s) | 76 / 102 ms | 55 kbps | 70 kbps |
| drop-recover | 8.03% | 163 / 448 ms | 18.1 | 17 (28.2 s) | 75 / 88 ms | 264 kbps | 326 kbps |

Six received video streams make 150 stream-seconds per profile, so loss-20
spends 16% of the time frozen and drop-recover 19%. Loss is applied on both
hops, so loss-10 is 19% end to end and loss-20 is 36%. cap-300k gives each person 300 kbps
for two remote voices and two remote videos, so each remote video gets about
55 kbps (21 fps on average) and pauses when even layer 0 does not fit. jitter-100 adds 200 ms of fixed path
delay to every measurement.

drop-recover, client p0 (both directions 2 Mbps, 300 kbps from 10 s, 2 Mbps
from 20 s; queue is the larger of its uplink and downlink queues):

| t (s) | 9 | 10 | 11 | 12 | 13 | 15 | 17 | 20 | 21 | 22 | 23 | 24 | 25 | 28 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| link (kbps) | 2000 | 300 | 300 | 300 | 300 | 300 | 300 | 2000 | 2000 | 2000 | 2000 | 2000 | 2000 | 2000 |
| uplink target | 1934 | 1934 | 289 | 146 | 146 | 191 | 292 | 292 | 584 | 1923 | 1923 | 1923 | 1923 | 1923 |
| encoder | 574 | 574 | 197 | 60 | 60 | 60 | 67 | 66 | 83 | 94 | 580 | 580 | 788 | 795 |
| SFU → p0 target | 1511 | 1632 | 530 | 136 | 151 | 293 | 202 | 216 | 270 | 304 | 1287 | 1910 | 1926 | 2014 |
| video received by p0 | 1108 | 1148 | 241 | 71 | 35 | 10 | 45 | 39 | 42 | 153 | 464 | 721 | 1002 | 1115 |
| queue (ms) | 1 | 6 | 370 | 86 | 0 | 8 | 0 | 0 | 0 | 0 | 5 | 15 | 5 | 1 |

The encoder runs below the uplink target because the SFU's hints follow
the other two subscribers, who share the same impaired links. Nearly all the
audio concealment in drop-recover comes in the 1.5 s after the drop: 2 Mbps
of traffic meets a 300 kbps drop-tail queue, which fills to 400 ms within
about 200 ms, before any feedback can arrive.

## What was measured to get here

The first end-to-end run (FEC only, two clients) froze 3 to 8 times in 7 s at
10% loss and asked for 17 keyframes. In order, these changes moved it:

1. AIMD no longer lowers the rate on the throughput cap, and the SFU's hint
   leaves headroom for the top layer. Before, the downstream estimate, the
   hint and the encoder pulled each other down to about 120 kbps.
2. Audio target at the 98th percentile, faster target decay, and smaller probe
   packets (cap-300k audio delay 372 to 162 ms).
3. The video buffer keeps a layer-0 frame that FEC repairs after its
   dependants timed out, and asks for a keyframe while it still waits.
4. Unequal protection for keyframes and layer 0 (keyframes sent at 10% loss:
   30 to 16), then repair groups with explicit members and per-class
   parameters.
5. Per-hop NACK for video: three clients at loss-10 went from 57 freezes
   (31 s) to 4 (0.8 s), and loss-5 from 18 to 1.
6. SFU shedding, overhead counting, queue-proportional decrease and the FEC
   budget under congestion: cap-300k audio concealment 52% to 2%,
   drop-recover 39% to 8%.

## Fuzzing and tests

Native Go fuzz targets: `wire.FuzzParseDatagram` (every kind round-trips
byte for byte), `wire.FuzzDecodeControl` (every decoded message re-encodes
to the same bytes, so the encoding is canonical), `wire.FuzzControlReader`
(arbitrary chunking), `fec.FuzzDecoder` (recovered packets always parse),
`frag.FuzzReassembler` (no state is left after expiry), `sfu.FuzzPeerInput`
and `fmm.FuzzClientInput` (arbitrary datagrams and control into a live
peer). Each ran for 20 s with no failure on 2026-10-21, for example
`go test -run XXX -fuzz '^FuzzDecodeControl$' -fuzztime 20s ./rtc/fmm/wire`.
`cd server && go test -race ./rtc/...` covers everything else.

## Not done yet

- Simulcast. Temporal layers alone mean a weak subscriber lowers the
  stream for everyone (to about twice its share).
- A per-track packet cache shared by all subscribers at the SFU. Today each
  subscriber's downstream sender keeps its own 512-packet video history.
- Room and ticket integration, failover and the WebRTC bridge (phase 4).
- Audio redundancy tuned to bursts (interleaving), and time-stretching
  instead of whole-frame drop and insert.
- Reports and NACKs ride the reliable control stream; under heavy loss QUIC
  retransmission delays them. A datagram variant may measure better.
- At 300 kbps the uplink estimate overshoots the cap by up to 30% between
  overuse signals, which shows up as short audio delay spikes (two-client
  cap-300k concealment 5%).
