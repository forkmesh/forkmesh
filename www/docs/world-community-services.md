# ForkMesh World community services

ForkMesh World uses D1 for its public Fediverse directory, authenticated
shared-media coordination, and UTC event announcements. The implementations
are migrations `0052_world_fediverse_media.sql` and
`0058_world_events.sql`, `0059_world_media_playback.sql`, and
`0061_world_social_directory.sql`.

These services do not use the multiplayer WebSocket. Directory records, room
lists, playlists, schedules, and moderation changes use ordinary HTTPS. The
socket remains limited to transient presence, movement, and small world events.

## UTC community events

`GET /api/world/events` and `GET /api/world/events/{event-id}` are public,
short-cacheable reads of scheduled events whose `endsAt` is still in the
future. The production client has no built-in event rows: an empty or
unavailable API produces an explicit empty or unavailable state, never a demo
announcement presented as live.

Only a signed-in platform administrator may create, update, or cancel:

- `POST /api/world/events`
- `PATCH /api/world/events/{event-id}`
- `DELETE /api/world/events/{event-id}`

Both `startsAt` and `endsAt` must be explicit ISO-8601 UTC strings ending in
`Z`. Events last at most seven days, may be scheduled at most two years ahead,
and use bounded type, title, description, and destination fields. `DELETE`
marks an event cancelled; cancelled and expired records are excluded from
public reads immediately. Retained old rows are pruned on later mutations and
by the hourly privacy-retention cron. A D1 trigger caps the catalog at 500 rows
even if administrator writes race.

Creates, updates, cancels, and denied attempts use the metadata-only sensitive
audit trail. The author blind index remains private and is never included in a
public response.

## Fediverse directory

`GET /api/world/fediverse` is public. It returns separate `mastodon`, `lemmy`,
`x`, and `reddit` arrays and the privacy policy applied to every record. The
world renders those records around their corresponding social centers.

The static `world/public/world/fediverse-directory.json` file is no longer the runtime data
source. An empty D1 directory is shown honestly as empty.

Only a signed-in platform administrator may mutate directory records:

- `POST /api/world/fediverse`
- `PATCH /api/world/fediverse/{instance-id}`
- `DELETE /api/world/fediverse/{instance-id}`

Every create or update must include:

```json
{
  "publicOnly": true,
  "consentConfirmed": true,
  "consentSource": "instance-operator",
  "consentCheckedAt": 1800000000000
}
```

`consentSource` is one of `instance-operator`, `public-api`,
`user-approved`, or `moderator-reviewed`. The timestamp is UTC milliseconds
and must describe a check within the previous year.

Instance base URLs must be public HTTPS origins. Credentials, fragments,
queries, paths below `/`, IP literals, localhost, internal names, and
non-default ports are rejected. Icons must be public HTTPS URLs on the same
instance host. Text, language, topic, community, relationship, and connection
lists all have server-side limits.

The API has no field for raw IPs, user agents, email addresses, search terms,
browsing history, private accounts, or an unfiltered follower/subscription
graph. A `consentedFollowers` or `approvedSubscriptions` entry is accepted only
when both `public` and `consent` are exactly `true`; a follower must also be a
public account. Invalid relationship data rejects the whole mutation rather
than being silently published.

X and Reddit use `consentedProfiles`. Each record requires a public platform
profile URL, explicit `public` and `consent` flags, public account visibility,
and an evidence source/time. Mastodon follower entries inherit the record
attestation unless a more specific valid source/time is supplied. The public
projection persists that bounded evidence as an `operator-attestation` and
always sets `oauthVerifiedByForkMesh` to `false`. It contains no OAuth token or
remote private identifier. These records describe an operator-recorded
attestation; they do not claim that ForkMesh independently verified remote
account ownership.

All create, update, delete, and authorization-denial decisions append
metadata-only entries to `sensitive_audit_log`. Audit targets are opaque/token
values; directory descriptions and relationship content are not copied into
the audit log.

## Shared media spaces

Every media endpoint requires a valid ForkMesh account session:

