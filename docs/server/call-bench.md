# Call bench

The bench is the bar a call engine has to beat. It runs real headless
Chromium clients through a call while an in-process UDP relay damages the
network between one of them and the server, and it measures what people in
the call would notice: how long joining takes, how smooth the video is, how
much audio is concealed, how late the picture is, and what it costs in CPU.
[fm-media.md](fm-media.md) uses it to decide when FM Media is better than the
WebRTC path in [rtc.md](rtc.md). Built 2026-10-21.

## Running it

The bench is a Go test that skips unless `FORKMESH_CALL_BENCH=1`, so a plain
`go test ./rtc/...` stays fast.

```sh
python3 tools/call_bench.py run                         # every profile, 30 s each, about 7 minutes
python3 tools/call_bench.py run --profiles clean,loss-10 --seconds 20
python3 tools/call_bench.py render <dir>/results.json   # Markdown from an earlier run
```

or straight from Go:

```sh
cd server
FORKMESH_CALL_BENCH=1 FORKMESH_CALL_BENCH_PROFILES=clean,cap-300k \
  go test -count=1 -run '^TestCallBench$' -timeout 60m -v ./rtc/bench/
```

| Variable | Meaning |
| --- | --- |
| `FORKMESH_CALL_BENCH=1` | run the bench at all |
| `FORKMESH_CALL_BENCH_PROFILES` | comma-separated profile names, default all |
| `FORKMESH_CALL_BENCH_SECONDS` | measured seconds per profile, default 30 |
| `FORKMESH_CALL_BENCH_OUT` | results folder; else `$FORKMESH_AGENT_SCRATCH/call-bench/<time>`, else a temp dir |
| `FORKMESH_TEST_CHROMIUM` | the browser; else the newest Playwright `chrome-headless-shell` in `~/.cache/ms-playwright` |

A run writes `results.json` (every metric of every profile), `raw/<profile>.json`
(each browser's full report: the getStats timeline, frame traces, logs, relay
counters) and, through the Python tool, `results.md` with the tables below.
Chromium runs with `HOME` and the `XDG_*` folders in a temp dir and without
`FORKMESH_HOME` or `FORKMESH_PROJECTS_DIR`, like every test subprocess.

## How a run works

For each profile the harness starts a fresh server, a relay and a web server,
then three browsers (`ana`, `ben`, `cai`):

1. `ben` and `cai` join a clean call, one second apart, and run for 5 s.
2. `ana` starts. That moment is t = 0: every scheduled change of the profile
   counts from it, and the measured window is the next 30 s.
3. Every browser reports its stats at the end of the window; the harness reads
   the CPU time of each browser's process tree, then kills them.

