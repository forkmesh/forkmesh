# World-to-desktop speech bridge

ForkMesh World can use the Qt client’s locally installed Whisper or Parakeet
engine without sending microphone audio to the Worker, a relay, another mirror,
or the browser. The browser is a controller and transcript recipient only. Audio
capture and speech-to-text remain inside the desktop client.

## Pairing

1. The user opens the World from the Qt client and explicitly enables voice
   control for that exact World origin.
2. Qt binds an ephemeral port on `127.0.0.1` and displays a short-lived pairing
   capability. Qt stores only its SHA-256 digest.
3. The user pastes the capability into the World voice panel. The World sends it
   once in an `Authorization: Bearer …` header to `POST /v1/pair`.
4. The one-use pairing capability is consumed before Qt returns an
   origin-bound, ten-minute browser-session capability in the JSON response.

Neither capability is placed in a URL, query string, fragment, referrer, cookie,
Worker request, analytics event, or browser storage that survives the tab.
Closing the panel or choosing **Revoke** invalidates the browser session. Closing
Qt stops the loopback listener and removes all capabilities.

## Capture protocol

The World asks the user to choose `chat`, `issue`, or `note` before enabling the
microphone control. Each start, stop, cancel, or revoke request carries a unique
`X-ForkMesh-Request-Id`; replayed mutation IDs are rejected. The Qt client:

- starts its normal local recorder only after `POST /v1/transcription/start`;
- changes the state to `transcribing` after an explicit stop;
- cancels and deletes the temporary recording after an explicit cancel;
- exposes status and transcript text through authenticated
  `GET /v1/transcription`; and
- inserts text into the chosen World composer only after the browser receives a
  completed transcript and the user confirms that destination.

The protocol intentionally has no audio upload or download endpoint and reports
`audioRelayed: false`. Request bodies are limited to 8 KiB, transfer encoding
and query strings are rejected, mutation IDs are single use, responses are
`no-store`, and transcript/error text is bounded and stripped of control
characters.

## Origin and browser boundary

Qt records the exact `http` or `https` origin at pairing time. It does not use
suffix, wildcard, substring, or `null` origin matching. CORS responses echo only
that known exact origin and never enable credential cookies. Chromium private
network preflights are accepted only for a still-active exact origin. The TCP
listener and every accepted peer are independently required to be loopback.

The Worker must never proxy this API. Deployments may change their public World
origin, but the desktop user must issue a new capability for that exact origin.

## Operational evidence

The bridge emits generalized audit events (issued, paired, started, stopped,
cancelled, completed, failed, revoked) without capability values, transcript
contents, audio, microphone names, or browsing history. Tests cover loopback
binding, exact-origin enforcement, one-use pairing and mutation capabilities,
expiry/revocation, cancellation, absence of audio transport, and transcript
status delivery.

## Completion-gate evidence

- **Threat review:** the protected assets are microphone control and transcript
  text. The reviewed threats are non-loopback access, DNS-rebinding/origin
  confusion, capability disclosure, replay, request smuggling, unbounded input,
  concurrent capture, capture surviving expiry, and a late process result
  crossing sessions. The controls are independent loopback listener/peer
  checks, exact canonical origins, digest-only capability retention, one-use
  pairing and mutation IDs, bounded single-request HTTP parsing, one active
  desktop capture, a monotonic periodic expiry sweep, and capture-ID binding.
- **Abuse/privacy review:** no IP-derived identity, browsing detail, audio,
  microphone label, transcript, or capability enters audit events. The bridge
  cannot submit a draft. Pairing, recording, stopping, cancellation,
  destination choice, copying, and revocation are explicit user actions.
- **Benchmark/budget evidence:** the protocol allows at most 16 KiB of headers
  and 8 KiB of body, retains at most 512 nonces during a ten-minute session,
  permits one local capture, polls status no faster than 450 ms, and adds no
  socket, audio upload, model execution, or Worker work. These are hard bounds,
  not advisory targets.
- **Current product state:** the World does not load a speech bridge script,
  stylesheet, launcher, pairing panel, or microphone control. The Qt bridge is
  retained as dormant local infrastructure for a possible future interface.
- **Regression evidence:** `forkmesh-world-speech-tests` drives the real Qt
  loopback server, while `test_world_speech_bridge_frontend.py` enforces that
  ForkMesh World exposes no browser speech entry point.
