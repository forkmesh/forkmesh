# Chat encryption and public integrations

ForkMesh chat always has an outer AES-256-GCM room cipher. Its passphrase is
normally derived from the controller deployment's `DATA_KEY`, so it protects
the wire and retained storage from third parties but not from the controller
operator.

The application layer applies one of two explicit policies:

| Conversation | Content policy | Controller integrations |
| --- | --- | --- |
| Public `#` channel | Controller-readable room encryption | ForkBot and public-channel webhooks; MCP for `mainnode/forkmesh #general` |
| Private channel or direct message | Automatic device-to-device encryption inside the room cipher | Unavailable |

Browser channels use their stored `visibility` field. Public World `#general`
is public by definition. Desktop and mobile `#` channels are public unless the
desktop channel is marked private; direct messages are protected. Authenticated
dashboard spaces are protected.

Changing a channel to Public is an intentional disclosure decision for new
messages. It does not decrypt already retained private history. Changing it to
Private rotates the room key and revokes its incoming webhooks.

## Why public channels are controller-readable

ForkBot, MCP, and incoming webhooks run on or through the controller. They
cannot read or create a recipient-sealed message without a participant device's
private key. Public channels therefore stop at the authenticated room cipher so
those integrations can read and post there.

This is not end-to-end encryption from the controller operator. The UI labels
these conversations **Public** and explains that they are controller-readable.
MCP is deliberately narrower than the general channel system: its chat tools
accept only public `mainnode/forkmesh #general`.

During migration, a public channel can contain older recipient-sealed frames
and newer controller-readable frames. Participant clients try to open both;
MCP skips old sealed frames and returns the public history it can read.

## Protected conversation device identities

There is no phrase or setup step. On first use, each installation creates an
X25519 private key:

- the browser keeps it in same-origin local storage;
- desktop keeps it in local application settings; and
- mobile keeps it in the platform secure store.

The private key never appears in a request or frame. Clients exchange this
public announcement through the ordinary room cipher:

```json
{ "type": "e2ee-key", "v": 2, "kid": "…", "pub": "…" }
```

`pub` is the raw 32-byte X25519 public key in unpadded base64url. `kid` is the
first 16 characters of `base64url(SHA-256(pub))`. Clients retain at most one
announcement per room per day so an offline device can be discovered. Live
announcements are sent on connection and answered when a new device is learned.
Learned keys are scoped locally to both controller and room.

The controller can read these public announcements. They are not content keys
and do not enable passive decryption.

## Protected message envelope

For every protected application frame, the sender:

1. generates a fresh random 32-byte content key;
2. encrypts the complete JSON frame with AES-256-GCM;
3. derives an X25519 shared secret with each known room device;
4. runs that secret through HKDF-SHA256 to obtain a pairwise wrapping key; and
5. wraps the content key for every recipient, including the sender's device.

The inner frame is:

```json
{
  "type": "sealed",
  "v": 2,
  "kid": "sender key id",
  "pub": "sender X25519 public key",
  "nonce": "…",
  "tag": "…",
  "body": "…",
  "keys": [
    { "kid": "recipient key id", "nonce": "…", "tag": "…", "key": "…" }
  ]
}
```

That frame is then encrypted by the room cipher. The durable flag stays on the
outer envelope, so retention and replay work without exposing the protected
message's type, sender, or content.

### Exact derivation

For sender key id `S`, recipient key id `R`, room `ROOM`, and content nonce
`N`:

| Value | Definition |
| --- | --- |
| Shared secret | `X25519(senderPrivate, recipientPublic)` |
| HKDF salt | `SHA-256("ForkMesh chat e2ee salt:" + ROOM)` |
| HKDF info | `"forkmesh-chat-e2ee-kek-v2\n" + ROOM + "\n" + S + "\n" + R` |
| Wrapping key | HKDF-SHA256, 32 bytes |
| Content AAD | `"forkmesh-chat-e2ee-content-v2\n" + ROOM + "\n" + S` |
| Wrap AAD | `"forkmesh-chat-e2ee-wrap-v2\n" + ROOM + "\n" + S + "\n" + R + "\n" + base64url(N)` |

The AAD prevents a controller from moving a frame to another room, relabelling
its sender or recipient, or moving a wrapped key to another message without
failing authentication. A protected conversation refuses an application frame
that is not a valid v2 recipient envelope. Recipient lists are capped at 64
device keys, favoring the most recently announcing devices.

## User-visible behavior

The web and desktop access badges are informational:

- **Public** means the controller can read and author messages for integrations.
- **End-to-end** means device-to-device encryption is automatic and the
  controller's room key cannot read or inject the protected content.

A new installation can open protected messages sent after other participants
learn its public key. It cannot retroactively decrypt retained messages whose
content keys were never wrapped for it. Clients report that condition instead
of silently presenting an empty conversation.

## Security boundary and limits

For private channels and DMs, the protocol removes the controller's `DATA_KEY`
as a passive message-decryption key. An operator inspecting the relay database,
WebSocket traffic, or retained envelopes sees only recipient ciphertext.

Automatic public-key discovery is not authenticated membership. A controller
that actively injects a new public key can make future senders treat that key
as another device. Preventing that requires an independently authenticated
participant directory or human verification, neither of which exists uniformly
across browser, desktop, and mobile. An operator serving modified JavaScript can
also read a browser client's local data and plaintext.

Other limits:

- public channels are intentionally readable and writable by the controller;
- timing, sizes, connection metadata, public device keys, and durable flags
  remain visible even for protected conversations;
- this is not a double ratchet and provides no post-compromise security;
- compromise of one device exposes content keys wrapped for that device;
- losing a device private key loses history addressed only to it; and
- the controller can drop, delay, replay, or withhold frames and key
  announcements, causing denial of service.

## Implementations and contracts

| Piece | File |
| --- | --- |
| Browser primitives and identity store | `app/public/chat-seal.js` |
| Browser policy, send, receive, and discovery | `app/public/chat-room-transport.js` |
| Web surfaces | `app/public/chat.js`, `app/public/dashboard-chat.js` |
| Desktop primitives and transport | `desktop/src/RoomSeal.{h,cpp}`, `ServerNode.cpp` |
| iOS primitives and transport | `native/ios/ForkMeshCore/Sources/FMCrypto/RoomSeal.swift`, `native/ios/ForkMeshCore/Sources/FMChat/Relay/RelayService.swift` |
| Android primitives and transport | `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/crypto/RoomSeal.kt`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/chat/RelayRoom.kt` |
| MCP public-channel bridge | `tools/forkmesh_mcp_server.py` |
| Cross-client contracts | `app/tests/test_chat_room_seal_frontend.py`, `desktop/tests/test_crypto.cpp`, `desktop/tests/test_chat_transport.cpp`, `native/ios/ForkMeshCore/Tests/FMCryptoTests/RoomSealTests.swift`, `native/android/core/src/test/kotlin/com/forkmesh/nativeapp/crypto/RoomSealCryptoTest.kt` |

Browser, desktop, iOS, and Android pin the same v2 recipient-envelope fixture. A
change to X25519/HKDF derivation, AAD, base64 encoding, or the wire shape fails
the interoperability tests before protected conversations on different client
releases silently stop understanding one another.
