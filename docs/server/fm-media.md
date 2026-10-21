# FM Media: our own call engine

Decided 2026-10-21: calls move off libwebrtc to an engine we write ourselves,
and it has to measure better than WebRTC before it becomes the default. This
page is the plan and the record of decisions; [rtc.md](rtc.md) describes the
WebRTC stack that serves calls until then, and [call-bench.md](call-bench.md)
the bench that decides "better".

## What we write and what we take from the platform

We write the transport, the media protocol, congestion control, loss
recovery, jitter buffers, end-to-end encryption, the SFU and every client.
We take from the device only what is hardware or the operating system's own
signal processing, the same split FaceTime uses:

| Piece | Phones | Browser |
| --- | --- | --- |
| Echo cancellation, noise suppression, gain | iOS voice-processing audio unit; Android `VOICE_COMMUNICATION` with the platform AEC | `getUserMedia` constraints |
| Video encode and decode | VideoToolbox (H.264, HEVC); MediaCodec | WebCodecs |
| Audio codec | Opus, built from source by us and pinned (like libwebrtc was) | WebCodecs Opus |
| Face tracking | ARKit on TrueDepth phones, else Vision; our own networks on Android | our own networks |

Everything else is ours.

## Layers

1. **Transport: QUIC v1** (RFC 9000/9001/9002) with DATAGRAM (RFC 9221),
   our own implementation in Go on `crypto/tls`'s QUIC API
   ([quic.md](quic.md)). Browsers reach it through WebTransport over HTTP/3.
   It gives a one round-trip join, connection migration (a call survives
   Wi-Fi to cellular without reconnecting) and no ICE: every client talks to
   an SFU with a public address.
2. **Media protocol.** Encoded frames, already end-to-end encrypted in the
   frame format of [rtc.md](rtc.md#frame-encryption), are split into
   datagrams with a per-stream sequence number, frame id, fragment index and
   send timestamp. Control (join, tracks, subscriptions, keyframe requests,
   receiver reports) runs on one QUIC stream. Forward error correction is
   ours (XOR parity first, Reed-Solomon if the bench asks for it), sized from
   measured loss.
3. **Congestion control.** Receivers report arrival times and losses;
   senders run a delay-gradient plus loss estimator and set the encoder
   bitrate and frame rate. The SFU adapts each receiver separately by
   choosing layers (temporal layers first, simulcast later) instead of
   asking the sender to slow down for everyone.
4. **Jitter buffers.** An adaptive audio buffer with time stretching and
   Opus concealment and in-band FEC; a video frame buffer that releases
   whole decodable frames and asks for a keyframe only when it must.
5. **SFU.** The rooms, tickets, account caps and failover of the current SFU,
   over QUIC. During the move it bridges to WebRTC participants in the same
   room, so old and new clients can share a call.

## One engine core for every client

The engine core (layers 2 to 4 plus the client half of 1) is written once in
Go and compiled into the phone apps with `gomobile bind`. The same code is
the Go test client the bench drives, so what the bench measures is what the
phones run. Capture, codecs, audio I/O and rendering stay native (Swift,
Kotlin) and exchange whole frames with the core. The browser client is a
JavaScript port held to the Go core by shared fixtures, the way the face and
voice code is.

## Beyond FaceTime

Decided 2026-10-21: matching WebRTC is not the goal; the engine has to be
first in class. FaceTime already uses its own stack, the device's voice
processing, hardware codecs and long-term reference frames, so beating it
takes ideas it does not use. Five of them, each measurable on the bench:

1. **The face keeps moving when the video cannot.** Below the bitrate where
   real video holds (about 150 kbps), the sender switches its camera track to
   a *face stream*: 3D pose, the 68 landmarks and expression weights, about
   2 kB/s, plus a real reference picture of the sender refreshed whenever
   the link allows. The receiver animates the reference with the face
   changer's warp renderer, which already exists in the browser and on
   iPhone. A call on a bad cellular link shows a live, talking face instead
   of a frozen frame, at a twentieth of the bitrate. It switches back as soon
   as real video fits, and the receiver is told which it is seeing. Format,
   codec, renderers and measurements: [../app/face-stream.md](../app/face-stream.md).
2. **Audio that survives a third of its packets lost.** Opus 1.5 built with
   deep redundancy (DRED: up to a second of history in every packet, a few
   kbps), deep concealment and decoder enhancement. The jitter buffer
   recovers a lost frame from the redundancy carried by the next packet that
   arrives.
3. **No keyframe storms.** Video recovers by long-term reference: receivers
   acknowledge decoded frames, and after a loss the encoder predicts from the
   newest frame every receiver holds instead of sending a keyframe to all.
   Temporal layers let the SFU thin video per receiver without asking the
   sender.
4. **No dropout on a network change, no dropout when a server dies.** QUIC
   multipath keeps Wi-Fi and cellular open together and sends audio over both
   while either is weak; each client also keeps a warm standby session on a
   second core, so a core failure moves the call in well under a second
   instead of a rejoin.
5. **Post-quantum, group-verified encryption.** Media keys come from a hybrid
   X25519 + ML-KEM-768 exchange per epoch, with a new epoch on every join and
   leave and a short verification code every participant can compare. The
   servers carry only ciphertext and headers.

Targets, measured on the bench (and by hand against FaceTime on two iPhones
with the Network Link Conditioner, since FaceTime cannot be driven by a
test):

| Measure | Target |
| --- | --- |
| Join to first audio | under 300 ms on a clean regional path |
| Mouth-to-ear delay | under 150 ms on a clean regional path |
| Audio at 30% random loss | intelligible, under 3% audible concealment |
| Video at 300 kbps | continuous real video at reduced resolution |
| Below 100 kbps | a live face stream, no frozen frames |
| Wi-Fi to cellular | no audible gap |
| A core dies mid-call | audio back in under 500 ms |
| Engine size on a phone | under 5 MB |

## Phases

| Phase | Done when |
| --- | --- |
| 1. Bench | baseline numbers for the WebRTC path on every network profile |
| 2. QUIC and WebTransport | headless Chromium holds a WebTransport session with our server; migration and loss tests pass |
| 3. Engine core | Go clients hold a call through the bench's impaired networks |
| 4. SFU over QUIC, bridged to WebRTC | mixed rooms work; failover still moves a call |
| 5. iPhone engine | the iPhone app calls on FM Media; voice changer, backgrounds and faces still apply |
| 6. Browser engine | WebTransport + WebCodecs client; browsers without WebTransport stay on WebRTC |
| 7. Android engine | same as iPhone |
| 8. Switch | FM Media is the default wherever it beats the baseline on the bench |
| 9. Beyond FaceTime | every target in the table above is met on the bench |

Phases 3 to 7 build the five ideas in from the start rather than after the
switch: the face stream and long-term reference recovery in the core,
DRED in the Opus build and the audio buffer, multipath and the standby core
in the QUIC layer, and the hybrid key exchange in the core every client
shares.

Status lives in the commits of each phase; this table changes only when the
plan does.

The engine core's wire format, algorithms and measurements are in
[fmm-protocol.md](fmm-protocol.md).
