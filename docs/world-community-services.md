# ForkMesh World community services

The shared services of ForkMesh World: the event calendar, the Fediverse
directory, shared media rooms, community-reviewed placements and the World
song. World keeps this data in the database and serves it over HTTPS; the
multiplayer socket carries only presence and movement. Empty or unavailable
data is shown as such, never as demo content. All mutations and denied
attempts write metadata-only entries to `sensitive_audit_log`. How World is
built is on the [World home page](world/README.md).

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
stored. Repository posts and replies are on
[Fediverse digests, mentions and replies](fediverse.md).

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

| Limit | Value |
| --- | --- |
| Spaces per owner | 5 |
| Items per space | 100 |
| Schedules per space | 50, UTC, at most one year ahead and 24 hours long |
| Moderators per space | 20 |
| Removed items and finished schedules | Purged after 30 days |
| Idle spaces | Archived after 180 days, purged 90 days later by the hourly retention cron |

## Community-reviewed placements

Approval means disclosed public evidence met an instance's current criteria
for a limited period; it is not an endorsement and can be appealed, suspended,
or expire. Criteria are served by `GET /api/world/community-ads/criteria`.
Proposals disclose sponsor, ownership, product, data use, tracking, labor,
accessibility, and impact evidence, with at least two public HTTPS sources.

Voting is one eligible verified account per proposal (active, email-verified,
at least seven days old), never wallet-weighted. Conflicted voters may only
abstain. Approval needs quorum and two-thirds of non-abstaining votes; review
deadlines are at most 180 days.

Each instance is disabled by default and chooses placement contexts and a
public revenue purpose. Placements receive only a coarse page context such as
`town-square` or `repository`; targeting does not use profiles, device or
country data, search terms, wallets, or behavioral history. Responses carry
`tracking: none`. Member text ads are a separate system,
[ForkMesh Ads](operations/ad-network.md); a community placement takes
priority.

Receipts use the `community_ad_revenue` ledger class. They are not user funds,
rewards, or on-chain transfers; the app does not custody money.

## World song

The World entrance sign links to a first-party track published by the project
at <https://www.youtube.com/watch?v=EsQMZuS6lbE>; it opens in a new tab and no
audio file is bundled or served. All other World sounds are short effects
synthesized locally with the Web Audio API.
