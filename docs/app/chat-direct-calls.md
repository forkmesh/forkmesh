# Direct messages in the app, group add, and encrypted peer-to-peer calls

Direct-message surfaces on the dashboard, turning a DM into a group, and
one-to-one audio/video calls with on-device face, background and voice
filters. Access rules and DM delivery are in
[chat-private-rooms.md](chat-private-rooms.md).

## Direct messages on the dashboard

`mountDirectMessageStrip` (in `dashboard-chat.js`) inserts a strip of DM chips
under the channel header. Each chip links to `/chat?dm=<name>` with its unread
badge; the `+` chip links to `/chat?dm=`, which opens the new-DM picker. The
strip is read once from `GET /api/chat/direct-messages?limit=20` and refreshed
only when the account event channel pushes the `direct-messages` topic
(re-broadcast as the `forkmesh:direct-messages` window event). It is skipped
in the World embed and for signed-out visitors.

## "Add person" in a direct message

A DM is strictly one-to-one, so "+ Add person" creates a **new private
channel** with the current partner and the picked person
(`POST /api/chat/channels`) and switches to it. The name is built from the
participants' slugs plus a random suffix within `CHANNEL_NAME_RE`. History is
not copied: it is end-to-end encrypted to the original two participants.

## Calls

`app/public/chat-calls.js` adds one-to-one calls to DMs on `/chat`.

- **Signaling** rides the DM room socket as non-persisted, sealed frames:
  `call-invite`, `call-accept`, `call-decline`, `call-offer`, `call-answer`,
  `call-ice`, `call-bye`. The relay sees only ciphertext and stores nothing.
- **Media** is WebRTC (DTLS-SRTP) directly between peers, with a public STUN
  server and no TURN, so two peers behind symmetric NAT cannot connect.
- **Flow**: fixed transceiver order (audio, video), the caller offers only
  after accept, trickle ICE with a pre-answer queue, `replaceTrack` instead of
  renegotiation. Frames carry `to` so multi-device accounts converge on one
  answering device.
- A ring reaches the callee only while the DM's room socket is open. An
  unanswered 45-second ring posts a persisted "Missed call" chat line, which
  takes the normal notification path.
- `/chat` has a `Permissions-Policy` override for camera and microphone and
  `mediastream:` in its CSP `media-src`.
- The desktop has no WebRTC stack; its call buttons open
  `/chat?dm=<name>&call=audio|video`, which asks for confirmation before
  ringing, so a link can never ring someone on its own.

### Mobile apps

iOS and Android run the same signaling state machine (`DirectCallEngine`)
through their `DirectMessageService` and interoperate with `/chat`. Media runs
in a WebView loading `direct-call.html` from an `https://` app-local origin (a
secure context for `getUserMedia`); the native side drives it through
`window.fmCall(id, op, args)` and receives replies through
`webkit.messageHandlers.fmCall` or `FmCallBridge.post`. There is no CallKit,
VoIP push or foreground service, so a call can drop in the background.

`app/tools/sync_native_call_faces.py` and
`app/tools/sync_native_call_voice.py` inline the face modules, assets, models
and the voice code into both copies of `direct-call.html`;
`app/tests/test_chat_call_faces.py` fails if a copy is stale.

## Face, background and voice filters

Everything runs on the device before encryption. Models and images are
bundled; nothing is fetched from a third party at runtime.

### Modules (`app/public/`)

| Module | Role |
| --- | --- |
| `chat-call-faces.js` | Built-in stand-in faces (`CALL_FACES`), `createFaceStage`, adaptive quality, iOS bridge |
| `chat-call-face-geometry.js` | Landmarks, head model, `buildHeadMesh`, `solvePose`, `liveHead` |
| `chat-call-face-render.js` | WebGL 1 compositor |
| `chat-call-segment.js` | WebGL inference engine for the bundled models; person and hair mattes |
| `chat-call-face-mesh.js` | Face detection and 468-point mesh mapped to 68 points |
| `chat-call-face-tracker.js` | Heuristic fallback tracker |
| `chat-call-face-expression.js` | Blink, gaze and tongue measures from the camera image |
| `chat-call-face-custom.js` | Builds a stand-in from an uploaded photo ("Your photo", id `custom`) |
| `chat-call-backgrounds.js` | Background list, saved choice, custom photo preparation |
| `chat-call-voice.js`, `chat-call-voice-worklet.js` | Voice list (`CALL_VOICES`) and the AudioWorklet engine |

### Head swap

