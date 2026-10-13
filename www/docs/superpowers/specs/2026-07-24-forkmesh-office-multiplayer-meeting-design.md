# ForkMesh Office Multiplayer Meeting Design

> **Historical record (2026-07-24).** This design or plan was written before
> the repository layout moved (`cloudflare_worker/` became `app/`, the
> marketing Worker became `www/`, World code moved to `world/`, `qt_client/`
> became `desktop/`), so its paths and step lists are out of date. It is kept
> for the reasoning. The shipped code is the authority:
> `world/public/world/world-office*.js`, `world/src/world_office_routes.py`,
> `world/src/world_durable_objects.py` (`ForkMeshOfficeRoom`),
> `app/src/chat_channels_api.py`, and `app/public/chat.js`. See
> `docs/world/README.md` in the repository for the current World architecture.

**Status:** Approved on 2026-07-24.

**Supersedes:** `2026-07-24-forkmesh-office-world-chat-design.md` wherever that document specifies a dock-first chat experience, excludes avatar speech bubbles, or forbids an Office-specific meeting presence service.

## Goal

Turn the ForkMesh Office into a real multiplayer meeting destination inside ForkMesh World.
Visitors enter the building, choose a channel they are authorized to access, join its spatial meeting room, see every live participant as an avatar, claim a chair, and communicate through encrypted messages rendered as bubbles above verified sender avatars.

The existing Office landmark, encrypted channel authorization, retained history, public and private channel rules, clipboard image sharing, and document sharing remain the foundation.
The existing compact chat embed becomes an accessible fallback and management path rather than the primary meeting experience.

## Product Experience

### Exterior and lobby

The existing `FORKMESH OFFICE` building remains at world coordinate `[11, 0, -21]`.
The existing approach hysteresis remains 6.5 world units to enter the nearby state and 7.5 units to leave it.
Clicking, tapping, or pressing `E` at the entrance transitions the visitor into an Office lobby rendered in the same Three.js canvas.

The lobby contains a meeting-room board.
The board lists only channels the current visitor may access.
Guests see only World `#general`.
Registered users see World `#general`, administrator-created public channels, and private channels where they are authorized members.
Administrators retain the existing implicit access rules.
Channel creation, membership management, and destructive moderation remain in full chat.

### Meeting room

Selecting a lobby room obtains fresh authorization for both meeting presence and encrypted chat before transitioning into the room.
The room uses one reusable interior template containing a central meeting table, eight chairs, bounded standing positions, a room label, a clear exit, and a compact composer.
Each authorized channel receives an isolated live meeting-presence namespace.
Private channel identifiers and membership never enter the global World presence stream.

Every connected meeting participant appears as an avatar.
The local avatar spawns at the room entrance.
Remote avatars render from the dedicated meeting-presence roster rather than the global town-square roster.
Participants without a chair remain standing at bounded positions that avoid overlap with chairs, exits, and the composer.

### Seating

Clicking or tapping a chair, or pressing `E` near it, requests that chair from the meeting-presence service.
The service grants a chair only when it is unoccupied.
Two concurrent claims for one chair must result in exactly one seated participant.
The losing participant remains standing and receives the text status `Seat just taken.`

While seated, local movement is paused and the avatar uses a seated pose aligned to the chair.
The visitor may stand using a visible control or `Escape`.
Standing releases the chair immediately.
Disconnecting, losing authorization, changing rooms, or leaving the Office releases the chair through live socket cleanup without persistent recovery work.

### Conversation

The meeting view uses a compact native composer rather than the full chat dock.
The composer supports text, pasted clipboard images, selected images, and selected documents through the existing chat limits and attachment model.
Pressing Enter sends text, while Shift+Enter inserts a line break.

New authorized messages from current meeting participants appear as DOM-projected bubbles above their avatars.
DOM projection keeps text sharp, accessible, responsive, and independent from Three.js texture resolution.
One bubble is visible per avatar and one additional bubble may be queued.
The newest bubble remains visible for seven seconds and then fades.
Reduced-motion mode changes bubble visibility without animation.

