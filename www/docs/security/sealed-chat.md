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
cannot read or post recipient-sealed messages. Making a channel public affects
only new messages; making it private rotates the room key and revokes its
incoming webhooks.

## Device identities

Each installation creates an X25519 key pair on first use (browser local
storage, desktop settings, mobile secure store). The private key never leaves
the device. Public keys are announced through the room cipher:

```json
{ "type": "e2ee-key", "v": 2, "kid": "…", "pub": "…" }
```

`pub` is the raw 32-byte public key in unpadded base64url; `kid` is the first
16 characters of `base64url(SHA-256(pub))`. Learned keys are scoped to
controller and room.

## Protected message envelope

For each protected frame the sender generates a random 32-byte content key,
encrypts the JSON frame with AES-256-GCM, and wraps the content key for every
known room device (including its own) with a key derived from X25519 and
HKDF-SHA256:

```json
{
  "type": "sealed", "v": 2, "kid": "…", "pub": "…",
  "nonce": "…", "tag": "…", "body": "…",
  "keys": [{ "kid": "…", "nonce": "…", "tag": "…", "key": "…" }]
}
```

For sender `S`, recipient `R`, room `ROOM` and content nonce `N`:

| Value | Definition |
| --- | --- |
| Shared secret | `X25519(senderPrivate, recipientPublic)` |
| HKDF salt | `SHA-256("ForkMesh chat e2ee salt:" + ROOM)` |
| HKDF info | `"forkmesh-chat-e2ee-kek-v2\n" + ROOM + "\n" + S + "\n" + R` |
| Wrapping key | HKDF-SHA256, 32 bytes |
| Content AAD | `"forkmesh-chat-e2ee-content-v2\n" + ROOM + "\n" + S` |
| Wrap AAD | `"forkmesh-chat-e2ee-wrap-v2\n" + ROOM + "\n" + S + "\n" + R + "\n" + base64url(N)` |

The AAD binds each frame to its room, sender and recipient. Protected
conversations reject frames that are not valid v2 envelopes. Recipient lists
are capped at 64 devices.

## Limits

- Key discovery is not authenticated membership: an active controller can
  inject a key, and a modified web client can read browser plaintext.
- Timing, sizes, connection metadata and public keys remain visible.
- No ratchet and no post-compromise security; a compromised device exposes the
  content keys wrapped for it.
- A new device cannot read messages sent before its key was known.
- The controller can drop, delay or replay frames.

## Implementations

Browser (`app/public/chat-seal.js`, `app/public/chat-room-transport.js`),
desktop (`desktop/src/RoomSeal.{h,cpp}`), iOS (`FMCrypto/RoomSeal.swift`) and
Android (`crypto/RoomSeal.kt`) pin the same v2 envelope fixture, so any change
to derivation, AAD, encoding or wire shape fails the cross-client tests.
