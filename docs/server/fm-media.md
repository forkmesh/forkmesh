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

Status lives in the commits of each phase; this table changes only when the
plan does.

The engine core's wire format, algorithms and measurements are in
[fmm-protocol.md](fmm-protocol.md).
