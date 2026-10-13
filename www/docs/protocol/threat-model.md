# Threat model

Suite version: `1.0.0-rc.1` · Register: `conformance/protocol/v1/threats.json`
· Part of the [ForkMesh protocol suite](README.md)

This model covers the wire protocol in this suite. Endpoint compromise,
Cloudflare platform compromise, and supply-chain attacks against clients
belong to [SECURITY.md](../../../SECURITY.md) and the [dependency
audit](../security/dependency-security-audit.md). The register file is
authoritative. The table below MUST match its severity and status, and the
suite verifier fails if they differ.

## Assets

- Private keys: device, account, router, and importer keys.
- Repository history and the integrity of refs.
- Signed collaboration history: issues, pulls, reviews, moderation.
- Private repository names, contents, and collaborator lists.
- Session and request authorizations.
- Availability of clone and collaboration flows.

## Trust boundaries

| ID | Boundary |
| --- | --- |
| B1 | Client device and its key store |
| B2 | Client ↔ relay (Worker HTTPS and WebSocket) |
| B3 | Relay ↔ mirror origin |
| B4 | Mirror ↔ source-of-truth node |
| B5 | Relay storage (D1, Durable Objects, KV) |
| B6 | External provider ↔ migration importer |
| B7 | Portable session ↔ independent mirror dashboard |

## Attacker capabilities

| ID | Attacker | Can | Cannot (assumed) |
| --- | --- | --- | --- |
| A1 | Network attacker | Observe timing and sizes; drop or delay traffic | Break TLS 1.2+ or Ed25519/SHA-256 |
| A2 | Malicious or compromised relay operator | Read and rewrite anything the relay receives; withhold, reorder, or replay messages; read relay-layer room traffic | Forge user signatures |
| A3 | Malicious mirror operator | Serve any bytes; lie in its own attestations; log requests | Forge a source-of-truth attestation |
| A4 | Malicious registered user | Sign any content with its own keys; equivocate; craft malformed input | Sign with another user's key |
| A5 | Holder of a stolen device key | Everything that key authorizes until rotation or revocation | Use the account password or TOTP |
| A6 | Malicious migration provider | Return inconsistent, oversized, or hostile API data | Access ForkMesh keys |
| A7 | Resource-exhaustion attacker | Send many or large requests within platform limits | Exceed Cloudflare edge limits |

## Register

| ID | Threat | Boundaries | Attackers | Severity | Status | Controls |
| --- | --- | --- | --- | --- | --- | --- |
| TM-001 | Forged authorship of signed objects | B2, B5 | A2, A4 | high | mitigated | FM-SIG-001, FM-SIG-003, FM-SIG-004 |
| TM-002 | Signature or encoding malleability that aliases event identities | B2 | A2, A4 | high | mitigated | FM-SIG-001, FM-ENC-004, FM-ENC-006, FM-ID-004 |
| TM-003 | Replay of collaboration events into another repository or policy epoch | B2, B5 | A2, A4 | high | mitigated | FM-ID-003, FM-SIG-004 |
| TM-004 | Replay of legacy issue/discussion/pull records whose canonical strings omit owner and repository | B2 | A2, A4 | high | open | FM-SIG-002, FM-MIG-002 |
| TM-005 | Cross-scheme or cross-version signature confusion | B2 | A2 | high | mitigated | FM-SIG-002, FM-VER-001, FM-VER-002 |
| TM-006 | Catalog v1 signatures, which do not bind visibility or descriptive fields, replayed for public records | B2, B5 | A2 | high | open | FM-AUTH-004, FM-VER-002 |
| TM-007 | Tampered or rolled-back history served to cloners | B3, B4 | A3 | high | mitigated | FM-REL-001, FM-REL-002, FM-ID-005 |
| TM-008 | Relay or mirror withholds collaboration events | B2, B3 | A2, A3 | high | open | FM-SYNC-003 |
| TM-009 | Signing-key equivocation | B2 | A4, A5 | high | mitigated | FM-SYNC-003 |
| TM-010 | Clock or timestamp manipulation to win conflicts or extend tokens | B2 | A4 | medium | mitigated | FM-SYNC-002, FM-SYNC-003, FM-AUTH-001, FM-AUTH-003 |
| TM-011 | Unauthorized moderation or role escalation | B2, B5 | A4 | high | mitigated | FM-ID-002, FM-SYNC-003 |
| TM-012 | Replay of view, rotate, or push proofs within the freshness window | B2 | A1, A2 | medium | open | FM-AUTH-001, FM-AUTH-002 |
| TM-013 | Theft and replay of portable session bearer tokens | B7 | A1, A3 | medium | open | FM-AUTH-003 |
| TM-014 | Tunnelled node traffic trusts the relay as an authenticated intermediary | B2, B3 | A2 | high | open | FM-SIG-002 |
| TM-015 | Relay operator reads the outer room layer, including public channel plaintext and presence | B2, B5 | A2 | medium | open | FM-SIG-002 |
| TM-016 | Metadata leakage through the catalog, presence, routing, and timing | B2, B5 | A1, A2 | medium | open | FM-REL-001 |
| TM-017 | Parser, verifier, or fold resource exhaustion | B2 | A7, A4 | medium | mitigated | FM-ENC-005, FM-SIG-004 |
| TM-018 | Account key compromise or loss without a specified recovery protocol | B1 | A5 | high | open | FM-AUTH-004 |
| TM-019 | Data loss through lossy migration, down-conversion, or snapshot trust | B5, B6 | A2, A6 | high | mitigated | FM-MIG-001, FM-MIG-002, FM-MIG-003, FM-SYNC-001 |
| TM-020 | Substituting the pack body of an authorized HTTPS push | B2, B3 | A2 | high | mitigated | FM-AUTH-002, FM-REL-003 |