Only `ana` is behind the impaired link, in both directions. So **down** is
what `ana` receives (the other two streams, each crossing `ana`'s downlink
once) and **up** is `ana`'s audio and video as `ben` and `cai` receive them
(crossing `ana`'s uplink once). `ben` and `cai` also receive each other on
clean links; that is the `control` direction in `results.json`, a check that
the server itself is not the bottleneck.

Only the media UDP path goes through the relay. The page, the WebSocket
signaling and the key exchange are plain loopback HTTP, so join times are the
media part of joining on that network, not the signaling round trips.

### The relay (`server/rtc/netsim`)

`netsim.Listen(address, target, Config)` opens a UDP socket that clients send
to and forwards to `target` (the server's media port). Every client source
address gets its own upstream socket, so the server sees one address per
client, as it would on the internet. Clients are told apart by a key taken
from their first packet: `netsim.ICEUfragKey` reads the sender's ICE ufrag
from the STUN binding request, which the bench page reports to the harness
before it sends its offer (`/bench/identify`). `Relay.Shape(key, start, up, down)`
then gives that client its own uplink and downlink. Every flow of one client
(Chromium checks from more than one local address) shares the same two links,
so a rate cap is per client, not per socket.

A link applies, per packet and in this order:

| Impairment | Field | Model |
| --- | --- | --- |
| random loss | `Loss` | independent per packet |
| burst loss | `Burst{Enter, Leave, LossGood, LossBad}` | Gilbert-Elliott: the state decides the loss of this packet, then moves good to bad with `Enter`, bad to good with `Leave`; mean burst 1/`Leave` packets, long-run loss `Enter/(Enter+Leave)·LossBad` |
| duplication | `Duplicate` | a second copy, which then queues like any packet |
| rate cap | `Rate` (bit/s), `QueueBytes` | a drop-tail queue drained at `Rate`; a packet that does not fit the bytes still queued is dropped (`DefaultQueueBytes` 64 KiB) |
| delay and jitter | `Delay`, `Jitter`, `Reorder` | fixed delay plus uniform ±`Jitter`; without `Reorder` a packet never overtakes the one before it (a FIFO path), with it packets leave in arrival-time order |

A link is a list of `Step{At, Profile, Rebind}`; the step whose `At` has
passed applies, so `2 Mbps → 300 kbps at 10 s → 2 Mbps at 20 s` is three
steps. `Rebind` at a step moves every flow of the client to a new upstream
socket, so the server sees the client arrive from a new address and port, the
way a phone does when it moves from Wi-Fi to cellular. All delivery runs on
one scheduler goroutine with a timer heap; equal times keep arrival order.
`Link.Plan(now, size)` returns the arrival times without any sockets, which is
what the unit tests drive.

### Putting the relay in front of the SFU

The SFU advertises one ICE host candidate per `Config.Addresses` entry, on
its own media port. The bench listens on the SFU's port on another loopback
address (`127.0.0.2:<port>`, the whole of 127.0.0.0/8 is local on Linux),
points the relay at `127.0.0.1:<port>` and gives the SFU
`Addresses: [127.0.0.2]`. Browsers then only know the relay's address, and no
SFU code changes for the bench. It needs Linux, or another system with the
whole loopback /8.

### The browser side, and other engines

`server/rtc/bench/web/` is embedded in the test binary:

- `bench.html` and `bench.js`, the engine-neutral page. It replaces
  `getUserMedia` so the video track is a 640x360, 24 fps canvas: the fake
  camera, a moving patch of noise so the encoder always has work, and a strip
  of 48 black or white blocks with the wall clock (40 bits of milliseconds
  and an 8-bit check) drawn into every frame (`stamp.js`). Audio is
  Chromium's fake microphone. It plays every remote track, reads the clock
  back from every presented remote frame (`requestVideoFrameCallback`),
  samples the adapter's stats once a second, and posts one report at the end.
- an adapter module, given to the page as `?adapter=`. It exports
  `join({name, log, identify})`, which joins the call and resolves to a
  session with `self()`, `state()`, `remote()` (a list of `{peer, stream}`),
  `stats()` (the common shape below) and `leave()`. It calls `identify(key)`
  with whatever its transport's first packet will reveal to the relay's key
  function.
- `adapters/webrtc-sfu.js` drives the production group-call client
  `app/public/chat-group-calls.js`, end-to-end encryption included (keys
  travel through the engine's `/engine/relay` route like the browser tests'
  key relay), and reports the ICE ufrag of each new peer connection.
  `adapters/webrtc-stats.js` maps `getStats()` to the common shape.

In Go an engine is `bench.Engine{Name, Adapter, Key, Start}`. `Start` opens
its media socket, asks the harness for a relay in front of it, advertises the
relay's address, and mounts its routes. `bench.WebRTC()` is the current SFU.
FM Media adds its own `Engine` and adapter (with a `Key` that reads its QUIC
connection id, say) and runs `bench.RunProfile` on the same `Profiles()`.

The common stats shape (fields an engine cannot provide are left out):

```
{
  inbound:  [{peer, kind, ssrc, packetsReceived, packetsLost, bytesReceived, jitter,
              jitterBufferDelay, jitterBufferEmittedCount,
              totalSamplesReceived, concealedSamples, concealmentEvents,
              framesDecoded, framesDropped, keyFramesDecoded, frameWidth, frameHeight,
              freezeCount, totalFreezesDuration, nackCount, pliCount, firCount}],
  outbound: [{kind, ssrc, bytesSent, packetsSent, framesSent, frameHeight,
              targetBitrate, qualityLimitationReason, nackCount, pliCount,
              retransmittedPacketsSent}],
  transport: {currentRoundTripTime, availableOutgoingBitrate}
}
```

The names and units are the W3C `getStats()` ones (seconds for durations and
delays, bits per second for bitrates, cumulative counters), so a new engine
can be compared field for field.

## Profiles

| Name | Each way, on `ana`'s link |
| --- | --- |
| `clean` | nothing |
| `loss-5`, `loss-10`, `loss-20` | 5, 10, 20% random loss |
| `burst` | Gilbert-Elliott `Enter 0.02, Leave 0.25, LossBad 0.9`: about 6.7% loss in bursts of about 4 packets |
| `jitter-100` | 100 ms delay, ±30 ms jitter, in order |
| `cap-1m` | 1 Mbps, 250 ms queue |
| `cap-300k` | 300 kbps, 400 ms queue |
| `drop-recover` | 2 Mbps; 300 kbps from 10 s; 2 Mbps again from 20 s |
| `path-change` | Wi-Fi (10 ± 3 ms); at 12 s cellular (60 ± 15 ms, 1% loss, 4 Mbps) from a new source port |

## Metrics

All video and audio figures are means per stream over the streams of a
direction (two each way).

| Metric | From |
| --- | --- |
| Join video, join audio | `ana`'s page start (`performance.timeOrigin`) to the first presented remote video frame, and to the first getStats sample with decoded (not concealed) remote audio; audio is polled every 50 ms until then. `never` if it did not happen in the window |
| fps | `framesDecoded` over the time since the stream's first frame |
| freezes | count and total seconds per stream from getStats `freezeCount` and `totalFreezesDuration` (Chromium's definition: a frame later than max(3 × the mean frame interval, mean + 150 ms)) |
| stalls | count and total seconds per stream of gaps over 500 ms between presented frames, the last frame to the end of the window included, and the whole window for a stream that never showed a frame. Chromium's freeze threshold grows with the mean frame interval, so a picture that has nearly stopped (one frame every few seconds) shows almost no freezes; stalls still count it. An engine without freeze fields is judged on stalls |
| height | median `frameHeight` over the samples |
| kbps | `bytesReceived` over the time since the first byte |
| delay p50/p95 | glass to glass minus the display: receiver wall clock when a frame is presented minus the clock drawn into it on capture, over every frame whose stamp reads back (all browsers share one clock). `stampReadPct` in the JSON is the share that read back |
| concealed % | `concealedSamples / totalSamplesReceived` |
| concealment events | `concealmentEvents` |
| jitter buffer ms | `jitterBufferDelay / jitterBufferEmittedCount` |
| packets lost, video loss % | `packetsLost`, and lost over lost plus received. Chromium's `packetsLost` can go negative when most of a stream is lost (big sequence jumps); such a stream is left out of the loss %, and the relay's own counters are the reliable figure |
| NACK/PLI/FIR | requests the receiver sent |
| frames dropped | `framesDropped` at the receiver |
| available out kbps | `ana`'s `availableOutgoingBitrate`, its sender's own estimate of the uplink, mean of the samples |
| sent kbps, sent height, limited by | `ana`'s outbound video: bytes sent per second, median `frameHeight`, the most common `qualityLimitationReason` |
| RTT | `ana`'s ICE `currentRoundTripTime` |
| relay lost/overflow | share of `ana`'s packets the relay dropped by loss and by a full queue, which checks that the profile was applied |
| CPU % | CPU time of the browser's whole process tree (utime + stime + children, from `/proc`) over its wall time, in percent of one core; the server figure is the Go test process (SFU, relay, web server and harness together) |
| phases, recovery | for profiles with scheduled changes: fps and kbps per phase from the 1 s samples, and the seconds after the last change until a 1 s interval is back to 80% of the stream's rate between 3 s and the first change; the slowest stream counts. `none` if a stream never gets back, `n/a` if there was no rate before the change to get back to |

`results.json` also has `delayProxyMs` (RTT / 2 plus the video jitter buffer
delay), a cruder delay figure for an engine that cannot draw into its frames.

## Baseline: the WebRTC path

Run 2026-10-21 on the branch that added the bench: the SFU of
[rtc.md](rtc.md) with the production client `app/public/chat-group-calls.js`
(end-to-end encrypted, VP8 640x360 at 24 fps, Opus), headless Chromium 151
(`chrome-headless-shell`), a 36-CPU Linux desktop, 30 s per profile, three
browsers, `python3 tools/call_bench.py run`. A second full run the same day
gave the same picture (fps within about 1 per stream, joins within a few
hundred ms except `loss-20`, whose audio join was 6.7 s in one run and 0.35 s
in the other). **Down** is what `ana` receives, **up** is `ana`'s media at the
others; means per stream.

### Join and video

| Profile | Join video ms | Join audio ms | Down fps | Down freezes | Down stalls | Down height | Down kbps | Down delay p50/p95 ms | Up fps | Up freezes | Up stalls | Up height | Up kbps | Up delay p50/p95 ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| clean | 293 | 318 | 24.0 | 0.0 / 0.0 s | 0.0 / 0.0 s | 360 | 1515 | 50 / 83 | 23.9 | 0.0 / 0.0 s | 0.0 / 0.0 s | 360 | 1425 | 69 / 98 |
| loss-5 | 332 | 370 | 0.9 | 2.0 / 10.6 s | 9.0 / 29.3 s | 360 | 1437 | 35 / 1326 | 0.6 | 3.0 / 15.3 s | 4.0 / 28.9 s | 360 | 1345 | 44 / 70 |
| loss-10 | 1272 | 440 | 0.3 | 0.0 / 0.0 s | 4.0 / 28.5 s | 360 | 1366 | 34 / 64 | 0.2 | 1.0 / 18.2 s | 4.0 / 29.5 s | 360 | 1316 | 63 / 285 |
| loss-20 | 8830 | 349 | 0.1 | 0.0 / 0.0 s | 1.5 / 17.0 s | 360 | 1210 | 27 / 43 | 0.0 | 0.0 / 0.0 s | 1.0 / 22.6 s | 360 | 1159 | 33 / 33 |
| burst | 299 | 334 | 1.3 | 5.0 / 21.3 s | 7.5 / 28.3 s | 360 | 1399 | 36 / 109 | 2.3 | 7.0 / 21.5 s | 9.0 / 25.3 s | 360 | 1314 | 57 / 85 |
| jitter-100 | 1512 | 1051 | 24.0 | 0.0 / 0.0 s | 0.0 / 0.0 s | 360 | 1480 | 190 / 232 | 23.9 | 0.0 / 0.0 s | 0.0 / 0.0 s | 360 | 1424 | 201 / 247 |
| cap-1m | never | 401 | 0.0 | 0.0 / 0.0 s | 1.0 / 30.1 s | 0 | 454 | - | 1.4 | 1.0 / 15.8 s | 2.0 / 27.0 s | 360 | 945 | 130 / 305 |
| cap-300k | never | 476 | 0.0 | 0.0 / 0.0 s | 1.0 / 30.0 s | 0 | 103 | - | 0.4 | 0.0 / 0.0 s | 1.0 / 28.6 s | 360 | 247 | 215 / 398 |
| drop-recover | 303 | 317 | 0.2 | 0.0 / 0.0 s | 1.0 / 29.6 s | 180 | 666 | 166 / 241 | 13.1 | 3.0 / 13.0 s | 1.0 / 12.6 s | 360 | 972 | 115 / 470 |
| path-change | 1348 | 369 | 11.4 | 3.0 / 11.2 s | 4.0 / 15.2 s | 360 | 1429 | 60 / 204 | 11.1 | 4.0 / 13.0 s | 5.0 / 15.9 s | 360 | 1364 | 70 / 203 |

### Audio

| Profile | Down concealed % | Down concealment events | Down jitter buffer ms | Down packets lost | Up concealed % | Up concealment events | Up jitter buffer ms | Up packets lost |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| clean | 0.0 | 0 | 31 | 0 | 0.0 | 0 | 30 | 0 |
| loss-5 | 4.0 | 66 | 26 | 66 | 4.0 | 79 | 24 | 75 |
| loss-10 | 9.3 | 188 | 17 | 138 | 8.5 | 176 | 20 | 137 |
| loss-20 | 18.0 | 267 | 22 | 310 | 18.9 | 271 | 22 | 307 |
| burst | 6.5 | 113 | 20 | 100 | 5.2 | 57 | 23 | 89 |
| jitter-100 | 0.0 | 0 | 54 | 0 | 0.0 | 0 | 65 | 0 |
| cap-1m | 1.1 | 12 | 66 | 9 | 0.9 | 8 | 166 | 1 |
| cap-300k | 2.4 | 20 | 67 | 18 | 1.6 | 19 | 177 | 9 |
| drop-recover | 1.6 | 15 | 176 | 16 | 1.5 | 6 | 263 | 7 |
| path-change | 5.6 | 9 | 54 | 8 | 5.3 | 9 | 48 | 6 |

### Loss repair and the sender

| Profile | Down video loss % | Down NACK/PLI/FIR | Down frames dropped | Up video loss % | Up NACK/PLI/FIR | Available out kbps | Sent kbps | Sent height | Limited by | RTT ms | Relay up lost/overflow | Relay down lost/overflow |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| clean | 0.0 | 0 / 0 / 0 | 0 | 0.0 | 0 / 2 / 0 | 1500 | 1438 | 360 | none | 1 | 0.0% / 0.0% | 0.0% / 0.0% |
| loss-5 | 5.1 | 0 / 9 / 0 | 395 | 4.8 | 0 / 10 / 0 | 1500 | 1418 | 360 | none | 3 | 4.7% / 0.0% | 5.0% / 0.0% |
| loss-10 | 9.6 | 0 / 16 / 0 | 294 | 9.5 | 0 / 10 / 0 | 1500 | 1433 | 360 | none | 1 | 9.6% / 0.0% | 9.5% / 0.0% |
| loss-20 | 20.0 | 0 / 60 / 0 | 86 | 19.7 | 0 / 39 / 0 | 1500 | 1427 | 360 | none | 1 | 19.8% / 0.0% | 20.4% / 0.0% |
| burst | 7.1 | 0 / 9 / 0 | 532 | 6.2 | 0 / 17 / 0 | 1500 | 1436 | 360 | none | 1 | 6.2% / 0.0% | 7.0% / 0.0% |
| jitter-100 | 0.0 | 0 / 4 / 0 | 12 | 0.0 | 0 / 0 / 0 | 1500 | 1425 | 360 | none | 225 | 0.0% / 0.0% | 0.0% / 0.0% |
| cap-1m | 69.7 | 0 / 123 / 0 | 0 | 33.7 | 0 / 12 / 0 | 1500 | 1423 | 360 | none | 446 | 0.0% / 23.7% | 0.0% / 51.8% |
| cap-300k | 74.2 | 0 / 134 / 0 | 0 | 82.4 | 0 / 11 / 0 | 1500 | 1419 | 360 | none | 733 | 0.0% / 58.0% | 0.0% / 69.1% |
| drop-recover | 48.0 | 0 / 70 / 0 | 0 | 29.8 | 0 / 8 / 0 | 1500 | 1418 | 360 | none | 435 | 0.0% / 21.2% | 0.0% / 41.1% |
| path-change | 0.5 | 0 / 10 / 0 | 236 | 0.6 | 0 / 5 / 0 | 1500 | 1428 | 360 | none | 92 | 0.6% / 0.0% | 0.5% / 0.0% |

### CPU

| Profile | ana CPU % | Others CPU % (mean) | Server process CPU % |
| --- | --- | --- | --- |
| clean | 53 | 51 | 11 |
| loss-5 | 41 | 50 | 11 |
| loss-10 | 39 | 49 | 10 |
| loss-20 | 39 | 50 | 10 |
| burst | 39 | 47 | 10 |
| jitter-100 | 55 | 53 | 11 |
| cap-1m | 36 | 48 | 11 |
| cap-300k | 35 | 48 | 10 |
| drop-recover | 36 | 48 | 11 |
| path-change | 48 | 52 | 11 |

### Phases

| Profile | Phase | Down fps | Down kbps | Up fps | Up kbps | Available out kbps | Sent kbps |
| --- | --- | --- | --- | --- | --- | --- | --- |
| drop-recover | 0-10 s | 0.0 | 928 | 21.9 | 1221 | 1500 | 1325 |
| drop-recover | 10-20 s | 0.0 | 95 | 0.1 | 240 | 1500 | 1455 |
| drop-recover | 20-30 s | 0.0 | 922 | 16.7 | 1482 | 1500 | 1447 |
| drop-recover | recovery after the last change | n/a | 1 s | 4 s | 1 s |  |  |
| path-change | 0-12 s | 23.9 | 1465 | 24.0 | 1370 | 1500 | 1373 |
| path-change | 12-30 s | 4.1 | 1352 | 3.1 | 1353 | 1500 | 1459 |
| path-change | recovery after the last change | 7 s | 2 s | none | 2 s |  |  |

### What the baseline shows

Each of these is what the SFU did when the bench was built, with the
evidence above. Items 1 to 4 were fixed the same day; the next section has
what that changed.

1. **No loss repair for video.** The SFU offers `nack pli` but not plain
   `nack` and no RTX (`describe.go`, `codecLines`), so receivers never send a
   NACK (0 in every profile) and senders never retransmit (0 packets). The only
   repair is a keyframe on PLI, at most one per second per stream
   (`keyframeInterval`), and a 1.4 Mbps keyframe is dozens of packets, so it
   rarely arrives whole. At 5% loss video falls from 24 to under 1 fps with
   about 29 of 30 s stalled; at 20% the first frame takes 8.8 s. Even the 1%
   loss of the cellular leg in `path-change` keeps video near 4 fps.
2. **Senders are never told to slow down.** `ana`'s available outgoing
   bitrate is 1500 kbps in every profile, which is the SFU's fixed REMB
   (`Config.VideoBitrate`, sent from `Room.tick`), and its encoder keeps
   sending about 1.42 Mbps into a 300 kbps link (`cap-300k`: 58% of its
   packets dropped by the full queue, RTT 0.73 s; `drop-recover` 10-20 s:
   1455 kbps sent), with `qualityLimitationReason` `none` throughout. The SFU's own receiver reports
   carry no report blocks (`rtp.NewReceiverReport`), receivers' REMB and
   receiver reports are dropped (`rtcpLocked` keeps only PLI, FIR and SR), and
   no transport-wide congestion control or `abs-send-time` extension is
   negotiated, so Chromium's estimator has neither a loss nor a delay signal.
   The same is why Opus never switches on in-band FEC although the SFU offers
   `useinbandfec=1` (0 FEC packets received, Opus target stuck at 32 kbps),
   and audio concealment tracks the loss rate one for one.
3. **No per-receiver adaptation.** The SFU forwards every stream at the
   sender's rate to every receiver, with no simulcast or layer choice. In a
   three-person call a receiver needs about 3 Mbps for two 1.45 Mbps streams,
   so at 1 Mbps, 300 kbps and even 2 Mbps (`drop-recover` before the drop)
   `ana` never shows a single remote frame (join video `never`, 0 fps), and
   only the sender's own uplink recovers after the drop (up fps back in 4 s
   because the queue empties, not because anything adapted).
4. **Join is slow under loss.** Join stays under 0.5 s on a clean link but
   takes 1.3 s (video) at 10% loss and 8.8 s at 20%, and 1.5 s on the 100 ms
   jitter path. Besides the keyframe problem above, the SFU's DTLS server
   retransmits a lost flight after 1 s, doubling, and gives up after four
   tries (`dtls/server.go`, `initialRetransmit`, `maxRetransmits`), so one lost
   handshake datagram costs a second; the run-to-run spread of the `loss-20`
   audio join (0.35 s against 6.7 s) fits that.
5. **The path change itself is handled.** When `ana` arrives from a new
   source port at 12 s the SFU's ICE-lite agent follows Chromium's next
   nominated check to the new address: media stops for about 1 to 2 s and
   then flows at the full rate (down kbps back within 2 s). What stays broken
   afterwards is item 1, the 1% cellular loss.
6. **Delay is fine when nothing is lost.** Glass to glass, less the display,
   is about 50 to 70 ms on a clean link and about 190 to 200 ms on the
   100 ± 30 ms path (one way 100 ms plus about 90 ms of jitter buffer and
   pipeline). Capped links add the queue: 115 to 215 ms p50 for what still
   gets through, and the audio jitter buffer grows to 180 to 260 ms in
   `drop-recover`.

## After loss repair and rate control

The SFU was changed on 2026-10-21 to answer the findings above (the design
and why each part is safe are in [rtc.md](rtc.md), "Loss repair" and "Rate
control"):

- receivers' NACKs are answered by the SFU from a 1 s cache, as RTX to that
  receiver only, and the SFU NACKs senders itself for what it lost upstream;
- transport-wide congestion control both ways: senders get feedback and adapt
  with their own estimator, receivers' feedback drives a per-receiver estimate
  that caps each sender by REMB, and a receiver far below the floor loses
  video but keeps audio;
- receiver reports carry real report blocks, so Opus turns in-band FEC on;
- the DTLS flight retransmit starts at 300 ms instead of 1 s;
- keyframe requests for a newcomer wait until its DTLS handshake ends.

### Before and after

The machine was shared with other agents' builds (load average 16 to 41 on
36 CPUs, as against a quiet machine for the baseline above), so the two engines
were run alternately, one profile at a time, the old SFU (commit 4c309afda4)
right before the new one, 30 s each, same Chromium 151. Before → after; means
per stream as above.

| Profile | Join video ms | Join audio ms | Down fps | Down stalls | Down kbps | Down delay p50/p95 ms | Up fps | Up stalls | Up kbps | Down concealed % | ana available / sent kbps |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| clean | 330 → 819 | 360 → 848 | 24.0 → 24.0 | 0.0 / 0.0 s → 0.0 / 0.0 s | 1508 → 1391 | 62 / 103 → 66 / 112 | 24.0 → 23.9 | 0.0 / 0.0 s → 0.0 / 0.0 s | 1420 → 1311 | 0.0 → 0.0 | 1500 / 1428 → 1500 / 1317 |
| loss-5 | 1372 → 365 | 392 → 360 | 0.8 → 24.0 | 7.0 / 27.5 s → 0.0 / 0.0 s | 1426 → 1394 | 34 / 69 → 56 / 95 | 0.8 → 22.7 | 6.5 / 28.2 s → 0.0 / 0.0 s | 1343 → 756 | 4.1 → 4.2 | 1500 / 1423 → 942 / 797 |
| loss-10 | 2614 → 366 | 383 → 394 | 0.4 → 24.0 | 5.5 / 25.9 s → 0.0 / 0.0 s | 1344 → 1391 | 39 / 2125 → 55 / 104 | 0.4 → 23.9 | 7.0 / 27.4 s → 0.0 / 0.0 s | 1275 → 1168 | 9.5 → 8.6 | 1500 / 1428 → 1466 / 1288 |
| loss-20 | 2569 → 1421 | 1855 → 1426 | 0.1 → 21.3 | 2.0 / 24.2 s → 4.0 / 5.1 s | 1144 → 1336 | 64 / 133 → 70 / 290 | 0.1 → 17.5 | 2.0 / 28.0 s → 3.0 / 4.3 s | 1119 → 94 | 17.7 → 18.6 | 1500 / 1392 → 210 / 115 |
| burst | 2673 → 423 | 1757 → 428 | 1.1 → 24.0 | 7.0 / 26.0 s → 0.0 / 0.0 s | 1300 → 1388 | 98 / 718 → 52 / 98 | 1.4 → 24.1 | 5.0 / 26.1 s → 0.0 / 0.0 s | 1300 → 1070 | 8.9 → 5.6 | 1500 / 1350 → 1307 / 1184 |
| jitter-100 | 1655 → 1417 | 1189 → 1428 | 23.8 → 23.0 | 0.0 / 0.0 s → 1.0 / 0.9 s | 1444 → 1221 | 223 / 296 → 674 / 1195 | 23.7 → 22.4 | 0.0 / 0.0 s → 1.0 / 0.7 s | 1391 → 1178 | 0.4 → 1.6 | 1500 / 1388 → 1482 / 1170 |
| cap-1m | never → 1205 | 617 → 1098 | 0.0 → 17.6 | 1.0 / 30.1 s → 1.0 / 3.1 s | 453 → 332 | - → 624 / 830 | 1.8 → 20.8 | 1.0 / 26.1 s → 0.0 / 0.0 s | 937 → 692 | 2.3 → 3.5 | 1500 / 1374 → 891 / 690 |
| cap-300k | 2156 → 3467 | 2259 → 559 | 0.0 → 18.9 | 1.0 / 28.9 s → 1.5 / 1.4 s | 100 → 76 | - → 81 / 277 | 0.2 → 22.3 | 1.0 / 28.6 s → 0.0 / 0.0 s | 244 → 209 | 3.1 → 0.8 | 1500 / 1423 → 280 / 203 |
| drop-recover | 393 → 399 | 387 → 395 | 0.2 → 18.0 | 1.0 / 29.5 s → 1.0 / 5.6 s | 663 → 436 | 158 / 252 → 239 / 1249 | 13.3 → 19.1 | 1.0 / 12.6 s → 2.0 / 4.4 s | 996 → 563 | 2.5 → 2.1 | 1500 / 1427 → 739 / 585 |
| path-change | 1368 → 544 | 424 → 552 | 10.2 → 21.3 | 5.0 / 16.4 s → 1.0 / 3.5 s | 1406 → 1248 | 62 / 196 → 194 / 288 | 10.5 → 19.5 | 5.0 / 16.2 s → 2.0 / 5.4 s | 1322 → 1049 | 6.1 → 6.1 | 1500 / 1404 → 1384 / 1183 |

Three after figures are the machine, not the SFU: in that run the jitter-100
browsers used 137% of a core each against about 60% elsewhere. Run again
right after, the new SFU gave clean join 452 / 482 ms, jitter-100 23.9 fps
down with 197 / 253 ms delay, and cap-1m 21.1 fps down with 66 / 128 ms delay
and 380 kbps per stream. Earlier runs of the new SFU the same afternoon
agreed: video 24 fps at 5 and 10% loss and in bursts, 15 to 19 fps at
300 kbps, 20 to 24 fps at 1 Mbps.

### After, in detail

Audio:

| Profile | Down concealed % | Down concealment events | Down jitter buffer ms | Down packets lost | Up concealed % | Up concealment events | Up jitter buffer ms | Up packets lost |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| clean | 0.0 | 0 | 37 | 0 | 0.0 | 0 | 35 | 0 |
| loss-5 | 4.2 | 82 | 23 | 67 | 3.6 | 63 | 32 | 65 |
| loss-10 | 8.6 | 143 | 26 | 153 | 9.1 | 168 | 24 | 152 |
| loss-20 | 18.6 | 240 | 39 | 292 | 14.3 | 198 | 62 | 272 |
| burst | 5.6 | 86 | 26 | 87 | 6.2 | 70 | 28 | 116 |
| jitter-100 | 1.6 | 6 | 241 | 0 | 1.7 | 4 | 243 | 0 |
| cap-1m | 3.5 | 12 | 219 | 0 | 1.0 | 8 | 166 | 0 |
| cap-300k | 0.8 | 2 | 137 | 2 | 1.2 | 4 | 167 | 0 |
| drop-recover | 2.1 | 11 | 322 | 0 | 1.7 | 5 | 298 | 0 |
| path-change | 6.1 | 11 | 54 | 8 | 5.8 | 12 | 54 | 11 |

Loss repair and the sender:

| Profile | Down video loss % | Down NACK/PLI/FIR | Down frames dropped | Up video loss % | Up NACK/PLI/FIR | Available out kbps | Sent kbps | Sent height | Limited by | RTT ms | Relay up lost/overflow | Relay down lost/overflow |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| clean | 0.0 | 0 / 0 / 0 | 0 | 0.0 | 0 / 0 / 0 | 1500 | 1317 | 360 | none | 1 | 0.0% / 0.0% | 0.0% / 0.0% |
| loss-5 | 4.2 | 235 / 4 / 0 | 14 | 0.0 | 208 / 2 / 0 | 942 | 797 | 270 | bandwidth | 1 | 4.8% / 0.0% | 4.5% / 0.0% |
| loss-10 | 9.4 | 618 / 0 / 0 | 0 | 0.0 | 670 / 0 / 0 | 1466 | 1288 | 360 | none | 1 | 10.1% / 0.0% | 10.5% / 0.0% |
| loss-20 | 16.7 | 1566 / 1 / 0 | 76 | 0.2 | 468 / 1 / 0 | 210 | 115 | 180 | bandwidth | 2 | 19.6% / 0.0% | 20.1% / 0.0% |
| burst | 5.8 | 166 / 0 / 0 | 0 | 0.0 | 199 / 1 / 0 | 1307 | 1184 | 360 | none | 2 | 6.9% / 0.0% | 6.2% / 0.0% |
| jitter-100 | 0.0 | 0 / 1 / 0 | 0 | 0.0 | 0 / 0 / 0 | 1482 | 1170 | 360 | none | 240 | 0.0% / 0.0% | 0.0% / 0.0% |
| cap-1m | 2.3 | 58 / 1 / 0 | 51 | 0.0 | 0 / 0 / 0 | 891 | 690 | 360 | none | 35 | 0.0% / 0.0% | 0.0% / 1.3% |
| cap-300k | 12.3 | 46 / 14 / 0 | 4 | 0.0 | 0 / 0 / 0 | 280 | 203 | 270 | bandwidth | 66 | 0.0% / 0.0% | 0.0% / 3.5% |
| drop-recover | 3.6 | 44 / 1 / 0 | 12 | 2.1 | 152 / 5 / 0 | 739 | 585 | 360 | none | 138 | 0.0% / 2.2% | 0.0% / 2.0% |
| path-change | 6.1 | 58 / 2 / 0 | 36 | 5.9 | 234 / 1 / 0 | 1384 | 1183 | 360 | none | 91 | 0.6% / 0.0% | 0.6% / 0.0% |

Phases:

| Profile | Phase | Down fps | Down kbps | Up fps | Up kbps | Available out kbps | Sent kbps |
| --- | --- | --- | --- | --- | --- | --- | --- |
| drop-recover | 0-10 s | 24.8 | 797 | 22.3 | 1205 | 1500 | 1233 |
| drop-recover | 10-20 s | 4.1 | 77 | 10.1 | 201 | 415 | 244 |
| drop-recover | 20-30 s | 25.6 | 408 | 23.8 | 307 | 395 | 306 |
| drop-recover | recovery after the last change | 1 s | none | 2 s | none |  |  |
| path-change | 0-12 s | 24.1 | 1340 | 23.8 | 1230 | 1500 | 1251 |
| path-change | 12-30 s | 20.3 | 1159 | 17.2 | 948 | 1306 | 1127 |
| path-change | recovery after the last change | 5 s | 3 s | 7 s | 10 s |  |  |

### What changed, and what is still weak

1. **Loss.** Video is smooth at 5 and 10% random loss and in bursts (24 fps,
   no stalls, from under 2 fps), and usable at 20% (21 fps down). NACKs now
   flow (hundreds a run) and the stream's own loss stays at the link's: the
   SFU repairs downstream from its cache and upstream by asking the sender.
   Opus FEC is on (an audio stream under 10% loss received 108 FEC packets
   in one run; none before), yet concealment still tracks the loss rate
   closely; FEC at 32 kbps only covers single losses.
2. **Senders adapt.** `ana`'s available outgoing bitrate now follows its link
   (280 kbps at a 300 kbps cap, 890 kbps at 1 Mbps, 1.5 Mbps when clean), and
   its encoder drops resolution (`limited by bandwidth`). At 20% loss Chrome's
   own loss-based estimator takes `ana`'s uplink down to about 100 kbps, so
   up video is 17.5 fps at a low rate; that is the browser reacting to real
   loss now that it hears about it, and the price of being told the truth.
   At 5% it sometimes settles lower than it needs to (797 kbps sent here, 1.3
   Mbps in other runs).
3. **Per receiver.** Video flows at 1 Mbps (about 380 kbps a stream) and at
   300 kbps (about 75 kbps a stream, 180p to 270p) where nothing arrived
   before. Without simulcast the slowest receiver holds every sender it
   receives down to its share, so `ben` and `cai` see each other at that rate
   too (the `control` direction in `results.json`). Video to a receiver below
   80 kbps a stream pauses rather than dragging everyone to that floor.
4. **Join.** Video joins in about 0.4 s at 5 and 10% loss and in bursts
   (1.4 to 2.7 s before), and in 1.4 s at 20%. A newcomer joining a capped
   link still floods it for the first second (the estimate starts unknown so
   a clean join is full quality at once), which is the 1.2 s join at 1 Mbps
   and 2 to 4.5 s at 300 kbps.
5. **Sudden drops are still slow.** When the link falls from 2 Mbps to 300
   kbps in both directions at once (`drop-recover` 10-20 s), down video is
   about 4 fps for that phase: the queue built before the senders slow down
   takes a few seconds to drain, and `ana`'s own uplink is congested by its
   sender's backlog, which delays its feedback and NACKs. After the link
   returns, fps is back in 1 to 2 s but the rate is not (`none` in the
   recovery row): the SFU's estimate is back in about 3 s (probes double it
   each second), but Chrome raises its encoder slowly after a cap is lifted
   (its target was 600 kbps at 180p for several seconds in the traces).
6. **Delay.** On a clean link delay is unchanged (about 60 to 70 ms). With
   NACK on, Chrome adds the round trip to its video jitter buffer target so a
   retransmission can arrive in time, so a high-RTT path pays it (jitter-100 about 200 ms as before; capped links 70
   to 140 ms p50 in quiet runs, up to 600 ms when the machine was loaded,
   because delay-based control also reacts to CPU stalls).
7. **Clean costs a little rate.** Clean video is about 1.39 Mbps a stream
   instead of 1.51 (same 24 fps, 360p). Chrome's own estimator now decides
   within the 1.5 Mbps cap, and our reading is that it counts audio and
   packet overhead against it; not checked further.

## Known limits

- **Not a phone.** Chromium on a Linux desktop with fake devices: no real
  camera pipeline, no hardware codecs (`--disable-gpu`, VP8 in software), no
  battery or thermal figures, no radio. Phones will differ, most of all in
  CPU.
- **Fake media.** The canvas content is synthetic and always busy, so the
  encoder runs at whatever bitrate it is allowed; a quiet talking head would
  need less. The fake microphone is a tone with silences, so concealment is
  measured on easy audio and nothing judges how concealment sounds.
- **One machine.** All browsers, the server and the relay share one host and
  clock. That makes the delay stamp exact, but browser CPU load shows up in the
  delay figures, and under load the 1 s stats samples are not exactly 1 s
  apart.
- **Only media is impaired.** Signaling, the page and the end-to-end key
  exchange are unimpaired loopback, so join times leave out the signaling
  round trips a real network adds.
- **Only one client is impaired.** A call where everybody is on a bad network
  compounds; the profiles say what one person on a bad link sees and causes.
- **Resolution of time.** Phases and recovery come from 1 s samples, so
  recovery reads in whole seconds. The presented-frame trace behind stalls
  and the delay stamp runs at the headless compositor's pace (15 to 20
  presented frames a second while 24 are decoded on the clean profile), which
  is why stalls only count gaps over 500 ms.
- **UDP only.** The relay shapes UDP; there is no TURN, TCP or TLS path to
  measure.
- **Run to run.** Loss draws are seeded, but timing is not, so numbers move a
  little between runs, most in the loss profiles where one keyframe more or
  less matters. Compare engines on the same machine in the same session.
