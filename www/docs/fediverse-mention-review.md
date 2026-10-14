# Verified fediverse feedback review

World lists an inbound ActivityPub Note only after HTTP-signature verification,
when it addresses a published public repository actor and the ActivityStreams
Public collection. Private or missing repositories are rejected before a
record is written; instance blocks and the repository's `federate` and
`acceptComments` controls apply.

Mentions never create issues automatically. States: `review` (available for
owner review), `pending` (owner confirmed a draft and the encrypted owner inbox
accepted it), `created` (the owner node committed the issue and returned its
number in a signed acknowledgement). An item stays pending until the
owner node confirms the committed issue number.

Redeliveries are deduplicated by a blind index of the remote Note id. The
public feed exposes only a random id, sanitized author/post fields, repository
name, and generalized progress.

Follow-up consent is off by default. With consent, one idempotent public reply
with the issue link is queued only after owner-node confirmation.

API: `GET /api/world/fediverse-mentions`, and
`POST /api/world/fediverse-mentions/{id}/preview|create|moderate|materialized`.
The desktop inbox acknowledgement carries
`materialized={review-id}:{real-issue-number}` entries.
