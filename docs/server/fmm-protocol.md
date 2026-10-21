# FM Media engine core: protocol and algorithms

This page specifies layers 2 to 4 of [FM Media](fm-media.md): the media
protocol (datagrams and the control stream), loss recovery (FEC and
retransmission), congestion control and pacing, the jitter buffers, the
session layer (`fmm.Client`) and the SFU forwarding core. It also records the
measurements that shaped them and the decisions taken on the way
(2026-10-21). The code is `server/rtc/fmm/`, standard library only, in the
`forkmesh.com/mirror-node` module.

The second round (also 2026-10-21) added the core's half of ideas 1 to 3 of
[Beyond FaceTime](fm-media.md#beyond-facetime): long-term reference recovery,
DRED-aware audio and the face stream with per-subscriber encodings
(simulcast), and fixed the weak spots the first round listed. Each section
below describes the protocol as it is now; [Round two](#round-two-what-changed-and-why)
explains the changes and [Simulations](#simulations) compares the numbers
before and after.

## Packages

| Package | Role |
| --- | --- |
| `fmm` | `Client`, the session layer the phones bind with `gomobile` |
| `fmm/wire` | Datagram headers, control messages, feedback datagrams, the control stream reader |
| `fmm/frag` | Packetizer (frames to datagrams, explicit references) and reassembler |
| `fmm/fec` | GF(256) Reed-Solomon (XOR as its first row), packet-level FEC encoder/decoder, the FEC policy |
| `fmm/cc` | Receiver feedback recorder, the sender's congestion controller, the pacer |
| `fmm/hop` | One hop's sender half (FEC, history, pacing, CC, probes) and receiver half (FEC recovery, feedback, NACK) |
| `fmm/jitter` | Audio (with DRED) and video (with long-term recovery) jitter buffers |
| `fmm/sfu` | SFU forwarding core: rooms, encodings, per-subscriber forwarding, layer and encoding choice, acknowledgement aggregation, recovery, publisher hints |
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

Receiver reports, NACKs, acknowledgements and recovery requests travel as
feedback datagrams (kind 3) by default; `Config.FeedbackOnControl` (client and
SFU) moves them to the control stream. Both ends accept both, so the two sides
need not agree. Feedback that does not fit one datagram falls back to the
control stream.

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
unknown kind, is dropped. Transport sequence numbers (bytes 2-3 in media,
repair and padding) count every datagram a hop's sender paces, so congestion
feedback covers media, repair, retransmissions and padding alike. Feedback
datagrams are not sequenced and not congestion controlled (they are small and
must not wait behind media in the pacer).

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
| 10 | 1 | flags: `0x80` keyframe, `0x40` FEC-protected, `0x20` recovery, `0x10` long-term, `0x08` face, `0x07` temporal layer |
| 11 | 1 | video: reference delta, the frame this one depends on is `frame id - delta`, 0 means none; audio: redundancy depth in 10 ms units (DRED), 0 means none |
| 12 | 4 | timestamp: the frame's send time in µs on the sender's clock, wrapping |
| 16 | n | payload: one fragment of the end-to-end encrypted frame |

Flags:

- **keyframe**: decodable alone, resets the receiver's decoder state. For a
  face-stream track (flag `0x08` set too) it marks one part of a reference
  picture (below).
- **recovery**: the encoder predicted this frame from an older frame that every
  subscriber acknowledged (a long-term reference), in answer to a recovery
  request. The receiver may release it as soon as its reference is decoded,
  skipping anything older.
- **long-term**: the encoder keeps this frame as a long-term reference and may
  predict from it later; receivers acknowledge it once decoded.
- **face**: the payload is a face-stream frame (pose, landmarks and expression
  weights from the face codec in `rtc/facestream`), not encoded video. The
  engine treats it as opaque bytes like any frame.

Rules: `index < count`; a keyframe has layer 0 and delta 0; a recovery frame
is not a keyframe, has layer 0 and a non-zero delta; an audio frame and a
face frame that is not a reference picture carry no reference (they stand
alone). The parser refuses keyframes and recovery frames that break these
rules; it cannot tell audio from video, so byte 11 is read according to the
stream's kind from Publish/TrackAdded.

The stream sequence numbers are gap-free per stream as the receiver sees
them. The SFU rewrites them per subscriber (below), so a gap always means a
lost packet and never a layer or an encoding the SFU chose not to forward.

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
reference or depth, timestamp, payload). Blocks are zero-padded to the
longest in the group and coded; a recovered block rebuilds the datagram with
stream id, stream sequence (`base + offset`) and transport sequence 0.

The member list is explicit because groups do not have to be consecutive
ranges. An earlier layout (base plus K consecutive sequences) broke on the
SFU's downstream: every upstream gap and every out-of-order packet recovered
upstream closed the group, leaving groups of one or two packets. With explicit
members a group spans gaps and reordering, and the encoder keeps two groups
open per video stream (below).

### Padding (kind 2), 4-byte header

`0x12`, a zero byte, the transport sequence, then any bytes. Padding fills
bandwidth probes and is otherwise ignored.

### Feedback (kind 3), 4-byte header

`0x13`, then three zero bytes, then one or more control messages in their
control-stream encoding (`type | length | body`, below), each fully
consumed. Only Report, Nack, Ack and Recovery may appear; anything else, an
empty body, a truncated message or non-zero header bytes drop the whole
datagram. `hop.PackFeedback` packs a tick's feedback into as few datagrams as
fit the transport's size.

## Fragmentation and reassembly

`frag.Packetizer` splits a frame into `ceil(len / maxPayload)` fragments of
nearly equal size (equal sizes keep FEC padding small), at most 255, so a frame
can be up to 255 × 1162 bytes (about 290 kB). Larger frames are refused
(`ErrTooLarge`). The packetizer assigns frame ids and references for temporal
layers: a keyframe resets the structure; a layer-0 frame references the
previous layer-0 frame; a layer-L frame references the most recent frame of
layer below L. That is the L1T2 and L1T3 structure the encoders produce, and
any structure whose frames reference the latest lower layer.

`PacketizeFrame(data, Opts, ts)` adds three options. `Ref` names the frame a
layer-0 frame predicts from explicitly; the frame gets the recovery flag, its
delta is `id - Ref` (1..255, else `ErrRef`), and the layer structure continues
from it. `LongTerm` and `Face` set their flags. `Independent` frames carry no
reference; for audio the packetizer writes `Aux` (the redundancy depth) into
byte 11 instead. `LastID` returns the id it assigned, which the client hands
back to the encoder.

`frag.Reassembler` unwraps frame ids, sequences and timestamps, keeps at most
64 partial frames and 4 MiB, drops a frame whose fragments disagree on count,
flags, reference or first sequence, ignores duplicates (also of frames
already delivered, through a 512-entry ring), and expires partial frames after
1 s (`Expire` reports them, with their layer when known, to the video buffer).
A complete frame carries its first and last stream sequence, its completion
time, its flags and byte 11 as `Aux`.

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
| 0x03 | Publish | client → SFU | stream (1), kind (1: 1 audio, 2 video), layers (1: audio 1, video 1..8), name (0..64), then optionally track (1, non-zero) and rank (1, 0..3) |
| 0x04 | Unpublish | client → SFU | stream |
| 0x05 | TrackAdded | SFU → client | downstream stream (1), kind, layers, publisher peer id (1..64), name (0..64) |
| 0x06 | TrackRemoved | SFU → client | downstream stream |
| 0x07 | Subscribe | client → SFU | downstream stream, max layer (0..7) |
| 0x08 | Unsubscribe | client → SFU | downstream stream |
| 0x09 | KeyframeRequest | either | stream (the receiver's downstream id, or the publisher's upstream id) |
| 0x0A | Report | receiver → sender of a hop | see below |
| 0x0B | BitrateHint | SFU → publisher, or client → SFU | stream (0xFF: the whole connection), bitrate (4, bps); 0 pauses an encoding |
| 0x0C | Leave | client → SFU | reason (0..256) |
| 0x0D | Error | SFU → client | code (2), text (0..256) |
| 0x0E | Nack | receiver → sender of a hop | stream (1), count (1..128), count × stream sequence (2) |
| 0x0F | Ack | receiver → SFU, SFU → publisher | stream (1), count (1..16), count × frame id (2) |
| 0x10 | Recovery | receiver → SFU, SFU → publisher | stream (1), frame id (2) |

Error codes: 400 track refused, 401 ticket refused or not joined, 409 room
runs on the other engine (from the call server, [fmm-server.md](fmm-server.md)), 503 room
full. Any message other than Join before joining is refused and closes the
connection.

**Publish tail (encodings).** Without the tail a Publish creates a track with
one encoding of rank 0, as before. With `track = stream` it creates a track
whose first encoding has the given rank (rank must then be non-zero, so the
encoding is canonical). With `track ≠ stream` it adds the stream as another
encoding of the publisher's video track `track`; ranks are unique within a
track and a track has at most four encodings. In a track with more than one
encoding, rank 0 is the face encoding and ranks 1 to 3 are video encodings,
higher meaning better. Subscribers see one TrackAdded per track (sent when
the track's first stream is published) and one downstream stream.

**Ack.** From a receiver: long-term frames it has decoded, by downstream frame
id. From the SFU: the newest long-term frame every current viewer of that
encoding has decoded, by upstream frame id, sent when it changes (at most every
100 ms).

**Recovery.** From a receiver: the downstream id of the oldest frame it holds
but cannot decode (or the next frame after the last one it released). From
the SFU: "predict the next frame from this frame id", the newest commonly
acknowledged long-term frame.

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
packet - feedback delay`. When reports ride datagrams, the recorder repeats
every received packet in two consecutive reports (`Recorder.Repeat = 1`) so
that one lost report does not turn received packets into losses; the sender
ignores packets it already knows.

### Nack

The receiver of a hop asks for video packets that are missing 5 ms after a
later one arrived and that FEC has not rebuilt. It asks again after
1.5 × RTT (at least 20 ms), at most three times, and forgets a gap after
600 ms or a jump of more than 512. The sender keeps the last 512 video packets
per stream for 1 s, retransmits each at most once per RTT, and spends at most
30% of its target per second on retransmissions. Retransmissions keep their
stream sequence, get a new transport sequence, and leave the pacer ahead of
fresh video and behind audio. Audio is never retransmitted: a retransmission
lands after its playout slot, so audio relies on FEC, DRED and Opus
concealment.

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

**Audio with DRED.** When an audio stream's packets carry at least 100 ms of
DRED (byte 11 ≥ 10), the hop keeps audio FEC only while it is cheap: the
chosen repair rate must be at most 15% of the hop target, otherwise audio FEC
is off and DRED alone covers loss. FEC adds no delay and wins whenever the
link has room; DRED costs a few kbps and wins on thin links, where 200% audio
FEC would crowd out everything else.

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
retransmission covers them, on audio DRED does.

## Congestion control

A sender-side estimator in the spirit of Google Congestion Control, one per
hop (`cc.Controller`), plus four delay and feedback rules added in round two:

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
4. **AIMD rate control.** Overuse: decrease to β × the decrease base, at most
   once per RTT, where β is 0.85 minus 0.5 × the queueing delay in seconds,
   floored at 0.5, so a 400 ms standing queue drains about twice as fast as
   GCC's fixed 0.85 would. The base is the acknowledged throughput (500 ms
   window), or 0.85 × the last successful probe's estimate when that is
   higher, recent (5 s, no cut since) and the queue is under 150 ms: an
   application-limited hop (a face stream, a paused video) otherwise
   collapses to its own small throughput on the first overuse. Underuse:
   hold. Normal: increase, multiplicatively 8%/s while no link capacity is
   known, additively about one packet per (RTT + 100 ms) near the last
   decrease point. An increase never passes max(1.5 × throughput + 10 kbps,
   throughput + 50 kbps), but that cap never lowers the rate. Lowering the
   rate on it (the first version) deadlocked the SFU: an application-limited
   downstream estimate pulled the publisher's hint down, the encoder sent
   less, and the estimate fell again. The extra 50 kbps lets a hop that only
   carries audio grow far enough to resume a face stream.
5. **Loss bound.** Each second (or 200 packets) of final outcomes gives a
   loss fraction and a mean burst. Above 10% (35% when there is no queueing
   evidence, so random radio loss does not collapse the rate; it was 25%, and
   30% path loss collapsed the uplink to its minimum) the bound becomes
   target × (1 - loss/2), at most once per 300 ms + RTT. Below 2%, or below
   the random-loss threshold with no queueing, it grows 8%/s. Packets sent
   before a delay or silence cut (below) do not count: their losses are the
   overflow the cut already answered.
6. **Probing.** At start, clusters at 3× and 6× the start rate; afterwards
   at 2× the target every 5 s when the hop has been quiet for 3 s, again
   after 0.5 s when a probe found at least 70% of its rate, 300 ms after the
   queue drained following a cut, and whenever the client or SFU asks
   (`RequestProbe`: at most every 2 s, 1 s after an overuse, without
   queueing evidence). A cluster is 15 ms of the probe rate and at least five
   packets of `clamp(rate × 40 ms / 8 / 5, 80, 250)` bytes, so a probe on a
   100 kbps link adds about 40 ms of queue, not 100. The result is the
   minimum of send and receive rates over the cluster; when it beats the
   target it lifts both the AIMD rate and the loss bound.
7. **Delay fast cut.** Each report gives the newest packet's one-way delay
   (`arrival - send` on the two clocks); minus its 10-second minimum it is
   the queueing delay. At 150 ms or more the rate drops at once to β × the
   receive rate of the last 200 ms of arrivals, at most once per
   max(RTT, 100 ms). The trendline needs many groups to see a sudden capacity
   drop; this sees the first report that crosses the queue.
8. **Standing-queue cut.** The minimum queueing delay of each 200 ms window
   filters out jitter (one packet on time proves there is no queue). When it
   is at least 40 ms and has risen by 4 ms or more over two windows in a row,
   the rate drops to β × the recent receive rate. This catches a slow
   overshoot that builds a queue too gently for the trendline (the SFU's
   estimate rose 22% above a 2 Mbps link in drop-recover and queued 80 ms of
   audio before the trendline fired). The rising condition matters: a plain
   40 ms threshold cut jitter-100 links 14 times per call.
9. **Feedback silence.** When no report has arrived for
   max(3 report intervals, 2 × minimum RTT) while packets were sent, the rate
   halves, and again every 100 ms of silence. When a link collapses, the
   reports travel back through the other direction's full queue (both
   directions are usually the same radio), so the sender is blind exactly
   when it matters. The cut is provisional: the first report covering packets
   sent after the silence began restores the rate if it shows less than
   40 ms of queue (the reports were lost, not queued).
10. **Target** = min(AIMD rate, loss bound), clamped to the configured range.
    Each datagram counts 48 bytes of transport overhead (UDP, IP, QUIC short
    header and AEAD tag), which the media budgets subtract again. At 300 kbps
    with 100 audio packets a second that overhead is about 40 kbps.

The pacer accrues budget at 2.5 × the target (at most 10 ms of it), raises its
rate to empty its queue within 0.5 s but never above the pacing rate, drops
video and retransmissions queued for more than 400 ms (or past 2 MiB), and
sends in order audio, retransmissions, video (with their repairs). While the
hop shows queueing evidence it paces at 1.25 × the target and drops video
queued for more than 200 ms: a backlog left from a capacity drop then drains
into the bottleneck slowly instead of refilling the queue that audio waits
behind. Probes temporarily raise the rate and fill with padding, and pad
until the probe has at least five packets: media sent during a probe count
towards it, and a few large video packets used to use up its bytes after
three packets, so the controller discarded every probe and an uplink stayed
at 300 kbps for 10 s after drop-recover's link came back.

### Measured curves

`go test -v ./rtc/fmm/cc` (one video-like source following the target through
the in-memory link: 25 ms each way, 400 ms drop-tail queue, 40 bytes of
per-packet overhead on the wire), every 2 s:

Convergence (mean target over 20-40 s; p95 of the link's queueing delay
sampled every 0.5 s; peak target over the same window):

| Cap | Mean target | Share of cap | p95 queue | Peak target | Link queue p95 before / after |
| --- | --- | --- | --- | --- | --- |
| 300 kbps | 288 kbps | 96% | 61 ms | 332 kbps (111% of cap) | 107 / 72 ms |
| 1000 kbps | 947 kbps | 95% | 32 ms | 1059 kbps (106% of cap) | 48 / 49 ms |
| 3000 kbps | 2750 kbps | 92% | 47 ms | 3259 kbps (109% of cap) | 32 / 49 ms |

Cap drop and rise (2.5 Mbps, 500 kbps from 20 s, 2.5 Mbps from 40 s): the drop
is handled within 1 s (one burst of tail drops, 246 ms of queue for one
sample), and after the rise the target is back above 1.5 Mbps in 3.0 s
(4 s before round two). Random loss on a 2 Mbps link: mean target 1785 kbps at
5% loss and 1632 kbps at 10% loss (1758 and 1654 before).

At 3 Mbps the controller now uses more of the link (92% against 88%) and the
link's p95 queue rose from 32 to 49 ms: probes that used to be discarded for
having too few packets now count, and win more often near the cap. At 300 kbps
the queue fell; the peak overshoot (about 110%) is unchanged.

## Jitter buffers

### Audio (`jitter.Audio`)

Frames are 20 ms (`FrameUs`); the frame id gives the position. For every
arrival the buffer records `arrival - (send time)` and keeps the minimum over
10 s (`d_min`) and the delays of the last 3 s. The base playout delay is
the 98th percentile of those delays minus `d_min`, plus 5 ms, between 20 and
400 ms; the target is the base plus a look-ahead for redundancy (below). It
rises at once and falls at most 100 ms per second. Late packets count too,
since they are the ones the target must cover. With the 95th percentile, 5%
of the frames were late by design, too many for voice (two-client jitter-100
concealment went from 5.7% to 1.6% with the 98th).

`PullOut(now)` (`Client.PullAudio`) is called on the device's 20 ms clock and
returns a status, bytes and an offset:

| Status | Meaning |
| --- | --- |
| `AudioWaiting` | not started yet: play silence |
| `AudioPlay` | the frame |
| `AudioDRED` | the frame is missing; decode it from the DRED in `Data` (a later packet) at `Offset` frames before that packet's start (`opus_decoder_dred_decode` with `Offset × 960` samples at 48 kHz). With offset 1 the same packet's LBRR is the better choice when `opus_packet_has_lbrr` says it has one |
| `AudioConceal` | the frame is missing and nothing covers it: run Opus concealment; when the next frame is already here its bytes come with the status, for Opus in-band FEC decoding |
| `AudioInsert` | the buffer is shorter than its target: play one concealment frame without consuming anything (deceleration) |

For a missing frame N the buffer looks for the nearest later packet M it
holds whose depth (byte 11 × 10 ms) covers it: `(M - N) × 20 ms ≤ depth`.
Only then does it fall back to FEC (the next packet) or plain concealment.
The sender sets the depth from its encoder configuration
(`Client.SetAudioRedundancy(stream, ms)`); `Client.AudioRedundancyMs` suggests
one: 200 ms while the uplink loses under 1%, else as much as 3% of the uplink
target pays for, up to 1 s.

**Look-ahead.** DRED can only recover a frame if a covering packet arrives
before the frame's playout, so on a lossy path a slightly later playout
recovers bursts. The buffer records every loss run (a gap in the arrivals,
corrected when a reordered frame fills it) with the extra delay it would have
needed: the arrival of the first packet after the run minus the base playout
time of the run's first frame. Runs settle for 200 ms and count for 15 s (at
most 64). The look-ahead is the smallest extra delay, in 10 ms steps up to
min(100 ms, the packets' DRED depth), that leaves at most 0.5% of the frames
in that span unrecoverable; without DRED it is at most one frame (20 ms, for
LBRR). With no loss it is zero. The first version used the 90th percentile
of run lengths × 20 ms and added 20-40 ms on every lossy path even where hop
FEC had already repaired everything; measuring what the delay would actually
buy cut audio delay at loss-10 from 115 to 95 ms with the same concealment.

The lag of the frame due now is `now - (its send time + d_min)`. Above
target + 15 ms the buffer drops one frame at most every 100 ms (accelerate,
about 20%); above target + 200 ms it flushes back to the target at once;
below target - 15 ms it inserts one frame at most every 50 to 100 ms, but not
in place of a missing frame that DRED can still recover. A packet older than
the playout point is counted late and dropped; one more than 3 s ahead resets
the buffer.

`go test -v -run Audio ./rtc/fmm/jitter` (3000 frames each):

| Profile | Concealment | Mean target |
| --- | --- | --- |
| constant 40 ms | 0.00% | 20 ms |
| 100 ± 30 ms uniform, reordering | 0.00% | 63 ms |
| Wi-Fi 10 ms with 5% spikes to 150 ms | 0.30% | 153 ms |
| 5% loss, 20 ± 5 ms | 4.67% (the losses; 24 ms target with the one-frame look-ahead) | 24 ms |
| 30 ms, a 100 ms step for 20 s, back | 0.63% | 40 ms |

DRED in the buffer alone (`TestDREDRecoversBurstsThatFECCannot`, 5000 frames,
20 ± 2.5 ms delay, every packet carrying 1 s of DRED or none):

| Loss | Without DRED: plain conceal / FEC-assisted | With DRED: plain conceal / DRED recovered |
| --- | --- | --- |
| random 10% | 0.98% / 9.55% | 0.04% / 10.53% |
| random 20% | 4.44% / 16.73% | 0.44% / 21.08% |
| random 30% | 9.27% / 21.32% | 0.60% / 30.26% |
| bursts of 3.3 at 12% | 8.37% / 3.62% | 1.42% / 10.65% |

### Video (`jitter.Video`)

Complete frames are released in frame-id (decode) order. The next frame is
released when it is a keyframe, or when its reference was released and either
its first stream sequence follows the last accounted sequence or it has
waited `wait` since it completed. Because the SFU rewrites sequences, no
missing sequence means nothing was lost, so a frame after a dropped layer
goes out at once. `wait` is the 95th percentile of
`completion - (timestamp + d_min)` over 3 s plus 10 ms, between 30 and 300 ms,
which covers FEC and retransmission latency.

Face-stream frames that are not reference pictures carry no reference; they
are released as soon as they are next, without waiting for a keyframe or for
missing sequences, since a lost face frame is one skipped pose.

A frame whose reference is missing waits for it for `wait`. For a layer-0
frame it waits up to 1 s, and once `wait` has passed the chain counts as
broken. A frame whose reference was dropped or skipped is discarded, and its
id is remembered as broken so frames that depend on it are discarded at once.
Discarded frames do not move the release point, so a layer-0 frame repaired
after its dependants timed out is still decoded (this removed most keyframe
requests at 10% loss). Sequence ranges of discarded frames are kept and
absorbed once contiguous.

**Long-term recovery.** A broken chain after the stream has started asks for
recovery, not a keyframe: `RecoveryNeeded` returns the id of the oldest frame
held but undecodable, at most once per max(2 × RTT + 100 ms, 200 ms), with
RTT being twice the client's own hop RTT (receiver to publisher and back).
If no layer-0 frame has been released after max(3 × RTT + 400 ms, 500 ms), it
escalates to a keyframe request. When a recovery frame arrives whose
reference was decoded (a long-term frame, possibly far behind the last
released one), every older frame still waiting is discarded and the recovery
frame goes out at once, without the gap wait. Released long-term frames are
queued as acknowledgements (`TakeAcks`). Keyframe requests are limited to one
per 500 ms at the client and one per 300 ms per encoding at the SFU, and
happen only before the first keyframe or after escalation.

## Session layer (`fmm.Client`)

```go
func NewClient(t Transport, cfg *Config) *Client
func (c *Client) Join(ticket string, now int64) error
func (c *Client) PublishAudio(name string, now int64) (int, error)
func (c *Client) PublishVideo(name string, layers int, now int64) (int, error)
func (c *Client) PublishVideoTrack(name string, layers, encodings int, face bool, now int64) (int, error)
func (c *Client) EncodingStream(track, index int) int  // video encoding streams, 0 = lowest
func (c *Client) FaceStream(track int) int             // 0 when the track has no face encoding
func (c *Client) SendAudio(stream int, frame []byte, now int64) error
func (c *Client) SetAudioRedundancy(stream, ms int) error
func (c *Client) AudioBitrate(stream int) int          // give this to the Opus encoder
func (c *Client) AudioRedundancyMs(stream int) int     // a DRED depth for the encoder
func (c *Client) SendVideo(stream int, frame []byte, keyframe bool, layer int, now int64) error
func (c *Client) SendVideoFrame(stream int, f *OutFrame, now int64) (int64, error) // returns the frame id
func (c *Client) VideoBitrate(stream int) int          // give this to the encoder; 0 means stop
func (c *Client) AckedFrame(stream int) int64          // newest frame every receiver decoded, or -1
func (c *Client) SendingRepresentation(stream int) int // RepOff, RepVideo or RepFace
func (c *Client) Subscribe(stream, maxLayer int, now int64) error
func (c *Client) PullAudio(stream int, now int64) *AudioFrame   // every 20 ms
func (c *Client) PollVideo(stream int, now int64) *VideoFrame   // nil when nothing is ready
func (c *Client) Representation(stream int) int        // what a received stream shows
func (c *Client) FaceReference(stream int) []byte      // newest reference picture received
func (c *Client) PollEvent() *Event
func (c *Client) HandleDatagram(p []byte, now int64)
func (c *Client) HandleControl(p []byte, now int64)
func (c *Client) Tick(now int64) int64
```

plus `Leave`, `Unpublish`, `Unsubscribe`, `SetReceiveLimit`, `TargetBitrate`,
`PeerID` and `Stats`. `Config` has `StartBitrate`, `MinBitrate`, `MaxBitrate`,
`FeedbackOnControl`, `FaceDownBitrate`/`FaceUpBitrate` (defaults 150 and
250 kbps) and, for comparisons only, `NoRecovery` and `NoLookAhead`. Frames go
in already end-to-end encrypted (the frame format of
[chat-group-calls.md](../app/chat-group-calls.md)); the engine never looks
inside them.

`OutFrame` is `{Data, Keyframe, Layer, LongTerm, Face, Recover, RefFrame}`.
`LongTerm` says the encoder keeps the frame as a long-term reference
(VideoToolbox hands out an LTR token for it; MediaCodec marks it). `Recover`
answers an `EventRecover`: the frame predicts from the event's frame (or from
`RefFrame` when the encoder chose another acknowledged one), is sent as a
layer-0 recovery frame, and must name a frame this stream sent with
`LongTerm`. `Face` without `Keyframe` is a face-stream frame; `Face` with
`Keyframe` is a reference picture (a JPEG or HEIC still, encrypted like any
frame, up to 16 kB).

Events (`Event.Kind`):

| Event | Fields | Meaning |
| --- | --- | --- |
| `EventJoined`, `EventTrackAdded`, `EventTrackRemoved`, `EventError` | as before | |
| `EventKeyframeRequest` | `Stream` | encode a keyframe |
| `EventBitrateHint` | `Stream`, `Bitrate` | the SFU's cap; 0 pauses an encoding |
| `EventRecover` | `Stream`, `FrameID` | encode the next frame predicting from `FrameID` (VideoToolbox `ForceLTRRefresh` with the acknowledged tokens, MediaCodec's use-LTR) |
| `EventSendRepresentation` | `Stream`, `Rep` | switch this stream's encoder: `RepVideo` (start with a keyframe), `RepFace` (send face frames), `RepOff` (stop) |
| `EventReferencePicture` | `Stream`, `MaxBytes` | encode a reference picture of at most `MaxBytes` and send it with `Face` and `Keyframe` |
| `EventRepresentation` | `Stream`, `Rep` | a received stream now shows video or face; show the right renderer |
| `EventFaceReference` | `Stream`, `MaxBytes` (its size) | a new reference picture arrived (`FaceReference`) |

The video bitrate of a track is the hop target minus each audio stream's
media, repair and overhead rate, minus video packet overhead, shared between
the published video tracks, divided by (1 + FEC ratio), times 0.97 for
headers. Within a track an active face encoding takes 32 kbps first; the
video encodings split the rest 1 : 3 (two) or 12 : 28 : 60 (three) among
those not paused; each is capped by the SFU's `BitrateHint` for its stream
and returned as 0 below 15 kbps (the encoder should stop rather than send an
unusable stream; it used to be floored at 30 kbps even when audio alone did
not fit). `AudioBitrate` is 20% of the target, between 16 and 32 kbps.

## SFU forwarding core (`fmm/sfu`)

Every hop is terminated at the SFU: congestion control, FEC, NACK and receiver
reports run separately between publisher and SFU and between SFU and each
subscriber, so one subscriber's loss or bandwidth never reaches the
publisher or the other subscribers. The SFU forwards end-to-end encrypted
payloads without reading them; it reads only the datagram header.

- **Rooms and admission.** `NewRoom(Config{Admit: func(ticket) (peerID, error)})`;
  `Connect(transport)` returns a `Peer` that handles that connection's
  datagrams and control. The call server ([fmm-server.md](fmm-server.md))
  checks the ticket of [rtc.md](rtc.md#signal), picks the room from it and
  answers `Admit` with the ticket's peer id; `Peer.Closed` tells it when the
  core closed a peer. `wire.DedupKey` (kind and transport sequence) is the
  key the QUIC transport uses to drop copies of redundantly sent datagrams.
  Limits: 16 peers, 4 tracks per peer, 4 encodings per track, peer ids
  unique in the room.
- **Tracks and encodings.** A Publish announces a TrackAdded to every other
  peer with a downstream stream id chosen per subscriber (1..254); a
  subscription starts forwarding. Additional encodings of a track are
  announced to nobody; each subscriber gets one encoding at a time on its one
  downstream stream. A new video subscription starts on the encoding that is
  carrying face frames, else the lowest active video encoding, else the
  primary (the encoding whose stream id is the track's), waits for a frame it
  can start on and asks the publisher for one.
- **Per-subscriber sequence and frame-id rewriting.** The forwarder decides
  per frame (cached by encoding and frame id): forward, or drop because the
  frame's layer is above the subscriber's current layer. A dropped frame
  whose sequences are beyond everything forwarded so far shifts the
  subscriber's offset by its fragment count, so the subscriber sees no gap. A
  dropped frame that arrives after later packets were already forwarded is
  left as a gap (it looks like loss). Frame ids pass unchanged until the
  first switch of encoding; from then on each encoding's ids are shifted to
  continue after the last forwarded one (references are deltas within an
  encoding, so they stay valid). Acknowledgements and recovery requests are
  mapped back to the encoding's own ids, and ignored if they name a frame
  from before the switch.
- **Switch points.** A subscriber changes encoding, starts, or resumes only on
  a frame decodable alone: a keyframe, or a face frame (which needs no
  reference). Switching from one video encoding to another therefore costs a
  keyframe of the target encoding, which the SFU requests when it picks the
  target; a long-term frame of another encoding cannot serve, since the
  subscriber's decoder never held it. Temporal layers still switch at
  layer-0 frames.
- **Allocation** (every 200 ms per subscriber). The budget is the
  subscriber's downstream target minus overhead (and its own receive limit)
  minus the audio it receives (with its FEC). Each video's options are its
  encodings' layers in rank order. When the track's primary is sending face
  frames, it is the only option. When the track has a face encoding (rank 0)
  and the subscriber's share is under 100 kbps (160 kbps to leave it again),
  the face encoding is the only option; otherwise the face encoding is not
  offered. Every video starts at its lowest option and climbs round-robin
  while the next option's extra cost (times 1 + FEC ratio) fits; a new layer
  needs 10% headroom, a new encoding 20%. An encoding's cost is its measured
  rate while it has been sending for the whole meter window, else its last
  rate measured that way, else 32 kbps (face or lowest) or 300 kbps: a paused
  encoding's decaying meter looked cheap and made subscribers flap between
  encodings every 3 s. Going down takes effect at the next switch point;
  going up to another video encoding waits 3 s after the last change.
- **Shedding.** When even the lowest options do not fit, video forwards are
  paused, the last first, so audio keeps the link. Here costs include the
  48-byte packet overhead (a 30 fps face stream is 20 kbps of payload and
  31 kbps on the wire), and the budget is the larger of the target-based one
  and the delivered rate when the hop loses under 5%: a delay-based target
  oscillates between 0.8 and 1 × the link, and pausing on the dips paused a
  face stream every 2 s at 100 kbps. A forward pauses at once when the need
  exceeds 1.3 × the budget, else after 1 s over it. A paused forward resumes
  behind a switch point when 1.5 × its cost (1.15 × under 60 kbps) fits the
  budget or 0.9 × the last probe's estimate, with a 2 s hold between changes.
  While any forward is paused the SFU asks its downstream controller to probe
  (otherwise a hop carrying only audio is application-limited and never
  learns that video would fit again), and it ignores keyframe requests from
  paused forwards (a paused subscriber whose stream never started asked for
  a keyframe every 500 ms, 59 keyframes in one 30 s loss-10 run). Before the
  shedding rule cap-300k with three clients concealed 52% of its audio;
  after it, 2%.
- **Publisher hints** (every 500 ms per encoding): an encoding is asked for
  min(lowest share of its subscribers / layer-0 share, highest share) × 0.9,
  at least 60 kbps, so the weakest of them can still take layer 0 and nobody
  gets more than the best one can use. The lowest video encoding of a
  multi-encoding track also counts paused subscribers and may go down to
  30 kbps: it is the one meant to follow the weakest link, and without that a
  paused subscriber waited forever for an encoding too big to resume. An
  encoding nobody receives gets hint 0 (paused), except the primary, which
  keeps 150 kbps while the track has no viewers at all.
- **Acknowledgements.** Each forwarder keeps the last 32 long-term frames its
  subscriber acknowledged (upstream ids); each encoding keeps the last 32
  long-term frames it carried, forgetting those before a keyframe. The common
  frame is the newest of them every viewer (subscribed, started, not waiting
  for a switch point, not paused) has acknowledged; it is sent to the
  publisher as an Ack when it changes.
- **Recovery.** A Recovery from a subscriber that is not viewing yet asks for
  a keyframe. Otherwise, if a recovery for this encoding went out with a high
  mark at or above the subscriber's stuck frame, the request is suppressed
  while that recovery can still be in flight (publisher RTT + subscriber RTT +
  150 ms) and becomes a keyframe request after that (the recovery did not
  help: the request repeats). Else the SFU sends the publisher one Recovery
  with the common frame, or a keyframe request when there is none or it is
  more than 240 frames behind. One loss seen by several subscribers yields one
  recovery.

## Long-term reference recovery (idea 3)

The whole loop, end to end:

1. The publisher marks long-term frames (`OutFrame.LongTerm`); the client
   keeps the last 32 it sent, cleared on a keyframe.
2. Each receiver acknowledges the long-term frames it decodes (Ack, repeated
   in two feedback datagrams, so one lost datagram costs nothing).
3. The SFU intersects the acknowledgements per encoding and tells the
   publisher the newest common frame (`AckedFrame`), so the encoder knows
   which references are safe (VideoToolbox's acknowledged LTR tokens).
4. A receiver whose layer-0 chain breaks sends a Recovery; the SFU
   aggregates and forwards one Recovery naming the common frame.
5. The publisher's client checks that the frame is one it kept and is within
   255 frames, then emits `EventRecover`; the native encoder predicts the next
   frame from it and sends it with `Recover`. A request the client cannot
   honour becomes `EventKeyframeRequest`.
6. Every receiver holds the reference, so the recovery frame is decodable
   everywhere; receivers that lost frames skip straight to it.
7. A receiver still stuck escalates to a keyframe; the SFU also turns a
   repeated request into a keyframe request.

The simulator's encoder marks every layer-0 frame long-term (one every
133 ms) and sizes a recovery frame at (1.6 + 0.04 × the frames between it and
its reference) × the mean frame size, at most a keyframe (5 ×). That is a
model; the real cost is the encoder's and should be measured on the
devices.

## DRED-aware audio (idea 2)

The other agent's Opus build (docs/native/opus.md) decides whether DRED is
compiled in; the core works either way. A packet with depth 0 behaves exactly
as before. With DRED the buffer recovers a lost frame from any later packet
that covers it, the look-ahead buys only the delay that recovers frames, and
the hop drops expensive audio FEC. The simulator models DRED as the task
states: a frame is recoverable if a later packet within the depth arrives in
time, at a cost of 6 bytes plus 1 byte per 40 ms of depth per packet (31
bytes for 1 s, about 12 kbps at 50 packets a second). The cost is an
assumption to replace with the Opus build's measurement.

## Face stream and encodings (idea 1, simulcast)

`PublishVideoTrack(name, layers, encodings, face, now)` publishes up to three
video encodings (ranks 1..3, the primary is the highest and has the track's
id) and optionally a face encoding (rank 0, one layer).

**Publisher.** Every 100 ms the client compares the track's video budget
(without the SFU's hints) with the face thresholds. Below 150 kbps for 1 s
the track switches to face: the primary sends face frames
(`EventSendRepresentation` with `RepFace`), the other encodings and the face
encoding stop. Above 250 kbps for 1 s it switches back, starting with a
keyframe. In face mode the client asks its controller to probe every 2 s, so
it learns when video would fit again. Separately, while the track sends
video, the face encoding sends face frames whenever the SFU hints it above 0,
that is while some subscriber is on it.

**Reference pictures.** The client asks for one (`EventReferencePicture`)
when a stream starts sending face frames, when the SFU asks a face stream for
a keyframe, and every 2 s if the face budget leaves at least 8 kbps.
`MaxBytes` is what half of the spare budget pays for in 2 s (600 bytes to
16 kB); the face budget is the track budget capped by the stream's hint, so a
face encoding serving a weak subscriber gets small pictures. The client splits
a picture into 500-byte parts, each a keyframe-and-face frame with a 4-byte
engine prefix (picture id, part index, part count) in front of the encrypted
bytes, and sends one part whenever half the spare budget has paid for it.
Sending the still as one frame put a 3 kB burst into a 100 kbps link, 240 ms
of queue in front of every audio packet. The receiver reassembles the parts,
keeps the newest complete picture (`FaceReference`) and returns it from
`PollVideo` as one frame with `Keyframe` and `Face`.

**Receiver.** `PollVideo` reports `Face` on every frame, and an
`EventRepresentation` whenever a stream changes between video and face.

## Round two: what changed and why

In the order it was built, each change measured in the simulator:

1. **Long-term recovery** (above). In the ablation (same code with recovery
   off), loss-20 calls send 27.4 keyframes (324 kB) without it and 13.1
   (119 kB) plus 7.6 recovery frames (46 kB) with it: half the keyframes and
   half the bytes, and 4 s less frozen per call. On burst 11.6 (197 kB)
   become 9.8 (151 kB) plus 1.9 (20 kB). On drop-recover (keyframes there come
   from subscribers resuming after a pause, which recovery cannot serve) it
   makes no difference.
2. **DRED** in the audio buffer, the depth field, the look-ahead and the
   cheap-FEC rule (above).
3. **Face stream, encodings, simulcast** (above).
4. **Drop-recover.** The trace of a 2 Mbps to 100 kbps drop showed the real
   causes: (a) the 400 ms queue fills in about 30 ms, and the reports that
   would slow the sender sit in the other direction's full queue; (b) after
   the cut the sender still offered more than the link carried (fixed-rate
   audio with 1 s of DRED, a 30 kbps video floor, FEC), so the queue stayed
   at 300-400 ms for 2 s; (c) the estimate then sat at the minimum, because
   an application-limited hop cannot grow past 1.5 × its throughput and its
   loss bound grew 8%/s from the overflow losses. The fixes: feedback-silence
   and delay fast cuts, pausing video below 15 kbps, audio bitrate and DRED
   depth that follow the target, not counting overflow losses, probing as
   soon as the queue drains, the probe floor for decreases, the
   application-limited room, smaller probe packets at low rates, and the
   congested pacer. Priority for audio in the pacer was already there (audio
   always first, and it may overdraw the budget); it cannot help once the
   packets are in the network's queue.
5. **Estimate overshoot at 300 kbps.** The two-client cap-300k concealment
   was 5% from short delay spikes as the uplink estimate overshot; with the
   standing-queue cut and the rules above it is 1.7% (mean of seeds 7 to 12;
   seed 7 alone gave 5% before). In the single-flow CC
   test the peak target is still 110% of a 300 kbps cap (as before), but the
   link's p95 queue fell from 107 to 72 ms. Increasing by frame-sized
   packets instead of 1200 bytes (as libwebrtc does) brought the peak to 105%
   but slowed the climb at 100 kbps enough to pause face streams, so it was
   not kept.
6. **Feedback datagrams.** Measured in the ablation table: loss-20 freezes
   92.8 (24.2 s) → 61.2 (15.5 s), burst freezes 32.6 (7.2 s) → 16.0 (3.7 s) (control stream first). Reliable reports
   and NACKs wait for QUIC retransmission behind lost packets (head-of-line
   blocking); unreliable ones, repeated, arrive on time or not at all.
7. **Keyframe storms and stuck estimates.** Eight-seed runs exposed single
   calls with 68 keyframes: a paused subscriber's receiver asked for a
   keyframe every 500 ms and the SFU passed each on, while its downstream,
   carrying only audio, could never grow enough to resume. The SFU now
   ignores those requests and probes while anything is paused. The same runs
   found the probe-size bug above.
8. **Random-loss threshold** 25% → 35% (30% path loss no longer collapses the
   uplink), and the SFU no longer costs layers with packet overhead (that made
   it drop layer 2 at burst loss; overhead counts only for shedding).

## Simulations

`server/rtc/fmm/sim` runs Go clients through the SFU core over the in-memory
link in virtual time (deterministic, single goroutine, a 30 s three-person
call takes about 0.3 s). Each client publishes audio and L1T3 video at 30 fps
following `VideoBitrate` (layer weights 1.6 / 0.9 / 0.75, keyframes five
times a mean frame, a rate-control balance that repays keyframes, a keyframe
on every request at most every 300 ms), subscribes to everything, pulls
audio every 20 ms and polls video every 5 ms. Audio frames follow
`AudioBitrate` (16-32 kbps) plus the modelled DRED bytes at the depth
`AudioRedundancyMs` suggests. The encoder marks layer-0 frames long-term and
answers `EventRecover` with a recovery frame (cost above). With face enabled
it sends 64-byte face frames at 30 fps on every stream told `RepFace`, and
3 kB (or `MaxBytes`) reference pictures when asked. Frames carry their
capture time, so delay is measured end to end (capture to playout or release,
through both hops and the jitter buffers). The profiles match the bench's
([call-bench.md](call-bench.md), `rtc/bench/profiles.go`) and apply to every
client's uplink and downlink unless a test says otherwise; all have 20 ms of
one-way delay and 40 bytes of per-packet overhead. The first 5 s are excluded
from the metrics. A freeze is an interval between released frames longer
than max(3 × mean interval, mean + 150 ms). Audio concealment counts plain
concealment and insertions ("audible"); frames rebuilt from FEC (the next
packet's LBRR) or DRED are reported separately, since a decoder plays them.

The simulator is chaotic: any change in the number of packets shifts every
later random loss decision, so one seed's numbers move by ±20% from changes
that do nothing on the profile (adding 11 bytes to each audio packet on a
lossless link moved jitter-100's video from 1238 to 1780 kbps). Every table
below is the mean of eight seeds (11 to 18) unless it says otherwise; the
capped profiles have no randomness and give the same result for every seed.

- `go test ./rtc/fmm/sim`: two clients, 12 s, clean, loss-10, jitter-100 and
  cap-300k, gated on the mean of seeds 7 to 12 (with `-short`: clean and
  loss-10), and the face-stream test below with its limits. The gate used one
  seed; on that seed jitter-100 passed by luck: over seeds 7 to 12 two-client
  jitter-100 conceals 3.2% of audio with the round-one code and 2.9% now, so
  its limit is 4%.
- `FMM_SIM=1 go test -v ./rtc/fmm/sim`: everything here.

"Before" numbers come from the round-one code (commit `ac7429636f`) with the
same profiles, seeds and metrics added to its harness.

### Three clients, every profile

| Profile | Audible audio concealment | FEC / DRED recovered (after) | Audio delay mean ms | Video fps | Freezes per call (total) | Video kbps | Keyframes per call (kB) | Recovery frames (kB, after) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| clean | 0.00% → **0.00%** | 0.00% / 0.00% | 70 → 70 | 30.0 → 30.0 | 0.0 (0.0 s) → 0.0 (0.0 s) | 2226 → 2125 | 6.0 (118) → 6.0 (105) | 0.0 (0) |
| loss-5 | 0.80% → **0.69%** | 0.00% / 1.30% | 77 → 80 | 28.5 → 29.2 | 1.0 (0.2 s) → 0.1 (0.0 s) | 1267 → 1310 | 10.5 (160) → 8.9 (129) | 0.0 (0) |
| loss-10 | 0.61% → **0.42%** | 0.00% / 1.14% | 90 → 94 | 26.6 → 27.5 | 3.5 (0.8 s) → 1.6 (0.3 s) | 884 → 1053 | 12.4 (144) → 10.9 (140) | 0.5 (3) |
| loss-20 | 0.87% → **0.43%** | 0.00% / 3.50% | 96 → 112 | 24.4 → 23.6 | 102.4 (27.5 s) → 61.2 (15.5 s) | 447 → 650 | 22.8 (172) → 13.1 (119) | 7.6 (46) |
| burst | 1.90% → **0.76%** | 0.00% / 3.89% | 80 → 102 | 28.5 → 27.5 | 30.6 (7.7 s) → 16.0 (3.7 s) | 1098 → 1157 | 16.2 (274) → 9.8 (151) | 1.9 (20) |
| jitter-100 | 1.91% → **1.62%** | 0.00% / 0.00% | 262 → 264 | 25.4 → 23.1 | 1.0 (3.6 s) → 0.8 (2.2 s) | 1064 → 1264 | 9.5 (105) → 8.6 (115) | 0.2 (1) |
| cap-300k | 2.37% → **2.17%** | 0.00% / 0.00% | 123 → 135 | 21.3 → 17.2 | 2.0 (4.8 s) → 0.0 (0.0 s) | 55 → 49 | 21.0 (50) → 8.0 (32) | 2.0 (1) |
| drop-recover | 8.03% → **8.64%** | 0.00% / 0.00% | 164 → 202 | 18.1 → 18.0 | 17.0 (28.2 s) → 28.0 (33.3 s) | 264 → 342 | 24.0 (184) → 21.0 (234) | 1.0 (1) |

cap-300k and drop-recover have no randomness, so all eight seeds give one
run, and that run swings with any change (drop-recover's audio moved between
6.6% and 8.6% across the last few changes of round two). With ±2 ms of
in-order jitter added to every link, which gives each seed its own run:

| Profile (±2 ms, 8 seeds) | Audible audio concealment | Audio delay mean ms | Video fps | Freezes per call (total) | Video kbps | Keyframes per call |
| --- | --- | --- | --- | --- | --- | --- |
| cap-300k | 2.35% → **1.99%** | 123 → 119 | 19.2 → 19.3 | 4.4 (10.0 s) → 0.5 (1.1 s) | 50 → 49 | 24.0 → 10.6 |
| drop-recover | 8.04% → **7.46%** | 163 → 194 | 17.0 → 19.3 | 16.9 (28.7 s) → 19.9 (27.1 s) | 258 → 312 | 23.2 → 18.0 |

Loss is applied on both hops, so loss-10 is 19% end to end and loss-20 is
36%. At loss-20 the new code sends more video (the controller no longer
collapses under random loss and feedback arrives on time), so frames are
bigger, more of them lose a packet, and more are skipped: fewer frames per
second, but far fewer and shorter freezes. Audio delay rises 15-20 ms on lossy
profiles from the DRED look-ahead and audio FEC.

### Feature ablation (each row turns one thing off)

| Variant | Profile | Audible conceal | Audio delay mean ms | fps | Freezes (total) | Video kbps | Keyframes (kB) | Recovery frames (kB) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all on | loss-10 | 0.42% | 94 | 27.5 | 1.6 (0.3 s) | 1053 | 10.9 (140) | 0.5 (3) |
| all on | loss-20 | 0.43% | 112 | 23.6 | 61.2 (15.5 s) | 650 | 13.1 (119) | 7.6 (46) |
| all on | burst | 0.76% | 102 | 27.5 | 16.0 (3.7 s) | 1157 | 9.8 (151) | 1.9 (20) |
| all on | cap-300k | 2.17% | 135 | 17.2 | 0.0 (0.0 s) | 49 | 8.0 (32) | 2.0 (1) |
| all on | drop-recover | 8.64% | 202 | 18.0 | 28.0 (33.3 s) | 342 | 21.0 (234) | 1.0 (1) |
| no long-term recovery | loss-10 | 0.33% | 94 | 26.9 | 2.6 (0.7 s) | 1011 | 11.8 (150) | 0.0 (0) |
| no long-term recovery | loss-20 | 0.50% | 118 | 24.6 | 61.1 (19.6 s) | 633 | 27.4 (324) | 0.0 (0) |
| no long-term recovery | burst | 0.72% | 101 | 27.9 | 16.9 (4.2 s) | 1207 | 11.6 (197) | 0.0 (0) |
| no long-term recovery | cap-300k | 1.44% | 122 | 21.2 | 0.0 (0.0 s) | 53 | 12.0 (38) | 0.0 (0) |
| no long-term recovery | drop-recover | 7.15% | 201 | 18.6 | 24.0 (31.4 s) | 345 | 20.0 (230) | 0.0 (0) |
| no DRED | loss-10 | 0.65% | 89 | 28.0 | 2.1 (0.5 s) | 1086 | 10.8 (140) | 0.6 (6) |
| no DRED | loss-20 | 0.88% | 95 | 22.3 | 63.0 (17.2 s) | 602 | 11.9 (112) | 7.6 (55) |
| no DRED | burst | 1.81% | 80 | 27.7 | 16.6 (4.0 s) | 1205 | 10.0 (154) | 1.9 (22) |
| no DRED | cap-300k | 1.97% | 122 | 20.5 | 0.0 (0.0 s) | 54 | 10.0 (35) | 3.0 (2) |
| no DRED | drop-recover | 8.05% | 163 | 15.3 | 25.0 (28.8 s) | 287 | 26.0 (271) | 1.0 (5) |
| feedback on the control stream | loss-10 | 0.44% | 95 | 28.1 | 2.5 (0.5 s) | 1065 | 11.0 (135) | 0.1 (1) |
| feedback on the control stream | loss-20 | 0.49% | 115 | 19.7 | 92.8 (24.2 s) | 460 | 15.8 (147) | 10.1 (48) |
| feedback on the control stream | burst | 0.72% | 100 | 27.6 | 32.6 (7.2 s) | 1161 | 10.0 (156) | 2.1 (24) |
| feedback on the control stream | cap-300k | 1.35% | 124 | 19.6 | 1.0 (2.2 s) | 51 | 10.0 (37) | 2.0 (2) |
| feedback on the control stream | drop-recover | 7.07% | 192 | 19.1 | 24.0 (31.1 s) | 338 | 29.0 (274) | 1.0 (6) |
| fixed 32 kbps audio | loss-10 | 0.38% | 94 | 27.7 | 2.0 (0.4 s) | 1021 | 10.9 (136) | 0.4 (1) |
| fixed 32 kbps audio | loss-20 | 0.46% | 117 | 21.9 | 64.0 (20.4 s) | 577 | 14.0 (129) | 9.0 (61) |
| fixed 32 kbps audio | burst | 0.74% | 103 | 27.8 | 13.9 (3.1 s) | 1160 | 9.6 (148) | 1.2 (15) |
| fixed 32 kbps audio | cap-300k | 1.60% | 121 | 17.4 | 2.0 (4.4 s) | 44 | 13.0 (42) | 2.0 (2) |
| fixed 32 kbps audio | drop-recover | 8.51% | 186 | 16.6 | 27.0 (40.2 s) | 341 | 19.0 (155) | 0.0 (0) |

### Audio through path loss (two clients, loss on p0's uplink only, audio p0 → p1)

| Path loss | Before: audible / FEC-assisted, delay | After, no DRED: audible / FEC, delay | After, DRED: audible / DRED, delay |
| --- | --- | --- | --- |
| random 10% | 0.53% / 0.44%, 88 ms | 0.52% / 0.54%, 85 ms | **0.47%** / 0.91%, 90 ms |
| random 20% | 0.19% / 0.22%, 92 ms | 0.11% / 0.11%, 91 ms | **0.06%** / 0.30%, 97 ms |
| random 30% | 0.34% / 0.24%, 92 ms | 0.44% / 0.38%, 92 ms | **0.51%** / 8.18%, 112 ms |
| burst (GE 0.02/0.25, 90%) | 0.95% / 1.70%, 79 ms | 1.10% / 1.73%, 78 ms | **0.94%** / 1.79%, 87 ms |
| burst (GE 0.05/0.2, 90%) | 2.64% / 2.51%, 89 ms | 2.18% / 2.42%, 89 ms | **0.55%** / 4.46%, 118 ms |

With room for it, hop-level FEC alone already holds random loss below 0.5%
(round one used 200% audio FEC here too); DRED adds little there, costs up to
20 ms of look-ahead, and at 30% one-hop loss is slightly worse than FEC
(0.51% against 0.34% before), because the cheap-FEC rule turns audio FEC off
once its rate passes 15% of the target. DRED wins on bursts (2.64% → 0.55% at
the heavier burst profile) and wherever FEC does not fit.

### Drop-recover at 100 kbps with the face stream (two clients)

2 Mbps both ways, 100 kbps from 10 s to 20 s, then 2 Mbps; p1 receiving p0.
Before, the same profile without a face stream: no video at all from 11 to
23 s, a standing 400 ms queue for the whole 100 kbps section, 28.9% audible
audio concealment over the call.

| Measure (the profile has no randomness; every seed gives the same) | Before | After |
| --- | --- | --- |
| Face (or video) frames per second from 13 to 20 s | 0 (video paused) | **32.6** face fps |
| Audible audio concealments from 13 to 20 s | not counted per second; the link held a 400 ms standing queue throughout | **1 of 400 frames** |
| Reference pictures received | none | 3 |
| Real video back | about 24-25 s (received video above 1 Mbps) | 26 s (25 fps or more) |
| Audible audio concealment over the whole call | 28.88% | 7.60% |
| Freezes over the call (total) | 7.0 (20.2 s) | 7.0 (3.6 s) |

Per second (seed 11):

| t (s) | link kbps | uplink target | SFU → p1 target | received video kbps | video fps | face fps | showing | audible conceal frames | queue ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 9 | 2000 | 2286 | 1572 | 1161 | 26 | 0 | video | 0 | 1 |
| 10 | 100 | 2286 | 1638 | 1262 | 30 | 0 | video | 0 | 5 |
| 11 | 100 | 65 | 88 | 138 | 3 | 0 | video | 47 | 381 |
| 12 | 100 | 60 | 85 | 0 | 0 | 0 | video | 40 | 116 |
| 13 | 100 | 60 | 74 | 23 | 0 | 45 | face | 0 | 90 |
| 14 | 100 | 68 | 87 | 17 | 0 | 35 | face | 0 | 7 |
| 15 | 100 | 79 | 91 | 15 | 0 | 31 | face | 0 | 6 |
| 16 | 100 | 137 | 98 | 15 | 0 | 30 | face | 0 | 26 |
| 17 | 100 | 91 | 106 | 15 | 0 | 30 | face | 0 | 0 |
| 18 | 100 | 144 | 115 | 15 | 0 | 30 | face | 1 | 6 |
| 19 | 100 | 77 | 78 | 15 | 0 | 30 | face | 0 | 65 |
| 20 | 2000 | 84 | 91 | 15 | 0 | 30 | face | 0 | 6 |
| 21 | 2000 | 139 | 136 | 15 | 0 | 31 | face | 0 | 0 |
| 22 | 2000 | 586 | 165 | 15 | 0 | 30 | face | 0 | 0 |
| 23 | 2000 | 1911 | 173 | 15 | 0 | 30 | face | 0 | 0 |
| 24 | 2000 | 1911 | 180 | 46 | 7 | 4 | video | 0 | 0 |
| 25 | 2000 | 1966 | 723 | 146 | 22 | 0 | video | 0 | 4 |
| 26 | 2000 | 2039 | 1384 | 814 | 29 | 0 | video | 1 | 2 |

The 1.5 s after the drop are lost to the queue (about 80 audio frames); from
13 s the face runs at 30 fps with almost no audio concealment, and real video
returns 4 to 6 s after the link does: the probes have to find the capacity
again (the periodic probe comes every 5 s, the client asks for one every 2 s
in face mode), then 1 s of hysteresis, then a keyframe.

### Weak subscriber (simulcast and face)

Three clients at 2.5 Mbps; p2's downlink is capped at 250 kbps.

| Setup | p0 ← p1 kbps | p1 ← p0 kbps | p2 ← p0 kbps | p2 ← p1 kbps | p2 fps | p0/p1 freezes | p2 audible conceal |
| --- | --- | --- | --- | --- | --- | --- | --- |
| before, one encoding (seed 23) | 73 | 80 | 25 | 14 | 4.3 | 0 | 7.32% |
| one encoding | 756 | 816 | 0 | 0 | 0.0 | 0 | 1.23% |
| simulcast x3 | 782 | 794 | 34 | 31 | 22.1 | 0 | 2.05% |
| simulcast x3 + face | 746 | 756 | 15 | 15 | 30.0 | 0 | 0.55% |
| one encoding + face | 830 | 752 | 15 | 15 | 30.0 | 0 | 0.41% |

Before (one encoding, seed 23): p0 received p1 at 73 kbps and p1 received p0
at 80 kbps, because the hint followed p2; p2 got 25 and 14 kbps at 6 and
3 fps with 3 freezes each and 7.3% audio concealment. Now the weak subscriber
never drags the others down: with one encoding it is paused (audio only),
with simulcast it gets the lowest encoding at 22 fps, and with a face
encoding it gets live faces at 30 fps.

### drop-recover, client p0 (300 kbps section)

| t (s) | 9 | 10 | 11 | 12 | 13 | 15 | 17 | 20 | 21 | 22 | 23 | 24 | 25 | 28 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | 
| link (kbps) | 2000 | 300 | 300 | 300 | 300 | 300 | 300 | 2000 | 2000 | 2000 | 2000 | 2000 | 2000 | 2000 |
| uplink target | 1918 | 1918 | 190 | 173 | 297 | 232 | 270 | 225 | 481 | 1927 | 1435 | 1447 | 1447 | 1940 |
| encoder | 808 | 900 | 86 | 73 | 150 | 60 | 150 | 60 | 60 | 421 | 848 | 600 | 600 | 766 |
| SFU → p0 target | 1713 | 1778 | 236 | 192 | 193 | 292 | 212 | 233 | 250 | 270 | 575 | 1214 | 1345 | 1681 |
| video received by p0 | 1482 | 1517 | 281 | 44 | 61 | 28 | 25 | 63 | 77 | 198 | 189 | 734 | 1366 | 1465 |
| audible conceal frames | 3 | 4 | 97 | 100 | 19 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 11 | 2 |
| queue (ms) | 1 | 6 | 363 | 276 | 0 | 4 | 2 | 0 | 0 | 3 | 0 | 6 | 3 | 5 |
| before: uplink target | 1934 | 1934 | 289 | 146 | 146 | 191 | 292 | 292 | 584 | 1923 | 1923 | 1923 | 1923 | 1923 |
| before: video received by p0 | 1108 | 1148 | 241 | 71 | 35 | 10 | 45 | 39 | 42 | 153 | 464 | 721 | 1002 | 1115 |
| before: queue (ms) | 1 | 6 | 370 | 86 | 0 | 8 | 0 | 0 | 0 | 0 | 5 | 15 | 5 | 1 |

## Targets (fm-media.md) against the simulator

| Target | Simulator now | Met in sim? |
| --- | --- | --- |
| Join to first audio under 300 ms (clean regional path) | not measured here (QUIC handshake and Join are outside the core's simulator) | not measured |
| Mouth-to-ear under 150 ms (clean regional path) | 70 ms capture to playout through the SFU at 20 ms one-way per hop, without device capture and playout latency | yes for the network part; device latency not modelled |
| Audio at 30% random loss: under 3% audible concealment | 0.51% (with DRED) / 0.44% (FEC only) at 30% path loss; 0.43% at loss-20 on both hops (36% end to end) | yes |
| Video at 300 kbps: continuous real video at reduced resolution | two clients: 25 fps, no freezes; three clients (two remote videos in 300 kbps): 17-19 fps, 0 to 0.5 freezes (1.1 s) per call | yes with two clients; with three, nearly (19 fps, occasional short freezes) |
| Below 100 kbps: a live face stream, no frozen frames | 32 fps face frames from 13 s (the drop is at 10 s), 1 audible concealment in 400 frames | yes once the face is up; the 2 s after a sudden drop still freeze and lose about 1.5 s of audio |
| Wi-Fi to cellular: no audible gap | not in this core (multipath is the QUIC layer's) | not measured |
| A core dies mid-call: audio back in under 500 ms | not in this core (standby core) | not measured |
| Engine size on a phone under 5 MB | not measured | not measured |

## Fuzzing and tests

Native Go fuzz targets: `wire.FuzzParseDatagram` (every kind round-trips
byte for byte), `wire.FuzzDecodeControl` (every decoded message re-encodes
to the same bytes, so the encoding is canonical, including the Publish tail,
Ack and Recovery), `wire.FuzzControlReader` (arbitrary chunking),
`wire.FuzzParseFeedback` (only feedback messages, and the datagram
round-trips), `fec.FuzzDecoder` (recovered packets always parse),
`frag.FuzzReassembler` (no state is left after expiry), `sfu.FuzzPeerInput`
and `fmm.FuzzClientInput` (arbitrary datagrams and control into a live
peer, seeded with feedback datagrams, Ack, Recovery, an encoding Publish and a
reference-picture part). Each ran for 20 s with no failure on 2026-10-21 on a
heavily loaded machine (from about 7 000 executions for the reassembler to
2 million for the control decoder), for example `go test -run XXX -fuzz '^FuzzParseFeedback$' -fuzztime 20s ./rtc/fmm/wire`.
`cd server && go test -race ./rtc/fmm/...` covers everything else.

## Not done yet

- Join to first audio, Wi-Fi-to-cellular gaps and core failover are measured
  by the bench and the QUIC layer, not here.
- The first 1.5 s after a sudden 20× capacity drop still lose audio: the
  network's queue fills before any signal can return. Only a smaller
  bottleneck queue or priority in the network would help.
- Three-person calls at 300 kbps each run at 17-19 fps per remote video and
  still freeze briefly now and then.
- Drop-recover with three clients improved only modestly (audio 8.0% → 7.5%
  with jitter averaged); its freezes did not.
- At loss-20, recovery replaces only some keyframes: most remaining ones are
  subscription starts and receivers escalating after a lost recovery frame.
- Simulcast switches between video encodings need a keyframe of the target
  encoding; long-term frames cannot serve across encodings.
- DRED's cost and quality in the simulator are a model; the Opus build has
  DRED off for now (docs/native/opus.md).
- A per-track packet cache shared by all subscribers at the SFU. Today each
  subscriber's downstream sender keeps its own 512-packet video history.
- The WebRTC bridge. Room and ticket integration and failover are in the
  call server ([fmm-server.md](fmm-server.md)).
- Audio time-stretching instead of whole-frame drop and insert.
