/*
 * ForkMesh scoped, community-operated trust lists, browser/Node implementation.
 *
 * Port of the reference resolver in app/src/trust_lists.py
 * (forkmesh-trust-list-v1, www/docs/design/scoped-trust-lists.md).  Every
 * client must produce a resolution whose canonical JSON is byte-identical for
 * each case in conformance/trust-lists/v1/vectors.json, so validation order
 * (first failing check wins), rejection codes, sort orders, and telemetry
 * counting mirror the reference exactly.
 */
(function trustListsModule(root) {
  "use strict";

  const LIST_SCHEMA = "forkmesh-trust-list-v1";
  const REVOCATION_SCHEMA = "forkmesh-trust-list-revocation-v1";
  const APPEAL_SCHEMA = "forkmesh-trust-list-appeal-v1";
  const RESOLUTION_SCHEMA = "forkmesh-trust-resolution-v1";
  const LIST_ID_DOMAIN = "forkmesh-trust-list-id-v1";

  const ROLES = Object.freeze([
    "maintainer", "moderator", "mirror-operator", "organization", "reviewer",
    "security-researcher",
  ]);
  const STANCES = Object.freeze(["distrust", "trust"]);
  const OVERRIDE_STANCES = Object.freeze(["distrust", "ignore", "trust"]);
  const REVOCATION_REASONS = Object.freeze(["compromised", "malicious", "retired", "superseded"]);
  const LIST_STATES = Object.freeze(["active", "conflicted", "expired", "missing", "recovered", "revoked"]);

  const SAFE_INTEGER = Number.MAX_SAFE_INTEGER;
  const MAX_DOCUMENT_BYTES = 262144;
  const MAX_ENTRIES = 2000;
  const MAX_EDGES = 16;
  const MAX_COMMUNITIES = 64;
  const MAX_REVOCATION_KEYS = 4;
  const MAX_NOTE = 280;
  const MAX_STATEMENT = 1000;
  const MAX_LIFETIME_MS = 400 * 24 * 60 * 60 * 1000;
  const MAX_PATH_LISTS = 4;
  const FUTURE_SKEW_MS = 300000;
  const MAX_NESTING = 8;

  const LIST_KEYS = [
    "schema", "issuer", "name", "version", "previous", "issuedAt",
    "expiresAt", "roles", "communities", "revocationKeys", "entries",
    "edges", "signature",
  ];
  const REVOCATION_KEYS = [
    "schema", "list", "issuer", "name", "fromVersion", "revokedBy",
    "revokedAt", "reason", "successor", "signature",
  ];
  const APPEAL_KEYS = [
    "schema", "subject", "list", "version", "role", "scope", "statement",
    "issuedAt", "signature",
  ];
  const ENTRY_KEYS = ["subject", "role", "scope", "stance", "note"];
  const EDGE_KEYS = ["kind", "issuer", "name", "roles", "communities", "expiresAt"];
  const SUCCESSOR_KEYS = ["issuer", "name"];

  const TEXT_RE = /^[\x20-\x21\x23-\x5b\x5d-\x7e]*$/;
  const KEY_TEXT_RE = /^[A-Za-z0-9_-]{43}$/;
  const SIG_TEXT_RE = /^[A-Za-z0-9_-]{86}$/;
  const SLUG_RE = /^[a-z0-9][a-z0-9-]{0,63}$/;
  const OWNER_RE = /^[a-z0-9][a-z0-9-]{0,38}$/;
  const REPO_RE = /^[A-Za-z0-9._-]{1,100}$/;
  const DIGEST_RE = /^sha256:[0-9a-f]{64}$/;
  const LIST_URN_RE = /^urn:forkmesh:trust-list:sha256:[0-9a-f]{64}$/;
  const B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  const encoder = new TextEncoder();

  class TrustListError extends Error {
    constructor(code) {
      super(code);
      this.name = "TrustListError";
      this.code = code;
    }
  }

  function reject(code) {
    throw new TrustListError(code);
  }

  // Python compares str by code point; JS `<` compares UTF-16 units.  They
  // agree on the ASCII that signed documents are limited to, but local follows
  // and overrides are unconstrained, so sort with code-point order throughout.
  function compareText(left, right) {
    if (left === right) return 0;
    const length = Math.min(left.length, right.length);
    for (let index = 0; index < length; index += 1) {
      const a = left.codePointAt(index);
      const b = right.codePointAt(index);
      if (a !== b) return a < b ? -1 : 1;
      if (a > 0xffff) index += 1;
    }
    return left.length < right.length ? -1 : (left.length > right.length ? 1 : 0);
  }

  function sortedUniqueText(values) {
    return [...new Set(values)].sort(compareText);
  }

  function hasOwn(value, key) {
    return Object.prototype.hasOwnProperty.call(value, key);
  }

  function isObject(value) {
    return value !== null && typeof value === "object" && !Array.isArray(value);
  }

  function exactKeys(value, expected) {
    const actual = Object.keys(value);
    return actual.length === expected.length && expected.every((key) => hasOwn(value, key));
  }

  function matches(pattern, value) {
    return typeof value === "string" && pattern.test(value);
  }

  // -------------------------------------------------------------------------
  // Canonical encoding.  Strings are restricted to printable ASCII without
  // quote or backslash, so the encoding needs no escapes and every language
  // produces identical bytes with a plain sorted-key writer.

  function canonicalJson(value) {
    if (value === null) return "null";
    if (value === true) return "true";
    if (value === false) return "false";
    if (typeof value === "number") {
      if (!Number.isSafeInteger(value)) reject("noncanonical");
      return String(value);
    }
    if (typeof value === "string") {
      if (!TEXT_RE.test(value)) reject("noncanonical");
      return `"${value}"`;
    }
    if (Array.isArray(value)) return `[${value.map(canonicalJson).join(",")}]`;
    if (value && typeof value === "object") {
      const parts = [];
      for (const key of Object.keys(value).sort(compareText)) {
        if (!TEXT_RE.test(key)) reject("noncanonical");
        parts.push(`"${key}":${canonicalJson(value[key])}`);
      }
      return `{${parts.join(",")}}`;
    }
    return reject("noncanonical");
  }

  async function sha256Hex(text) {
    const digest = new Uint8Array(await root.crypto.subtle.digest("SHA-256", encoder.encode(text)));
    return Array.from(digest, (byte) => byte.toString(16).padStart(2, "0")).join("");
  }

  // UTF-8 with Python's "surrogatepass": a lone surrogate is encoded as its
  // own three bytes instead of TextEncoder's U+FFFD, so ids stay identical.
  function utf8SurrogatePass(text) {
    const bytes = [];
    for (let index = 0; index < text.length; index += 1) {
      let unit = text.charCodeAt(index);
      if (unit >= 0xd800 && unit <= 0xdbff && index + 1 < text.length) {
        const next = text.charCodeAt(index + 1);
        if (next >= 0xdc00 && next <= 0xdfff) {
          unit = 0x10000 + ((unit - 0xd800) << 10) + (next - 0xdc00);
          index += 1;
        }
      }
      if (unit < 0x80) bytes.push(unit);
      else if (unit < 0x800) bytes.push(0xc0 | (unit >> 6), 0x80 | (unit & 63));
      else if (unit < 0x10000) bytes.push(0xe0 | (unit >> 12), 0x80 | ((unit >> 6) & 63), 0x80 | (unit & 63));
      else bytes.push(0xf0 | (unit >> 18), 0x80 | ((unit >> 12) & 63), 0x80 | ((unit >> 6) & 63), 0x80 | (unit & 63));
    }
    return Uint8Array.from(bytes);
  }

  async function documentId(raw) {
    const digest = new Uint8Array(await root.crypto.subtle.digest("SHA-256", utf8SurrogatePass(raw)));
    return `sha256:${Array.from(digest, (byte) => byte.toString(16).padStart(2, "0")).join("")}`;
  }

  async function listId(issuer, name) {
    return `urn:forkmesh:trust-list:sha256:${await sha256Hex(`${LIST_ID_DOMAIN}\n${issuer}\n${name}`)}`;
  }

  function signingText(document) {
    const unsigned = {};
    for (const key of Object.keys(document)) if (key !== "signature") unsigned[key] = document[key];
    return `${String(document.schema)}\n${canonicalJson(unsigned)}`;
  }

  // -------------------------------------------------------------------------
  // Scopes.

  function validScope(value) {
    if (typeof value !== "string") return false;
    if (value.startsWith("repo:")) {
      const rest = value.slice(5);
      const slash = rest.indexOf("/");
      if (slash < 0) return false;
      const owner = rest.slice(0, slash);
      const repo = rest.slice(slash + 1);
      return OWNER_RE.test(owner) && REPO_RE.test(repo) && repo !== "." && repo !== "..";
    }
    if (value.startsWith("org:")) return OWNER_RE.test(value.slice(4));
    if (value.startsWith("community:")) return SLUG_RE.test(value.slice(10));
    return false;
  }

  /** True when trust in `outer` includes `inner`. */
  function covers(outer, inner) {
    if (outer === inner) return true;
    return outer.startsWith("org:") && inner.startsWith(`repo:${outer.slice(4)}/`);
  }

  function covered(scopes, scope) {
    return scopes.some((outer) => covers(outer, scope));
  }

  function normalizeScopes(scopes) {
    const unique = sortedUniqueText(scopes);
    return unique.filter((scope) => !unique.some((outer) => outer !== scope && covers(outer, scope)));
  }

  function intersectScopes(left, right) {
    const kept = left.filter((scope) => covered(right, scope));
    for (const scope of right) if (covered(left, scope)) kept.push(scope);
    return normalizeScopes(kept);
  }

  function intersectRoles(left, right) {
    const wanted = new Set(right);
    return sortedUniqueText(left).filter((role) => wanted.has(role));
  }

  // -------------------------------------------------------------------------
  // Field checks.

  function safeInt(value, minimum = 0) {
    return typeof value === "number" && Number.isInteger(value) && value >= minimum && value <= SAFE_INTEGER;
  }

  function base64urlEncode(bytes) {
    let text = "";
    let bits = 0;
    let buffer = 0;
    for (const byte of bytes) {
      buffer = (buffer << 8) | byte;
      bits += 8;
      while (bits >= 6) {
        bits -= 6;
        text += B64URL[(buffer >> bits) & 63];
      }
      buffer &= (1 << bits) - 1;
    }
    if (bits) text += B64URL[(buffer << (6 - bits)) & 63];
    return text;
  }

  // Strict decoding: the alphabet is already pinned by the caller's regex, and
  // re-encoding must round-trip so non-zero trailing bits are refused.
  function base64urlDecode(text) {
    const bytes = [];
    let bits = 0;
    let buffer = 0;
    for (const character of text) {
      buffer = (buffer << 6) | B64URL.indexOf(character);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        bytes.push((buffer >> bits) & 255);
        buffer &= (1 << bits) - 1;
      }
    }
    const decoded = Uint8Array.from(bytes);
    return base64urlEncode(decoded) === text ? decoded : null;
  }

  function keyBytes(value) {
    return matches(KEY_TEXT_RE, value) ? base64urlDecode(value) : null;
  }

  function signatureBytes(value) {
    return matches(SIG_TEXT_RE, value) ? base64urlDecode(value) : null;
  }

  function sortedUnique(values, check, maximum, empty) {
    if (!Array.isArray(values) || values.length > maximum) return false;
    if (!values.length && !empty) return false;
    if (!values.every((item) => check(item))) return false;
    for (let index = 0; index + 1 < values.length; index += 1) {
      if (!(compareText(values[index], values[index + 1]) < 0)) return false;
    }
    return true;
  }

  function validRoles(values) {
    return sortedUnique(values, (role) => typeof role === "string" && ROLES.includes(role), ROLES.length, false);
  }

  function validCommunities(values) {
    if (!sortedUnique(values, validScope, MAX_COMMUNITIES, false)) return false;
    const normalized = normalizeScopes(values);
    return normalized.length === values.length && normalized.every((scope, index) => scope === values[index]);
  }

  // -------------------------------------------------------------------------
  // Ed25519.  WebCrypto checks the verification equation, but the reference
  // verifier is stricter about encodings and keys: it refuses non-canonical
  // points and identity, small-order, or mixed-order public keys.  Those
  // checks are reproduced here with BigInt before WebCrypto is consulted.

  const FIELD_Q = (1n << 255n) - 19n;
  const GROUP_L = (1n << 252n) + 27742317777372353535851937790883648493n;

  function mod(value) {
    const result = value % FIELD_Q;
    return result < 0n ? result + FIELD_Q : result;
  }

  function powMod(base, exponent) {
    let result = 1n;
    let current = mod(base);
    let remaining = exponent;
    while (remaining > 0n) {
      if (remaining & 1n) result = (result * current) % FIELD_Q;
      current = (current * current) % FIELD_Q;
      remaining >>= 1n;
    }
    return result;
  }

  const CURVE_D = mod(-121665n * powMod(121666n, FIELD_Q - 2n));
  const SQRT_M1 = powMod(2n, (FIELD_Q - 1n) / 4n);

  function littleEndian(bytes) {
    let value = 0n;
    for (let index = bytes.length - 1; index >= 0; index -= 1) value = (value << 8n) | BigInt(bytes[index]);
    return value;
  }

  function xRecover(y) {
    const xx = mod((y * y - 1n) * powMod(CURVE_D * y * y + 1n, FIELD_Q - 2n));
    let x = powMod(xx, (FIELD_Q + 3n) / 8n);
    if (mod(x * x - xx) !== 0n) x = (x * SQRT_M1) % FIELD_Q;
    if (x & 1n) x = FIELD_Q - x;
    return x;
  }

  function decodePoint(bytes) {
    if (bytes.length !== 32) return null;
    const encoded = littleEndian(bytes);
    const sign = encoded >> 255n;
    const y = encoded & ((1n << 255n) - 1n);
    if (y >= FIELD_Q) return null;
    let x = xRecover(y);
    // x = 0 has no negative; with the sign bit set, FIELD_Q - 0 would slip an
    // unreduced identity past the identity and subgroup checks.
    if (x === 0n && sign) return null;
    if ((x & 1n) !== sign) x = FIELD_Q - x;
    if (mod(-x * x + y * y - 1n - CURVE_D * x * x * y * y) !== 0n) return null;
    if ((y | ((x & 1n) << 255n)) !== encoded) return null;
    return [x, y];
  }

  function extendedAdd([x1, y1, z1, t1], [x2, y2, z2, t2]) {
    const a = mod((y1 - x1) * (y2 - x2));
    const b = mod((y1 + x1) * (y2 + x2));
    const c = mod(2n * CURVE_D * t1 * t2);
    const d = mod(2n * z1 * z2);
    const e = mod(b - a);
    const f = mod(d - c);
    const g = mod(d + c);
    const h = mod(b + a);
    return [mod(e * f), mod(g * h), mod(f * g), mod(e * h)];
  }

  function extendedDouble([x, y, z]) {
    const a = mod(x * x);
    const b = mod(y * y);
    const c = mod(2n * z * z);
    const d = mod(-a);
    const e = mod((x + y) * (x + y) - a - b);
    const g = mod(d + b);
    const f = mod(g - c);
    const h = mod(d - b);
    return [mod(e * f), mod(g * h), mod(f * g), mod(e * h)];
  }

  function scalarMultAffine([x, y], scalar) {
    let result = [0n, 1n, 1n, 0n];
    let addend = [x, y, 1n, mod(x * y)];
    let remaining = scalar;
    while (remaining) {
      if (remaining & 1n) result = extendedAdd(result, addend);
      addend = extendedDouble(addend);
      remaining >>= 1n;
    }
    const inverse = powMod(result[2], FIELD_Q - 2n);
    return [mod(result[0] * inverse), mod(result[1] * inverse)];
  }

  const subgroupKeyCache = new Map();

  function subgroupKey(publicKey) {
    const cacheKey = base64urlEncode(publicKey);
    if (subgroupKeyCache.has(cacheKey)) return subgroupKeyCache.get(cacheKey);
    const point = decodePoint(publicKey);
    let result = point !== null && !(point[0] === 0n && point[1] === 1n);
    if (result) {
      const multiple = scalarMultAffine(point, GROUP_L);
      result = multiple[0] === 0n && multiple[1] === 1n;
    }
    if (subgroupKeyCache.size >= 1024) subgroupKeyCache.delete(subgroupKeyCache.keys().next().value);
    subgroupKeyCache.set(cacheKey, result);
    return result;
  }

  async function ed25519Verify(publicKey, signature, message) {
    if (publicKey.length !== 32 || signature.length !== 64) return false;
    if (decodePoint(signature.subarray(0, 32)) === null) return false;
    if (littleEndian(signature.subarray(32)) >= GROUP_L) return false;
    if (!subgroupKey(publicKey)) return false;
    try {
      const key = await root.crypto.subtle.importKey("raw", publicKey, {name: "Ed25519"}, false, ["verify"]);
      return await root.crypto.subtle.verify({name: "Ed25519"}, key, signature, message);
    } catch (_error) {
      return false;
    }
  }

  async function verifySignature(document, signer) {
    const signature = signatureBytes(document.signature);
    if (signature === null) reject("invalid_signature_encoding");
    if (littleEndian(signature.subarray(32)) >= GROUP_L) reject("invalid_signature");
    const publicKey = keyBytes(signer);
    const message = encoder.encode(signingText(document));
    if (publicKey === null || !await ed25519Verify(publicKey, signature, message)) reject("invalid_signature");
  }

  // -------------------------------------------------------------------------
  // Document validation.  Each check runs in the reference order.

  async function validateList(doc) {
    if (keyBytes(doc.issuer) === null) reject("invalid_issuer");
    if (!matches(SLUG_RE, doc.name)) reject("invalid_name");
    if (!safeInt(doc.version, 1)) reject("invalid_version");
    const previous = doc.previous;
    if (doc.version === 1) {
      if (previous !== null) reject("invalid_previous");
    } else if (!matches(DIGEST_RE, previous)) {
      reject("invalid_previous");
    }
    const issued = doc.issuedAt;
    const expires = doc.expiresAt;
    if (!safeInt(issued) || !safeInt(expires) || expires <= issued || expires - issued > MAX_LIFETIME_MS) {
      reject("invalid_time");
    }
    if (!validRoles(doc.roles) || !validCommunities(doc.communities)) reject("invalid_scope");
    const keys = doc.revocationKeys;
    if (!sortedUnique(keys, (key) => keyBytes(key) !== null, MAX_REVOCATION_KEYS, true) || keys.includes(doc.issuer)) {
      reject("invalid_revocation_keys");
    }

    const entries = doc.entries;
    if (!Array.isArray(entries) || entries.length > MAX_ENTRIES) reject("invalid_entry");
    let previousKey = null;
    for (const entry of entries) {
      if (
        !isObject(entry)
        || !exactKeys(entry, ENTRY_KEYS)
        || keyBytes(entry.subject) === null
        || entry.subject === doc.issuer
        || !ROLES.includes(entry.role)
        || !validScope(entry.scope)
        || !STANCES.includes(entry.stance)
        || typeof entry.note !== "string"
        || entry.note.length > MAX_NOTE
      ) {
        reject("invalid_entry");
      }
      if (!doc.roles.includes(entry.role) || !covered(doc.communities, entry.scope)) reject("entry_outside_scope");
      const key = `${entry.subject}\n${entry.role}\n${entry.scope}`;
      if (previousKey !== null && compareText(key, previousKey) <= 0) reject("unsorted_entries");
      previousKey = key;
    }

    const edges = doc.edges;
    if (!Array.isArray(edges) || edges.length > MAX_EDGES) reject("invalid_edge");
    previousKey = null;
    for (const edge of edges) {
      if (
        !isObject(edge)
        || !exactKeys(edge, EDGE_KEYS)
        || (edge.kind !== "delegate" && edge.kind !== "include")
        || keyBytes(edge.issuer) === null
        || !matches(SLUG_RE, edge.name)
        || !validRoles(edge.roles)
        || !validCommunities(edge.communities)
        || (edge.kind === "delegate" && !safeInt(edge.expiresAt))
        || (edge.kind === "include" && edge.expiresAt !== null)
        || (edge.issuer === doc.issuer && edge.name === doc.name)
      ) {
        reject("invalid_edge");
      }
      if (
        !edge.roles.every((role) => doc.roles.includes(role))
        || !edge.communities.every((scope) => covered(doc.communities, scope))
      ) {
        reject("edge_outside_scope");
      }
      const key = `${edge.issuer}\n${edge.name}`;
      if (previousKey !== null && compareText(key, previousKey) <= 0) reject("unsorted_edges");
      previousKey = key;
    }

    await verifySignature(doc, doc.issuer);
  }

  async function validateRevocation(doc) {
    if (keyBytes(doc.issuer) === null) reject("invalid_issuer");
    if (!matches(SLUG_RE, doc.name)) reject("invalid_name");
    if (doc.list !== await listId(doc.issuer, doc.name)) reject("list_mismatch");
    if (!safeInt(doc.fromVersion, 1)) reject("invalid_version");
    if (keyBytes(doc.revokedBy) === null) reject("invalid_revoker");
    if (!safeInt(doc.revokedAt)) reject("invalid_time");
    if (!REVOCATION_REASONS.includes(doc.reason)) reject("invalid_reason");
    const successor = doc.successor;
    if (successor !== null && (
      !isObject(successor)
      || !exactKeys(successor, SUCCESSOR_KEYS)
      || keyBytes(successor.issuer) === null
      || !matches(SLUG_RE, successor.name)
      || (successor.issuer === doc.issuer && successor.name === doc.name)
    )) {
      reject("invalid_successor");
    }
    await verifySignature(doc, doc.revokedBy);
  }

  async function validateAppeal(doc) {
    if (keyBytes(doc.subject) === null) reject("invalid_subject");
    if (!matches(LIST_URN_RE, doc.list)) reject("invalid_list");
    if (!safeInt(doc.version, 1)) reject("invalid_version");
    if (!ROLES.includes(doc.role) || !validScope(doc.scope)) reject("invalid_scope");
    const statement = doc.statement;
    if (typeof statement !== "string" || statement.length < 1 || statement.length > MAX_STATEMENT) {
      reject("invalid_statement");
    }
    if (!safeInt(doc.issuedAt)) reject("invalid_time");
    await verifySignature(doc, doc.subject);
  }

  const SCHEMAS = new Map([
    [LIST_SCHEMA, {keys: LIST_KEYS, validate: validateList}],
    [REVOCATION_SCHEMA, {keys: REVOCATION_KEYS, validate: validateRevocation}],
    [APPEAL_SCHEMA, {keys: APPEAL_KEYS, validate: validateAppeal}],
  ]);

  /** Deepest bracket nesting outside strings, found before any parser runs. */
  function nestingDepth(raw) {
    let depth = 0;
    let deepest = 0;
    let inString = false;
    let escaped = false;
    for (const character of raw) {
      if (inString) {
        if (escaped) escaped = false;
        else if (character === "\\") escaped = true;
        else if (character === "\"") inString = false;
      } else if (character === "\"") {
        inString = true;
      } else if (character === "[" || character === "{") {
        depth += 1;
        deepest = Math.max(deepest, depth);
      } else if (character === "]" || character === "}") {
        depth -= 1;
      }
    }
    return deepest;
  }

  /** Parse and verify one raw document, throwing `TrustListError`. */
  async function verifyDocument(raw) {
    // Lone surrogates encode to three bytes either way, so TextEncoder's
    // length equals the reference's surrogatepass length.
    if (typeof raw !== "string" || encoder.encode(raw).length > MAX_DOCUMENT_BYTES) reject("too_large");
    if (nestingDepth(raw) > MAX_NESTING) reject("invalid_json");
    let document;
    try {
      document = JSON.parse(raw);
    } catch (_error) {
      reject("invalid_json");
    }
    if (canonicalJson(document) !== raw) reject("noncanonical");
    if (!isObject(document)) reject("unsupported_schema");
    const schema = hasOwn(document, "schema") ? document.schema : undefined;
    const spec = typeof schema === "string" ? SCHEMAS.get(schema) : undefined;
    if (!spec) reject("unsupported_schema");
    if (!exactKeys(document, spec.keys)) reject("invalid_keys");
    await spec.validate(document);
    return document;
  }

  // -------------------------------------------------------------------------
  // Resolution.

  function listGroupState(lid, docs, revocations, now) {
    const byVersion = new Map();
    for (const item of docs) {
      const version = item[1].version;
      if (!byVersion.has(version)) byVersion.set(version, []);
      byVersion.get(version).push(item);
    }
    const versions = [...byVersion.keys()].sort((a, b) => a - b);
    const cutoffs = [];
    const reasons = new Set();
    const conflicts = [];
    const conflictListId = docs.length ? lid : null;
    for (const version of versions) {
      const held = byVersion.get(version);
      if (held.length > 1) {
        cutoffs.push(version);
        reasons.add("equivocation");
        conflicts.push({kind: "equivocation", listId: conflictListId, version});
        continue;
      }
      const prior = byVersion.get(version - 1);
      if (prior !== undefined && prior.length === 1 && held[0][1].previous !== prior[0][0]) {
        cutoffs.push(version);
        reasons.add("history_fork");
        conflicts.push({kind: "history_fork", listId: conflictListId, version});
      }
    }
    let successor = null;
    if (revocations.length) {
      reasons.add("revoked");
      const ordered = [...revocations].sort((a, b) => (
        a[1].fromVersion - b[1].fromVersion || compareText(a[0], b[0])
      ));
      cutoffs.push(ordered[0][1].fromVersion);
      for (const [, revocation] of ordered) {
        if (revocation.successor !== null) {
          successor = {...revocation.successor};
          break;
        }
      }
    }
    const cutoff = cutoffs.length ? Math.min(...cutoffs) : null;
    let headId = null;
    let head = null;
    for (let index = versions.length - 1; index >= 0; index -= 1) {
      const version = versions[index];
      if (cutoff !== null && version >= cutoff) continue;
      if (byVersion.get(version).length === 1) {
        [headId, head] = byVersion.get(version)[0];
        break;
      }
    }
    let state;
    if (head === null) state = revocations.length ? "revoked" : (docs.length ? "conflicted" : "missing");
    else if (now >= head.expiresAt) state = "expired";
    else if (cutoff !== null) state = "recovered";
    else state = "active";
    return {
      state,
      head,
      headId,
      cutoff,
      reasons: [...reasons].sort(compareText),
      successor,
      conflicts,
      heldVersions: versions,
    };
  }

  /** Verify, compose, and merge trust lists for one local client. */
  async function resolve(documents, {now, follows = [], overrides = []} = {}) {
    const unique = new Map();
    for (const raw of documents) {
      const id = await documentId(raw);
      if (!unique.has(id)) unique.set(id, raw);
    }
    const duplicates = documents.length - unique.size;

    const rejected = [];
    const lists = [];
    const revocations = [];
    const appeals = [];
    for (const docId of [...unique.keys()].sort(compareText)) {
      let doc;
      try {
        doc = await verifyDocument(unique.get(docId));
      } catch (error) {
        if (!(error instanceof TrustListError)) throw error;
        rejected.push({documentId: docId, code: error.code});
        continue;
      }
      const schema = doc.schema;
      if (schema !== REVOCATION_SCHEMA && doc.issuedAt > now + FUTURE_SKEW_MS) {
        rejected.push({documentId: docId, code: "issued_in_future"});
      } else if (schema === LIST_SCHEMA) {
        lists.push([docId, doc]);
      } else if (schema === REVOCATION_SCHEMA) {
        revocations.push([docId, doc]);
      } else {
        appeals.push([docId, doc]);
      }
    }

    const docsByList = new Map();
    const names = new Map();
    for (const item of lists) {
      const lid = await listId(item[1].issuer, item[1].name);
      if (!docsByList.has(lid)) docsByList.set(lid, []);
      docsByList.get(lid).push(item);
      names.set(lid, [item[1].issuer, item[1].name]);
    }

    const revocationsByList = new Map();
    let revocationCount = 0;
    for (const [docId, revocation] of revocations) {
      const lid = revocation.list;
      const authorized = revocation.revokedBy === revocation.issuer || (docsByList.get(lid) || []).some(
        ([, doc]) => doc.version < revocation.fromVersion && doc.revocationKeys.includes(revocation.revokedBy),
      );
      if (!authorized) {
        rejected.push({documentId: docId, code: "unauthorized_revoker"});
        continue;
      }
      if (!revocationsByList.has(lid)) revocationsByList.set(lid, []);
      revocationsByList.get(lid).push([docId, revocation]);
      revocationCount += 1;
      if (!names.has(lid)) names.set(lid, [revocation.issuer, revocation.name]);
    }

    const groups = new Map();
    function group(lid) {
      if (!groups.has(lid)) {
        groups.set(lid, listGroupState(lid, docsByList.get(lid) || [], revocationsByList.get(lid) || [], now));
      }
      return groups.get(lid);
    }
    for (const lid of new Set([...docsByList.keys(), ...revocationsByList.keys()])) group(lid);

    function usable(lid) {
      const info = group(lid);
      return info.state === "active" || info.state === "recovered" ? info.head : null;
    }

    const followed = new Set();
    const reached = new Set();
    let expiredDelegations = 0;
    let depthLimited = 0;
    let cycles = 0;
    // tuple key -> listId -> best source by (path length, joined path).
    const sources = new Map();

    // Counters are per path visit: a list reached along two paths walks its
    // edges twice, exactly like the reference recursion.
    async function admit(lid, path, roles, communities) {
      reached.add(lid);
      const head = usable(lid);
      if (head === null) return;
      for (const entry of head.entries) {
        if (!roles.includes(entry.role) || !covered(communities, entry.scope)) continue;
        const tupleKey = `${entry.subject}\n${entry.role}\n${entry.scope}`;
        const candidate = {
          listId: lid,
          version: head.version,
          stance: entry.stance,
          note: entry.note,
          path: [...path],
        };
        if (!sources.has(tupleKey)) sources.set(tupleKey, new Map());
        const perList = sources.get(tupleKey);
        const current = perList.get(lid);
        if (
          current === undefined
          || path.length < current.path.length
          || (path.length === current.path.length && compareText(path.join("\n"), current.path.join("\n")) < 0)
        ) {
          perList.set(lid, candidate);
        }
      }
      for (const edge of head.edges) {
        if (edge.kind === "delegate" && now >= edge.expiresAt) {
          expiredDelegations += 1;
          continue;
        }
        const child = await listId(edge.issuer, edge.name);
        if (!names.has(child)) names.set(child, [edge.issuer, edge.name]);
        if (path.includes(child)) {
          cycles += 1;
          continue;
        }
        if (path.length >= MAX_PATH_LISTS) {
          depthLimited += 1;
          continue;
        }
        let childRoles = intersectRoles(roles, edge.roles);
        let childCommunities = intersectScopes(communities, edge.communities);
        const childHead = usable(child);
        if (childHead !== null) {
          childRoles = intersectRoles(childRoles, childHead.roles);
          childCommunities = intersectScopes(childCommunities, childHead.communities);
        }
        if (!childRoles.length || !childCommunities.length) {
          reached.add(child);
          continue;
        }
        await admit(child, [...path, child], childRoles, childCommunities);
      }
    }

    const followRoots = [];
    for (const follow of follows) {
      const lid = await listId(follow.issuer, follow.name);
      if (!names.has(lid)) names.set(lid, [follow.issuer, follow.name]);
      followRoots.push([lid, follow]);
    }
    followRoots.sort((a, b) => compareText(a[0], b[0]));
    for (const [lid, follow] of followRoots) {
      followed.add(lid);
      const head = usable(lid);
      if (head === null) {
        reached.add(lid);
        continue;
      }
      let roles = [...head.roles];
      let communities = [...head.communities];
      if (follow.roles !== undefined && follow.roles !== null) roles = intersectRoles(roles, follow.roles);
      if (follow.communities !== undefined && follow.communities !== null) {
        communities = intersectScopes(communities, follow.communities);
      }
      if (roles.length && communities.length) await admit(lid, [lid], roles, communities);
      else reached.add(lid);
    }

    const overrideMap = new Map();
    for (const override of overrides) {
      overrideMap.set(`${override.subject}\n${override.role}\n${override.scope}`, override.stance);
    }
    const appealStatusCounts = {open: 0, resolved: 0, unattached: 0};
    const assertions = [];
    const conflicts = [];
    for (const tupleKey of sortedUniqueText([...sources.keys(), ...overrideMap.keys()])) {
      const [subject, role, scope] = tupleKey.split("\n");
      const perList = sources.get(tupleKey) || new Map();
      const tupleSources = [...perList.keys()].sort(compareText).map((key) => perList.get(key));
      const stances = new Set(tupleSources.map((source) => source.stance));
      const stance = !stances.size ? "none" : (stances.size === 1 ? [...stances][0] : "conflict");
      const override = overrideMap.has(tupleKey) ? (overrideMap.get(tupleKey) ?? null) : null;
      if (stance === "conflict") {
        conflicts.push({kind: "stance", subject, role, scope, overridden: override !== null});
      }
      assertions.push({
        subject,
        role,
        scope,
        stance,
        override,
        effective: override !== null ? override : stance,
        sources: tupleSources,
        appeals: [],
      });
    }

    const byTuple = new Map(assertions.map((assertion) => [
      `${assertion.subject}\n${assertion.role}\n${assertion.scope}`, assertion,
    ]));
    for (const [docId, appeal] of appeals) {
      const assertion = byTuple.get(`${appeal.subject}\n${appeal.role}\n${appeal.scope}`);
      if (assertion === undefined || !assertion.sources.some((source) => source.listId === appeal.list)) {
        appealStatusCounts.unattached += 1;
        continue;
      }
      const head = usable(appeal.list);
      let status = "open";
      if (head !== null && head.version > appeal.version && !head.entries.some((entry) => (
        entry.subject === appeal.subject
        && entry.role === appeal.role
        && entry.scope === appeal.scope
        && entry.stance === "distrust"
      ))) {
        status = "resolved";
      }
      appealStatusCounts[status] += 1;
      assertion.appeals.push({appealId: docId, listId: appeal.list, version: appeal.version, status});
    }

    const listRows = [];
    const stateCounts = Object.fromEntries(LIST_STATES.map((state) => [state, 0]));
    for (const lid of sortedUniqueText([...groups.keys(), ...followed, ...reached])) {
      const info = group(lid);
      conflicts.push(...info.conflicts);
      const [issuer, name] = names.get(lid);
      stateCounts[info.state] += 1;
      const head = info.head;
      listRows.push({
        listId: lid,
        issuer,
        name,
        state: info.state,
        followed: followed.has(lid),
        reachable: reached.has(lid),
        headVersion: head ? head.version : null,
        headDocumentId: info.headId,
        expiresAt: head ? head.expiresAt : null,
        cutoffVersion: info.cutoff,
        cutoffReasons: info.reasons,
        successor: info.successor,
        heldVersions: info.heldVersions,
      });
    }

    const conflictKeys = new Map(conflicts.map((conflict) => [conflict, canonicalJson(conflict)]));
    conflicts.sort((a, b) => compareText(conflictKeys.get(a), conflictKeys.get(b)));
    rejected.sort((a, b) => compareText(`${a.documentId}\n${a.code}`, `${b.documentId}\n${b.code}`));
    const rejectionCodes = {};
    for (const item of rejected) rejectionCodes[item.code] = (rejectionCodes[item.code] || 0) + 1;

    return {
      schema: RESOLUTION_SCHEMA,
      now,
      lists: listRows,
      assertions,
      conflicts,
      rejected,
      telemetry: {
        documents: unique.size,
        duplicateDocuments: duplicates,
        rejectedDocuments: rejected.length,
        rejectionCodes,
        revocations: revocationCount,
        listStates: stateCounts,
        followedLists: followed.size,
        reachableLists: reached.size,
        assertions: assertions.length,
        stanceConflicts: conflicts.filter((conflict) => conflict.kind === "stance").length,
        overrides: overrideMap.size,
        appeals: appealStatusCounts,
        expiredDelegations,
        depthLimitedEdges: depthLimited,
        cycleEdges: cycles,
      },
    };
  }

  const api = {
    APPEAL_SCHEMA,
    FUTURE_SKEW_MS,
    LIST_ID_DOMAIN,
    LIST_SCHEMA,
    LIST_STATES,
    MAX_DOCUMENT_BYTES,
    MAX_NESTING,
    MAX_PATH_LISTS,
    OVERRIDE_STANCES,
    RESOLUTION_SCHEMA,
    REVOCATION_REASONS,
    REVOCATION_SCHEMA,
    ROLES,
    STANCES,
    TrustListError,
    canonicalJson,
    covers,
    documentId,
    intersectScopes,
    listId,
    resolve,
    signingText,
    validScope,
    verifyDocument,
  };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  root.ForkMeshTrustLists = api;
}(typeof globalThis !== "undefined" ? globalThis : this));
