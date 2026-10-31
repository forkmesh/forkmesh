# Version negotiation

Suite version: `1.0.0-rc.1` · Vectors: `events.json`, `identifiers.json`,
`signatures.json`, `sessions.json` · Part of the [ForkMesh protocol
suite](README.md)

ForkMesh has no interactive handshake. Every signed or hashed structure
carries an explicit version identifier, and each receiver selects exactly one
verifier from it:

| Structure | Identifier | Where |
| --- | --- | --- |
| Collaboration event | `forkmesh-collaboration-event-v1` | `schema` field and signing domain |
| Collaboration policy | `forkmesh-collaboration-policy-v1` | `schema` field |
| Collaboration state | `forkmesh-collaboration-state-v1` | `schema` field |
| Canonical-string schemes | `forkmesh-<scheme>-v<N>` | first line of the signed message |
| Catalog publication | `catalogSigVersion` 1 or 2 | request field; selects the scheme |
| Portable session | `v1.` token prefix and claim `v: 1` | token framing and claim |
| Repository migration | `forkmesh-repository-migration-v1`, `forkmesh-repository-migration-state-v1` | job and receipt `schema` |
| Room key proof | `forkmesh-room-key-v2` (v1 only for the compatibility room) | first line of the signed message |

The only advertised negotiation is the catalog upgrade response: HTTP 426
with `{"error": "client_upgrade_required", "requiredCatalogSigVersion": 2}`.
Git protocol v0/v2 negotiation belongs to stock Git.

- **FM-VER-001** MUST — Receivers match version identifiers exactly. They
  reject an unknown or future identifier with the structure's stable code
  (`unsupported_schema`, `invalid_policy`, `token_invalid`, `claim_invalid`),
  never parse it heuristically, and never ignore unknown fields in a signed
  structure.
- **FM-VER-002** MUST NOT — A receiver MUST NOT downgrade. A message that
  fails verification under its declared version is never retried under an
  older version's rules. An older version stays accepted only within its
  published scope: `forkmesh-catalog-v1` only for public records, and
  `forkmesh-room-key-v1` only for the public compatibility room. Because
  prefixes differ, a signature over one version never verifies as another.
- **FM-VER-003** MUST — The bytes an identifier covers are frozen. Any change
  to signed or hashed bytes, field order, normalization, or meaning requires a
  new identifier, and the committed vectors for the existing identifier stay
  unchanged.
- **FM-VER-004** MUST — A new identifier rolls out in a fixed order. First,
  readers that verify and preserve the new identifier deploy. Then writers
  begin emitting it. Readers keep accepting the previous identifier for as
  long as retained history contains it. Rollback reverses the order
  (FM-MIG-003).
