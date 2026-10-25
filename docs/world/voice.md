# Voice, camera and channels in the World

People in the World can turn on their microphone and camera and talk to each
other: in the whole World, in one World channel, in a voice room they start,
or inside a privacy bubble with the people next to them. The camera does not
send video by default. It moves the person's avatar: head direction, every
facial expression on the avatar's face, arms and hands, and hand gestures that
send reactions to whoever is speaking. Each speaker wears a live word count.
One Discord-style list holds every World channel, voice room, Office room,
private channel and direct message. Added 2026-10-24.

It is one system with the `/chat` calls: the same group-call engine
(`chat-group-calls.js`), the same face changer and voice changer, the same
quality meter and the same call server ([../app/chat-group-calls.md](../app/chat-group-calls.md),
[../server/rtc.md](../server/rtc.md)).

## Where the code is

| Piece | Code |
| --- | --- |
| Voice controller: rooms, mic and camera, rig sending, reactions, bubbles, voice rooms, performance badge, diagnostic report | `world/public/world/world-voice.js` (loaded on first use) |
| Room helpers shared by the shell, the controller and the channel list | `world/public/world/world-voice-rooms.js` |
| One channel list | `world/public/world/world-channels.js` (loaded on first use) |
| Remote rigs to avatars: live faces, word badges, reactions | `world/public/world/world-avatar-puppets.js` (loaded on the first rig or voice frame) |
| Live drawn face | `paintProceduralAvatarFace(context, key, expression)` in `world/public/world/world-avatar-face.js` |
| Head, arms, hands, word badge, bubble sphere on the avatar | `world/public/world/scene/avatar-rig.js` |
| Per-frame scene runtime, scene API | `world/public/world/scene/runtime/avatar-voice.js`, registered in `scene/core.js` |
| Shell glue: HUD buttons, socket routing, presence fields, member card actions | `world/public/world/world.js` (`ensureVoice`, `ensureAvatarPuppets`, `ensureChannels`, `sendVoiceFrame`, `receiveWorldReaction`, `worldVoiceDetailMarkup`) |
| Camera to rig: face mesh, hands, expression, pose | `app/public/call-avatar-rig.js` |
| Rig wire format, reaction vocabulary | `app/public/call-avatar-rig-wire.js` |
| Photo avatar warp | `app/public/call-avatar-puppet.js` |
| Hand detection and gestures | `app/public/chat-call-hand-tracker.js`, models in `app/public/assets/hand-tracking/` |
| Talking and word count | `app/public/call-speech-meter.js` |
| Protocol, sanitizers | `world/src/world.py` (`sanitize_rig`, `sanitize_voice_signal`, `world_voice_room`, interactions) |
| Relay | `world/src/world_durable_objects.py` (`_relay_live_frame`, bubble and reaction interactions) |
| Ticket route | `world/src/world_routes.py` (`world_voice_ticket_handler`), routed in `app/src/app_runtime.py` |

## Rooms

A room id is one of:

