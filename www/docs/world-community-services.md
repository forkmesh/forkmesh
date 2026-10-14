# ForkMesh World community services

World keeps its event calendar, Fediverse directory and shared media rooms in
the database and serves them over HTTPS; the multiplayer socket carries only
presence and movement. Empty or unavailable data is shown as such, never as
demo content. All mutations and denied attempts write metadata-only entries to
`sensitive_audit_log`.

## UTC community events

`GET /api/world/events[/{id}]` is public and lists unexpired events.
Administrators use `POST`, `PATCH` and `DELETE` (cancel). `startsAt` and
`endsAt` are ISO-8601 UTC ending in `Z`; events last at most seven days, start
at most two years ahead, and the catalog is capped at 500 rows.

## Fediverse directory

`GET /api/world/fediverse` returns `mastodon`, `lemmy`, `x` and `reddit`
arrays. Administrator writes require `publicOnly`, `consentConfirmed`,
`consentSource` (`instance-operator`, `public-api`, `user-approved`,
`moderator-reviewed`) and a `consentCheckedAt` within the last year. Origins
must be public HTTPS with no credentials, paths, IPs or custom ports.
Relationship entries are accepted only when `public` and `consent` are both
`true`; X and Reddit records are operator attestations with
`oauthVerifiedByForkMesh: false`. No raw IPs, emails or private graphs are
stored.

## Shared media spaces

`/api/world/media/spaces` and its `items`, `playback`, `stop`, `schedules` and
`roles` sub-routes require a session. The creator owns a space and may grant
moderators; owners and moderators manage playlists, schedules and playback.
The actor always comes from the session.

Playlist items are links to allowlisted provider pages (SomaFM, YouTube,
Vimeo, SoundCloud, Twitch, Internet Archive, PeerTube) with
`termsConfirmed`, `noRebroadcast` and `autoplay: false`; raw media files are
rejected. ForkMesh never fetches or rebroadcasts media, and shared playback
state only coordinates clients.

Limits: 5 spaces per owner, 100 items, 50 schedules, 20 moderators per space.
Schedules are UTC, at most one year ahead and 24 hours long. Removed items and
finished schedules are purged after 30 days; spaces idle 180 days are archived
and purged 90 days later by the hourly retention cron.
