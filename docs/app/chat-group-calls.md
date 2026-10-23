# Group audio and video calls in private channels

Anyone in a private channel can start or join a group call from the channel's
Voice call and Video call buttons on `/chat` (2026-10-21). Up to 16 people can
be in one call. Media goes through ForkMesh's own call server (an SFU,
`docs/server/rtc.md`) and stays end-to-end encrypted: the server forwards
every frame without being able to decrypt it. One-to-one calls in direct
messages are unchanged and stay peer to peer
([chat-direct-calls.md](chat-direct-calls.md)).

Public channels and the notices channel have no group calls: their room keys
are known to the relay, so a call there could not be end-to-end encrypted
(`private_channel_required`).

## Joining

1. The client asks the app for a ticket:
   `POST /api/chat/channels/<id>/call-ticket` (session auth, member of the
   private channel). The answer is
   `{"ticket", "room", "peer", "url"}`: a 60-second ticket for the call
   server (`chat_call_ticket` in `entry.py`), the opaque room id (HMAC of the
   channel id), a random peer id for this join and the `wss://` URL of the
   call server that hosts this room. The app needs `FORKMESH_CALLS_SECRET`
   (32 characters or more, shared with every call server) and
   `FORKMESH_CALLS_NODES`, the comma-separated `wss://<node>/calls/ws` URLs of
   all call servers; without them the route answers 503
   `calls_unavailable`.
2. The client opens the WebSocket, sends `join` with the ticket, offers its
   microphone (and camera) as `sendonly` tracks, and answers every offer the
   server sends as people join and leave (protocol in `docs/server/rtc.md`,
   "signal"). The server names each forwarded stream after its sender's peer
   id (`msid`).

## Every core runs a call server

The fleet's three cores each run `forkmesh-calls` (`docs/deploy/calls.md`), so
losing one or two of them keeps calls working. A room lives in one process at
a time, so everybody in a call has to reach the same server: `chat_call_pick`
in `entry.py` uses rendezvous hashing, the highest SHA-256 of
`room + "\n" + url` among the servers that answered their
`https://<node>/calls/health` in the last 10 s (`_chat_call_healthy`, a 1.5 s
probe cached per process). Every app process therefore sends every member of
a room to the same server, a dead server only moves the calls it held, and
losing a server never moves a call that was elsewhere. When no probe
succeeds, the hash runs over all listed servers so a broken probe cannot turn
calls off.

When the client's call server goes away (the WebSocket closes, the peer
connection fails, or the server reports an error other than `room_full`),
`chat-group-calls.js` keeps the camera, microphone and encryption worker,
says `group-call-presence` `out` for the old peer id, asks for a new ticket
(which now names a surviving server) and joins again, after 1, 2, 4, 8 and
16 s, then gives up. Everybody in that call does the same and meets on the new
server; every join draws and shares a new key (see "Keys" below). The panel says
"Reconnecting…" meanwhile. `TestAGroupCallMovesToASurvivingNodeWhenItsNodeDies`
(`server/rtc/sfu`) kills the server under a three-browser encrypted call and
checks that all three decode each other again on the other one.

## FM Media tickets

A client that can speak FM Media ([../server/fmm-server.md](../server/fmm-server.md))
sends `{"engines": ["fmm", "webrtc"]}` as the body of the same
`call-ticket` POST. The response then has every usual field, unchanged, plus
`fmm` (`call_fm_media` in `entry.py`):

```json
"fmm": {
  "engine": "fmm",
  "url": "https://n2.next.forkmesh.com:4433/calls/fmm",
  "addr": "n2.next.forkmesh.com:4433",
  "alpn": "fmm/1",
  "hashes": ["<base64 SHA-256 of the current certificate>", "<of the next one>"],
  "ticket": "<ticket with \"engine\":\"fmm\">",
  "standby": {"url": "...", "addr": "...", "alpn": "fmm/1", "hashes": [...],
              "ticket": "<ticket with \"engine\":\"fmm\",\"standby\":true>"}
}
```

- Browsers open `url` with WebTransport and `serverCertificateHashes` built
  from `hashes`. Phones dial `addr` over QUIC with ALPN `alpn` and pin the
  same hashes.