The chosen face replaces the outgoing video track with a canvas track
(`captureStream`, 24 fps) through `replaceTrack`. Per frame: landmarks
(Apple Vision on iOS, the face mesh elsewhere) smoothed with a One Euro
filter; a person matte refined with a guided filter; a hair matte around the
head; a 2.5D warp of the whole stand-in head with a depth buffer so near
surfaces win when turning; removal of the caller's head and hair outside the
stand-in, filled from an estimate of the room; per-frame skin and hair colour
matching; eyelid, gaze and tongue transfer. Stand-in assets
(`app/public/assets/call-faces/<id>.jpg` and `<id>-matte.png`) are built by
`app/tools/build_call_face_assets.py`.

**Privacy rule:** when tracking fails the frame is blurred until a face is
found again; raw video is never shown in place of a chosen face.

### Backgrounds

No background, Blur, Strong blur, built-in images
(`app/public/assets/call-backgrounds/`, rendered in code by
`app/tools/render_call_backgrounds.cjs`), or a custom photo. Blur weights only
room pixels so the person never bleeds in. Choices are saved in
`localStorage` (`forkmesh.chat.callFace`, `forkmesh.chat.callFaceImage`,
`forkmesh.chat.callBackground`, `forkmesh.chat.callBackgroundImage`) or app
preferences (`chat.callFace`, `chat.callBackground`).

### Models

`app/public/assets/call-segmentation/*.fmnn`: selfie segmentation (landscape
and square), face detector, face mesh, and hair segmentation, all Apache 2.0
(licence text in `LICENSE` beside them). `.fmnn` is a JSON graph plus float16
weights, produced from `.tflite` by
`app/tools/build_call_segmentation_model.py`. `chat-call-segment.js` runs one
fragment-shader draw per layer on half-float textures; without
`OES_texture_half_float` and 12 texture units the background picker is
hidden and faces use the fallback tracker. On iOS, Apple Vision supplies
landmarks and the person mask; the hair model still runs in the page.

**Adaptive quality** steps between four levels (how often the selfie and hair
models run, guide resolution) based on per-frame time, stepping down above
34 ms and up below 20 ms.

### Voices

The microphone track goes through an `AudioWorklet` after the browser's echo
cancellation, and the processed track replaces the audio sender's track. If
the worklet cannot start, the plain microphone is sent. The engine is a port
of the desktop voice changer: high-pass and light denoise, an STFT formant
(envelope) warp, PSOLA pitch shifting anchored on glottal pulses, a McLeod
pitch tracker with hysteresis and an octave guard, and a look-ahead limiter.
Added latency is 1728 samples (36 ms at 48 kHz).

Each voice in `CALL_VOICES` is a target median pitch, a vocal-tract scale,
brightness, breath and optional `range`, `jitter` and `tremor` (clamped by
`VOICE_LIMITS`). The pitch ratio is `target / callerMedian`, where the
caller's median is tracked continuously and saved in
`forkmesh.chat.callVoicePitch`. The choice ("Match face", "Your own voice" or a
voice id) is stored in `forkmesh.chat.callVoice` / `chat.callVoice`; a face
without a matching voice id gets the caller's own voice.

### Preview and reuse outside a call

`/chat` and the apps have a Preview screen that opens the camera through the
same stage with Face, Background and Voice pickers and "Hear myself"; Start
video call keeps the stage and offers its track, with no camera restart.

```js
const stage = createFaceStage({ cameraStream, faceId, backgroundId, backgroundImage });
stage.track;                      // MediaStreamTrack, 24 fps canvas capture
stage.setFace(id);                // "" for the caller's own face
stage.setBackground(id, image);   // ids from CALL_BACKGROUNDS; image = data URL for "custom"
stage.stop();
```

`createVoiceStage({sourceTrack, voiceId, userPitchHz, moduleUrl, onStats})`
returns a processed `track` plus `setVoice`, `monitor(on)` and `stop()`.
Neither needs an `RTCPeerConnection`, so view-once video messages record from
them ([chat-private-rooms.md](chat-private-rooms.md)). With `{manual: true}`,
`stage.processFrame(source, time)` renders on demand for tests.

## Measuring

`app/tools/call_video_harness.cjs` drives the real modules in headless
Chromium (`render`, `native`, `models`; `PLAYWRIGHT_DIR` names a Playwright
install, `--gpu` uses the machine GPU). `app/tools/call_video_metrics.py`
reports landmark jitter and output stability. Test footage is kept out of the
repository.

## Tests

`app/tests/test_chat_direct_calls_frontend.py`,
`app/tests/test_chat_call_faces.py`, `app/tests/test_chat_call_voice.py`,
`app/tests/test_chat_call_voice_realism.py`,
`app/browser_tests/tests/native-call-record.spec.js`, plus the iOS and
Android `DirectCall*` tests.
