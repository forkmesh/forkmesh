# Verified fediverse feedback review

ForkMesh World lists sanitized activity only after the inbound ActivityPub
Note has passed HTTP-signature verification and has addressed a published
public repository actor. The Create activity or embedded Note must also
explicitly address the ActivityStreams Public collection; a valid signature
does not turn a direct or private Note into public content. Private and missing
repositories are rejected before a review record is written. Instance blocks
and the repository's `federate` and `acceptComments` controls remain
authoritative.

Repository mentions do not automatically create issues. The lifecycle is:

1. `review`: a verified public post is available for authorized manual review.
2. `pending`: the repository owner previewed and edited the draft, explicitly
   confirmed it, and the encrypted owner inbox accepted it.
3. `created`: the owner node materialized and committed the open event, then
   returned the actual issue number in its signed inbox acknowledgement.

A proposed number is never displayed as a created issue number. Delayed or
offline owner nodes leave the item pending until the owner node confirms the
committed issue number. Queue failures remain retryable and the public feed
exposes only a generalized failure message.

The owner can dismiss or restore reviewable activity with an audited reason.
Redeliveries are deduplicated by a blind index of the public remote Note id.
The public feed exposes a random record id, sanitized public author/post fields,
repository display name, and generalized progress. It does not expose raw IP
addresses, inbox URLs, signing keys, internal evidence, session data, encrypted
payloads, or private repository existence.

Follow-up consent is off by default. The owner must separately consent during
manual creation. Even with consent, no outbound reply is sent while the item is
merely pending. After owner-node confirmation, ForkMesh queues one idempotent
public reply to the original public Note with the confirmed issue backlink. A
delivery failure does not undo the issue and is shown only as delayed follow-up.

API surface:

- `GET /api/world/fediverse-mentions` — privacy-safe public activity feed.
- `POST /api/world/fediverse-mentions/{id}/preview` — owner-authorized draft.
- `POST /api/world/fediverse-mentions/{id}/create` — explicit manual queueing.
- `POST /api/world/fediverse-mentions/{id}/moderate` — audited dismiss/restore.
- `POST /api/world/fediverse-mentions/{id}/materialized` — owner-node signed
  confirmation and idempotent retry path.

The Qt inbox acknowledgement normally carries bounded
`materialized={review-id}:{real-issue-number}` entries, so confirmation and
inbox drain share the same owner signature.
