# FM Media in the browser (phase 6)

Phase 6 of [FM Media](../server/fm-media.md): the browser's client for the
call server of [fmm-server.md](../server/fmm-server.md). The engine core
(datagrams, fragmentation, FEC, congestion control and pacing, jitter
buffers, the session layer) is a JavaScript port of the Go core in
`server/rtc/fmm`, held to it by shared fixtures. Media goes through
WebTransport, WebCodecs and an AudioWorklet, encrypted with call e2ee v2
([call-e2ee.md](../server/call-e2ee.md)). Built 2026-10-21. Browsers without
WebTransport, WebCodecs or a call signer stay on the WebRTC path of
[chat-group-calls.md](chat-group-calls.md), unchanged.

## Where the code is

| Piece | Code |
| --- | --- |
| Engine core, one module per Go package | `app/public/fmm/{seqnum,wire,frag,fec,cc,hop,jitter,client}.js` |
| Frame sealing (AES-256-GCM frames, media signer and verifier) | `app/public/fmm/fmm-seal.js` |
| Worker: WebTransport, the `Client`, codecs, sealing, standby | `app/public/fmm/fmm-worker.js` |
| Playout | `app/public/fmm/fmm-playout-worklet.js` |
| Page side: capture streams, playout, tiles, face stream | `app/public/fmm/fmm-call.js` |
| Call e2ee v2 integration (presence, keys, verification round) | `app/public/fmm/fmm-e2ee.js` |
| Looks and voice for the outgoing media | `app/public/fmm/fmm-local.js` |
| Wiring | `app/public/chat-group-calls.js` (engine choice, frames), `chat.js` (signer, call id, UI), `chat-seal.js` (`signCall`, sealed sender on call frames), `chat.html`, `chat.css` |
| Parity fixture generators (Go, tests only) | `server/rtc/fmm/{wire,frag,fec,cc,hop,jitter}/jsparity_test.go`, `server/rtc/fmm/jsparity_test.go` |
| Parity and unit tests | `app/tests/fmm_*.test.mjs`, `app/tests/test_fmm_parity.py` |
| Browser end-to-end tests | `server/rtc/fmmserver/browser_fmm_test.go` |
| Bench engine and adapter | `server/rtc/bench/fmm.go`, `server/rtc/bench/web/adapters/fmm-media.js` |

## Choosing the engine

`createGroupCalls` takes four new options: `callSigner` (resolves to
`{idk, sign}` for the chat v3 identity), `callAccount`, `callId` (the chat
room name, which is the call id of call e2ee) and `prepareLocal` (looks and
voice). It uses FM Media when all of these hold:

1. the browser has `WebTransport`, `VideoEncoder`/`VideoDecoder`,
   `AudioEncoder`/`AudioDecoder`, `MediaStreamTrackProcessor`,
   `MediaStreamTrackGenerator`, `AudioWorkletNode` and module workers
   (`fmmSupported`);
2. `callSigner` gives a signer (`chat.js` builds it from the seal store's new
   `signCall`, which signs only `forkmesh-call-e2ee-` messages, as the e2ee
   design asks);
3. the ticket response, requested with `{"engines": ["fmm", "webrtc"]}`, has
   an `fmm` block with a URL, a ticket and certificate hashes, and a peer id.

Otherwise it joins over WebRTC exactly as before. A core that answers
Error 409 (`engine webrtc`: the room already runs on WebRTC) makes the client
leave FM Media and join with the WebRTC ticket of the same response, with no
second request; later reconnects of that call stay on WebRTC. Error 503 ends
the join ("This call is full.").

## Threads

- **Page.** `getUserMedia` (echo cancellation, noise suppression and gain
  control on), the look and voice stages, one `MediaStreamTrackProcessor` per
  outgoing track, the `AudioContext` with the playout worklet, one
  `MediaStreamTrackGenerator` per remote video, the face-stream views, and the
  call e2ee session (it needs the identity key, which stays in the seal
  store).
