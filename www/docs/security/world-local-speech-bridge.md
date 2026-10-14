# World-to-desktop speech bridge

The desktop client can expose its local speech-to-text engine to a World page
without sending microphone audio anywhere. The browser is only a controller and
transcript recipient; capture and transcription stay in the desktop client. The
World currently ships no speech controls, and the desktop bridge is dormant
infrastructure.

## Pairing

1. The user enables voice control for one exact World origin in the desktop
   client.
2. The client listens on an ephemeral `127.0.0.1` port and shows a short-lived
   pairing capability, storing only its SHA-256 digest.
3. The page sends the capability once as `Authorization: Bearer …` to
   `POST /v1/pair`, which consumes it and returns an origin-bound ten-minute
   session capability.

Capabilities never appear in URLs, cookies, server requests, analytics or
persistent browser storage. Revoking, closing the panel or quitting the client
invalidates them.

## Capture protocol

- `POST /v1/transcription/start`, stop and cancel each carry a single-use
  `X-ForkMesh-Request-Id`; replays are rejected.
- `GET /v1/transcription` returns status and transcript text.
- Text is inserted into a World composer only after the user confirms the
  destination (`chat`, `issue` or `note`).

There is no audio upload or download endpoint, and responses report
`audioRelayed: false`. Bodies are limited to 8 KiB and headers to 16 KiB, query
strings and transfer encodings are rejected, responses are `no-store`, and
transcript and error text is bounded and stripped of control characters. One
capture runs at a time.

## Origin boundary

The origin is matched exactly (no suffix, wildcard or `null` matching). CORS
echoes only that origin and never enables credentials. Both the listener and
each peer must be loopback. The bridge is never proxied by a server.

## Auditing

Audit events (issued, paired, started, stopped, cancelled, completed, failed,
revoked) are written without capability values, transcript contents, audio,
device names or browsing history.