Text bubbles display at most 180 characters in the scene.
The complete authorized message remains available in the semantic transcript.
An image bubble shows a bounded thumbnail with a text alternative.
A document bubble shows a bounded filename chip and file type.
Opening an attachment uses the existing authorized in-memory attachment URL and download behavior.

Messages from channel participants who are not currently in the spatial meeting remain visible in the transcript but do not attach to an avatar.
They render with the neutral label `Remote channel participant`.

### Transcript and fallback

An optional transcript drawer mirrors every authorized message in chronological order.
The drawer is the semantic live log for assistive technology and the complete-history surface for long text, attachments, edits, deletions, and moderation events.
It is collapsed by default for sighted desktop users and remains reachable by keyboard.

If WebGL is unavailable, the existing full `/chat` route remains the functional fallback.
If native Office initialization fails after room authorization, the compact `/chat?embed=office` experience may open as a bounded fallback without creating a second simultaneous chat socket.

## Architecture

### Global World presence

The existing global World Durable Object continues to own town-square avatars, coarse movement, and public presence.
It may publish only the consent-controlled activity `visiting-office` while a visitor is inside the building.
It never receives or publishes a channel identifier, room name, private membership, message identifier, message content, attachment metadata, encryption material, typing state, chair claim, or Office participant key.

The town-square avatar remains at the Office entrance while the visitor is in an interior meeting room.
Other town visitors see only the existing coarse public activity if the visitor enabled activity sharing.

### Office meeting presence

A dedicated `ForkMeshOfficeRoom` Durable Object owns one live authorized meeting namespace.
It uses WebSocket Hibernation API attachments and no persistent Durable Object storage.
Its socket attachment contains the bounded participant id, public avatar fields, standing position, heading, pose, chair id, ephemeral avatar-binding public key, liveness timestamps, and rate-limit counters.

The routing Worker performs room authorization before resolving or fetching the Durable Object.
World `#general` accepts guests through a bounded anonymous meeting ticket.
Administrator-created channels reuse the existing channel membership and version checks.
The Durable Object name is derived server-side from an opaque room scope and channel key version.
A membership version change creates a different namespace and invalidates old tickets.

The service accepts only these client frame families:

- `presence` updates bounded public avatar preferences and one ephemeral P-256 public key.
- `move` updates bounded standing position and heading only while the participant is not seated.
- `seat-request` asks for one allowlisted chair id or releases the current chair.
- `ping` refreshes liveness without changing public state.
- `leave` provides an eager cleanup hint, while socket close remains authoritative.

The service emits `welcome`, `join`, `presence`, `move`, `seat`, `seat-denied`, `leave`, and `pong` frames.
It does not accept arbitrary event names or unknown fields.
It enforces message byte limits, fixed-window client rate limits, room-wide broadcast limits, stale-client cleanup, and a bounded room capacity.

### Shared encrypted chat transport

The current chat cryptography, room-access request, WebSocket lifecycle, retained-frame replay, attachment parsing, and durable-message behavior move into focused ES modules shared by full chat and the Office meeting client.
The full `/chat` page keeps its current UI and behavior while consuming the shared transport.
The Office meeting client consumes the same transport through callbacks for messages, edits, deletions, reactions, connection state, and authorization loss.

The chat relay continues to receive and retain ciphertext envelopes.
The meeting-presence Durable Object never receives chat envelopes or decrypted content.
The browser is the only place where authorized chat content and current meeting avatars meet.

### Verified avatar binding

Each Office meeting client generates an ephemeral non-exportable ECDSA P-256 private key when it joins a room.
It sends only the public JWK to the meeting-presence socket.
The server binds that public key to the sender's live participant attachment and publishes it to authorized peers in that meeting namespace.

When the Office composer creates a chat message, it adds a `meetingProof` object inside the encrypted plaintext message.
The proof contains the current meeting participant id, a bounded timestamp, the hash algorithm version, and an ECDSA signature.
The signature covers this canonical value:

```text
forkmesh-office-bubble-v1
<participant-id>
<message-id>
<chat-sender-id>
<timestamp>
<sha256-of-normalized-visible-content-and-attachment>
```

