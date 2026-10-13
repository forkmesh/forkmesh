# Breakout authorization replica

ForkMesh mirrors use Git and signed repository ledgers as the replicated data
plane. Repository management during a controller outage also needs an
authorization plane, but copying D1 or password/session tables to every public
host would turn each mirror into a credential database. ForkMesh therefore uses
a narrow, signed authorization projection backed by SQLite on each node, plus
a controller-signed portable browser grant for login continuity.

## Replicated fields

Each monotonically numbered generation may contain only:

- stable opaque account id and public username;
- enabled device or passkey public keys and their capabilities;
- public repository ids and owner/collaborator roles;
- key, account, and repository revocation generations; and
- controller issue time, expiry, previous digest, and Ed25519 signature.

It must not contain email addresses, password salts or hashes, password reset
state, TOTP seeds, controller bearer/session tokens, private-message metadata, payment
state, administrator privileges, or the encrypted `users.data`/repository rows.
The source is a purpose-built D1 query and projection, never a D1 export.

## Synchronization

The node refreshes after each controller authorization event and on the normal
short reconcile interval. It verifies the controller signature, exact schema,
strictly increasing generation, previous-generation digest, size limits, and
expiry before applying one SQLite transaction. A bad or partial generation is
discarded and the last good generation remains available. Nodes exchange only
controller-signed generations, so peer catch-up does not create a new authority.

Repository code, refs, releases, issues, pulls, reviews, discussions, and signed
write events continue to synchronize through Git and durable event spools. The
authorization database decides who may create those signed changes; it is not a
second repository database.

## Login and sessions

Every successful controller login also mints a narrow Ed25519-signed portable
session grant. The grant contains only an opaque account id, public username,
opaque session/grant ids, issue/expiry times, and the mirror-dashboard audience;
it contains neither the controller bearer token nor a D1 row. On the managed
`*.forkmesh.com` fleet it is carried in a separate HttpOnly, Secure,
SameSite=Strict parent-domain cookie. An independent mirror verifies the grant
with the already-published router public key, so visiting that mirror keeps the
browser signed in even when the controller cannot answer.

The grant lives at most twelve hours and never outlives its controller session
(which lasts five years unless revoked), is reissued on authenticated
session/device reads, and is cleared with the controller cookie on logout,
current-session revocation, or current-device revocation. Signature tampering, duplicate-cookie ambiguity, future grants,
expiry, and a different signing key all fail closed. During a controller outage
an already stolen grant cannot receive a newer revocation decision, so mirrors
limit it to its signed expiry and prohibit account recovery, billing, platform
administration, and other controller-only powers. Custom-domain fleets must
configure `PORTABLE_SESSION_COOKIE_DOMAIN`; unrelated domains deliberately do
not receive the cookie.

Password-only reauthentication still never happens on a public mirror. If no
portable grant is present, an enrolled device/passkey may instead sign a node
nonce bound to the requested origin and authorization generation. That proof
creates a short-lived node-local session scoped to projected repository roles.

Password-only accounts cannot safely authenticate on a public mirror: a
password verifier distributed to every node is reusable offline attack
material. The UI must ask those users to enroll a device key or passkey while
the controller is available before breakout access can work.

## Recovery

During an outage, authorized repository mutations land in local Git and signed
spools. On recovery the node reconciles portable and breakout-session
revocations, forwards events
idempotently, fetches the newest authorization generation, and reconciles refs
according to the signed event/ref policy. Billing, password reset, account
recovery, private data, and platform administration always fail closed on a
mirror.
