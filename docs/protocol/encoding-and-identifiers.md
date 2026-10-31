# Encoding and identifiers

Suite version: `1.0.0-rc.1` · Vectors: `canonical-json.json`,
`identifiers.json` · Part of the [ForkMesh protocol suite](README.md)

## Canonical JSON

ForkMesh hashes and signs a deliberately small JSON subset. It is chosen so
that Python `json.dumps(..., ensure_ascii=False, sort_keys=True,
separators=(",", ":"))` and ECMAScript `JSON.stringify` with sorted keys emit
identical bytes for every admitted value.

- **FM-ENC-001** MUST — A canonical document is UTF-8 without a byte-order
  mark, without insignificant whitespace, and without a trailing newline. A
  signed document's top level is an object. Object keys match
  `^[A-Za-z0-9_.:/-]{1,256}$` and appear in ascending code-point order.
- **FM-ENC-002** MUST — Values are objects, arrays, strings, `true`, `false`,
  `null`, or integers whose magnitude is at most 2^53−1. Integers are written
  in shortest decimal form: no fraction, exponent, leading zero, or negative
  zero. `NaN` and infinities are not JSON and are rejected. Individual fields
  can narrow the range further; envelope counters, for example, are
  non-negative.
- **FM-ENC-003** MUST — Strings are Unicode NFC and contain no unpaired
  surrogate. An encoder normalizes strings to NFC before hashing. Within a
  string, `"` and `\` are written `\"` and `\\`; U+0008, U+000C, U+000A,
  U+000D, and U+0009 use `\b \f \n \r \t`; all other code points below U+0020
  use `\u00XX` with lowercase hex. Every other code point, including U+007F,
  U+2028, U+2029, `/`, and characters outside the BMP, is written literally.
- **FM-ENC-004** MUST — A receiver parses with duplicate-key detection,
  re-encodes the parsed value under FM-ENC-001..003, and rejects the input
  unless the result is byte-for-byte identical. An alternate spelling of the
  same value — whitespace, escapes, key order, or a non-NFC string — is a
  different, invalid input, not an equivalent one.
- **FM-ENC-005** MUST — A signed document is at most 1 MiB (1,048,576 bytes)
  and non-empty. Nesting depth is at most 32, counting the top-level object as
  depth 0 and each nested array or object as one more.

Some integrity receipts use variants that differ from signed collaboration
documents. The portable session claim (FM-AUTH-003) and the catalog v2 record
hash (FM-AUTH-004) serialize with ASCII escaping: every code point above
U+007E is written as lowercase `\uXXXX`, using surrogate pairs outside the
BMP. Migration state receipts (FM-ID-006) use the literal form without NFC
normalization. Each variant is pinned by vectors.

Rejection codes, in the order a parser meets them: `invalid_event_size`,
`invalid_json`, `invalid_number`, `floating_point_forbidden`, `duplicate_key`,
`event_not_object`, `json_too_deep`, `invalid_key`, `unsafe_integer`,
`invalid_unicode`, `noncanonical_encoding`.

## Binary encodings

- **FM-ENC-006** MUST — Keys, signatures, and object keys are base64url
  (RFC 4648 §5) without padding. A decoder rejects padding, characters outside
  the alphabet, the wrong decoded length, and any spelling that does not
  re-encode identically. The last check catches non-zero unused trailing bits:
  two textual signatures for the same bytes would otherwise produce two
  distinct envelopes. Ed25519 public keys decode to 32 bytes, signatures to
  64, and collaboration object keys to 16.

Digest identifiers are written `sha256:` followed by 64 lowercase hex digits,
except where a field is specified as bare hex.

## Canonical strings

Older signed actions sign newline-joined strings rather than JSON.

- **FM-ENC-007** MUST — A canonical string is the UTF-8 encoding of a frozen
  scheme prefix `forkmesh-<scheme>-v<N>`, then each field in the order the
  scheme lists, joined by a single LF (0x0A) with no trailing LF. Timestamps
  are decimal milliseconds since the Unix epoch. Senders write them without
  sign or leading zeros; verifiers sign-check the field text exactly as
  transmitted. Variable-length content is bound as a lowercase hex SHA-256
  field, never inline.

The scheme field lists live in [Signatures and
authentication](signatures-and-authentication.md#canonical-string-schemes).

## Identifiers

- **FM-ID-001** MUST — A repository identity is
  `urn:forkmesh:repository:sha256:` followed by
  `hex(SHA-256("forkmesh-repository-identity-v1\n" || NFC(verified root)))`.
  The verified root is established out of band, for example by a signed
  repository manifest. Display names, organization aliases, and URLs are
  routing hints, never identities.
- **FM-ID-002** MUST — A collaboration policy document has exactly the keys
  `schema` (`forkmesh-collaboration-policy-v1`), `repository`, `maintainers`,
  `moderators`, and `notifiers`. Each role is a sorted, duplicate-free list of
  32-byte base64url Ed25519 keys. Its identity is
  `sha256:` + `hex(SHA-256(canonical JSON))`, with no domain prefix because
  the schema value is inside the hashed bytes. A document that is not already
  in this form is rejected, never repaired.
- **FM-ID-003** MUST — A collaboration object identity is
  `urn:forkmesh:collaboration:<family>:sha256:` followed by
  `hex(SHA-256("forkmesh-collaboration-object-v1\n" || canonical JSON of
  {repository, family, creator, objectKey, references}))`. `references` is
  the validated, sorted list of `{relation, family, object}` triples that the
  object's first event carries.
- **FM-ID-004** MUST — A collaboration event identity is
  `sha256:` + `hex(SHA-256(signingBytes || 0x00 || rawSignature))`. Here
  `signingBytes` is defined by FM-SIG-003 and `rawSignature` is the 64 decoded
  signature bytes. A receiver recomputes it and rejects a mismatch
  (`event_id_mismatch`).
- **FM-ID-005** MUST — A repository state digest is taken over the refs a node
  serves. From a protocol-v0 `upload-pack --advertise-refs` pkt-line stream,
  keep only `refs/heads/*` and `refs/tags/*`. Drop `HEAD`, peeled `^{}` lines,
  and every other namespace, including `refs/pull/` and `refs/forkmesh/`.
  Strip capabilities after the first NUL, and sort by refname in byte order.
  The canonical text joins `<object-id> <refname>` lines with LF and has no
  trailing LF. `stateHash` is its lowercase hex SHA-256; an empty
  advertisement hashes the empty string.
- **FM-ID-006** MUST — Migration identifiers are domain-separated SHA-256 over
  LF-joined parts: `hex(SHA-256(domain || "\n" || join("\n", parts)))`.
  A job is `mig_` followed by the first 24 hex digits for the domain
  `forkmesh-migration-job-v1`. An object key uses
  `forkmesh-migration-object-v1` with parts object type and source id. A
  state receipt is `sha256:` + hex SHA-256 of the literal-form canonical JSON
  (see above) of the state document.