- The primary is the core `url` names for WebRTC, the first rendezvous
  choice. The standby is the second choice, the core every client of the
  room moves to if the first dies. Both tickets carry the WebRTC ticket's
  peer id.
- The block appears only when the picked core advertises FM Media in its
  `/calls/health` (`{"ok": true, "fmm": {"port", "path", "alpn", "hashes"}}`,
  read by the same cached probe and checked by `chat_call_fm_media_info`).
  `standby` appears only when the second choice advertises it too: a standby
  anywhere else would split the call when the first core dies.
- No body, or `engines` without `"fmm"`, gives exactly the old response, so
  old clients see no change.
- The client tries FM Media first. If the core answers Error 409 (`engine
  webrtc`), the room is a WebRTC call already and the client joins with
  `ticket`/`url` from the same response. The first joiner's engine holds a
  room until it empties. An old client asking to join a room that runs on
  FM Media gets `room_engine` from the WebRTC server.

`app/tests/test_chat_call_fm_media.py` checks the ticket against the Go
vector, the primary and standby choice, health validation, and that old
clients get the old response.

The browser's FM Media client (WebTransport, WebCodecs, call e2ee v2, the
standby move) and how `chat-group-calls.js` chooses between the engines are in
[fmm-browser.md](fmm-browser.md).

## Who is in the call

The call server knows peer ids, not names. Participants announce themselves
in the channel's own end-to-end encrypted room with non-persisted frames:

- `group-call-presence` `{peer, call: "in" | "out", media}`, sent on joining,
  every 20 s while in the call, and on leaving. A peer not heard from for 60 s
  drops out of the roster. The channel shows "A call is in progress here · N
  people" with Join buttons to members who are not in the call.
- The name shown on a tile is the sealed frame's sender, so it is as
  trustworthy as the chat message author.