Open high-severity threats — TM-004, TM-006, TM-008, TM-014, and TM-018 — block
a stable suite release until they are mitigated or explicitly accepted.

## Discussion by class

**Replay.** Collaboration v1 binds repository, object, and policy inside the
signed bytes (TM-003). The legacy `forkmesh-issue-event-v1` family binds only
type, number, author, timestamp, and a content hash. The same signed record can
therefore be presented to another repository that has a matching issue number
(TM-004). The mitigation path is FM-MIG-002 migration to collaboration v1 plus
a relay rule that stops accepting legacy inbox records after the migration
window. Request proofs carry no nonce, so capture inside the five-minute window
permits replay (TM-012). Push proofs bind the method and body digest, so a
replay can only resubmit the identical pack.

**Downgrade.** Exact identifier matching and distinct prefixes defeat
cross-version confusion (TM-005). `forkmesh-catalog-v1` is still accepted for
public records. Descriptive fields and visibility are not signed under v1, and
the rollback check permits an equal `updatedAt`. A relay-level replayer could
therefore reissue an old public record (TM-006). Removing v1 acceptance is a
major suite change (see the compatibility policy).

**Equivocation.** A key signing two events in one sequence slot loses both
(TM-009). Detection needs both events in the same fold, so equivocation hidden
by withholding reduces to TM-008.

**Omission and availability.** No fold detects missing events (TM-008).
Closing it requires availability receipts or peer exchange that cross-checks
relay-delivered logs. Clone availability degrades to fail-closed, never to
unverified bytes (TM-007).

**Metadata leakage.** The relay learns public repository names, sizes, node
versions, presence, and request timing (TM-016). Private repository names never
enter the public routing index; private reads use opaque handles and ciphertext
([private repository
confidentiality](../security/private-repository-confidentiality.md)). By
design, default rooms use a relay-derivable key; private channels add a
recipient layer (TM-015, [sealed chat](../security/sealed-chat.md)). Telemetry
exports counts only.

**Abuse.** Size and depth limits bound parsing (TM-017). Verification caches
key on fixed-size digests. Catalog publication is rate-limited per owner.
Inbox signatures prove integrity, not reputation, and owner review gates every
merge.

**Recovery and data loss.** Signed raw bytes are the source of truth, and
snapshots are disposable (TM-019). Account key rotation requires the old key
(`forkmesh-rotate-v1`). Recovering from a lost or stolen key currently relies
on account credentials and the procedures in [portable
identities](../portable-identities.md). There is no normative, independently
verifiable recovery protocol yet (TM-018).

## Review

A security reviewer changes a threat's status only by editing `threats.json`
and this table together. The reviewer cites the requirement or test that closes
it, or records `acceptance` with reviewer, date, and rationale. The 2026-07
security audit is the origin of TM-004 and TM-014. This register does not
record any acceptance until a named reviewer signs one.
