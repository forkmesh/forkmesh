# Private Admin Chat Channels Design

> **Superseded security boundary (August 28, 2026):** This design accurately
> records the original relay-readable implementation, not current protected
> chat. Private channels and direct messages now add automatic X25519 recipient
> encryption inside the relay-derived room cipher; public channels remain
> controller-readable for integrations. See
> [`security/sealed-chat.md`](../../security/sealed-chat.md) for the shipped
> protocol, migration behavior, and active-controller limitations.

## Summary

ForkMesh will keep public World `#general` unchanged and add private channels that only platform administrators and explicitly invited registered users can access.
Platform administrator authority comes exclusively from `users.is_admin = 1`.
An invitation grants membership immediately, without a separate acceptance step.

Private channels will not reuse the existing authenticated room where channel names are only client-declared labels.
Each private channel will receive its own relay-derived shared key and Durable Object room namespace so membership can be enforced before clients receive the key or enter the room.
The relay controls `DATA_KEY`, so this remains encrypted shared-key chat rather than end-to-end encryption against the relay.

## Goals

- Allow a platform administrator to create a private channel.
- Allow any platform administrator to invite an active registered ForkMesh user by username.
- Make the channel appear automatically for the invited user.
- Allow any platform administrator to remove an invited member.
- Allow all current platform administrators to access and manage every private channel.
- Prevent other signed-in users and guests from discovering channel metadata, obtaining its key, opening its WebSocket, or reading its retained history.
- Preserve the existing public World `#general` behavior.
- Reuse the existing encrypted message protocol and bounded seven-day retained-history behavior.

## Non-goals

- Organization-owned channels or organization-team-derived membership are outside this change.
- User-created channels are outside this change.
- Invitation acceptance, email invitations, and invitation notifications are outside this change.
- Channel renaming, ownership transfer, deletion, and archival are outside this change.
- Per-channel roles beyond platform administrator and invited member are outside this change.
- Desktop-client channel management is outside this first implementation.
- Hiding message plaintext from the relay is outside this change because the existing protocol uses relay-derived shared keys.

## Authorization Model

Every request begins with the existing bearer session and resolves to an active user account.
Node-only sessions and unauthenticated requests cannot list or access private channels.

A requester may access a private channel when either condition is true:

- The requester's authoritative `users.is_admin` value is `1`.
- The requester's `user_bi` has a membership row for that channel.

Only current platform administrators may create channels, list channel members, add members, or remove members.
The server never trusts `isAdmin` from browser storage or request JSON.
It reads the authoritative database flag for every privileged mutation.

Missing and unauthorized channels return the same `404 not_found` response on metadata, key, and WebSocket routes.
This prevents those routes from becoming channel-existence oracles.

## Storage Model

Migration `0069` and the lazy schema will add `chat_channels` and `chat_channel_members`.

`chat_channels` contains:

- `channel_id`: a random opaque identifier used in API and room routing.
- `name_bi`: a keyed blind index of the normalized channel name for uniqueness checks.
- `data`: an encrypted object containing the display name and creator label.
- `created_by_bi`: the creating administrator's account blind index.
- `created_at`: the creation timestamp.
- `updated_at`: the latest membership or key-version change timestamp.
- `key_version`: a positive integer beginning at `1`.

`chat_channel_members` contains:

- `channel_id`: the referenced private channel.
- `member_bi`: the invited user's existing `user_bi`.
- `invited_by_bi`: the administrator who granted access.
- `joined_at`: the immediate membership-grant timestamp.
- A composite primary key on `channel_id` and `member_bi`.
- An index on `member_bi` for efficient per-user channel listing.

The channel name is normalized to a lowercase slug of 1 to 40 characters using letters, numbers, and internal hyphens.
The normalized name is stored only inside encrypted `data`; the blind index enforces global uniqueness without exposing it in a searchable plaintext column.
Channel identifiers are unguessable and are not derived from names.

The initial limits are 100 active private channels and 500 invited members per channel.
These fixed caps bound administrative mistakes and database work without adding a general quota system.

## Key and Room Isolation

The Worker derives a channel passphrase from `DATA_KEY`, `channel_id`, and `key_version` through the existing SHA-256-based room-passphrase pattern.
The Worker returns that passphrase only after the requester passes the channel authorization check.

The Durable Object room key includes both `channel_id` and `key_version`.
Messages therefore remain isolated from public `#general`, the legacy authenticated room, every other private channel, and earlier versions of the same private channel.

Adding a member does not rotate the key.
The newly added member can read retained history for the current channel version, which matches the expected behavior of directly joining an existing channel.