- **A peer id belongs to its first sender.** The first `group-call-presence`
  or `group-call-key` frame for a peer id in a conversation binds that peer id
  to the frame's sender name. Every later presence, `out` or key frame for that
  peer id from a different sender is ignored, so one member cannot drop
  another from the roster, rename their tile or replace their key. The binding
  lasts until its sender says `out` (which also forgets that peer's key) or is
  not heard from for 60 s. Frames whose sender name is empty are ignored
  altogether. A participant announces its random per-join peer id right after
  `joined`, so the window in which another member could claim it first is
  short (the id also shows as the stream's `msid` to people already in the
  call). A member who wins that race can blank that participant's tile, since
  the owner's key is then ignored, but cannot speak as them: the stream under
  that peer id still carries the owner's media, which the claimed key does not
  open. The same rule
  is in `chat-group-calls.js` (`claim`), `GroupCallRoster` in
  `native/ios/ForkMeshCore/Sources/FMChat/Group/GroupCalls.swift` and
  `GroupCallRoster` in
  `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/chat/GroupCalls.kt`,
  each with a test (`app/tests/test_chat_group_calls_client.py`,
  `GroupCallsTests`, `GroupCallsTest`).

## Frame encryption

Every sender encrypts each encoded audio and video frame before it is
packetized, in a worker (`app/public/chat-group-call-crypto.js`): through
`RTCRtpScriptTransform` where the browser has it, otherwise Chrome's
`createEncodedStreams` (the peer connection then sets
`encodedInsertableStreams`). A browser with neither cannot join ("This browser
cannot join end-to-end encrypted group calls"), so nothing is ever sent in the
clear.

- **Layout.** The first bytes a decoder needs stay readable (Opus: 1; VP8: 10
  on a keyframe, 3 otherwise), then AES-128-GCM ciphertext with its 16-byte
  tag, then an 8-byte frame counter and a 1-byte key id. The IV is four zero
  bytes and the counter; the readable bytes are the additional data, so they
  cannot be altered either.
- **One counter per sender.** AES-GCM must never see the same key and IV
  twice, so each sender has exactly one counter, shared by its audio and its
  video: the browser's single worker counts for both senders, and the iPhone
  attaches one `RTCFmFrameEncryptor` (one `fm::FrameSendKey`, whose mutex
  guards the counter; libwebrtc calls `Encrypt` from the audio and the video
  encoder threads) to both its senders. Until 2026-10-21 the iPhone had one
  encryptor per sender, each counting from 1 under the same key, so audio frame
  n and video frame n shared an IV. Installing the key that is already in use
  keeps the counter running (worker `send-key`, `FrameSendKey::Set`); only a
  different key starts it again from 1. The worker also handles its messages
  one at a time, so two installs cannot interleave around a key import.
- **Keys.** Each participant draws a random 128-bit key and sends it to the
  channel in a `group-call-key` frame `{peer, keyId, key}` over the room's
  end-to-end encryption, never to the call server. It draws a new key with the
  next key id on every `joined`, the first join and every rejoin after a
  failover alike, installs it and shares it; before 2026-10-21 a rejoin
  installed the old key again and the counter restarted, repeating IVs. It
  sends its current key again whenever a new participant appears. A key frame
  for a peer id bound to a different sender is ignored (see "Who is in the
  call").
- **Rotation.** When somebody leaves, every remaining sender draws a new key,
  sends it, and switches to it one second later; receivers keep the last
  three keys per peer. A frame with an unknown key or a failed tag is dropped.
- **Replay window.** Receivers keep, per peer and key id, the highest counter
  that decrypted and a 1024-entry bitmap of the counters below it (the
  worker's `fresh`/`mark`; `fm::FrameReplayWindow` in each libwebrtc
  `FrameDecryptor`, which is per receiver track, so per key id inside it). A
  frame whose counter is more than 1024 below the highest, or was already
  seen, is dropped; frames reordered inside the window are accepted. The window
  moves only after the tag checks, so a forged frame cannot spend a counter.
  The window is 1024 rather than SFrame's usual 128 because audio and video
  share the counter (about 80 frames a second together) and a video frame
  waiting on retransmissions can arrive a second or more after the audio sent
  with it. Receiving the same key again (senders re-share whenever someone
  joins) keeps the window; a different key under the same id starts a new one.
- **What the server could do.** It still cannot decrypt or forge media, and it
  can no longer replay old frames into the call. It can drop, delay or reorder
  media, or label one person's stream with another's peer id; the receiver
  then holds the wrong key and decodes nothing.

## Live quality widget

The group call panel shows the same quality pill and charts as direct calls
(`docs/app/chat-direct-calls.md`, "Live quality widget", 2026-10-22). `chat.js`
mounts it in the panel head when you join (`syncGroupCallQuality`): on FM
Media it reads the worker's `stats` reply through `fmmStatsReader` (round-trip
time and loss from congestion control, bitrate and frame rate from the
counters; there is no jitter figure), and after a fall back to WebRTC it reads
the SFU connection's `getStats()`. The voice chart shows your microphone up
and the loudest other person down ("Others"); FM Media plays received audio
through its own worklet, so only received WebRTC audio tracks are measured.

## Limits

- The key exchange rides the channel's room socket, which `/chat` keeps open
  only for the channel on screen. A participant who switches to another
  channel stays in the call, but somebody who joins meanwhile does not get
  that participant's key (and sees a blank tile) until they come back.
- The iPhone joins natively once the app carries the native WebRTC engine
  (`docs/server/rtc.md`, "Phone media engine"). Android (2026-10-23) runs
  this file's `createGroupCalls` in a WebView page
  (`native/android/app/src/main/assets/group-call/`, copies kept current by
  `app/tools/sync_android_group_call.py`), WebRTC only: FM Media is off
  there, so an Android phone cannot join a room that already runs on FM
  Media (`docs/native/ANDROID.md`, "Group calls").
- `app/tests/test_group_call_frame_crypto.py` runs the real worker in node
  against `fm_frame_crypto.cc` built with OpenSSL: frames cross both ways,
  replays are dropped and reordering inside the window is accepted on both
  sides, re-installing the same send key keeps the counter, a new key resets
  it, and audio and video through one `FrameSendKey` never repeat a counter
  (`fm_frame_crypto_tool send` / `receive`).
