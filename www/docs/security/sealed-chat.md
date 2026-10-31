# Chat encryption and public integrations

All chat traffic uses an outer AES-256-GCM room cipher whose passphrase is
derived from the controller's `DATA_KEY`. It protects the wire and retained
storage from third parties, not from the controller operator. On top of it each
conversation has one policy:

| Conversation | Content policy | Who can read it |
| --- | --- | --- |
| World `#general`, public `#` channel | Device-to-device encryption to every device in the room | Anyone who joins the room: guests, and the controller, which can join as a device |
| Private channel or direct message | Device-to-device encryption inside the room cipher | Only the members (the server checks who may join); a private channel's webhook and notice lines are also readable by the server |

Every person's message is sealed and signed by their device, in public rooms
too, so nobody can post, edit or delete as that person. That does not hide
public rooms from the server: they run the open policy, so any device that
joins gets the keys of the messages sealed after it arrived, and the
controller can join as a device. Public channels are controller-readable;
nothing private belongs there. Direct messages, private channels and Cove
invites never travel through a shared room; they use the server's direct and
private rooms, and clients drop such frames if they show up in a shared room.

Apps, webhooks and ForkMesh's own notices are posted unsealed by the server,
in public and private channels alike (never in direct messages), so the server
can read them; clients mark a webhook's line "server-visible". The server marks
each frame it injects, removes that mark from anything a client sends, and
clients show a marked frame as the app or webhook it names, never as a person.
An unmarked, unsealed frame is dropped.
A line a device posts for ForkBot shows as "forkbot · via" the person who
posted it. Making a channel public affects only new messages; making it
private rotates the room key and revokes its incoming webhooks.

Device-to-device encryption uses chat protocol v3 (October 2026), and direct
messages use v4, which adds a per-message ratchet. Every client (web, desktop,
iOS, Android) speaks only these; older v2 frames are ignored. The full
specifications are `docs/app/chat-protocol-v3.md` and
`docs/app/chat-protocol-v4.md` in the ForkMesh repository.

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

- In every room except direct messages the sender id is the device's own
  Ed25519 node key, and the bundle carries a second signature by that key. A
  bundle without it is dropped, and a key id already seen keeps its sender id,
  so one device cannot speak, edit or delete as another. Browsers keep a
  separate node key for a signed-out visitor and for each account, so a
  guest's messages cannot be linked to the account signed in later.
- The account name in a bundle is only a claim. Clients show it after the
  server confirms that the node key is bound to that account (each device binds
  its key at sign-in); otherwise the sender is shown as a guest.
- Edits, deletes, reactions, stickers and thread replies count only from
  sealed frames, and an edit or delete only from the message's author.

- Before v3 the server could add a key to a direct message and every client
  would start encrypting to it. Now, in a direct message, a bundle that names anyone other than the two people in the
  conversation is dropped, and a second device for an account that already
  has one is **held**: it receives nothing and its messages are not shown until
  you approve it. You see a line in the conversation and a banner with Approve
  and Block.
- Channels and Office rooms accept every validly signed device whose bundle
  is also signed by its node key.

## Sealed messages

For each message the sender picks a random content key and encrypts
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

## Direct messages: a key for every message

In a direct message each pair of devices runs a ratchet, like Signal's Double
Ratchet. The first message starts it with the same X25519 + ML-KEM-768 exchange
against the other device's prekey; after that every message has its own key,
derived from a chain that only moves forward, and the key is deleted as soon as
the message is opened. Each time the conversation changes direction, both
devices mix in a fresh X25519 exchange and a fresh ML-KEM-768 encapsulation.

- A device stolen today cannot open direct messages it had already received:
  their keys are gone.
- A device that was compromised recovers once the other person replies; against
  a quantum-capable attacker, once its prekey rotates (24 hours).
- A device never encrypts to itself; your own messages stay in your device's
  history, not in a copy the server holds for you.
- If a device loses its ratchet state, it asks the sender to start over and
  the sender re-sends what it still holds.

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
  messages hold new devices. World and public channels are open rooms: anyone
  who joins can read what is said there from then on.
- In channels and Office rooms, forward secrecy is per prekey, not per
  message: a device compromised today exposes the channel messages sealed to
  the prekeys it still holds (up to 8 days). Channels keep this because the web
  reopens their history from the relay on every load.
- In direct messages, the messages sent before the other device first replies
  still rest on its prekey (up to 8 days); Signal adds one-time prekeys for
  these.
- A new device cannot read messages sent before it was approved.
- A modified web client served by a compromised host can read what it shows
  you, as with any web app.
- None of this has had an outside security audit yet.

## Compared with Signal

Direct messages now have per-message forward secrecy, as Signal does. Signal
is still ahead in one way: sealed sender hides who sent a message from Signal's
own server, while the ForkMesh server still knows which two accounts are in a
direct message. That is the next step. ForkMesh adds post-quantum key agreement
at the start of every session and on every ratchet step, a signature on every
frame, holds a new device of a contact until you approve it (Signal warns and
keeps sending), and lets nearby devices chat directly over Wi-Fi or Bluetooth
without a server.

## Implementations

Browser (`app/public/chat-seal.js`, `app/public/chat-mlkem.js`,
`app/public/chat-room-transport.js`, `app/public/chat-frame-auth.js`), desktop (`desktop/src/RoomSeal.{h,cpp}`),
iOS (`FMCrypto/RoomSeal.swift`) and Android (`crypto/RoomSeal.kt`) all open the
shared wire vectors in `conformance/chat/v3/vectors.json`,
`conformance/chat/v3/node-binding.json` and
`conformance/chat/v4/vectors.json`, so a change to key
derivation, signed strings, encoding or frame shape fails the cross-client
tests.
