/*
 * ForkMesh portable social signals v1, browser/Node implementation.
 *
 * Written from www/docs/social-signals.md, independently of
 * tools/social_signals.py. Both must agree on every case in
 * conformance/social-signals/v1/vectors.json, and the quality gate round-trips
 * a signed bundle between them. Signing and verification use WebCrypto; the
 * FM-SIG-001 checks WebCrypto omits (S < L, prime-order public key) are added
 * here with BigInt arithmetic.
 */
(function socialSignalsModule(root) {
  "use strict";

  const SIGNAL_SCHEMA = "forkmesh-social-signal-v1";
  const REVOCATION_SCHEMA = "forkmesh-social-revocation-v1";
  const BUNDLE_SCHEMA = "forkmesh-social-signal-bundle-v1";
  const STATE_SCHEMA = "forkmesh-social-signal-state-v1";
  const RECOMMENDATION_SCHEMA = "forkmesh-social-recommendations-v1";
  const TIERS = Object.freeze({
    star: "interest",
    watch: "interest",
    bookmark: "interest",
    follow: "interest",
    recommendation: "interest",
    "usage-endorsement": "attestation",
    "security-review": "attestation",
    "mirror-commitment": "attestation",
  });
  const WEIGHTS = Object.freeze({
    star: 1,
    watch: 1,
    recommendation: 2,
    "mirror-commitment": 2,
    "usage-endorsement": 3,
    "security-review": 3,
  });
  const VISIBILITIES = ["public", "followers", "private"];
  const AUDIENCES = Object.freeze({
    public: ["public"],
    followers: ["public", "followers"],
    self: VISIBILITIES,
  });
  const REASONS = ["withdrawn", "superseded", "compromised", "mistaken"];
  const SIGNAL_KEYS = [
    "schema", "kind", "tier", "signer", "subject", "issuedAt", "expiresAt",
    "visibility", "scope", "evidence", "note", "signalId", "signature",
  ];
  const REVOCATION_KEYS = ["schema", "signer", "revokes", "revokedAt", "reason", "revocationId", "signature"];
  const BUNDLE_KEYS = ["schema", "exportedAt", "signals", "revocations"];
  const CONTROL_KEYS = ["viewer", "enabled", "asOf", "limit", "mutedSigners", "mutedSubjects"];
  const EVIDENCE_KEYS = ["label", "uri", "sha256"];
  const SCOPE_KEYS = Object.freeze({
    "usage-endorsement": ["environment", "version", "since"],
    "security-review": ["revision", "method", "findings"],
    "mirror-commitment": ["endpoint", "protocols"],
  });
  const ENVIRONMENTS = ["production", "staging", "development"];
  const REVIEW_METHODS = ["manual-audit", "automated-scan", "threat-model"];
  const FINDINGS = ["none", "resolved", "open"];
  const MIRROR_PROTOCOLS = ["forkmesh-relay", "git-https", "git-ssh"];
  const MAX_CLOCK_SKEW_MS = 300000;
  const MAX_NOTE_BYTES = 1024;
  const MAX_EVIDENCE = 16;
  const MAX_BUNDLE_RECORDS = 10000;
  const MAX_CONTROL_MUTES = 1000;
  const MAX_URI_BYTES = 2048;
  const MAX_DEPTH = 32;
  const KEY_RE = /^[A-Za-z0-9_.:/-]{1,256}$/;
  const SHA_RE = /^sha256:[0-9a-f]{64}$/;
  const REPOSITORY_RE = /^urn:forkmesh:repository:sha256:[0-9a-f]{64}$/;
  const IDENTITY_PREFIX = "urn:forkmesh:identity:ed25519:";
  const REVISION_RE = /^[0-9a-f]{40}(?:[0-9a-f]{24})?$/;
  const URI_RE = /^https:\/\/[!-~]+$/;
  const B64_RE = /^[A-Za-z0-9_-]*$/;
  const encoder = new TextEncoder();

  class SocialSignalError extends Error {
    constructor(code) {
      super(code);
      this.name = "SocialSignalError";
      this.code = code;
    }
  }

  function reject(code) {
    throw new SocialSignalError(code);
  }

  const own = (value, key) => Object.prototype.hasOwnProperty.call(value, key);
  const isPlainObject = (value) => Boolean(value) && typeof value === "object" && !Array.isArray(value);
  const utf8Length = (value) => encoder.encode(value).length;

  function sameKeys(value, expected) {
    const actual = Object.keys(value).sort();
    const wanted = [...expected].sort();
    return actual.length === wanted.length && actual.every((key, index) => key === wanted[index]);
  }

  // Canonical JSON: safe integers only, NFC strings, sorted keys, no whitespace.
  function normalize(value, depth = 0) {
    if (depth > MAX_DEPTH) reject("json_too_deep");
    if (value === null || typeof value === "boolean") return value;
    if (typeof value === "number") {
      if (!Number.isSafeInteger(value)) reject("invalid_number");
      return value;
    }
    if (typeof value === "string") {
      const normalized = value.normalize("NFC");
      for (let index = 0; index < normalized.length; index += 1) {
        const unit = normalized.charCodeAt(index);
        if (unit >= 0xd800 && unit <= 0xdfff) {
          const next = normalized.charCodeAt(index + 1);
          if (unit > 0xdbff || !(next >= 0xdc00 && next <= 0xdfff)) reject("invalid_unicode");
          index += 1;
        }
      }
      return normalized;
    }
    if (Array.isArray(value)) return value.map((item) => normalize(item, depth + 1));
    if (!isPlainObject(value) || Object.getPrototypeOf(value) !== Object.prototype) reject("invalid_json_type");
    const result = Object.create(null);
    for (const key of Object.keys(value)) {
      if (!KEY_RE.test(key)) reject("invalid_key");
      result[key] = normalize(value[key], depth + 1);
    }
    return Object.assign({}, result);
  }

  function encode(value) {
    if (value === null || typeof value !== "object") return JSON.stringify(value);
    if (Array.isArray(value)) return `[${value.map(encode).join(",")}]`;
    return `{${Object.keys(value).sort().map((key) => `${JSON.stringify(key)}:${encode(value[key])}`).join(",")}}`;
  }

  function canonicalBytes(value) {
    return encoder.encode(encode(normalize(value)));
  }

  function domainBytes(domain, value) {
    const body = canonicalBytes(value);
    const prefix = encoder.encode(`${domain}\n`);
    const bytes = new Uint8Array(prefix.length + body.length);
    bytes.set(prefix, 0);
    bytes.set(body, prefix.length);
    return bytes;
  }

  function hex(bytes) {
    return Array.from(bytes, (item) => item.toString(16).padStart(2, "0")).join("");
  }

  async function digest(domain, value) {
    const hash = await root.crypto.subtle.digest("SHA-256", domainBytes(domain, value));
    return `sha256:${hex(new Uint8Array(hash))}`;
  }

  function base64url(bytes) {
    let binary = "";
    for (const item of bytes) binary += String.fromCharCode(item);
    return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  }

  function base64urlDecode(value, size, code) {
    if (typeof value !== "string" || !B64_RE.test(value) || value.length % 4 === 1) reject(code);
    const binary = atob(value.replace(/-/g, "+").replace(/_/g, "/") + "=".repeat((4 - (value.length % 4)) % 4));
    const bytes = Uint8Array.from(binary, (character) => character.charCodeAt(0));
    if (bytes.length !== size || base64url(bytes) !== value) reject(code);
    return bytes;
  }

  function without(record, ...drop) {
    const result = {};
    for (const key of Object.keys(record)) if (!drop.includes(key)) result[key] = record[key];
    return result;
  }

  // ------------------------------------------------------------------------
  // Ed25519 strictness WebCrypto does not enforce (FM-SIG-001).

  const P = (1n << 255n) - 19n;
  const L = (1n << 252n) + 27742317777372353535851937790883648493n;
  const mod = (value, modulus = P) => ((value % modulus) + modulus) % modulus;

  function power(base, exponent) {
    let result = 1n;
    let current = mod(base);
    let remaining = exponent;
    while (remaining > 0n) {
      if (remaining & 1n) result = (result * current) % P;
      current = (current * current) % P;
      remaining >>= 1n;
    }
    return result;
  }

  const D = mod(-121665n * power(121666n, P - 2n));
  const SQRT_M1 = power(2n, (P - 1n) / 4n);

  function littleEndian(bytes) {
    let value = 0n;
    for (let index = bytes.length - 1; index >= 0; index -= 1) value = (value << 8n) | BigInt(bytes[index]);
    return value;
  }

  function decodePoint(bytes) {
    const number = littleEndian(bytes);
    const y = number & ((1n << 255n) - 1n);
    const sign = number >> 255n;
    if (y >= P) return null;
    const x2 = mod((y * y - 1n) * power(D * y * y + 1n, P - 2n));
    if (x2 === 0n) return sign ? null : [0n, y, 1n, 0n];
    let x = power(x2, (P + 3n) / 8n);
    if (mod(x * x - x2) !== 0n) x = mod(x * SQRT_M1);
    if (mod(x * x - x2) !== 0n) return null;
    if ((x & 1n) !== sign) x = P - x;
    return [x, y, 1n, mod(x * y)];
  }

  function addPoints(p, q) {
    const a = mod((p[1] - p[0]) * (q[1] - q[0]));
    const b = mod((p[1] + p[0]) * (q[1] + q[0]));
    const c = mod(2n * p[3] * q[3] * D);
    const d = mod(2n * p[2] * q[2]);
    const e = b - a;
    const f = d - c;
    const g = d + c;
    const h = b + a;
    return [mod(e * f), mod(g * h), mod(f * g), mod(e * h)];
  }

  function isIdentity(point) {
    return mod(point[0]) === 0n && mod(point[1] - point[2]) === 0n;
  }

  function inPrimeOrderSubgroup(point) {
    let result = [0n, 1n, 1n, 0n];
    let current = point;
    let scalar = L;
    while (scalar > 0n) {
      if (scalar & 1n) result = addPoints(result, current);
      current = addPoints(current, current);
      scalar >>= 1n;
    }
    return isIdentity(result);
  }

  async function strictVerify(publicKey, signature, message) {
    const point = decodePoint(publicKey);
    if (!point || isIdentity(point) || !inPrimeOrderSubgroup(point)) return false;
    if (!decodePoint(signature.slice(0, 32)) || littleEndian(signature.slice(32)) >= L) return false;
    let key;
    try {
      key = await root.crypto.subtle.importKey("raw", publicKey, { name: "Ed25519" }, false, ["verify"]);
    } catch (_error) {
      return false;
    }
    return root.crypto.subtle.verify({ name: "Ed25519" }, key, signature, message);
  }

  // ------------------------------------------------------------------------
  // Records

  const safeNonnegative = (value) => Number.isSafeInteger(value) && value >= 0;
  const text = (value, minimum, maximum) =>
    typeof value === "string" && utf8Length(value) >= minimum && utf8Length(value) <= maximum;
  const httpsUri = (value) => typeof value === "string" && utf8Length(value) <= MAX_URI_BYTES && URI_RE.test(value);

  function identityKey(subject) {
    if (typeof subject !== "string" || !subject.startsWith(IDENTITY_PREFIX)) return null;
    const key = subject.slice(IDENTITY_PREFIX.length);
    try {
      base64urlDecode(key, 32, "invalid_subject");
    } catch (_error) {
      return null;
    }
    return key;
  }

  const signalId = (record) => digest(SIGNAL_SCHEMA, without(record, "signalId", "signature"));
  const revocationId = (record) => digest(REVOCATION_SCHEMA, without(record, "revocationId", "signature"));

  function validateScope(kind, record) {
    const { scope, evidence } = record;
    if (TIERS[kind] === "interest") {
      const emptyScope = isPlainObject(scope) && Object.keys(scope).length === 0;
      if (!emptyScope || !Array.isArray(evidence) || evidence.length !== 0) reject("interest_claims_scope");
      return;
    }
    if (!isPlainObject(scope) || !sameKeys(scope, SCOPE_KEYS[kind])) reject("invalid_scope");
    let ok;
    if (kind === "usage-endorsement") {
      ok = ENVIRONMENTS.includes(scope.environment) && text(scope.version, 1, 64)
        && safeNonnegative(scope.since) && scope.since <= record.issuedAt;
    } else if (kind === "security-review") {
      ok = typeof scope.revision === "string" && REVISION_RE.test(scope.revision)
        && REVIEW_METHODS.includes(scope.method) && FINDINGS.includes(scope.findings);
    } else {
      const protocols = scope.protocols;
      ok = httpsUri(scope.endpoint) && Array.isArray(protocols) && protocols.length > 0
        && protocols.every((item, index) => MIRROR_PROTOCOLS.includes(item) && (index === 0 || protocols[index - 1] < item));
    }
    if (!ok) reject("invalid_scope");
    if (!Array.isArray(evidence)) reject("invalid_evidence");
    if (evidence.length === 0) reject("evidence_required");
    if (evidence.length > MAX_EVIDENCE) reject("invalid_evidence");
    for (const item of evidence) {
      if (!isPlainObject(item) || !sameKeys(item, EVIDENCE_KEYS) || !text(item.label, 1, 120) || !httpsUri(item.uri)
        || !(item.sha256 === null || (typeof item.sha256 === "string" && SHA_RE.test(item.sha256)))) {
        reject("invalid_evidence");
      }
    }
  }

  async function validateSignal(raw, asOf) {
    const record = normalize(raw);
    if (!isPlainObject(record)) reject("invalid_record");
    if (!sameKeys(record, SIGNAL_KEYS)) reject("invalid_record_keys");
    if (record.schema !== SIGNAL_SCHEMA) reject("unsupported_schema");
    const kind = record.kind;
    if (typeof kind !== "string" || !own(TIERS, kind)) reject("unknown_kind");
    if (record.tier !== TIERS[kind]) reject("tier_mismatch");
    const signer = base64urlDecode(record.signer, 32, "invalid_signer");
    if (kind === "follow") {
      const followed = identityKey(record.subject);
      if (followed === null) reject("invalid_subject");
      if (followed === record.signer) reject("self_follow");
    } else if (typeof record.subject !== "string" || !REPOSITORY_RE.test(record.subject)) {
      reject("invalid_subject");
    }
    if (!safeNonnegative(record.issuedAt)) reject("invalid_issued_at");
    const expires = record.expiresAt;
    if (expires !== null && !(safeNonnegative(expires) && expires > record.issuedAt)) reject("invalid_expires_at");
    if (kind === "mirror-commitment" && expires === null) reject("commitment_requires_expiry");
    if (!VISIBILITIES.includes(record.visibility)) reject("invalid_visibility");
    if (!text(record.note, 0, MAX_NOTE_BYTES)) reject("invalid_note");
    validateScope(kind, record);
    const signature = base64urlDecode(record.signature, 64, "invalid_signature_encoding");
    if (record.signalId !== await signalId(record)) reject("signal_id_mismatch");
    const message = domainBytes(SIGNAL_SCHEMA, without(record, "signalId", "signature"));
    if (!await strictVerify(signer, signature, message)) reject("invalid_signature");
    if (record.issuedAt > asOf + MAX_CLOCK_SKEW_MS) reject("issued_in_future");
    return record;
  }

  async function validateRevocation(raw, asOf) {
    const record = normalize(raw);
    if (!isPlainObject(record)) reject("invalid_record");
    if (!sameKeys(record, REVOCATION_KEYS)) reject("invalid_record_keys");
    if (record.schema !== REVOCATION_SCHEMA) reject("unsupported_schema");
    const signer = base64urlDecode(record.signer, 32, "invalid_signer");
    if (typeof record.revokes !== "string" || !SHA_RE.test(record.revokes)) reject("invalid_revokes");
    if (!safeNonnegative(record.revokedAt)) reject("invalid_revoked_at");
    if (!REASONS.includes(record.reason)) reject("invalid_reason");
    const signature = base64urlDecode(record.signature, 64, "invalid_signature_encoding");
    if (record.revocationId !== await revocationId(record)) reject("revocation_id_mismatch");
    const message = domainBytes(REVOCATION_SCHEMA, without(record, "revocationId", "signature"));
    if (!await strictVerify(signer, signature, message)) reject("invalid_signature");
    if (record.revokedAt > asOf + MAX_CLOCK_SKEW_MS) reject("issued_in_future");
    return record;
  }

  // PKCS#8 wrapping of a raw 32-byte Ed25519 seed (RFC 8410).
  const PKCS8_PREFIX = [0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20];

  async function importSeed(seed) {
    if (!(seed instanceof Uint8Array) || seed.length !== 32) reject("invalid_seed");
    const pkcs8 = new Uint8Array(48);
    pkcs8.set(PKCS8_PREFIX, 0);
    pkcs8.set(seed, 16);
    const privateKey = await root.crypto.subtle.importKey("pkcs8", pkcs8, { name: "Ed25519" }, true, ["sign"]);
    const jwk = await root.crypto.subtle.exportKey("jwk", privateKey);
    return { privateKey, publicKey: jwk.x };
  }

  async function signSignal(seed, unsigned) {
    const { privateKey, publicKey } = await importSeed(seed);
    const record = normalize(Object.assign({
      schema: SIGNAL_SCHEMA,
      tier: TIERS[unsigned.kind] || "interest",
      expiresAt: null,
      visibility: "public",
      scope: {},
      evidence: [],
      note: "",
    }, without(unsigned, "signalId", "signature"), { signer: publicKey }));
    const message = domainBytes(SIGNAL_SCHEMA, record);
    const signature = new Uint8Array(await root.crypto.subtle.sign({ name: "Ed25519" }, privateKey, message));
    return Object.assign(record, { signalId: await signalId(record), signature: base64url(signature) });
  }

  async function signRevocation(seed, revokes, revokedAt, reason = "withdrawn") {
    const { privateKey, publicKey } = await importSeed(seed);
    const record = { schema: REVOCATION_SCHEMA, signer: publicKey, revokes, revokedAt, reason };
    const message = domainBytes(REVOCATION_SCHEMA, record);
    const signature = new Uint8Array(await root.crypto.subtle.sign({ name: "Ed25519" }, privateKey, message));
    return Object.assign(record, { revocationId: await revocationId(record), signature: base64url(signature) });
  }

  function describe(record) {
    const { kind, scope } = record;
    const evidence = `(${record.evidence.length} evidence)`;
    if (kind === "usage-endorsement") {
      return `Usage endorsement: ${scope.environment} use of ${scope.version} ${evidence}`;
    }
    if (kind === "security-review") {
      return `Security review: ${scope.method} of ${scope.revision.slice(0, 12)}, findings ${scope.findings} ${evidence}`;
    }
    if (kind === "mirror-commitment") {
      return `Mirror commitment: ${scope.endpoint} until ${record.expiresAt} ${evidence}`;
    }
    return { star: "Star", watch: "Watching", bookmark: "Bookmark", follow: "Follow", recommendation: "Recommendation" }[kind];
  }

  // Code-point order, matching Python's sorted() on the ASCII identifiers used here.
  const byString = (left, right) => (left < right ? -1 : left > right ? 1 : 0);

  // ------------------------------------------------------------------------
  // Store

  class SignalStore {
    constructor() {
      this.signals = new Map();
      this.revocations = new Map();
    }

    async importBundle(raw, asOf) {
      if (!isPlainObject(raw) || !sameKeys(raw, BUNDLE_KEYS)) reject("invalid_bundle");
      if (raw.schema !== BUNDLE_SCHEMA) reject("unsupported_bundle_schema");
      if (!safeNonnegative(raw.exportedAt) || !Array.isArray(raw.signals) || !Array.isArray(raw.revocations)) {
        reject("invalid_bundle");
      }
      if (raw.signals.length + raw.revocations.length > MAX_BUNDLE_RECORDS) reject("bundle_too_large");
      const report = { accepted: 0, duplicate: 0, rejected: [] };
      const passes = [
        ["signal", raw.signals, validateSignal, "signalId", this.signals],
        ["revocation", raw.revocations, validateRevocation, "revocationId", this.revocations],
      ];
      for (const [type, records, validate, idKey, target] of passes) {
        for (const item of records) {
          let record;
          try {
            record = await validate(item, asOf);
          } catch (error) {
            if (!(error instanceof SocialSignalError)) throw error;
            const claimed = isPlainObject(item) ? item[idKey] : null;
            report.rejected.push({ type, id: typeof claimed === "string" ? claimed : null, error: error.code });
            continue;
          }
          if (target.has(record[idKey])) {
            report.duplicate += 1;
          } else {
            target.set(record[idKey], record);
            report.accepted += 1;
          }
        }
      }
      return report;
    }

    digest() {
      return digest(STATE_SCHEMA, {
        revocations: [...this.revocations.keys()].sort(byString),
        signals: [...this.signals.keys()].sort(byString),
      });
    }

    async fold(asOf) {
      const revokedBy = new Map();
      const revocations = [];
      for (const id of [...this.revocations.keys()].sort(byString)) {
        const revocation = this.revocations.get(id);
        const target = this.signals.get(revocation.revokes);
        let status = "applied";
        if (!target) status = "pending";
        else if (target.signer !== revocation.signer) status = "signer_mismatch";
        else if (!revokedBy.has(revocation.revokes)) revokedBy.set(revocation.revokes, id);
        revocations.push({ revocationId: id, revokes: revocation.revokes, status });
      }
      const groupKey = (record) => JSON.stringify([record.signer, record.kind, record.subject]);
      const latest = new Map();
      for (const record of this.signals.values()) {
        const key = groupKey(record);
        const current = latest.get(key);
        if (!current || record.issuedAt > current.issuedAt
          || (record.issuedAt === current.issuedAt && record.signalId > current.signalId)) {
          latest.set(key, record);
        }
      }
      const signals = [];
      for (const id of [...this.signals.keys()].sort(byString)) {
        const record = this.signals.get(id);
        const newest = latest.get(groupKey(record)).signalId;
        let status = "active";
        if (revokedBy.has(id)) status = "revoked";
        else if (newest !== id) status = "superseded";
        else if (record.expiresAt !== null && record.expiresAt <= asOf) status = "expired";
        signals.push({
          signalId: id,
          kind: record.kind,
          tier: record.tier,
          signer: record.signer,
          subject: record.subject,
          issuedAt: record.issuedAt,
          expiresAt: record.expiresAt,
          visibility: record.visibility,
          status,
          revokedBy: revokedBy.get(id) || null,
          supersededBy: newest === id ? null : newest,
        });
      }
      return { schema: STATE_SCHEMA, asOf, digest: await this.digest(), signals, revocations };
    }

    exportBundle(audience, exportedAt, signer = null) {
      if (!own(AUDIENCES, audience)) reject("invalid_audience");
      const allowed = AUDIENCES[audience];
      const signals = [...this.signals.keys()].sort(byString).map((id) => this.signals.get(id))
        .filter((record) => allowed.includes(record.visibility) && (signer === null || record.signer === signer));
      const included = new Set(signals.map((record) => record.signalId));
      const revocations = [...this.revocations.keys()].sort(byString).map((id) => this.revocations.get(id))
        .filter((record) => (signer === null || record.signer === signer)
          && (included.has(record.revokes) || (audience === "self" && !this.signals.has(record.revokes))));
      return { schema: BUNDLE_SCHEMA, exportedAt, signals, revocations };
    }

    async recommend(controls) {
      if (!isPlainObject(controls) || !sameKeys(controls, CONTROL_KEYS)) reject("invalid_controls");
      const { viewer, mutedSigners, mutedSubjects } = controls;
      base64urlDecode(viewer, 32, "invalid_controls");
      if (typeof controls.enabled !== "boolean" || !safeNonnegative(controls.asOf)
        || !Number.isSafeInteger(controls.limit) || controls.limit < 1 || controls.limit > 100
        || !Array.isArray(mutedSigners) || !Array.isArray(mutedSubjects)
        || mutedSigners.length > MAX_CONTROL_MUTES || mutedSubjects.length > MAX_CONTROL_MUTES
        || ![...mutedSigners, ...mutedSubjects].every((item) => typeof item === "string")) {
        reject("invalid_controls");
      }
      const excluded = { inactive: 0, mutedSigner: 0, untrusted: 0, mutedSubject: 0, private: 0, alreadyKnown: 0 };
      const result = {
        schema: RECOMMENDATION_SCHEMA,
        viewer,
        enabled: controls.enabled,
        considered: 0,
        excluded,
        recommendations: [],
      };
      if (!controls.enabled) return result;
      const state = await this.fold(controls.asOf);
      const active = state.signals.filter((item) => item.status === "active");
      const trusted = new Set(active.filter((item) => item.kind === "follow" && item.signer === viewer)
        .map((item) => item.subject.slice(IDENTITY_PREFIX.length)));
      const known = new Set(active.filter((item) => item.signer === viewer).map((item) => item.subject));
      const mutedSignerSet = new Set(mutedSigners);
      const mutedSubjectSet = new Set(mutedSubjects);
      const supporters = new Map();
      for (const item of state.signals) {
        if (!own(WEIGHTS, item.kind) || item.signer === viewer) continue;
        result.considered += 1;
        let reason = null;
        if (item.status !== "active") reason = "inactive";
        else if (mutedSignerSet.has(item.signer)) reason = "mutedSigner";
        else if (!trusted.has(item.signer)) reason = "untrusted";
        else if (mutedSubjectSet.has(item.subject)) reason = "mutedSubject";
        else if (item.visibility === "private") reason = "private";
        else if (known.has(item.subject)) reason = "alreadyKnown";
        if (reason) {
          excluded[reason] += 1;
          continue;
        }
        const record = this.signals.get(item.signalId);
        if (!supporters.has(item.subject)) supporters.set(item.subject, new Map());
        const bySigner = supporters.get(item.subject);
        if (!bySigner.has(item.signer)) bySigner.set(item.signer, []);
        bySigner.get(item.signer).push({
          signalId: item.signalId,
          signer: item.signer,
          kind: item.kind,
          tier: item.tier,
          weight: WEIGHTS[item.kind],
          summary: describe(record),
          scope: record.scope,
          evidence: record.evidence,
        });
      }
      const ranked = [...supporters].map(([subject, bySigner]) => ({
        subject,
        score: [...bySigner.values()].reduce((total, reasons) => total + Math.max(...reasons.map((r) => r.weight)), 0),
        supporters: bySigner.size,
        reasons: [...bySigner.keys()].sort(byString).flatMap((signer) => bySigner.get(signer)),
      }));
      ranked.sort((left, right) => right.score - left.score || byString(left.subject, right.subject));
      result.recommendations = ranked.slice(0, controls.limit);
      return result;
    }
  }

  const api = {
    AUDIENCES,
    BUNDLE_SCHEMA,
    RECOMMENDATION_SCHEMA,
    REVOCATION_SCHEMA,
    SIGNAL_SCHEMA,
    STATE_SCHEMA,
    SignalStore,
    SocialSignalError,
    TIERS,
    WEIGHTS,
    canonicalBytes,
    describe,
    signRevocation,
    signSignal,
    validateRevocation,
    validateSignal,
  };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  root.ForkMeshSocialSignals = api;
}(typeof globalThis !== "undefined" ? globalThis : this));