- **Worker** (`fmm-worker.js`, a module worker). The WebTransport sessions,
  the `Client` ticked every 5 ms, the Opus and video encoders and decoders,
  sealing and opening frames, the auth track. The readable streams of the
  track processors are transferred to it. Chromium 151 exposes
  `MediaStreamTrackProcessor` only on the page, cannot transfer a
  `MediaStreamTrack` to a worker, and a generator's writable transferred to a
  worker never completes a write; transferring the processors' readables
  works, and decoded `VideoFrame`s are posted back (transferred) for the
  page to write into the generators.
- **Worklet.** Mixes one queue of decoded PCM per remote speaker. It
  receives the PCM straight from the worker over a `MessagePort` handed to
  it through the node's port, so audio never waits on the page, and reports
  its underruns and trims once a second.

## Transport

- `new WebTransport(url, {serverCertificateHashes, congestionControl:
  "low-latency", requireUnreliable: true})`, hashes from the ticket response.
  One bidirectional stream carries the control messages, session datagrams
  carry media and feedback. Chromium's own QUIC congestion control still
  governs datagrams on the browser's uplink; `low-latency` asks for its
  delay-based controller instead of the loss-based one, the browser-side
  counterpart of the cores' `DatagramLossExempt`
  ([fmm-server.md](../server/fmm-server.md#datagrams-and-quics-own-congestion-control)).
  Stale datagrams are dropped after 400 ms in Chromium's queue
  (`outgoingMaxAge`).
- The engine is told `min(maxDatagramSize - 4, 1152)`: four bytes of room as
  `fmmcall.Session` leaves, and never more than a native subscriber at the
  default 1,200-byte packets can take, since the SFU forwards datagrams
  unchanged in size.
- **Standby.** With a `standby` block the worker opens a second session at
  once, writes `Join` with the standby ticket, and sends a 4-byte padding
  datagram every 5 s so neither QUIC side idles it out (the server reads
  nothing from a parked connection but its next control message). The
  primary counts as dead when its session closes (crash, restart, stop: the
  core sends CONNECTION_CLOSE), or when nothing has arrived for
  max(400 ms, 3 × RTT + 200 ms), capped at 1 s, while this client has been
  sending. Silence does not count while the worker itself was stalled
  (a gap of more than 40 ms between its own ticks), and only from the moment
  it started sending again: both caused false moves on a loaded machine.
  Then it starts a new `Client` on the standby's stream, sends the same
  `Join` (the activation), publishes again and subscribes from the
  `TrackAdded` that follow. Keys, the e2ee session and the peer id stay. A
  call without a standby treats only a closed session, or 10 s of silence,
  as lost and rejoins through the app.

## The engine core port

Each Go package is one module with the same names (types and constants as in
Go, functions, methods and fields in lowerCamelCase), so the modules could be
written in parallel and each replays its own Go fixture. Rules that the
ports had to follow to stay byte- and decision-identical:

- Integer arithmetic wraps exactly where Go's does (`& 0xffff`, `>>> 0`),
  and integer division truncates (`Math.trunc`).
- **Constant folding.** Go evaluates untyped constant expressions exactly,
  so `1 - 0.9` is 0.1 in Go and 0.09999999999999998 in JS; that drifted the
  trendline's threshold. Every folded constant is written out as its value.
- **`math.Pow`.** V8's `Math.pow(0.998, 49)` is one ulp from Go's and flipped
  one FEC choice. `fec.js` carries `goPow`, a port of Go's pure-Go `Pow`
  (exact for integer exponents; Go uses it on amd64 and arm64). The
  controller's `Pow(1.08, dt)` with fractional `dt` matched Go on every
  recorded trace with `Math.pow`.
- Go maps iterated in random order where the result is order dependent:
  `trackBudget` sums float rates over the publications; the JS sums in
  ascending stream order. Over four Go runs the Go order never changed a
  result.
- No locks (one thread). Methods that return an error in Go throw an
  `FmmError` with the Go message; `(T, bool)` returns `T` or `null`; two
  results return an array.
- JS strings cannot hold invalid UTF-8, so a ticket or name with invalid
  UTF-8 decodes with U+FFFD and does not re-encode to the same bytes; the
  wire fixture marks those cases.

Fixtures (`FMM_JS_PARITY_WRITE=1` regenerates; without it the Go test fails
when the stored file is stale, and every generator builds its fixture twice
to prove it is deterministic):

| Package | What the fixture holds |
| --- | --- |
| wire, seqnum | every datagram kind and control message (Publish tails, TrackAuth, Reports with negative and escaped deltas, Nack, Ack, Recovery), encoder refusals with Go's error text, about 1,100 decode cases on mutated and random bytes, control-reader chunking, `DedupKey`, sequence unwrapping |
| frag | packetizer runs (L1T2, L1T3, recovery `Ref`, long-term, face, audio `Aux`, wraps over 70,000 frames) and reassembler traces (shuffled, dropped, duplicated, disagreeing fragments, eviction, expiry) |
| fec | Reed-Solomon encode up to 48+16, 108 reconstructions, 1,200 `Residual` values, 1,470 `Choose` and 175 `ChooseVideo` cases, encoder and decoder traces |
| cc | recorder reports byte for byte; eight controller traces recorded on `fmm/link` (cap steps, 5% and 30% loss, a standing queue, feedback silence, an application-limited flow, a drop to 100 kbps, bursts with jitter): every input and 20 outputs after each report; pacer traces |
| jitter | 17 audio traces (the jitter profiles, DRED at 10 to 30% and bursts) and 11 video traces (L1T3 recovery, bursts with jitter, escalation, face frames) |
| hop | a sender and receiver over `fmm/link` in four scenarios (20% loss, DRED and NACK, a 300 kbps cap, bursts with reordering): every input, a hash of every output datagram, and sender state every 40 ms |
| client | real Go clients through `fmm/sfu`; every call on one client (join, publish, send, subscribe, every datagram and control chunk in, ticks, pulls, polls, events) with its result and transport output, in five scenarios (three clients with auth tracks, 10% loss, a 1 Mbps to 100 kbps drop with the face switch and reference pictures, three-encoding simulcast with a 250 kbps subscriber, feedback on the control stream with recovery, unsubscribe and leave) |

## Video

- **Codec.** H.264 constrained baseline (`avc1.42E01F`, Annex B) when
  `VideoEncoder` and `VideoDecoder` support it with `L1T3`, else VP8. The
  track is published as `video/h264` or `video/vp8`; receivers pick the
  decoder from the name. FM Media H.264 frames are Annex B for every client.
- **Temporal layers.** `scalabilityMode: "L1T3"`; the layer of each chunk is
  `metadata.svc.temporalLayerId`, keyframes are layer 0. That is the L1T3
  structure `frag` assumes.
- **Rate.** The encoder follows `VideoBitrate` of its stream, reconfigured
  when it moves by more than 8% (at most every 250 ms). The resolution steps
  with the rate (640 wide from 450 kbps, 480 from 250 kbps, else 320, keeping
  the camera's aspect ratio); a step costs a keyframe. A rate of 0 stops the
  encoder for that stream.
- **Simulcast.** `fmm.simulcast: 2` publishes two encodings (320 and 640
  wide), each following its own `VideoBitrate` (the core pauses one with a
  0 hint, which also stops its encoder). Off by default: the bench measures
  one encoding, like the WebRTC path.
- **Long-term references: not available, so keyframes.** WebCodecs has no
  reference control. Chromium 151 accepts `scalabilityMode: "manual"` in
  `isConfigSupported` but has no `getAllFrameBuffers` unless started with
  `--enable-blink-features=WebCodecsVideoEncoderBuffers`, and even then
  returned no buffers for VP8 (probe, 2026-10-21). The browser therefore
  never marks frames `LongTerm`, so the `Client` turns recovery requests into
  `EventKeyframeRequest`, and an `EventRecover` (which cannot happen without
  long-term frames) is answered with a keyframe too. Receivers on the browser
  still acknowledge and recover frames from phones that do send long-term
  frames.
- **Receive.** `VideoDecoder` with `optimizeForLatency`. A frame that fails
  to open (no key yet, a forged frame, a stream the e2ee layer is dropping)
  or a decoder error makes the receiver drop deltas until a keyframe and send
  a `KeyframeRequest` for that downstream stream itself (at most every
  500 ms), because the jitter buffer cannot see losses after it. Decoded
  frames go to a `MediaStreamTrackGenerator`, at most two writes in flight
  per tile (older frames are dropped, not queued).

## Audio

- **Capture.** Mono at the device rate (48 kHz with the fake device) from
  `getUserMedia({audio: {echoCancellation: true, noiseSuppression: true,
  autoGainControl: true}})`, through the voice changer when one is chosen.
- **Encode.** `AudioEncoder` Opus, 20 ms frames, `complexity` 9,
  `useinbandfec`, `packetlossperc` from the uplink loss (reconfigured at most
  every 2 s, also when `AudioBitrate` moves by 4 kbps or more), `signal:
  voice`, `application: voip`, no DTX (a silent stream would look like a dead
  core to the receiver's failover check).
- **No DRED.** WebCodecs exposes no DRED encoder or decoder option, so the
  browser publishes redundancy depth 0 (`SetAudioRedundancy(stream, 0)`) and
  the hop keeps ordinary audio FEC for it.
- **Decode and concealment.** The jitter buffer's instructions are followed
  as far as WebCodecs allows:

  | Status | Browser action |
  | --- | --- |
  | `AudioPlay` | open the frame, decode it |
  | `AudioConceal` (with or without the next frame's bytes) | concealment: decode a 1-byte Opus packet holding only the last TOC byte with code 0, which libopus treats as a lost frame and conceals |
  | `AudioDRED` | the same concealment (no DRED decoder) |
  | `AudioInsert` | one concealment frame |
  | `AudioWaiting` | nothing (silence) |

  WebCodecs offers no `decode_fec`, so in-band FEC (LBRR) from the next
  packet cannot be used; only Opus's own concealment runs. A 0-byte chunk is
  refused ("Null or empty decoder buffer") and closes the decoder; the TOC-only
  packet decodes to 960 samples of concealment whose energy decays like
  libopus PLC (probe: 0.17, 0.08, then 0.02 RMS over a 440 Hz tone). A
  WebAssembly libopus would bring FEC and DRED decoding; not built.
- **Playout clock.** `PullAudio` is called from the worker's own schedule:
  one pull per 20 ms of `performance.now()` since the first one, with `now`
  set to that frame's slot (so it is never early and always 20 ms apart),
  catching up after a stall and skipping the backlog when it passes 200 ms.
  The worklet's queue absorbs the difference between that clock and the
  sound card's. The first design pulled once per 960 samples the worklet
  played, the device's clock as the Go API describes. Headless Chromium's
  fake output on a loaded machine (load average 30 to 65 from other jobs)
  delivers those ticks in bursts, early as well as up to 290 ms late, and
  the jitter buffer then judged most frames late: one bench browser
  concealed every frame of a clean call, another 561 of 626 after a
  failover. Two smoothed variants of the device clock did not hold either.
  On the worker's clock the same machine at load 65 conceals 0 to 31 of
  about 660 frames per stream (`TestThreeBrowsersHoldAnFMMediaCall`). The
  cost: a sound card whose clock drifts from the system's shows up as a
  worklet underrun or trim (counted in the stats as `playoutUnderruns` and
  `playoutTrimmed`) instead of as the jitter buffer's own insert or drop.
- **Worklet buffering.** Each remote stream starts playing once 30 ms are
  queued, restarts that wait after an underrun, and drops the oldest audio
  beyond 200 ms. That adds 20 to 30 ms to the jitter buffer's own delay.

## Echo cancellation in the browser

The known risk of [fm-media.md](../server/fm-media.md): Chromium's echo
canceller must take the call audio that the worklet plays as its reference,
or the far end hears itself.

What was measured (headless Chromium 151, fake devices, 2026-10-21):

- `getCapabilities().echoCancellation` is `[true, false, "remote-only"]`;
  `"all"` is refused (OverconstrainedError), also with
  `--enable-features=ChromeWideEchoCancellation`.
- The capture track reports `echoCancellation: true` with the constraints
  above.
- Echo itself cannot be measured headless: the fake microphone is a
  generated tone and the fake output goes nowhere, so there is no acoustic
  path to cancel. The headless tests therefore cannot show whether
  worklet audio is cancelled.

What follows from it:

- `"remote-only"` exists to *narrow* the reference to WebRTC peer audio. That
  only makes sense because the default (`true`) on desktop Chromium is the
  Chrome-wide canceller, which takes everything Chromium plays on that output
  device as its reference, Web Audio included. Under `true`, audio played
  from the worklet is in the reference; under `"remote-only"` it would not
  be. FM Media must therefore never ask for `"remote-only"`, and it asks for
  `true`. `"all"` (everything the system plays) is not offered on Linux.
- Not resolved: a build or platform where Chromium's canceller is still
  renderer-side (only WebRTC playout is the reference) would leave worklet
  playout uncancelled, and so would playout routed to a different output
  device than the one the canceller listens to.
- Mitigations in the code: `playout: "element"` routes the worklet through a
  `MediaStreamAudioDestinationNode` into an `<audio>` element instead of
  `context.destination`. That only helps where the canceller references media
  elements but not Web Audio, which no current desktop Chromium does, so
  it is off by default. The sure fallback, if a device shows echo, is a
  loopback `RTCPeerConnection` pair carrying the mixed playout, so that it
  becomes WebRTC peer audio. Not built.
- Next step: measure by hand on a laptop with speakers (one browser on FM
  Media, the other end on headphones) and with a PipeWire null sink and
  loopback on the Linux box.

## Encryption (call e2ee v2)

- **Frames.** Every Opus frame, video frame, face-stream frame and reference
  picture is sealed in the worker before `SendAudio`/`SendVideoFrame`: the
  layout of `e2ee.FrameSender` (clear bytes, AES-256-GCM ciphertext and tag,
  an 8-byte counter, the key id), one counter per sender shared by all its
  streams. Clear bytes on FM Media: 1 for Opus (the TOC), 0 for every video
  and face frame (the SFU never reads inside frames, and the receiver must
  know the count before it can open a frame, whatever the codec). `fmm-seal.js`
  produces exactly Go's `FrameSender` bytes and auth message on the shared
  vectors (`app/tests/fmm_seal.test.mjs`).
- **Media authentication.** Every sealed frame's digest goes into the
  sender's `createMediaSigner`; every 200 ms its auth messages go out on the
  publisher's auth track (`PublishAuth`, kind 3, which the iPhone engine work
  added to the core). Receivers subscribe to every auth track, pass its
  frames (`PollAuth`) to `createMediaVerifier().accept`, and open media with
  `verifier.frame` after decrypting; a frame it rejects is treated as lost.
  Verifier states reach the tiles: amber "Can't confirm this is Bob right
  now", red "Bob's video failed a check. Someone in this call may be faking
  it." (the texts of call-e2ee.md).
- **Session** (`fmm-e2ee.js`, the integration steps of call-e2ee.md). The
  call id is the chat room name. `group-call-presence` frames carry the e2ee
  presence (heartbeat every 20 s, rotation on joins, leaves, every 10 minutes
  and on `recipient-epoch`, at most once per 10 s). `group-call-key` frames
  carry `{peer, e2ee: keyMessage}`: rekeys are debounced by 250 ms and the
  own key is installed 1 s after its message went out; keys received go to the
  worker. `group-call-verify` frames carry commits and reveals, re-sent every
  2 s while someone is missing. Every e2ee frame must come with the sealed
  chat frame's sender identity: `authenticatedPlain` in `chat-seal.js` now
  stamps `sealKid` and `sealAcct` on `group-call-*` frames, overwriting
  whatever the sender put there, and the integration drops a presence, key
  message or commit whose `kid` (and account) is not the sealer's.
  `group-call-key` frames without `e2ee` are ignored on FM Media calls.
- **Verification UI.** The panel shows the four emoji with their names,
  "Or compare numbers: 123 456", "They match" (`markVerified`) and "They don't
  match", plus "Waiting for Bob and Carol to confirm…", "Erin is not
  verified…" and "The verification code changed because Erin joined". The QR
  form ("Scan a code instead") is not built in the browser.
- **The cost of joining.** A sender's frames are readable by a newcomer only
  after the sender has heard the newcomer's presence, rekeyed (250 ms
  debounce), sent the key message and switched to the new key 1 s later. A
  joiner therefore sees and hears nobody for about 1.3 s, which is most of
  FM Media's join time on the bench (the WebRTC path shares raw keys at once).

## Face stream

When the core switches the camera track to `RepFace` (`EventSendRepresentation`),
the worker stops encoding video on that stream and the page starts a 30 fps
feed: the face stage's live landmarks and expression reading go through
`createFaceStreamSender().frame` (chat-call-facestream.js) and are sent as
face frames; `EventReferencePicture` asks the page for
`sender.reference(...)` from the stage's canvas, sent as a face keyframe when
it fits `MaxBytes`; a keyframe request on a face stream calls `requestKey()`.
Receivers feed face frames and pictures to `createFaceStreamView`, whose
canvas (`captureStream`) replaces the tile's video track while the stream
shows the face, and the tile shows the "Face stream" badge
(`EventRepresentation`).

The landmarks exist only while a face stage runs, which is when a look or a
background is on (`fmm-local.js`). Without one the client keeps sending
video instead of switching (`FaceDownBitrate: 1`), since a face stream
without landmarks would show nothing. The headless tests have no face in
the fake camera, so the face path is covered by the core's fixtures and the
face-stream module's own tests, not end to end.

## Looks and voice

`fmm-local.js` (`createFmmLocal`, passed by `chat.js` as `prepareLocal`)
applies the saved voice (`createVoiceStage`) and the saved face or background
(`createFaceStage`) to the outgoing tracks, the choices the one-to-one call
UI stores. The self tile shows the processed stream. The WebRTC group-call
path still sends the raw camera and microphone.

## Tests

```
node --test app/tests/fmm_*.test.mjs
python3 -m pytest -q app/tests/test_fmm_parity.py
cd server && GOTOOLCHAIN=local go test -run JSParity ./rtc/fmm/...
cd server && GOTOOLCHAIN=local go test -run 'TestThreeBrowsersHoldAnFMMediaCall|TestBrowserFMMediaCallMovesToTheStandbyCore' -v ./rtc/fmmserver/
```

- `fmm_{wire,frag,fec,cc,hop,jitter,client}.test.mjs`: the parity replays
  above.
- `fmm_seal.test.mjs`: Go's frame and auth vectors, replays, a forged frame,
  counters across `setKey`, clear bytes.
- `fmm_e2ee.test.mjs`: three integrations over an in-memory chat relay
  exchange keys (each holds the others' current send key and auth key),
  agree on one code, confirm it, and rekey when one leaves; a presence whose
  identity is not the sealer's is ignored.
- `test_fmm_parity.py`: runs every suite, checks each ported package has its
  module, fixture and suite, and that the modules hold no comments.
- `TestThreeBrowsersHoldAnFMMediaCall`: three headless Chromiums with fake
  devices join one in-process core over WebTransport with e2ee v2 and
  publish audio and H.264 video. Over 20 s each must decode at least 100
  video frames and play at least 150 audio frames from both others, see both
  as `verified`, and all three must show the same code. Typical run: 290 to
  345 frames decoded per stream at 360p, 0 to 3 of 850 audio frames
  concealed, about 60 frames per pair lost at the start while keys arrive.
- `TestBrowserFMMediaCallMovesToTheStandbyCore`: the same with a standby on
  the second rendezvous core; the primary is stopped 8 s in. All three must
  move, the call must end on the standby, and media must continue. Three runs
  on 2026-10-21 (load 15 to 30): the death was seen 0 to 5 ms after the stop
  (the core closes its sessions), audio from every speaker was back 72 to
  125 ms after it, and no frame was concealed after the move.

## Not done yet

- DRED and in-band FEC decoding (WebCodecs has neither; a WebAssembly libopus
  would).
- Long-term reference recovery from the browser's encoder (no reference
  control in WebCodecs).
- An acoustic echo measurement (above).
- The QR verification code and remembering verified identities across calls.
- A silent core death (no CONNECTION_CLOSE) in the browser test; the
  detection rule is there, but only the closed case is tested end to end.
- Multipath: Chromium does not negotiate it, so browsers stay single-path.