Recipients verify the signature against the public key on the matching live meeting participant before attaching the message to an avatar.
The content digest covers the complete text plus normalized attachment filename, MIME type, byte length, and content digest.
An invalid, missing, stale, or mismatched proof never creates an avatar bubble.
The message remains available in the authorized transcript using the normal unverified chat identity treatment.

Ephemeral keys expire when the meeting socket closes and are never stored in local storage, D1, KV, R2, URLs, analytics, or retained chat history outside the already encrypted message envelope.

## Authorization and Revocation

The Office lobby room list reuses the existing chat-channel listing endpoint and its nondisclosure rules.
Joining a room requires a short-lived Office meeting ticket in addition to normal chat room access.
The ticket includes only the opaque meeting scope, channel version, bounded account claim, expiry, and a nonce.
The browser never chooses the Durable Object name.

Removing a member, rotating a private channel version, deleting a channel, or disabling an account closes both chat-room sockets and Office meeting-presence sockets for the affected namespace.
The client clears bubbles, releases the chair, removes the room from the lobby, and returns to World `#general` or the Office lobby without exposing the denied room.

Office position never grants channel authorization.
The Office meeting ticket never grants chat authorization.
Both checks must succeed before the meeting composer becomes active.

## Responsive and Accessible Behavior

Desktop uses the full canvas for the interior with a bottom-centered composer and optional right transcript drawer.
Portrait mobile keeps the composer above the safe-area inset, uses tap targets of at least 44 CSS pixels, and frames the local avatar plus nearest chairs without covering the exit.
Landscape mobile uses a single-line composer and a bounded overlay transcript that never covers more than 46 percent of the viewport width.
The experience must not create horizontal overflow at 320 CSS pixels.

Every chair is represented by a semantic button in the scene overlay with an accessible label and occupied state.
Every avatar appears in an accessible participant list with standing or seated status.
Every visible bubble is mirrored in an `aria-live` transcript log without duplicate announcements.
Connection, authorization, room-full, seat-conflict, upload, and relay errors use visible text.

Keyboard behavior is fixed:

- `E` enters the Office or claims the focused nearby chair.
- `Enter` sends from the composer.
- `Shift+Enter` inserts a line break.
- `Escape` closes transient UI, stands if seated, or opens the leave-room confirmation in that order.
- The transcript and room board follow normal tab order.

## Failure and Recovery

If meeting presence connects but chat does not, avatars render and the composer shows `Encrypted chat is reconnecting.`
If chat connects but meeting presence does not, the transcript remains available and avatar bubbles stay disabled with `Meeting presence is reconnecting.`
If authorization expires, both sockets close and the visitor returns to the lobby with the existing actionable login message.
If a room is full, the visitor remains in the lobby and may open that channel in full chat.
If a chair claim races, the losing client remains standing and may select another chair.
If signature verification fails, no avatar is blamed and the message appears only in the transcript.
If an image cannot decode, the bubble and transcript show a safe attachment label without executing content.

Reconnect uses bounded exponential backoff.
The client obtains fresh room authorization before reconnecting after an authorization close.
It may request the previously occupied chair after reconnect, but the service grants it only if still free.
No client assumes seat ownership from local storage.

## File Boundaries

`src/world.py` owns pure validation and rate-limit helpers for both global and Office presence protocols without importing chat content logic.
`src/entry.py` owns routing, ticket verification, Durable Object wiring, revocation fan-out, and the `ForkMeshOfficeRoom` runtime class.
`src/chat_channels_api.py` owns reuse of existing channel membership decisions when issuing Office room authorization.

`public/chat-crypto.js` owns PBKDF2, AES-GCM envelope handling, byte conversion, and meeting-proof hashing and signing helpers.
`public/chat-room-transport.js` owns room access, WebSocket lifecycle, retained encrypted frames, reconnect, and normalized transport events.
`public/chat-attachments.js` owns bounded attachment encoding, validation, object URL lifecycle, and attachment metadata normalization.
`public/chat.js` remains the full chat UI consumer of those modules.

