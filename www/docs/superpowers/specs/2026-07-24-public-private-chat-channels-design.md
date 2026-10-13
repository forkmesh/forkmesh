# Public and Private Chat Channels Design

> **Historical record (2026-07-24).** This design or plan was written before
> the repository layout moved (`cloudflare_worker/` became `app/`, the
> marketing Worker became `www/`, World code moved to `world/`, `qt_client/`
> became `desktop/`), so its paths and step lists are out of date. It is kept
> for the reasoning. The shipped code is the authority:
> `world/public/world/world-office*.js`, `world/src/world_office_routes.py`,
> `world/src/world_durable_objects.py` (`ForkMeshOfficeRoom`),
> `app/src/chat_channels_api.py`, and `app/public/chat.js`. See
> `docs/world/README.md` in the repository for the current World architecture.

## Summary

ForkMesh administrators can create either public or private encrypted channels from the full chat page.
Public channels are discoverable and joinable by every active registered ForkMesh user.
Private channels remain discoverable and joinable only by current platform administrators and explicitly selected active registered users.
Guests remain limited to public World `#general` and cannot discover or enter administrator-created public channels.

## Creation Experience

The create-channel form contains a required visibility dropdown with `Private` and `Public` choices.
`Private` is the default so existing administrator expectations stay fail-closed.
Selecting `Private` reveals a searchable multi-user picker populated from the existing public registered-user directory endpoint.
Selecting `Public` hides and clears the initial-member picker because membership rows do not control public-channel access.
The administrator may select zero or more active registered users while creating a private channel.
The server validates every selected username and creates the channel and its initial membership rows atomically.
If any selected user is missing, inactive, duplicated after normalization, or beyond the member cap, no channel is created.

## Authorization Model

Only an active account with authoritative `users.is_admin = 1` may create either visibility type.
Only current administrators may list, add, or remove private-channel memberships.
Every active registered user may list, obtain room access for, and connect to a public channel.
Administrators and explicit members may list, obtain room access for, and connect to a private channel.
Guests, node-only sessions, disabled users, and missing sessions cannot access administrator-created channels of either type.
The Worker rechecks the current account and channel policy before issuing room access and before accepting a WebSocket.

## Storage and Compatibility

The existing encrypted `chat_channels.data` record gains a `visibility` field whose value is `public` or `private`.
Legacy channel records without that field are interpreted as `private` so an upgrade never widens access accidentally.
No plaintext visibility column or schema migration is required because the deployment already bounds the channel count at 100 records.
Non-admin listing may decrypt those bounded records and include a record only when it is public or the requester has a matching membership.
Private initial membership rows use the existing encrypted member payload and blind-index identity model.

## Atomic Creation

The API accepts `{ "name": "release-team", "visibility": "private", "members": ["alice", "bob"] }`.
The `members` field is optional and defaults to an empty array.
Public creation rejects a non-empty member list with `members_not_allowed` so stale or malicious clients cannot create misleading grants.
The API resolves and seals all members before writing anything.
It then submits the channel insert and all membership inserts through one D1 batch.
The response includes the normalized visibility and the validated initial member summaries.

## Room Isolation and Retention

Public and private administrator-created channels continue to use distinct relay-derived passphrases and versioned Durable Object room names.
Visibility changes are not part of this feature, so a channel cannot silently move between public and private after creation.
Private member removal rotates the version and closes the old room as before.
Public channels have no membership removal path and remain available to every active registered user.
Text, pasted images, and selected documents continue to use opaque encrypted retained frames.

## Interface Details

Room buttons expose `Public channel` or `Private channel` through their accessible labels and titles.
The selected channel header shows a compact visibility badge without reducing the message area on narrow screens.
The Members management action appears only for manageable private channels.
The create dialog uses the same dark neutral visual language, border radius, typography, and focus styling already present on `/chat`.
The user picker uses checkboxes rather than a native multi-select so names, selection state, keyboard focus, and empty results remain clear.

## Acceptance Criteria

- An administrator can create a public channel from the visibility dropdown.
- Every active registered user can discover and join that public channel.
- A guest cannot discover or join that public channel.
- An administrator can create a private channel and select registered users in the same form.
- Selected users receive immediate private access after the atomic create succeeds.
- Unselected users cannot discover or access the private channel.
- Existing channels without a visibility field remain private.
- Public creation cannot carry private membership rows.
- Failed initial-member validation leaves no partial channel or membership data.
- Text, clipboard images, and selected documents remain visible after refresh in both visibility types.
