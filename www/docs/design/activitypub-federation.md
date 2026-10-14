# ActivityPub federation (fediverse interop)

The relay joins the ActivityPub network (Mastodon-compatible) with followable
actors on its own domain.

| Actor | Handle | Actor URL | Type |
|---|---|---|---|
| User profile | `@alice@forkmesh.com` | `/ap/users/alice` | `Person` |
| Repository | `@owner.repo@forkmesh.com` | `/ap/repos/owner/repo` | `Group` |
| Service actor | `@forkmesh.com@forkmesh.com` | `/ap/actor` | `Application` |

User names never contain a dot, so `owner.repo` splits on the first dot.
Handles are lowercase. Only active public accounts and published public
repositories federate.

## What federates

**Outbound.** Verified signed-inbox submissions (issue, pull request and
discussion opens, comments and reviews) are posted as `Create(Note)` by the
repository actor and, when the author is a local account, by the user actor.
Releases and merges are announced through the owner-signed
`POST /api/repo/{owner}/{repo}/ap-publish`. Inline `data:` images in a body
(up to 4) are re-served at `/ap/o/{uuid}/media/{n}` and attached as `Image`.

**Inbound.** `Follow` (auto-accepted), `Undo(Follow)`, `Create(Note)` replies
and mentions, and `Delete`. Remote replies are stored as federated comments
(`GET /api/repo/{owner}/{repo}/fedi-comments`) and kept outside the
Ed25519-signed event log, because they can never carry a ForkMesh author
signature.

**Mentions never create issues automatically.** A verified public Note that
mentions a repository actor becomes an encrypted review record. The owner can
edit and explicitly authorize it as an issue-inbox submission; the feed says
`created` only after the owner's node commits the issue. A backlink reply is
sent only with separate consent.

## Wire protocol

- RSA-2048 key per actor, published as `publicKeyPem` (`<actor>#main-key`);
  private keys encrypted at rest.
- draft-cavage HTTP signatures (`rsa-sha256`). Outbound signs
  `(request-target) host date digest content-type`. Inbound requires
  `(request-target)` and `digest`, checks the digest byte for byte, enforces a
  ±12 h date window and requires the `keyId` host to match the actor. Remote
  keys are cached for 24 h with one forced refresh on failure.
- Discovery: `/.well-known/webfinger`, `/.well-known/nodeinfo` →
  `/nodeinfo/2.1`, and content negotiation on `/@name` and `/@owner.repo`.

The protocol core (documents, signing strings, digests, sanitization) is
`app/src/activitypub.py`, tested by `tests/test_activitypub.py`.

## Delivery

Each publish writes one `ap_outbox` row per destination inbox (shared inbox
preferred). A few are delivered inline; a periodic drain retries the rest with
exponential backoff (5 minutes to 24 hours, 8 attempts; 404/410 drop at once).
Actor keys are created lazily on first lookup.

## Profiles

- Instance and user actors fall back to the instance avatar and banner; a
  repository actor never uses instance branding.
- Repository owners can upload a logo and banner through
  `POST /api/repo/{o}/{r}/about`; otherwise the committed
  `.forkmesh/banner.png` and `logo.png` are used. The About description is the
  actor bio.
- Every actor publishes `PropertyValue` links (canonical page and relay)
  verified by a reciprocal `rel="me"` link.
- A repository actor's `url` is its `/@owner.repo` profile page.
- Saving a profile broadcasts `Update(actor)` to followers.
- Owners can list and delete federated posts (`POST /api/repo/{o}/{r}/ap-posts`);
  a delete sends `Delete(Tombstone)` to followers.

## Administration

Signed admin routes `GET /api/accounts/admin-ap` and
`POST /api/accounts/admin-ap-update` read state and switch federation:
`enable`/`disable` (while disabled every ActivityPub endpoint returns 404) and
`block`/`unblock` a domain, which rejects its activities and purges its
followers, cached actors and queued deliveries, subdomains included.
`PUBLIC_BASE_URL` sets the actor origin for self-hosted relays.

## Limitations

- Local users cannot follow remote accounts; inbound `Like`, `Announce` and
  `Update` are acknowledged and dropped.
- Follower collections expose `totalItems` only.
