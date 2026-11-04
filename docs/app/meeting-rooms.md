# Meeting rooms (`/call`)

Part of [the App docs](README.md). A meeting room is a browser video call
anyone with its link can join, with no ForkMesh account. The owner asked for
the chat calls to be available to the [Booking Page](../marketplace/booking.md)
plug-in, for every new booking to get a meeting link, for the room to have all
the face and voice features and to look and be trustworthy, and for an offline
transcript. Booking is the only plug-in that opens rooms today.

It is the group-call engine of `/chat` ([chat-group-calls.md](chat-group-calls.md))
with a different signalling path: the keys travel sealed under a secret in the
link instead of over a chat room.

## Code map

| Path | What it does |
| --- | --- |
| `app/public/call.html`, `call-room.js`, `call-room.css` | The page: lobby, call grid, controls, side panel (People, Transcript, Effects), trust panel. |
| `app/public/call-meeting-crypto.js` | Link parsing, HKDF keys, sealed boxes, the meeting code, the envelope around each call frame. |
| `app/public/call-transcribe.js` | On-device speech recognition and the transcript file. |
| `app/src/entry.py` (`plugin_call_ticket`, `_PluginApiRuntime.call_ticket`) | Call-server tickets for a package's room. |
| `marketplace/packages/booking/content/worker/booking.py` (`_room_*`) | Room records, the open window, tickets and the sealed relay. |
| `app/tests/test_call_meeting_room.py` | Crypto, transcription and a two-engine key exchange in node. |

## The link

`https://app.forkmesh.com/call?<room>#<key>`: `room` is 32 random hex digits,
`key` 64 hex digits (256 bits). Browsers never send the part after `#` to a
server, so the call server and the relay never see the key. The page keeps
`referrer` off. The booking worker makes the link, stores it sealed with the
owner's account key, and emails it, so the mail path sees it once
([booking.md](../marketplace/booking.md#video-rooms)); that is the honest
limit, and the trust panel says to treat the link like a key.

`/call` is reserved (`static_routes`, `repo_ssr`), `/call.html` is blocked like
the other direct documents, forkmesh.com/call redirects to the App, and the
page gets `camera=(self)`, `microphone=(self)` and `mediastream:` with
`frame-ancestors 'none'` (`_headers`, `_apply_browser_security_headers`).

## Keys and signalling

From the link key the page derives, with HKDF-SHA256 and salt
`forkmesh-meeting-v1\n<room>`:

- `seal` (AES-256-GCM): every signalling message is sealed with it, a random
  12-byte nonce, the room id as associated data, and a timestamp inside;
  boxes older than five minutes are dropped.
- `code` (64 bits): the meeting code, six digits and three words from a fixed
  list. Everyone with the same link sees the same code, so reading it aloud
  shows that nobody was handed a different room.

The engine is `createGroupCalls` with `fmm: {enabled: false}` (FM Media needs
a chat signing identity) and the WebRTC SFU. Each person draws their own
per-sender frame key and announces it with `group-call-presence` and
`group-call-key`, exactly as in a private channel; the frame encryption worker
is the same (`chat-group-call-crypto.js`). A browser without encoded
transforms cannot join at all, so a meeting never falls back to unencrypted
media.

Each frame is wrapped as `{t, f, n, tr, cam, mic}` (type, the engine's frame,
display name, transcribing, camera, microphone; `meetingEnvelope`) and sealed.
`meetingFrame` unwraps it, drops anything without a 32-hex `peer`, and hands
the engine `f` with `type` and the lowercased name as `sender`.
`meeting-status` frames only update the people list. The relay is
`POST`/`GET /api/booking/-/call/<room>/signal` ([booking.md](../marketplace/booking.md#video-rooms)),
polled every 1.5 s (4 s when the tab is hidden and quiet). On joining, the
first poll returns the last two minutes, which holds everyone's current key.

The relay sees box sizes and timing, not names or keys. Anyone with the link
can join and could pose under any name; the people list, the join and leave
notices and the meeting code are how people notice an unexpected guest.

## The page

1. **Lobby.** The meeting's title, host and time in the visitor's time zone;
   "opens at" before the window; the camera preview with mute and camera
   buttons; Face, Background, Outfit and Voice (with "Hear myself"); the name;
   Join; and "Private by design" with How it works.
2. **Call.** A tile per person (name, muted, "Transcribing on their device"),
   the quality pill with Diagnose ([call quality](chat-direct-calls.md#live-quality-widget)),
   controls for mute, camera, effects, transcript, people and leave, and
   notices when people join, leave or start a transcript.
3. **Trust panel.** The lock pill in the header opens what is encrypted, what
   the server sees, where the key lives, and the meeting code.

The effects are the ones in [call-filters.md](call-filters.md), through
`call-studio.js` like the Face Changer page: the camera opens once, the face
stage and voice stage are kept from the lobby into the call (`acquireMedia`,
`prepareLocal`), and changing a face, background, outfit or voice in the
Effects tab changes what is being sent at once. Choices use the call's
`localStorage` keys, so a face picked in `/chat` or the Face Changer is the
meeting's face too.

## Offline transcript

The Transcript tab uses the browser's speech recognition only when it can run
on the device: the class must offer `available()` (or the older
`availableOnDevice()`) and `processLocally` (or `mode`), and each recognizer is
set to local before it starts. Otherwise the tab says the transcript is off;
it never falls back to a cloud recognizer.

- `available` starts at once; `downloadable` shows **Download offline speech
  model** (`install()`), once per language; `unavailable` asks for another
  language.
- With the newer API (`start(track)`) your own voice comes from the call's
  microphone track, so muting pauses it, and each other person's incoming
  audio track is recognised too. With the older one only your own voice is,
  from the default microphone, and the tab says so.
- Lines are `[time] Name: text`, kept in memory, with Copy, Download `.txt`
  and Clear; leaving offers the download, and closing the tab with a
  transcript asks first.
- Starting one sets `tr` on your frames, so everyone sees "Transcribing on
  their device" on your tile and a notice.

## Not done yet

- Phones join in their browser; the apps have no native meeting screen.
- No screen sharing, recording or waiting room.
- The meeting code is not tied to who is in the call; it proves the same link.
