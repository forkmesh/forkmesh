# ActivityPub federation (fediverse interop)

Status: implemented in the relay Worker (2026-07). Desktop/host wiring for
release + merge announcements is a follow-up (the relay endpoint is live).

## What federates

ForkMesh joins the ActivityPub network (Mastodon-compatible) with two kinds of
followable actors on the relay's domain:

| Actor | Handle | Actor URL | Type |
|---|---|---|---|
| User profile | `@alice@forkmesh.com` | `/ap/users/alice` | `Person` |
| Repository | `@owner.repo@forkmesh.com` | `/ap/repos/owner/repo` | `Group` |
| Service actor | `@forkmesh.com@forkmesh.com` | `/ap/actor` | `Application` |

User names can never contain a dot, so `owner.repo` splits unambiguously on
the first dot (repo names may themselves contain dots). Handles and actor ids
are lowercase-normalized. Only active, public user accounts and published,
non-private repos federate; the visibility checks mirror the public-profile
and catalog rules.

Outbound: verified signed-inbox submissions federate as `Create(Note)` —
issue opens/comments, PR opens/comments/reviews, discussion opens/comments.
The repo actor posts to its followers; when the event's
author maps to a local account, the user actor posts to theirs as well.
Releases and merges never pass through the relay inboxes (they are canonical
on the owner's node), so the owner-signed `POST
/api/repo/{owner}/{repo}/ap-publish` endpoint lets the desktop push those
announcements; until the desktop adopts it they simply don't federate.

Images attached to an issue/comment travel as inline `![name](data:...)`
markdown in the body (there is no separate upload channel). Since remote
servers can't fetch a `data:` URL, `ap.extract_body_images` pulls up to 4 of
them out of the body before it becomes note text, and each is re-served at
`/ap/o/{uuid}/media/{n}` (from the same encrypted `ap_objects` row) so it can
be listed as a proper `Image` `attachment` on the Note.

Inbound: `Follow` (auto-accepted), `Undo(Follow)`, `Create(Note)` replies to
our objects, `Create(Note)` posts that Mention a repo actor, `Delete`. Remote
replies become **federated comments** — stored in `ap_comments`, surfaced via
`GET /api/repo/{owner}/{repo}/fedi-comments?kind=issue&number=N`, and shown
as clearly-marked fediverse comments. They are deliberately kept OUTSIDE the
Ed25519-signed event log: a remote reply can never carry a forkmesh author
signature, so it must never enter the signed spine that nodes replicate.

**Repo mentions → manual issue review.** A post whose Mention tag points at a
repo actor ("@owner.repo@forkmesh.com the save button crashes") never creates
an issue automatically. After ActivityPub HTTP-signature verification and the
public-repository/federation gates, the Create or embedded Note must explicitly
address the ActivityStreams Public collection before a sanitized projection
enters the World's verified-public-feedback feed. A valid signature alone
does not publish a direct/private Note. Remote content, repository routing,
consent, and failure detail remain in an encrypted review record; its
blind-indexed remote-note id deduplicates redelivery.

An authenticated repository owner can open a preview, edit the proposed title
and body, and explicitly authorize a pending issue-inbox submission. Follow-up
consent is a separate, default-off checkbox. The feed says `pending` while the
encrypted inbox row waits for the owner node. The Qt node reports the real
issue number in its signed acknowledgement only after `IssueStore` has
materialized and committed the open event; only then does the feed say
`created`. If separately authorized, the repo actor queues one deduplicated
public reply with the confirmed issue backlink. Failures keep a generalized
retry/delayed status and never turn a proposal or proposed number into a
creation claim.

Legacy `ap_mentions` rows are still honored as dedupe tombstones, so a note
auto-processed by an older deployment cannot reappear as a new review item.
New notes use `world_fediverse_mentions`; no AI classification or attachment
fetch occurs during inbox handling.

## Wire protocol

Follows Mastodon/Wildebeest practice:

* RSA-2048 keypair per actor, public key published as `publicKeyPem`
  (`<actor>#main-key`), private key stored AES-GCM-encrypted under `DATA_KEY`
  (the standard `encrypt_row` pattern) in `ap_actors`.
* draft-cavage HTTP signatures. Outbound signs
  `(request-target) host date digest content-type` (GETs:
  `(request-target) host date accept`), `algorithm="rsa-sha256"`. Inbound
  verifies whatever header list the peer signed but REQUIRES
  `(request-target)` and `digest`, checks `Digest: SHA-256=<b64>`
  byte-for-byte, enforces a ±12 h date window, and requires the `keyId` host
  to match the activity actor's host. Key fetches are signed with the
  service actor (secure-mode servers answer) and cached in
  `ap_remote_actors` for 24 h with one forced refresh on verify failure
  (key rotation).
* Discovery: `/.well-known/webfinger`, `/.well-known/nodeinfo` →
  `/nodeinfo/2.1`, and content negotiation on `/@name` (users) and
  `/@owner.repo` (repos): an ActivityPub `Accept` header gets the actor
  document, a browser gets the HTML profile page.

The pure protocol spine (documents, signing strings, digest, sanitization)
lives in `app/src/activitypub.py` — stdlib-only, imported
directly by `tests/test_activitypub.py`. WebCrypto RSA glue, D1 state and
handlers live in `entry.py` (section "ActivityPub federation").

## Delivery without Queues (free plan)

There is no Cloudflare Queues binding. A publish writes one `ap_outbox` row
per unique destination inbox (sharedInbox preferred), best-effort drains 5
inline, and the per-minute cron's `minute % 5 == 0` slot drains 20 more with
exponential backoff (5 m → 24 h cap, 8 attempts, 404/410 drop immediately).
Cost while unused is near zero: actor rows are minted lazily on the first
WebFinger lookup, so an unfollowed repo's publish path is two indexed
SELECT misses.

## Branding

Every actor ships an avatar (`icon`) and profile header (`image`):

* Defaults: the brand images at `/assets/fediverse-avatar.png` (400×400) and
  `/assets/fediverse-banner.png` (1500×500 — Mastodon's exact header size).
  These are the **instance's own** identity, so only the instance actor and
  user actors fall back to them. A repo actor never does: a ForkMesh-branded
  header on somebody else's project reads as the project being ours.
* Per-repo overrides: the repo owner uploads `logoPng` / `bannerPng` (base64
  PNG, ≤256 KB / ≤1 MB) through the existing About endpoint
  (`POST /api/repo/{o}/{r}/about`, session-auth, `""` clears). They are stored
  encrypted in `repo_media` (one row per image so a banner never nears D1's
  2 MB value cap) and served at `/api/repo/{o}/{r}/media/{logo|banner}.png`
  with a `?v=<updated_at>` cache-buster in the actor document. The About
  `description` doubles as the repo actor's fediverse bio, and a user actor's
  bio comes from their profile `profile_bio`.
* Committed repo artwork (`_committed_repository_banner_url`): with no
  uploaded banner, a repo actor's `image` is the project's own
  **`.forkmesh/banner.png`**, served immutably as
  `/api/repo/{o}/{r}/raw?path=.forkmesh%2Fbanner.png&ref=<commit>` — the
  pinned commit is the cache-buster, so pushing new artwork refreshes it.
  Catalog metadata only lists top-level names, so the URL is a *candidate*:
  it is skipped outright when the repo has no `.forkmesh` directory, and a
  404 from the mirror means "no banner" (the actor then ships no `image` at
  all, and the About card / fediverse profile page drop the header strip
  rather than substitute brand artwork). The avatar keeps its own
  precedence — uploaded, then committed root `logo.png`, then approved
  artwork, then the brand default.
* Profile metadata (verified links): every actor ships `attachment`
  PropertyValue rows — the canonical page (`Repository` for repo actors,
  `Profile` for users) and the `Relay` it lives on (origin-derived, so
  self-hosted relays advertise their own domain). The row is
  Mastodon-verifiable: the served page carries a reciprocal `<link rel="me">`
  pointing back at the actor's `url` (injected per-page at serve time by
  `_serve_repo_page` / `_serve_repo_profile_page`, and baked into
  `_public_profile_html`), which is what turns the row green.
* Actor `url` (click-through target): Mastodon sends anyone who clicks a
  handle — a mention in a post, or the profile's external-link — to the
  actor's `url`. For **users** that is the `/@name` profile page. For **repos**
  it is the `/@owner.repo` **fediverse profile page** (`_serve_repo_profile_page`:
  banner, avatar, bio, follower/post counts and a feed of the repo's federated
  posts), *not* the raw git page — landing a social-timeline visitor on a code
  forge was jarring (adhoc #50). The git page stays the verified `Repository`
  row and is linked prominently on the profile; because the `Repository` row
  now differs from `url`, the git page's `rel="me"` points at `/@owner.repo`.
* Change propagation: saving the repo About (or a user profile) broadcasts an
  `Update(actor)` activity to all existing followers, so remote servers
  refetch the avatar/header/bio immediately instead of waiting out their
  actor-cache TTL.
* Web surface: `GET /api/repo/{o}/{r}/about` (public) returns the description,
  logo/banner URLs and `{handle, followers}` for the repo actor. The dashboard
  repo page uses it for the social badge header above About and the Watch
  button, whose count IS the fediverse follower count (the button opens a
  follow-from-Mastodon card with the copyable handle). The gear editor
  uploads/removes the logo and banner and edits the description; the page's
  displayed About text prefers the repo's committed `.forkmesh/info.json`
  (about + website), matching the desktop app. `bannerUrl` may be empty and
  has no default, so the card probes the image and drops the header strip
  (releasing the logo's overlap offset) when the repo has no banner of its
  own — it never substitutes brand artwork. The About rail also carries an
  owner-only **Fediverse posts** dropdown (issue #426): `POST /api/repo/{o}/{r}
  /ap-posts` (session/owner-key authed, `action: list|delete`) lists the repo
  actor's federated posts and deletes one. A delete drops the local `ap_objects`
  row (the `/ap/o/{uuid}` Note starts 404ing and it leaves the profile feed) and
  queues a `Delete(Tombstone)` to every follower inbox through `ap_outbox`, so
  the post disappears from Mastodon timelines too. The `actor_bi` filter keeps
  one owner's token from reaching another actor's objects.

## Configuration

Operator config is admin-managed (signed admin API, same gate as the relay
allowlist — the caller signs with their node key and must carry `is_admin`):

* `GET /api/accounts/admin-ap?node=&ts=&sig=` — signature over
  `forkmesh-admin-ap-v1\n<node>\n<ts>`. Returns `{enabled, blockedDomains,
  stats}` (actors, followers, remote actors, objects, comments, queued
  deliveries).
* `POST /api/accounts/admin-ap-update` — body `{node, ts, sig, action,
  domain?}`, signature over
  `forkmesh-admin-ap-update-v1\n<node>\n<action>\n<domain>\n<ts>`. Actions:
  `enable` / `disable` (global switch — while disabled every AP endpoint 404s,
  publish hooks no-op and the cron drain pauses) and `block` / `unblock`
  (defederation — blocking a domain rejects its inbound activities, refuses
  actor fetches, and purges its followers, cached actors and queued
  deliveries, subdomains included).

Settings live in `ap_settings` / `ap_blocked_domains` with a 60 s per-isolate
cache. Deployment-level config stays in wrangler vars: `PUBLIC_BASE_URL`
(actor-id origin for self-hosted relays; defaults to the request host).

The Solana payout relay mesh is a separate system; its canonical routes moved
to `/api/relay-mesh/*` with `/api/federation/*` kept as a served legacy alias
(federated relays also retry the legacy path against older main relays), so
the two federations no longer share a name.

## Known limitations / follow-ups

* Desktop node should call `ap-publish` on release publish + PR merge, and
  render federated comments in issue/PR/discussion views (API is live).
* Local users cannot yet follow remote fediverse accounts (no outbound
  Follow); Like/Announce/Update inbound are acknowledged and dropped.
* Followers collections expose `totalItems` only (no paging) — Mastodon is
  fine with this.
* Object content is public by design (public repos only), stored encrypted
  at rest like everything else.