| Id | Who hears it |
| --- | --- |
| `world` | everyone in the World who joined voice (#general) |
| `space:<space>` | everyone in that World channel's voice room (`#world-sky-campus` and the rest) |
| `adhoc:<16 hex>` | only the people who joined that voice room; it is listed for everyone |
| `bubble:<16 hex>` | only the people inside the privacy bubble |

Private chat channels keep their own end-to-end encrypted calls in `/chat`;
the channel list opens them inside the World (below).

Turning the microphone on with no room joins the room of the World channel the
chat box is on (`#general` means `world`). Joining without a microphone
listens with a silent track, so nobody has to allow a microphone to listen;
turning the microphone on later rejoins with it. A mute keeps the device open
and disables the track.

**Ticket.** `POST /api/world/voice/ticket` with `{"room": "<id>", "secret":
"<32 hex>"}` (the secret only for a bubble) needs a signed-in, active user
session and a same-origin request. It answers the same ticket as a chat
channel call (`chat_call_ticket` in `entry.py`) for the channel id
`world-voice:<room>`, so the call server, its health-ranked node choice and
the 16-person room limit are the ones `/chat` uses. A bubble's id is
`bubble:` plus the first 16 hex of `SHA-256("forkmesh-world-bubble-v1\n" +
secret)`; the route checks that the secret matches the id and keys the call
room on the secret, so knowing a bubble's public id is not enough to get in.
FM Media is off for World rooms because it needs a sealed-room signing
identity; World rooms use the WebRTC engine.

**Signalling.** The group-call engine announces itself and shares its frame
keys with `group-call-presence` and `group-call-key` frames. In the World they
travel over the World socket as `{"type": "voice-signal", "room", "frame"}`.
The server accepts one only when the sender's presence `voice` equals `room`,
keeps just the call fields (`peer`, `call`, `media`, `keyId`, 16-byte `key`),
adds the sender's display name and peer id, and delivers it only to sockets
whose presence `voice` is the same room. At most 6 per second per socket.

**What is private.** Media goes to the call server encrypted in transit
(DTLS-SRTP) and frame-encrypted with per-sender keys. In World rooms those
keys travel through the World server, so the ForkMesh servers could read them:
World voice is private from everyone outside the room, not end-to-end
encrypted against ForkMesh. The panel says who can hear ("Everyone in the
World can hear this room", "Only people inside this bubble can hear you").
Private-channel calls stay end-to-end encrypted.

## Presence fields

`presence` frames carry `voice` (a room id or `""`, never `channel`; guests
always `""`), `voiceName` (an ad-hoc room's name, 24 letters, digits, spaces,
apostrophes and hyphens), `mic`, `cam` and `words`. `mic` and `cam` are false
outside a room. Everyone uses them to list who is in which room, to draw
bubbles and to show word badges for people further away than rig frames
reach.

## Avatar rig

With the camera on, `createAvatarCapture` runs the call face mesh
(`chat-call-face-mesh.js`) and the hand tracker on each camera frame in its
own WebGL context and turns them into a rig:

- **Pose.** `solvePose` against the shared 68-point 3D template (now in
  `chat-call-face-template.js`, also used by the face stream) gives yaw,
  pitch and roll.
- **Expression points.** Each landmark minus where the template puts it for
  that pose, in eye spans and with roll removed, minus the person's own
  neutral face. The neutral face is learned only from calm frames (mouth
  closed, eyes open, facing the camera) at 0.6% a frame, and its inner lips
  are always closed, so an open mouth always shows as open. Until a calm frame
  arrives the reference is the current frame with the mouth closed. The first
  version took the first frame as neutral and under-reported every mouth that
  was open at the start.
- **Channels.** Lid closure per eye (eye aspect against the widest seen,
  combined with the call expression probe), gaze, mouth opening, smile and
  pucker from the mouth width against neutral, each brow's lift, tongue.
- **Hands.** Up to two hands: which hand, where the palm is relative to the
  face in eye spans, five finger curls and the palm's roll.

The wire frame is `{"type": "rig", "v": 1, "p": [yaw, pitch, roll] (mrad),
"d": base64 of 68 x 2 signed bytes (1/64 eye span), "x": ten channels 0-100,
"h": [[side, x, y, five curls, roll], ...], "t": talking, "l": level, "w":
words, "a": photo anchors}`, about 290 bytes, 500 with anchors. `a` is the
avatar photo's own 68 points (0-255 of the image), detected once on the
sender and sent every 3 s. A frame without `d` is speech only: talking, level
and words for people with the microphone on and the camera off (every 250 ms
while talking, 2.5 s otherwise).

The relay (`_relay_live_frame`) handles `rig` before the per-socket rate
budget so it never costs presence or movement frames: at most 16 a second are
relayed (12 are sent), more are dropped, and over 40 in a second closes the
socket. A rig goes only to sockets in the same space within 90 units; the
audience positions are re-read once a second. `type` must be the first key so
the fast path finds it in the first 40 characters.

On the receiving side `world-avatar-puppets.js` decodes rigs, hands head and
hands to the scene (`setAvatarRig`) and repaints the face at up to 15 fps:

- with a photo face and anchors, `createFacePuppet` warps the photo along a
  Delaunay mesh of its 68 points plus the image border, after drawing a mouth
  interior and teeth that show when the lips part;
- otherwise the drawn portrait is painted with the live lids, gaze, brows and
  the actual lip outline. Brows are drawn dark on the darker skin tones,
  where the old brown stroke disappeared.

The scene (`avatar-rig.js`) adds the yaw, pitch and roll to the look rig
(remembering what it added so the look code and the rig never fight), points
each arm at its hand target from the shoulder with small hands and curling
fingers, and leaves the arms alone during a wave, handshake or emote. Avatars
with a live rig, a face canvas or a word badge are kept out of the figure
batches (`forceFullDetail`) because their textures change every frame. A rig
not heard from for 1.5 s settles back; after 6 s it is dropped.

Directions: the camera image is not mirrored, so a person turning to their own
left turns toward the image's right; the avatar faces whoever looks at it the
way the person faces the camera, so it turns to its own left (`+rotation.y`,
fronts face -Z, the avatar's right is +X).

## Faces, voices and video

The camera's default face is the person's avatar: their photo when they wear
it on their World face, or their drawn portrait. "Also send video" adds a
video track to the room. It goes through the face changer with the face
chosen in `forkmesh.chat.callFace` on the World origin, `avatar` by default,
and the voice changer applies to the room audio in every mode. These are the
same `createFmmLocal` stages `/chat` uses.

## Hands and reactions

The hand models are MediaPipe's `palm_detection_lite` and
`hand_landmark_lite` (Apache 2.0), converted to `.fmnn` by
`app/tools/build_call_segmentation_model.py`, which now maps
`FULLY_CONNECTED` on a 1 x 1 input to a 1 x 1 convolution and knows fused
`RELU6`; the WebGL runner gained `relu6`. They live in
`app/public/assets/hand-tracking/` (about 4 MB, fetched only when the camera
turns on) so the call segmentation folder keeps its size budget.

Palm detection runs at most every 0.9 s while fewer than two hands are
followed; each followed hand is tracked from its last landmarks. Hand tracking
runs every second camera frame, every fourth when it takes longer than 30 ms.
`classifyHand` reads thumbs up and down, peace, OK, rock, point up, open palm
and fist from finger extension; two hands give heart or clap; an open palm
swung three times in 1.4 s is a wave. A gesture held 450 ms fires once, with
2.5 s between any two. On the five MediaPipe test photos (thumb up, victory,
pointing up, fist, two open hands) the tracker found every hand and named
every gesture.

A fired gesture, or a reaction button in the voice panel, sends
`{"type": "interaction", "kind": "reaction", "reaction", "target"}` where the
target is whoever spoke most recently in the last 15 s. The server accepts the
`WORLD_REACTION_VALUES` vocabulary and broadcasts it; everyone sees the emoji
fly from the reactor to the speaker, and the speaker gets a notice. The
"Hand gestures send reactions" switch turns hand tracking off.

## Word count badge

`call-speech-meter.js` measures the microphone's 300-3400 Hz band every 20 ms,
follows a noise floor, calls it talking 9 dB above the floor (with 320 ms of
hangover), and counts a syllable at each energy peak at least 4 dB above the
dip before it and 110 ms after the last one. Words are syllables / 1.25.
Nothing is transcribed and no audio leaves the device for this. On the two
public-domain Harvard sentence recordings (OSR_us_000_0010 and 0011, 80 and 79
words) it counted 78 and 67. Chrome's on-device speech recognition was tried
first: in the headless Chromium used here, merely asking
`SpeechRecognition.available({processLocally: true})` hung or crashed the
tab, so it is not used.

The badge sits on the avatar's right shoulder, opposite the verified pin. It
shows the count (1.2k, 34k, 1.2M) and its colour says when the person last
spoke: green and blinking while talking, amber within 30 s, slate otherwise.
Voice panel rows use the same states.

## Privacy bubbles

Select someone and press "Start a privacy bubble" (member card, Voice
section). The starter draws a 32-hex secret, joins `bubble:<tag>` and sends
`bubble-invite` with the room and secret to that one person. The invitee sees
Join / Decline in the voice panel. Someone outside can press "Ask to join
their bubble" (`bubble-knock`); anyone inside can let them in, which sends
them an invite. `bubble-invite` is accepted only from someone whose presence
is in that bubble and only from accounts. Everyone sees a soft iridescent
sphere around the members (`createPrivacyBubble`), sized to the group and
wobbling slightly; nobody outside hears anything.

## One channel list

The # button on the chat box opens the list (`world-channels.js`):

- **World**: #general and the World channels. Selecting one switches the chat
  box to it; the microphone icon joins its voice room; members in voice are
  listed under it.
- **Voice rooms**: every voice room and bubble people are in, with "+" to
  start one. A bubble you are not in is shown locked.
- **Office**: the #general meeting room. Every channel row also has an Office
  button that walks you into that channel's meeting room.
- **Channels** and **Direct messages**: open inside the World in
  `/chat?embed=world#room=<id>` or `#direct=<id>`, which keeps every chat
  feature, including voice and video calls (with the "Your avatar" face) and
  their own call buttons. "Open Chat in a new tab" is still at the bottom, and
  the chat box's channel menu opens channels and DMs here too, falling back to
  a new tab if the list cannot load.

The Office meeting rooms are the person's chat channels. `openLobby` now loads
them (`fetchRooms`), and `joinRoom` loads them when asked for a room it does
not know yet. Before this, `fetchRooms` was never called, so the Office
meeting board only ever offered #general.

## Performance badge and diagnostic report

While a room, the microphone or the camera is on, a small badge above the
chat box shows the World's frame rate, the face and hand tracking time and the
call's round trip, with a dot that is green, amber or red. "Good" needs at
least 24 fps and a good call; tracking over 50 ms or a fair call is amber;
under 15 fps or a poor call is red. Clicking it opens the voice panel with the
details and "Send diagnostic report", which posts to `/api/bug-reports`
(`screen: "world-voice"`, severity high when red) with a JSON attachment: room
kind and size, mic, camera, engine and call state, the last 20 call quality
samples, `diagnoseCall`'s findings, tracking statistics, renderer quality and
device. It carries no names, room ids, secrets or audio.

## Verified

- `world/tests/test_world_voice.py`: presence fields, rig and signal
  sanitizers, bubble and reaction interactions, ticket scoping, and the relay
  run through `ForkMeshWorld.webSocketMessage` (rigs reach nearby people in the
  same space only, leave the rate budget alone, are capped and close a
  flooder; signals reach only the same room).
- `world/tests/test_world_voice_frontend.py`: room helpers, the bubble tag
  against the server hash, the unified list, every App module the voice
  stack loads being routed by `world/worker.js`, `world/wrangler.toml` and
  `deploy/core/caddy_routes.py`, and the shell, scene and Office wiring.
- `app/tests/test_call_avatar_rig.py`: wire round trip and size, the closed
  neutral mouth, gestures from hand shapes, one fire per hold, syllable
  counting, the photo warp, the hand model files.
- In headless Chromium with a fake camera playing the Frania talking clip from
  `tools/testdata/face-stream/`: the face was tracked in every frame, mouth
  opening ran 0.05-0.77, and both the drawn face and a photo avatar (a still
  of the Keshav clip) opened and closed their mouths, blinked and raised their
  brows with her. A mocked World socket carried rig frames to a second avatar,
  which turned its head and raised its hand; the World loaded, the HUD
  buttons, voice panel, channel list, performance badge and diagnostic report
  worked end to end, and mic join reported the server's "calls_unavailable"
  plainly. The software renderer reached 3-8 fps there; real GPUs are needed
  for numbers worth quoting.

## Not done yet

- A real call between two browsers through `forkmesh-calls` from the World
  (the route, signalling and engine are the tested `/chat` ones, but no
  multi-browser run was made against a live call server).
- Spatial audio: everyone in a room is heard at full volume.
- Rig frames for people more than 90 units away; their badges come from
  presence every few seconds instead.
- Phones: the World pages on iOS and Android get this through the browser;
  no native port.
