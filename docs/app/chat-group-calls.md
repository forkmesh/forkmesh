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
`chat-group-calls.js` keeps the camera, microphone, key and encryption worker,
says `group-call-presence` `out` for the old peer id, asks for a new ticket
(which now names a surviving server) and joins again, after 1, 2, 4, 8 and
16 s, then gives up. Everybody in that call does the same and meets on the new
server; keys are re-shared on joining as usual. The panel says
"Reconnecting…" meanwhile. `TestAGroupCallMovesToASurvivingNodeWhenItsNodeDies`
(`server/rtc/sfu`) kills the server under a three-browser encrypted call and
checks that all three decode each other again on the other one.

## Who is in the call

The call server knows peer ids, not names. Participants announce themselves
in the channel's own end-to-end encrypted room with non-persisted frames:

- `group-call-presence` `{peer, call: "in" | "out", media}`, sent on joining,
  every 20 s while in the call, and on leaving. A peer not heard from for 60 s
  drops out of the roster. The channel shows "A call is in progress here · N
  people" with Join buttons to members who are not in the call.
- The name shown on a tile is the sealed frame's sender, so it is as
  trustworthy as the chat message author.

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
- **Keys.** Each participant draws a random 128-bit key per call and sends it
  to the channel in a `group-call-key` frame `{peer, keyId, key}` over the
  room's end-to-end encryption, never to the call server. It sends the key
  again whenever a new participant appears. A key frame for a peer whose
  presence names a different sender is ignored.
- **Rotation.** When somebody leaves, every remaining sender draws a new key,
  sends it, and switches to it one second later; receivers keep the last
  three keys per peer. A frame with an unknown key or a failed tag is dropped.
- **What the server could do.** It could withhold or reorder media, or label
  one person's stream with another's peer id; the receiver then holds the
  wrong key and decodes nothing, so it cannot put words in someone's mouth.

## Limits

- The key exchange rides the channel's room socket, which `/chat` keeps open
  only for the channel on screen. A participant who switches to another
  channel stays in the call, but somebody who joins meanwhile does not get
  that participant's key (and sees a blank tile) until they come back.
- Phones join group calls once the apps carry the native WebRTC engine
  (`docs/server/rtc.md`, "Phone media engine").
