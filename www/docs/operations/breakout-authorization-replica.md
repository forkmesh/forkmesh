# Breakout authorization replica

Mirrors replicate repository data through Git and signed ledgers. Managing
repositories while the controller is unreachable also needs authorization
data, but copying account or session tables to every public host would make
each mirror a credential database. Instead each node holds a narrow, signed
authorization projection in SQLite, and browsers carry a controller-signed
portable session grant.

## Projection contents

Each monotonically numbered generation contains only: opaque account id and
public username; enabled device or passkey public keys and their capabilities;
public repository ids with owner/collaborator roles; key, account, and
repository revocation generations; and issue time, expiry, previous digest,
and Ed25519 signature.

It never contains email addresses, password hashes or reset state, TOTP seeds,
bearer or session tokens, private-message metadata, payment state, administrator
privileges, or encrypted account/repository rows. It is built by a dedicated
query, never a database export.

## Synchronization

A node refreshes after each controller authorization event and on the reconcile
interval. It verifies signature, schema, strictly increasing generation,
previous digest, size, and expiry, then applies one SQLite transaction. A bad
generation is discarded and the last good one stays. Peers exchange only
controller-signed generations. Repository content keeps syncing through Git and
event spools; this database only decides who may sign changes.

## Sessions

Each controller login also mints a signed portable session grant (opaque ids,
public username, issue/expiry, mirror-dashboard audience; no bearer token). It
travels in a separate HttpOnly, Secure, SameSite=Strict parent-domain cookie,
and mirrors verify it with the published router key. The grant lives at most
twelve hours, never outlives its controller session, and is cleared on logout
or session/device revocation. Tampering, ambiguous cookies, future grants,
expiry, and unknown keys fail closed. Custom-domain fleets set
`PORTABLE_SESSION_COOKIE_DOMAIN`.

Mirrors never accept password-only authentication: a password verifier on
every node would be offline attack material. Without a portable grant, an
enrolled device or passkey signs a node nonce bound to the origin and
authorization generation, creating a short-lived node-local session.

## Recovery

During an outage, authorized mutations land in local Git and signed spools.
On recovery the node reconciles revocations, forwards events idempotently,
fetches the newest generation, and reconciles refs. Billing, password reset,
account recovery, private data, and platform administration always fail
closed on a mirror.