Removing a member increments `key_version` in the same logical operation that deletes membership.
Current administrators and remaining members reconnect into the new room version and receive a new passphrase.
The removed user cannot obtain the new passphrase or a new WebSocket ticket.
Previously shared messages and keys cannot be revoked from a user who already received them.
Old-version retained history remains subject to the existing seven-day expiry, but it is not replayed into the new version because the relay cannot safely decrypt and re-encrypt user history during membership removal.

## WebSocket Admission

Browser WebSockets cannot attach the existing bearer authorization header reliably, so private channels use a short-lived room ticket.
After channel authorization, the room-access endpoint returns the passphrase and an opaque signed WebSocket URL valid for 60 seconds.

The ticket binds the opaque channel ID, current key version, requester account blind index, and expiry.
The Worker validates the signature, expiry, channel version, and the requester's current admin-or-member authorization before selecting the Durable Object.
Only then does it forward the upgrade request to the room.

Tickets are bearer credentials during their short validity window and must never be written to persistent browser storage.
The client requests a fresh ticket whenever it reconnects.
Removal blocks new connections immediately and key rotation separates any already-open old-version socket from subsequent traffic.

## API Contract

### `GET /api/chat/channels`

Requires an active user session.
Administrators receive every private channel.
Other users receive only channels where they have membership.
Each item contains `id`, `name`, `updatedAt`, `keyVersion`, and `canManage`.
The response never contains the passphrase or member account blind indexes.

### `POST /api/chat/channels`

Requires a current platform administrator.
Accepts `{ "name": "release-team" }` and returns the created channel with status `201`.
Invalid names return `400 invalid_channel_name`, duplicate names return `409 channel_name_taken`, and the fixed channel cap returns `429 too_many_channels`.

### `GET /api/chat/channels/{channel_id}/members`

Requires a current platform administrator.
Returns invited active users as public username labels with `joinedAt`.
Platform administrators retain implicit access and are not duplicated as channel membership rows.

### `POST /api/chat/channels/{channel_id}/members`

Requires a current platform administrator.
Accepts `{ "username": "alice" }`.
The target must be an active registered user account.
The operation is idempotent and returns the current membership.

### `DELETE /api/chat/channels/{channel_id}/members`

Requires a current platform administrator.
Accepts `{ "username": "alice" }`.
Removing an existing member rotates the channel key version.
Removing a user who is not a member is an idempotent success without another rotation.

### `GET /api/chat/channels/{channel_id}/room-access`

Requires an authorized administrator or invited member.
Returns the channel identity, current key version, relay-derived passphrase, and short-lived WebSocket URL.
It uses `Cache-Control: no-store`.

### `GET /api/chat/channels/{channel_id}/ws?ticket=...`

Requires a valid short-lived ticket and a WebSocket upgrade.
The Worker rechecks the current channel version and current admin-or-member authorization before routing to the channel's Durable Object room.
Invalid, expired, removed, and unknown access all return `404 not_found`.

## Backend Structure

A focused `src/chat_channels_api.py` module will own input normalization, endpoint dispatch, channel and membership rules, and response shaping.
It will use a small runtime interface supplied by `src/entry.py` for sessions, encryption, blind indexes, D1 operations, auditing, passphrase derivation, and ticket issuance.
This keeps private-channel behavior testable without growing the already large Worker router further.

`src/urls.py` will own the compiled channel route patterns.
`src/entry.py` will connect those patterns to the API module and keep the security-sensitive Durable Object routing gate adjacent to the existing room routing.

All create, invite, removal, and denied privileged mutation attempts will use the existing metadata-only sensitive audit log.
Audit details will contain only allowlisted counts or reasons, never channel names, passphrases, tickets, or member usernames.

## Web Chat Experience

The full `/chat` three-pane experience is the management and participation surface for private channels.
Public World `#general` remains the first channel and stays available to guests.

After session hydration, the client requests the authorized private-channel list.
Private channels appear beneath `#general` and use a lock indicator.
Selecting one closes the previous room connection, fetches fresh room access, derives the channel-specific AES key, opens the ticketed WebSocket, and renders that channel's retained and live messages.

The client polls the lightweight channel list while the page is open.
A newly invited channel appears automatically.
If a membership is removed or a key version changes, the client closes the stale socket, clears the stale in-memory key, and either reconnects with fresh access or returns to `#general`.

Administrators see a Create channel control in the rooms pane and a Manage members control for the selected private channel.
Member management accepts an exact registered username and shows the server-validated invited-member list with remove actions.
Regular members do not see administration controls.
All controls have labels, keyboard behavior, visible focus states, loading states, and inline error messages.

The existing dashboard chat remains focused on public `#general` in this first implementation.
It will include a visible link to the full `/chat` channel directory so private-channel participation and management are discoverable from the dashboard.

