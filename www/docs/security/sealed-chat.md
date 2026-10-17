# Chat encryption and public integrations

All chat traffic uses an outer AES-256-GCM room cipher whose passphrase is
derived from the controller's `DATA_KEY`. It protects the wire and retained
storage from third parties, not from the controller operator. On top of it each
conversation has one policy:

| Conversation | Content policy | Controller integrations |
| --- | --- | --- |
| Public `#` channel | Controller-readable room encryption | Bot, webhooks and MCP |
| Private channel or direct message | Automatic device-to-device encryption inside the room cipher | Unavailable |

Public channels are controller-readable on purpose: server-side integrations
cannot read or post device-sealed messages. Making a channel public affects
only new messages; making it private rotates the room key and revokes its
incoming webhooks.

Device-to-device encryption uses chat protocol v3 (October 2026). Every
client (web, desktop, iOS, Android) speaks only v3; older v2 frames are
ignored. The full specification is `docs/app/chat-protocol-v3.md` in the
ForkMesh repository.

## Device keys

Each installation keeps two kinds of keys on the device (browser storage
wrapped by a non-extractable key where the browser supports it, the desktop
secure store, the iOS keychain, Android Keystore-wrapped storage):

- an **identity key** (Ed25519) for the life of the install, which signs
  everything the device sends;
- **prekeys**, each an X25519 key plus an ML-KEM-768 (post-quantum) key. A new
  prekey is made every 24 hours and its private half is deleted 8 days after it
  was made.

Private keys never leave the device.

## Bundles and new devices

A device announces itself in each room with a signed bundle: its identity key,
its newest prekey, the account it belongs to and the sender id it uses in that
room. Other devices check the signature before using it.

- Before v3 the server could add a key to a direct message and every client
  would start encrypting to it. Now, in a direct message, a bundle that names anyone other than the two people in the
  conversation is dropped, and a second device for an account that already
  has one is **held**: it receives nothing and its messages are not shown until
  you approve it. You see a line in the conversation and a banner with Approve
  and Block.
- Channels and Office rooms accept every validly signed device.

## Sealed messages

For each protected message the sender picks a random content key and encrypts
the message with AES-256-GCM. For each approved device it then combines a fresh
X25519 exchange with an ML-KEM-768 encapsulation to that device's prekey,
derives a wrapping key with HKDF-SHA256, and wraps the content key. The whole
frame, including every wrapped key, is signed by the sender's identity key and
carries a counter and a timestamp.

A receiving device rejects a frame that:

- is not signed by an approved device;
- claims a sender id other than the one in that device's signed bundle;
- repeats a counter it has already accepted (the relay replaying a frame);
- was not wrapped for one of its kept prekeys.

Relay-held copies are kept for up to 7 days; prekeys live 8, so a message can
still be opened while it is waiting. After a prekey is deleted, nothing sealed
to it can be opened again, even with the device in hand.

## Safety numbers

In a direct message, the encryption panel shows a 60-digit safety number and a
QR code. It covers every approved device of both people. Compare the digits
with the other person, or scan their code, then mark the conversation verified.
If an approved device is added or removed the number changes, the verified mark
is cleared and the conversation says so.

## Limits

- The controller still sees who talks to whom, when, and how much, and it can
  drop or delay frames.
- Channels approve every device that presents a valid bundle; only direct
  messages hold new devices.
- Forward secrecy is per prekey, not per message: a device compromised today
  exposes the messages sealed to the prekeys it still holds (up to 8 days).
  Recovery after a compromise comes with the next prekey rotation (24 hours).
- A new device cannot read messages sent before it was approved.
- A modified web client served by a compromised host can read what it shows
  you, as with any web app.
- None of this has had an outside security audit yet.

## Compared with Signal

Signal is ahead in two ways: its Double Ratchet gives forward secrecy for every
single message, and sealed sender hides who sent a message from Signal's own
server. ForkMesh v3 adds post-quantum key agreement on every message, a
signature on every frame, holds a new device of a contact until you approve it
(Signal warns and keeps sending), and lets nearby devices chat directly over
Wi-Fi or Bluetooth without a server. A ratchet and sealed-sender delivery are
the next steps.

## Implementations

Browser (`app/public/chat-seal.js`, `app/public/chat-mlkem.js`,
`app/public/chat-room-transport.js`), desktop (`desktop/src/RoomSeal.{h,cpp}`),
iOS (`FMCrypto/RoomSeal.swift`) and Android (`crypto/RoomSeal.kt`) all open the
shared wire vectors in `conformance/chat/v3/vectors.json`, so a change to key
derivation, signed strings, encoding or frame shape fails the cross-client
tests.