- `GET /api/world/media/spaces`
- `POST /api/world/media/spaces`
- `GET /api/world/media/spaces/{space-id}`
- `PATCH /api/world/media/spaces/{space-id}`
- `DELETE /api/world/media/spaces/{space-id}`
- `POST /api/world/media/spaces/{space-id}/items`
- `DELETE /api/world/media/spaces/{space-id}/items/{item-id}`
- `GET|POST|PATCH /api/world/media/spaces/{space-id}/playback`
- `POST /api/world/media/spaces/{space-id}/stop`
- `POST /api/world/media/spaces/{space-id}/schedules`
- `DELETE /api/world/media/spaces/{space-id}/schedules/{schedule-id}`
- `GET|POST /api/world/media/spaces/{space-id}/roles`
- `DELETE /api/world/media/spaces/{space-id}/roles/{role-id}`

The creator is the room owner. Owners may update/archive a room and
grant/revoke explicit moderator roles. Owners and explicit moderators may add
or remove playlist links, create or cancel schedules, and stop shared playback
state. A platform administrator does not automatically inherit media-room
control.

The Worker derives the actor from the signed session, never from a body
`owner`, `actor`, or `moderator` field. Role checks are repeated server-side for
every mutation. All sensitive changes and denied role checks use the existing
metadata-only audit log.

### Provider-link policy

Playlist entries are links, not media objects:

```json
{
  "title": "Community listening hour",
  "provider": "somafm",
  "url": "https://somafm.com/groovesalad/",
  "termsConfirmed": true,
  "noRebroadcast": true,
  "autoplay": false
}
```

Supported provider-page families are SomaFM, YouTube, Vimeo, SoundCloud,
Twitch, Internet Archive, and public PeerTube watch/embed pages. Hostnames and
provider-specific query keys are allowlisted. Raw `.mp3`, `.mp4`, `.m3u8`, and
similar media/playlist file URLs are rejected.

ForkMesh stores only the bounded title, validated external provider page, and
the no-autoplay/no-rebroadcast policy flags. It does not fetch, proxy, cache,
record, copy, or rebroadcast the linked media. Opening or playing a provider
page remains an individual, explicit client action subject to that provider's
terms and controls.

A playlist title is a user-supplied coordination label, not the provider's
current track. The API currently returns provider metadata as
`unavailable/not_received`; user input cannot override that result. The world
therefore says current provider track metadata is unavailable unless a future
provider-authorized integration actually supplies a permitted metadata event.
Locally generated focus tones are labeled as local synthesis, where provider
track metadata is not applicable.

The shared playback endpoint stores only item id, state, position, UTC change
time, and an optimistic revision. It coordinates clients but cannot start
playback on their devices. Every client must open the provider and consent
locally.

### UTC schedules and retention

Schedules use UTC millisecond `startsAt` and `endsAt` values. They may be at
most one year in the future and last at most 24 hours. The world converts the
start time for local display while retaining UTC as the shared event time.

Storage and response sizes are bounded:

- five active spaces per owner;
- 100 active playlist entries per space;
- 50 active schedules per space;
- 20 explicit moderators per space;
- 50 spaces in one listing response.

Removed/stopped playlist entries and completed/cancelled schedules are purged
after 30 days. A space with no activity for 180 days becomes archived; archived
spaces and their child records are purged after another 90 days. The hourly
privacy-retention cron applies this policy even when nobody opens the world.

## World rendering

The browser loads the D1 directory from `/api/world/fediverse` and unexpired
announcements from `/api/world/events`. Signed-in visitors also load
`/api/world/media/spaces` and a selected space detail. None of this service
state is persisted in local storage.

The social center renders Mastodon/Lemmy services and consent-attested X/Reddit
public-profile records plus only explicitly public, consented connection
markers. The Broadcast Garden renders shared-room
nodes, external playlist markers, and UTC schedule markers. These 3D objects
are visualizations of HTTPS API records; they are not media playback,
surveillance, or a claim that ForkMesh has content custody.