Private passphrases, derived CryptoKeys, and tickets remain in memory only.
Only the selected channel ID may be stored locally as a preference, and the client must fall back to `#general` when that ID is no longer authorized.

## Encrypted Image and Document Attachments

Every browser chat surface will use the desktop-compatible encrypted attachment fields: `fileName`, `fileMime`, and base64 `file`.
This keeps images and documents interoperable with existing desktop clients and keeps attachment bytes inside the same AES-GCM encrypted message as the rest of the chat payload.

The full `/chat` page and the dashboard chat will expose an accessible attachment button backed by a file picker.
Pasting a clipboard image into either composer will send it as an image attachment while ordinary text paste continues unchanged.
The browser accepts one attachment at a time and caps the decoded file at 1 MiB so the twice-base64-encoded encrypted envelope remains within the Worker frame limit and Cloudflare D1's 2,000,000-byte row limit.

Image attachments render as bounded previews with an explicit download action.
Other documents render as filename, MIME type, and size cards with an explicit download action.
Filenames are reduced to their final path component and limited to 180 characters before they are displayed or used as download suggestions.
The client treats MIME labels as untrusted metadata and previews only `image/*` payloads that the browser can decode.

Attachment messages are durable chat messages and are replayed after refresh just like text messages.
The Worker will raise the encrypted retained-frame ceiling only as far as the D1 row limit safely permits and will add a 16 MiB aggregate retained-byte budget per room alongside the existing seven-day and 500-frame limits.
When a room exceeds that byte budget, the oldest encrypted frames are removed first.
The relay continues to store only opaque encrypted envelopes and does not inspect attachment contents.

## Error Handling

Authentication failures return `401 invalid_session` before database mutation.
Non-admin mutation attempts return `403 admin_required` and are audited as denied.
Unknown or inactive invitation targets return `404 user_not_found` without creating partial membership rows.
Unauthorized channel reads, room-access requests, and WebSocket upgrades return the same `404 not_found` as missing channels.
Database or encryption failures return a generic `500 unavailable` and do not expose ciphertext, SQL, keys, or exception details.

The browser keeps the current room usable when a create or invite request fails.
It displays actionable validation messages for expected errors and a generic retry message for unavailable responses.
It never reuses a room-access response after a membership or key-version change.

## Testing Strategy

Backend contract tests will exercise the API module through its runtime boundary with real authorization decisions and deterministic D1 fakes.
They will cover admin creation, non-admin denial, direct membership, admin implicit access, list filtering, inactive or missing users, idempotent addition, removal, key rotation, and indistinguishable missing-versus-unauthorized responses.

Room-access tests will prove that different channels and different key versions produce different passphrases and Durable Object room keys.
Ticket tests will cover signature tampering, expiry, wrong-channel replay, stale key versions, removed members, and administrators.

Schema and routing tests will pin the migration, lazy schema, indexes, route shapes, cache headers, and authorization gate occurring before Durable Object lookup.

Frontend unit-contract tests will pin session-authenticated API requests, removal of the insecure fixed authenticated-channel labels, in-memory key handling, list refresh, admin-only controls, and safe fallback to `#general`.

Attachment tests will pin the desktop-compatible wire fields, 1 MiB rejection path, safe filename handling, clipboard-image interception, image preview, document download cards, and dashboard/full-chat parity.

A Playwright end-to-end test will paste a real clipboard image and select a document through the file input, capture the encrypted frames, reload the page, replay retained frames, and verify that the user's own image and document remain visible and downloadable.

A Playwright end-to-end test will exercise the product as an administrator and an invited user with network and WebSocket fixtures.
It will verify channel creation, direct invitation, immediate channel visibility, encrypted message exchange, non-member exclusion, member removal, and fallback after key rotation.
The visual assertions will cover narrow and desktop layouts so the rooms list and administration controls do not overflow or obscure the composer.

## Acceptance Criteria

- A user with `users.is_admin = 1` can create a private channel from `/chat`.
- A non-admin cannot create channels or mutate membership through either the UI or direct API calls.
- An administrator can add an active registered user by username, and the channel appears for that user without acceptance.
- Administrators can access every private channel even without an explicit membership row.
- A user sees only `#general` and private channels where they are currently a member.
- An unauthorized user receives neither channel metadata nor room credentials and cannot enter the Durable Object.
- Two private channels use different passphrases and Durable Object room namespaces.
- Removing a member increments the key version and isolates subsequent traffic from the removed member.
- Existing public World `#general` behavior and retained history continue to work.
- The targeted backend, frontend, and browser tests pass without lint failures or warnings.