`public/world/world-office-meeting.js` owns lobby room selection, dual-socket lifecycle, meeting authorization, composer state, transcript state, avatar-proof verification, and cleanup.
`public/world/world-office.js` owns exterior proximity and Office entry and exit transitions.
`public/world/world-scene.js` owns Office interior geometry, meeting avatars, chair meshes, seated poses, projected bubble anchors, camera framing, and pointer interaction.
`public/world/world.js` wires the World shell, global presence, lobby, and meeting controller without implementing encryption.
`public/world/world.css` owns lobby, composer, transcript, bubble, seat, responsive, safe-area, and accessibility presentation.

The focused modules may expose narrow callbacks, but no module may import a UI controller merely to reach its internal state.

## Compatibility and Migration

The full `/chat` page remains available and retains its channel management, message history, reactions, edits, deletes, clipboard paste, and document upload behavior.
Existing encrypted chat envelopes without `meetingProof` continue to render normally.
They simply do not attach to an Office avatar.

The existing global World WebSocket path and protocol remain compatible.
The new Office meeting socket uses a separate route and Durable Object binding.
A Cloudflare Durable Object migration adds `ForkMeshOfficeRoom` without modifying existing chat or World object classes.
The production deployment configuration must bind the new class before the route can issue meeting tickets.

The current compact Office embed remains available as a fallback until the native meeting path has passed the complete E2E matrix.
The implementation must prevent the native transport and fallback iframe from connecting to the same room simultaneously.

## Testing Strategy

Backend unit tests cover ticket creation and expiry, authorization before Durable Object lookup, opaque instance names, public and private room rules, protocol sanitization, byte limits, rate limits, capacity, seat conflict, disconnect release, stale cleanup, and revocation fan-out.

Frontend contract tests cover shared crypto vectors, unchanged full-chat behavior, transport reconnection, attachment normalization, proof canonicalization, valid and forged proof handling, bubble queue limits, bubble expiry, transcript mirroring, and cleanup.

Playwright runs two independent browser contexts to cover real participant joins, avatar appearance, concurrent chair claims, standing, reconnect, leave cleanup, text bubbles, paste images, document selection, retained history, channel revocation, expired sessions, and relay failure.
Authorization journeys cover guest, registered public, invited private, uninvited private, administrator, deleted channel, and disabled account behavior.

Desktop, portrait, landscape, and 320 CSS pixel snapshots cover the exterior, lobby, standing room, seated room, simultaneous bubbles, image bubble, document bubble, transcript drawer, connection error, and room-full state.
Visual review rejects overlapping bubbles, unreadable avatars, clipped controls, obscured exits, unsafe-area collisions, and horizontal overflow.

Security tests assert that global World frames contain none of the forbidden chat and room fields.
They also assert that Office meeting frames contain no message text, attachment content, room keys, session tokens, or retained history.
Forged participant ids, copied signatures, stale proofs, altered text, altered attachment metadata, foreign origins, oversized frames, and unauthorized room tickets must fail closed.

## Acceptance Criteria

- Entering the ForkMesh Office opens a Three.js lobby rather than a conventional chat dock.
- The lobby lists exactly the channels authorized for the current visitor.
- Selecting a room creates an isolated authorized meeting presence and encrypted chat session.
- Every live meeting participant appears as an avatar.
- Eight chairs can be claimed without duplicate ownership.
- A disconnect or authorization loss removes the avatar and releases its chair.
- Valid new messages appear above the verified sender avatar and in the transcript.
- Invalid or absent meeting proofs never attach a message to an avatar.
- Text, clipboard images, and selected documents work through the same encrypted retained chat transport.
- Guests can join only World `#general`.
- Uninvited users cannot discover or enter private meeting rooms.
- Global World presence reveals no channel, membership, message, key, attachment, or typing information.
- Full chat remains functional and compatible with existing clients.
- Desktop, portrait, landscape, reduced-motion, keyboard, screen-reader, and 320 CSS pixel journeys pass.
- Production Worker configuration includes the new Durable Object binding and migration.

## Explicit Exclusions

Voice chat and live audio rooms are not part of this feature.
Video calls and screen sharing are not part of this feature.
Persistent seat reservations are not supported.
Users cannot create or manage channels inside the Three.js meeting room.
The Office does not expose private room identity through global World presence.
Typing indicators are not shown above avatars.
Messages from non-present channel participants do not receive synthetic avatars.
