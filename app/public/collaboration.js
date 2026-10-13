/*
 * ForkMesh conflict-safe collaboration protocol, browser/Node implementation.
 *
 * Keep this implementation independent from app/src/collaboration.py.  Both
 * consume the public v1 contract and shared signed corpus; CI compares their
 * complete materialized state and digest.
 */
(function collaborationModule(root) {
  "use strict";

  const EVENT_SCHEMA = "forkmesh-collaboration-event-v1";
  const STATE_SCHEMA = "forkmesh-collaboration-state-v1";
  const POLICY_SCHEMA = "forkmesh-collaboration-policy-v1";
  const FAMILIES = [
    "issue", "pull_request", "comment", "label", "reaction", "review",
    "milestone", "board", "notification", "moderation",
  ];
  const FAMILY_SET = new Set(FAMILIES);
  const SAFE_INTEGER = Number.MAX_SAFE_INTEGER;
  const MAX_JSON_DEPTH = 32;
  const KEY_RE = /^[A-Za-z0-9_.:/-]{1,256}$/;
  const SHA_RE = /^sha256:[0-9a-f]{64}$/;
  const REPOSITORY_RE = /^urn:forkmesh:repository:sha256:[0-9a-f]{64}$/;
  const OBJECT_RE = /^urn:forkmesh:collaboration:([a-z_]+):sha256:([0-9a-f]{64})$/;
  const REASON_RE = /^[a-z][a-z0-9_.-]{0,63}$/;
  const encoder = new TextEncoder();
  const signingDomain = encoder.encode("forkmesh-collaboration-event-v1\n");
  const objectDomain = encoder.encode("forkmesh-collaboration-object-v1\n");
  const repositoryDomain = encoder.encode("forkmesh-repository-identity-v1\n");
  const topKeys = [
    "schema", "repository", "policy", "family", "object", "action",
    "author", "sequence", "clock", "issuedAt", "parents", "references",
    "payload", "signature", "eventId",
  ];
  const unsignedKeys = topKeys.filter((key) => key !== "signature" && key !== "eventId");

  const scalarFields = {
    issue: ["title", "body", "status", "priority", "dueAt"],
    pull_request: ["title", "body", "status", "base", "head", "mergeCommit"],
    comment: ["body"],
    label: ["name", "color", "description"],
    reaction: ["emoji"],
    review: ["state", "body", "revision"],
    milestone: ["title", "description", "status", "dueAt"],
    board: ["name", "description", "status"],
    notification: ["kind", "status", "recipient", "message"],
    moderation: ["decision", "reasonCode", "note"],
  };
  const requiredFields = {
    issue: ["title", "body"],
    pull_request: ["title", "base", "head"],
    comment: ["body"],
    label: ["name", "color"],
    reaction: ["emoji"],
    review: ["state", "revision"],
    milestone: ["title"],
    board: ["name"],
    notification: ["kind", "recipient"],
    moderation: ["decision", "reasonCode"],
  };
  const setFields = {
    issue: ["labels", "assignees"],
    pull_request: ["labels", "reviewers"],
    comment: [], label: ["subjects"], reaction: [], review: [],
    milestone: ["subjects"], board: ["items"], notification: [], moderation: [],
  };
  const actions = Object.fromEntries(FAMILIES.map((family) => [
    family,
    new Set(["create", "update", "add", "remove", "tombstone", "restore"]),
  ]));
  actions.reaction = new Set(["create", "tombstone", "restore"]);
  actions.moderation = new Set(["decide", "appeal", "resolve"]);

  class CollaborationError extends Error {
    constructor(code, detail = "") {
      super(detail ? `${code}: ${detail}` : code);
      this.name = "CollaborationError";
      this.code = code;
    }
  }

  function reject(code, detail = "") {
    throw new CollaborationError(code, detail);
  }

  function normalizeJson(value, depth = 0) {
    if (depth > MAX_JSON_DEPTH) reject("json_too_deep");
    if (value === null || typeof value === "boolean") return value;
    if (typeof value === "number") {
      if (!Number.isSafeInteger(value)) reject("unsafe_integer");
      return value;
    }
    if (typeof value === "string") {
      const normalized = value.normalize("NFC");
      for (let index = 0; index < normalized.length; index += 1) {
        const unit = normalized.charCodeAt(index);
        if (unit >= 0xd800 && unit <= 0xdfff) {
          const next = normalized.charCodeAt(index + 1);
          if (unit > 0xdbff || !Number.isInteger(next) || next < 0xdc00 || next > 0xdfff) reject("invalid_unicode");
          index += 1;
        }
      }
      return normalized;
    }
    if (Array.isArray(value)) return value.map((item) => normalizeJson(item, depth + 1));
    if (!value || typeof value !== "object" || Object.getPrototypeOf(value) !== Object.prototype) {
      reject("invalid_json_type");
    }
    // A null-prototype target makes keys such as "__proto__" ordinary data
    // and prevents canonicalization from becoming a prototype-pollution sink.
    const result = Object.create(null);
    for (const key of Object.keys(value)) {
      if (!KEY_RE.test(key)) reject("invalid_key", key);
      const normalized = key.normalize("NFC");
      if (Object.prototype.hasOwnProperty.call(result, normalized)) reject("duplicate_key", normalized);
      result[normalized] = normalizeJson(value[key], depth + 1);
    }
    return result;
  }

  function canonicalJson(value) {
    function encode(current) {
      if (current === null || typeof current === "boolean" || typeof current === "number" || typeof current === "string") {
        return JSON.stringify(current);
      }
      if (Array.isArray(current)) return `[${current.map(encode).join(",")}]`;
      return `{${Object.keys(current).sort().map((key) => `${JSON.stringify(key)}:${encode(current[key])}`).join(",")}}`;
    }
    return encode(normalizeJson(value));
  }

  function parseCanonicalEvent(raw) {
    if (typeof raw !== "string" || !raw || encoder.encode(raw).length > 1048576) reject("invalid_event_size");
    let value;
    try {
      value = JSON.parse(raw);
    } catch (_error) {
      reject("invalid_json");
    }
    if (!value || typeof value !== "object" || Array.isArray(value)) reject("event_not_object");
    if (canonicalJson(value) !== raw) reject("noncanonical_encoding");
    return value;
  }

  function concatBytes(...parts) {
    const length = parts.reduce((total, part) => total + part.length, 0);
    const result = new Uint8Array(length);
    let offset = 0;
    for (const part of parts) {
      result.set(part, offset);
      offset += part.length;
    }
    return result;
  }

  async function sha256(bytes) {
    return new Uint8Array(await root.crypto.subtle.digest("SHA-256", bytes));
  }

  function hex(bytes) {
    return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("");
  }

  function base64urlDecode(value, length, code) {
    if (typeof value !== "string" || value.includes("=")) reject(code);
    let bytes;
    try {
      if (typeof Buffer !== "undefined") {
        bytes = new Uint8Array(Buffer.from(value, "base64url"));
      } else {
        const binary = atob(value.replace(/-/g, "+").replace(/_/g, "/") + "=".repeat((4 - value.length % 4) % 4));
        bytes = Uint8Array.from(binary, (character) => character.charCodeAt(0));
      }
    } catch (_error) {
      reject(code);
    }
    if (bytes.length !== length || base64url(bytes) !== value) reject(code);
    return bytes;
  }

  function base64url(bytes) {
    if (typeof Buffer !== "undefined") return Buffer.from(bytes).toString("base64url");
    let binary = "";
    for (const value of bytes) binary += String.fromCharCode(value);
    return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  }

  function exactKeys(value, expected) {
    if (!value || typeof value !== "object" || Array.isArray(value)) return false;
    const actual = Object.keys(value).sort();
    const wanted = [...expected].sort();
    return actual.length === wanted.length && actual.every((key, index) => key === wanted[index]);
  }

  function matches(pattern, value) {
    return typeof value === "string" && pattern.test(value);
  }

  function signingBytes(event) {
    const allowed = [topKeys, unsignedKeys, [...unsignedKeys, "signature"]];
    if (!allowed.some((candidate) => exactKeys(event, candidate))) reject("invalid_envelope_keys");
    const unsigned = Object.fromEntries(unsignedKeys.map((key) => [key, event[key]]));
    return concatBytes(signingDomain, encoder.encode(canonicalJson(unsigned)));
  }

  async function eventId(event) {
    const signature = base64urlDecode(event.signature, 64, "invalid_signature_encoding");
    return `sha256:${hex(await sha256(concatBytes(signingBytes(event), Uint8Array.of(0), signature)))}`;
  }

  function validateReferences(family, references) {
    if (!Array.isArray(references) || references.length > 16) reject("invalid_references");
    const result = references.map((reference) => {
      if (!exactKeys(reference, ["relation", "family", "object"])) reject("invalid_reference");
      const match = typeof reference.object === "string" && reference.object.match(OBJECT_RE);
      if (!matches(REASON_RE, reference.relation)) reject("invalid_reference_relation");
      if (!FAMILY_SET.has(reference.family) || !match || match[1] !== reference.family) {
        reject("invalid_reference_target");
      }
      return {...reference};
    });
    const sorted = [...result].sort((left, right) => {
      const a = `${left.relation}\0${left.family}\0${left.object}`;
      const b = `${right.relation}\0${right.family}\0${right.object}`;
      return a < b ? -1 : a > b ? 1 : 0;
    });
    if (canonicalJson(result) !== canonicalJson(sorted) || new Set(result.map((item) => canonicalJson(item))).size !== result.length) {
      reject("noncanonical_references");
    }
    const subjects = result.filter((item) => item.relation === "subject");
    if (family === "comment" && (subjects.length !== 1 || !["issue", "pull_request", "comment"].includes(subjects[0].family))) reject("comment_subject_required");
    if (family === "reaction" && (subjects.length !== 1 || !["issue", "pull_request", "comment", "review"].includes(subjects[0].family))) reject("reaction_subject_required");
    if (family === "review" && (subjects.length !== 1 || subjects[0].family !== "pull_request")) reject("review_pull_required");
    if (family === "moderation" && (subjects.length !== 1 || ["moderation", "notification"].includes(subjects[0].family))) reject("moderation_subject_required");
    if (["issue", "pull_request", "label", "milestone", "board"].includes(family) && subjects.length) reject("unexpected_subject");
    if (family === "notification" && subjects.length > 1) reject("invalid_notification_subject");
    return result;
  }

  async function objectId(repository, family, creator, objectKey, references = []) {
    if (!matches(REPOSITORY_RE, repository) || !FAMILY_SET.has(family)) reject("invalid_object_identity_input");
    base64urlDecode(creator, 32, "invalid_author");
    base64urlDecode(objectKey, 16, "invalid_object_key");
    const identity = {repository, family, creator, objectKey, references: validateReferences(family, references)};
    return `urn:forkmesh:collaboration:${family}:sha256:${hex(await sha256(concatBytes(objectDomain, encoder.encode(canonicalJson(identity)))))}`;
  }

  async function repositoryId(canonicalGitIdentity) {
    if (typeof canonicalGitIdentity !== "string" || !canonicalGitIdentity) reject("invalid_repository_identity");
    const normalized = normalizeJson(canonicalGitIdentity);
    return `urn:forkmesh:repository:sha256:${hex(await sha256(concatBytes(repositoryDomain, encoder.encode(normalized))))}`;
  }

  function validateEventIds(values, allowEmpty) {
    if (!Array.isArray(values) || (!allowEmpty && values.length === 0)) reject("invalid_event_id_list");
    if (values.some((value) => !matches(SHA_RE, value))) reject("invalid_event_id");
    const sorted = [...new Set(values)].sort();
    if (canonicalJson(values) !== canonicalJson(sorted)) reject("noncanonical_event_id_list");
  }

  function validateText(value, maximum, empty = true) {
    if (typeof value !== "string" || encoder.encode(value).length > maximum || (!empty && !value)) {
      reject("invalid_field_value");
    }
  }

  function validateFields(family, fields, create) {
    if (!fields || typeof fields !== "object" || Array.isArray(fields) || Object.keys(fields).length === 0) reject("invalid_fields");
    if (Object.keys(fields).some((key) => !scalarFields[family].includes(key))) reject("unknown_field");
    if (create && requiredFields[family].some((key) => !(key in fields))) reject("missing_create_field");
    const immutable = {reaction: ["emoji"], notification: ["kind", "recipient"], review: ["revision"]}[family] || [];
    if (!create && Object.keys(fields).some((key) => immutable.includes(key))) reject("immutable_field");
    for (const [name, value] of Object.entries(fields)) {
      if (["title", "name", "kind"].includes(name)) validateText(value, 512, false);
      if (["body", "description", "message", "note"].includes(name)) validateText(value, 900000);
      if (["base", "head", "mergeCommit", "revision"].includes(name)) validateText(value, 512, false);
      if (name === "color" && (typeof value !== "string" || !/^#[0-9a-f]{6}$/.test(value))) reject("invalid_color");
      if (name === "emoji") validateText(value, 64, false);
      if (name === "recipient") base64urlDecode(value, 32, "invalid_recipient");
      if (name === "priority" && (!Number.isInteger(value) || value < 0 || value > 9999)) reject("invalid_priority");
      if (name === "dueAt" && value !== null && (!Number.isSafeInteger(value) || value < 0)) reject("invalid_due_at");
      if (name === "reasonCode" && !matches(REASON_RE, value)) reject("invalid_reason_code");
      if (name === "decision" && !["allow", "hide", "quarantine"].includes(value)) reject("invalid_moderation_decision");
      if (name === "status") {
        const allowed = {issue: ["open", "closed"], pull_request: ["open", "closed", "merged"], milestone: ["open", "closed"], board: ["open", "closed", "archived"], notification: ["unread", "read"]}[family] || [];
        if (!allowed.includes(value)) reject("invalid_status");
      }
      if (name === "state" && (family !== "review" || !["commented", "approved", "changes_requested", "dismissed"].includes(value))) reject("invalid_review_state");
    }
  }

  function validateSetValue(family, field, value) {
    if (["assignees", "reviewers"].includes(field)) return base64urlDecode(value, 32, "invalid_set_identity");
    if (field === "labels") {
      const match = typeof value === "string" && value.match(OBJECT_RE);
      if (!match || match[1] !== "label") reject("invalid_label_reference");
      return;
    }
    if (["subjects", "items"].includes(field)) {
      const match = value && typeof value.object === "string" && value.object.match(OBJECT_RE);
      if (!exactKeys(value, ["family", "object"]) || !FAMILY_SET.has(value.family) || !match || match[1] !== value.family) reject("invalid_set_reference");
      return;
    }
    reject("invalid_set_field");
  }

  function validatePayload(family, action, payload) {
    if (!payload || typeof payload !== "object" || Array.isArray(payload)) reject("invalid_payload");
    if (["create", "decide"].includes(action)) {
      if (!("objectKey" in payload) || !("fields" in payload) || Object.keys(payload).some((key) => !["objectKey", "fields", "legacy"].includes(key))) reject("invalid_create_payload");
      base64urlDecode(payload.objectKey, 16, "invalid_object_key");
      validateFields(family, payload.fields, true);
      if ("legacy" in payload) {
        const legacy = payload.legacy;
        if (!exactKeys(legacy, ["schema", "digest", "signatureStatus"])) reject("invalid_legacy_provenance");
        validateText(legacy.schema, 128, false);
        if (!matches(SHA_RE, legacy.digest)) reject("invalid_legacy_digest");
        if (!["valid", "invalid", "unavailable"].includes(legacy.signatureStatus)) reject("invalid_legacy_signature_status");
      }
    } else if (action === "update") {
      if (!exactKeys(payload, ["fields"])) reject("invalid_update_payload");
      validateFields(family, payload.fields, false);
    } else if (action === "add" || action === "remove") {
      const expected = action === "add" ? ["field", "values"] : ["field", "values", "observed"];
      if (!exactKeys(payload, expected) || !setFields[family].includes(payload.field) || !Array.isArray(payload.values) || !payload.values.length) reject(`invalid_${action}_payload`);
      const encoded = payload.values.map(canonicalJson);
      if (canonicalJson(encoded) !== canonicalJson([...new Set(encoded)].sort())) reject("noncanonical_set_values");
      payload.values.forEach((value) => validateSetValue(family, payload.field, value));
      if (action === "remove") validateEventIds(payload.observed, false);
    } else if (action === "tombstone") {
      if (!exactKeys(payload, ["reasonCode"]) || !matches(REASON_RE, payload.reasonCode)) reject("invalid_tombstone_payload");
    } else if (action === "restore") {
      if (!exactKeys(payload, ["reasonCode", "observed"]) || !matches(REASON_RE, payload.reasonCode)) reject("invalid_restore_payload");
      validateEventIds(payload.observed, false);
    } else if (action === "appeal") {
      if (!("predecessor" in payload) || !("reasonCode" in payload) || Object.keys(payload).some((key) => !["predecessor", "reasonCode", "body"].includes(key))) reject("invalid_appeal_payload");
      if (!matches(SHA_RE, payload.predecessor)) reject("invalid_appeal_predecessor");
      if (!matches(REASON_RE, payload.reasonCode)) reject("invalid_reason_code");
      if ("body" in payload) validateText(payload.body, 100000);
    } else if (action === "resolve") {
      if (!exactKeys(payload, ["predecessor", "appeal", "fields"])) reject("invalid_resolution_payload");
      if (!matches(SHA_RE, payload.predecessor) || !matches(SHA_RE, payload.appeal)) reject("invalid_resolution_lineage");
      validateFields(family, payload.fields, false);
      if (!("decision" in payload.fields) || !("reasonCode" in payload.fields)) reject("incomplete_resolution");
    }
  }

  async function policyId(policy) {
    if (!exactKeys(policy, ["schema", "repository", "maintainers", "moderators", "notifiers"]) || policy.schema !== POLICY_SCHEMA || !matches(REPOSITORY_RE, policy.repository)) reject("invalid_policy");
    for (const role of ["maintainers", "moderators", "notifiers"]) {
      if (!Array.isArray(policy[role]) || canonicalJson(policy[role]) !== canonicalJson([...new Set(policy[role])].sort())) reject("noncanonical_policy");
      policy[role].forEach((key) => base64urlDecode(key, 32, "invalid_policy_key"));
    }
    return `sha256:${hex(await sha256(encoder.encode(canonicalJson(policy))))}`;
  }

  async function validateEvent(event, policy, expectedPolicy) {
    if (!exactKeys(event, topKeys)) reject("invalid_envelope_keys");
    if (event.schema !== EVENT_SCHEMA) reject("unsupported_schema");
    if (!matches(REPOSITORY_RE, event.repository)) reject("invalid_repository");
    if (event.repository !== policy.repository) reject("wrong_repository");
    if (!matches(SHA_RE, event.policy)) reject("invalid_policy_id");
    if (event.policy !== expectedPolicy) reject("unknown_policy");
    if (!FAMILY_SET.has(event.family) || !actions[event.family].has(event.action)) reject("unknown_family_action");
    const objectMatch = typeof event.object === "string" && event.object.match(OBJECT_RE);
    if (!objectMatch || objectMatch[1] !== event.family) reject("invalid_object_id");
    const publicKey = base64urlDecode(event.author, 32, "invalid_author");
    const signature = base64urlDecode(event.signature, 64, "invalid_signature_encoding");
    for (const name of ["sequence", "clock", "issuedAt"]) {
      const value = event[name];
      if (!Number.isSafeInteger(value) || value < 0) reject(`invalid_${name}`);
    }
    validateEventIds(event.parents, true);
    if (event.parents.length > 64) reject("too_many_parents");
    const refs = validateReferences(event.family, event.references);
    validatePayload(event.family, event.action, event.payload);
    if (event.eventId !== await eventId(event)) reject("event_id_mismatch");
    if (["create", "decide"].includes(event.action)) {
      if (event.object !== await objectId(event.repository, event.family, event.author, event.payload.objectKey, refs)) reject("object_id_mismatch");
    }
    let key;
    try {
      key = await root.crypto.subtle.importKey("raw", publicKey, {name: "Ed25519"}, false, ["verify"]);
    } catch (_error) {
      reject("invalid_author");
    }
    if (!await root.crypto.subtle.verify({name: "Ed25519"}, key, signature, signingBytes(event))) reject("invalid_signature");
  }

  function ancestorsOf(events) {
    const indegree = new Map([...events.keys()].map((id) => [id, 0]));
    const children = new Map([...events.keys()].map((id) => [id, []]));
    for (const [id, event] of events) for (const parent of event.parents) if (events.has(parent)) {
      indegree.set(id, indegree.get(id) + 1);
      children.get(parent).push(id);
    }
    const ready = [...events.keys()].filter((id) => indegree.get(id) === 0).sort();
    const ancestors = new Map();
    for (let cursor = 0; cursor < ready.length; cursor += 1) {
      const id = ready[cursor];
      const lineage = new Set();
      for (const parent of events.get(id).parents) if (events.has(parent)) {
        lineage.add(parent);
        for (const ancestor of ancestors.get(parent)) lineage.add(ancestor);
      }
      ancestors.set(id, lineage);
      for (const child of children.get(id).sort()) {
        indegree.set(child, indegree.get(child) - 1);
        if (indegree.get(child) === 0) ready.push(child);
      }
    }
    if (ancestors.size !== events.size) reject("causal_cycle");
    return ancestors;
  }

  function eventOrder(left, right) {
    return left.clock - right.clock || (left.eventId < right.eventId ? -1 : left.eventId > right.eventId ? 1 : 0);
  }

  function authorized(event, creator, policy, subjectCreator = "", recipient = "") {
    const maintainers = new Set(policy.maintainers);
    if (event.family === "moderation") return ["decide", "resolve"].includes(event.action) ? new Set(policy.moderators).has(event.author) : event.action === "appeal" && event.author === subjectCreator;
    if (event.action === "create") {
      if (["label", "milestone", "board"].includes(event.family)) return maintainers.has(event.author);
      if (event.family === "notification") return event.author === event.payload.fields.recipient || new Set(policy.notifiers).has(event.author);
      return true;
    }
    if (["issue", "pull_request"].includes(event.family)) return event.author === creator || maintainers.has(event.author);
    if (["comment", "reaction"].includes(event.family)) return event.author === creator;
    if (event.family === "review") return event.author === creator || (event.action === "tombstone" && maintainers.has(event.author));
    if (["label", "milestone", "board"].includes(event.family)) return maintainers.has(event.author);
    if (event.family === "notification") return event.author === recipient;
    return false;
  }

  function semanticError(event, accepted, ancestors) {
    const lineage = ancestors.get(event.eventId);
    const payload = event.payload;
    if (event.action === "remove") {
      const wanted = new Set(payload.values.map(canonicalJson));
      const covered = new Set();
      for (const id of payload.observed) {
        const source = accepted.get(id);
        const sourceValues = new Set(source && source.action === "add" ? source.payload.values.map(canonicalJson) : []);
        const overlap = [...wanted].filter((value) => sourceValues.has(value));
        if (!lineage.has(id) || !source || source.object !== event.object || source.action !== "add" || source.payload.field !== payload.field || !overlap.length) return "invalid_observed_add";
        overlap.forEach((value) => covered.add(value));
      }
      if ([...wanted].some((value) => !covered.has(value))) return "invalid_observed_add";
    } else if (event.action === "restore") {
      for (const id of payload.observed) {
        const source = accepted.get(id);
        if (!lineage.has(id) || !source || source.object !== event.object || source.action !== "tombstone") return "invalid_observed_tombstone";
      }
    } else if (event.action === "appeal") {
      const source = accepted.get(payload.predecessor);
      if (!lineage.has(payload.predecessor) || !source || source.object !== event.object || !["decide", "resolve"].includes(source.action)) return "invalid_moderation_lineage";
    } else if (event.action === "resolve") {
      const predecessor = accepted.get(payload.predecessor);
      const appeal = accepted.get(payload.appeal);
      if (!lineage.has(payload.predecessor) || !lineage.has(payload.appeal) || !predecessor || !appeal || predecessor.object !== event.object || appeal.object !== event.object || !["decide", "resolve"].includes(predecessor.action) || appeal.action !== "appeal" || appeal.payload.predecessor !== payload.predecessor) return "invalid_moderation_lineage";
    }
    return null;
  }

  function maximal(candidates, ancestors) {
    return candidates.filter((candidate) => !candidates.some((other) => other.eventId !== candidate.eventId && ancestors.get(other.eventId).has(candidate.eventId))).sort(eventOrder);
  }

  function foldObject(family, events, ancestors) {
    const ordered = [...events].sort(eventOrder);
    const creates = ordered.filter((event) => ["create", "decide"].includes(event.action));
    const first = creates[0];
    const writes = new Map();
    for (const event of ordered) {
      if (["create", "decide", "update", "resolve"].includes(event.action)) {
        for (const [name, value] of Object.entries(event.payload.fields)) {
          if (!writes.has(name)) writes.set(name, []);
          writes.get(name).push({event, value});
        }
      }
    }
    const values = {};
    const conflicts = {};
    for (const name of [...writes.keys()].sort()) {
      const entries = writes.get(name);
      const ids = new Set(maximal(entries.map((entry) => entry.event), ancestors).map((event) => event.eventId));
      const current = entries.filter((entry) => ids.has(entry.event.eventId));
      if (new Set(current.map((entry) => canonicalJson(entry.value))).size === 1) values[name] = current[0].value;
      else conflicts[name] = current.map((entry) => ({eventId: entry.event.eventId, value: entry.value})).sort((a, b) => a.eventId < b.eventId ? -1 : a.eventId > b.eventId ? 1 : 0);
    }
    const sets = {};
    for (const field of [...setFields[family]].sort()) {
      const additions = new Map();
      const removed = new Set();
      for (const event of ordered) {
        if (event.action === "add" && event.payload.field === field) for (const value of event.payload.values) additions.set(`${event.eventId}\0${canonicalJson(value)}`, value);
        if (event.action === "remove" && event.payload.field === field) {
          for (const observed of event.payload.observed) for (const value of event.payload.values) {
            const tag = `${observed}\0${canonicalJson(value)}`;
            if (additions.has(tag)) removed.add(tag);
          }
        }
      }
      const visible = new Map();
      for (const [tag, value] of additions) if (!removed.has(tag)) visible.set(canonicalJson(value), value);
      sets[field] = [...visible.keys()].sort().map((key) => visible.get(key));
    }
    const tombstones = new Set(ordered.filter((event) => event.action === "tombstone").map((event) => event.eventId));
    for (const event of ordered) if (event.action === "restore") event.payload.observed.forEach((id) => tombstones.delete(id));
    const state = {
      family,
      object: first.object,
      creator: first.author,
      references: first.references,
      values,
      sets,
      conflicts,
      tombstoned: tombstones.size > 0,
      tombstones: [...tombstones].sort(),
      visibility: "visible",
      moderation: [],
      heads: maximal(ordered, ancestors).map((event) => event.eventId),
      history: ordered.map((event) => event.eventId),
    };
    if (creates.length > 1) state.conflicts.object = creates.map((event) => event.eventId);
    if (family === "moderation") {
      const resolved = new Set(ordered.filter((event) => event.action === "resolve").map((event) => event.payload.appeal));
      state.pendingAppeals = ordered.filter((event) => event.action === "appeal" && !resolved.has(event.eventId)).map((event) => event.eventId).sort();
    }
    return state;
  }

  async function foldEvents(suppliedEvents, policy, deliveries = null) {
    if (!Array.isArray(suppliedEvents)) reject("invalid_event_collection");
    const expectedPolicy = await policyId(policy);
    const uniqueRaw = new Map();
    for (const supplied of suppliedEvents) {
      const raw = typeof supplied === "string" ? supplied : canonicalJson(supplied);
      const digest = `sha256:${hex(await sha256(encoder.encode(raw)))}`;
      if (!uniqueRaw.has(digest)) uniqueRaw.set(digest, raw);
    }
    const candidates = new Map();
    const rejected = new Map();
    let signatureValid = 0;
    const rawEntries = [...uniqueRaw.entries()].sort((left, right) => left[0] < right[0] ? -1 : left[0] > right[0] ? 1 : 0);
    for (const [digest, raw] of rawEntries) {
      try {
        const event = parseCanonicalEvent(raw);
        await validateEvent(event, policy, expectedPolicy);
        signatureValid += 1;
        candidates.set(event.eventId, event);
      } catch (error) {
        if (!(error instanceof CollaborationError)) throw error;
        rejected.set(digest, error.code);
      }
    }
    const slots = new Map();
    for (const event of candidates.values()) {
      const slot = `${event.author}\0${event.sequence}`;
      if (!slots.has(slot)) slots.set(slot, []);
      slots.get(slot).push(event.eventId);
    }
    let equivocations = 0;
    for (const ids of slots.values()) if (ids.length > 1) {
      equivocations += 1;
      ids.forEach((id) => {
        candidates.delete(id);
        rejected.set(id, "author_sequence_equivocation");
      });
    }
    let ancestors;
    try {
      ancestors = ancestorsOf(candidates);
    } catch (error) {
      if (!(error instanceof CollaborationError)) throw error;
      for (const id of candidates.keys()) rejected.set(id, error.code);
      candidates.clear();
      ancestors = new Map();
    }
    const structural = new Map();
    const pending = new Map();
    for (const event of [...candidates.values()].sort(eventOrder)) {
      if (event.parents.some((parent) => !candidates.has(parent))) {
        pending.set(event.eventId, "missing_parent");
        continue;
      }
      const expectedClock = event.parents.length ? 1 + Math.max(...event.parents.map((parent) => candidates.get(parent).clock)) : 0;
      if (event.clock !== expectedClock) {
        rejected.set(event.eventId, "invalid_clock");
        continue;
      }
      const sameAuthor = [...ancestors.get(event.eventId)].filter((id) => candidates.get(id).author === event.author).map((id) => candidates.get(id).sequence);
      if (sameAuthor.length && event.sequence <= Math.max(...sameAuthor)) {
        rejected.set(event.eventId, "sequence_rollback");
        continue;
      }
      structural.set(event.eventId, event);
    }
    const creates = new Map();
    for (const event of structural.values()) if (["create", "decide"].includes(event.action)) {
      if (!creates.has(event.object)) creates.set(event.object, []);
      creates.get(event.object).push(event);
    }
    const accepted = new Map();
    for (const event of [...structural.values()].sort(eventOrder)) {
      const badParent = event.parents.find((parent) => !accepted.has(parent));
      if (badParent) {
        if (pending.has(badParent) || !candidates.has(badParent)) pending.set(event.eventId, "pending_parent");
        else rejected.set(event.eventId, "invalid_parent");
        continue;
      }
      const objectCreates = creates.get(event.object) || [];
      if (!objectCreates.length) {
        pending.set(event.eventId, "missing_create");
        continue;
      }
      const createIds = new Set(objectCreates.map((item) => item.eventId));
      if (!["create", "decide"].includes(event.action) && ![...createIds].some((id) => ancestors.get(event.eventId).has(id))) {
        rejected.set(event.eventId, "not_descendant_of_create");
        continue;
      }
      const first = [...objectCreates].sort(eventOrder)[0];
      if (canonicalJson(event.references) !== canonicalJson(first.references)) {
        rejected.set(event.eventId, "object_reference_mismatch");
        continue;
      }
      let subjectCreator = "";
      if (event.family === "moderation" && event.action === "appeal") {
        const subject = event.references.find((reference) => reference.relation === "subject");
        const subjectCreates = creates.get(subject.object) || [];
        if (subjectCreates.length) subjectCreator = [...subjectCreates].sort(eventOrder)[0].author;
        if (!subjectCreator) {
          pending.set(event.eventId, "missing_subject_create");
          continue;
        }
      }
      const recipient = event.family === "notification" ? first.payload.fields.recipient : "";
      if (!authorized(event, first.author, policy, subjectCreator, recipient)) {
        rejected.set(event.eventId, "unauthorized");
        continue;
      }
      const semantic = semanticError(event, accepted, ancestors);
      if (semantic) {
        rejected.set(event.eventId, semantic);
        continue;
      }
      accepted.set(event.eventId, event);
    }
    const objects = Object.fromEntries(FAMILIES.map((family) => [family, {}]));
    const groups = new Map();
    for (const event of accepted.values()) {
      const key = `${event.family}\0${event.object}`;
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(event);
    }
    for (const key of [...groups.keys()].sort()) {
      const events = groups.get(key);
      objects[events[0].family][events[0].object] = foldObject(events[0].family, events, ancestors);
    }
    const moderationBySubject = new Map();
    for (const moderation of Object.values(objects.moderation)) {
      const subject = moderation.references.find((reference) => reference.relation === "subject").object;
      if (!moderationBySubject.has(subject)) moderationBySubject.set(subject, []);
      moderationBySubject.get(subject).push(moderation);
    }
    for (const family of FAMILIES) if (family !== "moderation") for (const [id, state] of Object.entries(objects[family])) {
      const decisions = moderationBySubject.get(id) || [];
      if (!decisions.length) continue;
      state.moderation = decisions.map((item) => item.object).sort();
      const resolved = new Set();
      let conflict = false;
      for (const decision of decisions) {
        if (decision.conflicts.decision) conflict = true;
        else if (decision.values.decision) resolved.add(decision.values.decision);
      }
      if (conflict || resolved.size > 1) {
        state.visibility = "quarantined";
        state.moderationConflict = decisions.map((item) => item.object).sort();
      } else if (resolved.has("hide")) state.visibility = "hidden";
      else if (resolved.has("quarantine")) state.visibility = "quarantined";
    }
    const state = {schema: STATE_SCHEMA, repository: policy.repository, policy: expectedPolicy, objects, pending: [...pending.keys()].sort()};
    const rejectionReasons = {};
    for (const reason of [...rejected.values()].sort()) rejectionReasons[reason] = (rejectionReasons[reason] || 0) + 1;
    let conflicts = 0;
    for (const family of FAMILIES) for (const value of Object.values(objects[family])) {
      conflicts += Object.keys(value.conflicts).length + (value.moderationConflict ? 1 : 0);
    }
    const acceptedByFamily = {};
    for (const family of FAMILIES) acceptedByFamily[family] = [...accepted.values()].filter((event) => event.family === family).length;
    const audit = {
      accepted: [...accepted.keys()].sort(),
      pending: [...pending.keys()].sort().map((eventIdValue) => ({eventId: eventIdValue, reason: pending.get(eventIdValue)})),
      rejected: [...rejected.keys()].sort().map((id) => ({id, reason: rejected.get(id)})),
    };
    const telemetry = {
      received: Math.max(suppliedEvents.length, deliveries || 0),
      uniqueRaw: uniqueRaw.size,
      signatureValid,
      accepted: accepted.size,
      pending: pending.size,
      rejected: rejected.size,
      duplicateDeliveries: Math.max(0, Math.max(suppliedEvents.length, deliveries || 0) - uniqueRaw.size),
      equivocations,
      conflicts,
      rejectionReasons,
      acceptedByFamily,
    };
    return {
      state,
      stateDigest: `sha256:${hex(await sha256(encoder.encode(canonicalJson(state))))}`,
      audit,
      telemetry,
    };
  }

  class SignedEventLog {
    constructor() {
      this.raw = new Map();
      this.deliveries = 0;
    }
    async append(event) {
      const raw = typeof event === "string" ? event : canonicalJson(event);
      const digest = `sha256:${hex(await sha256(encoder.encode(raw)))}`;
      this.deliveries += 1;
      if (!this.raw.has(digest)) this.raw.set(digest, raw);
      return digest;
    }
    static async fromNdjson(data) {
      if (typeof data !== "string") reject("invalid_ndjson");
      const log = new SignedEventLog();
      for (const line of data.split("\n")) if (line !== "") await log.append(line);
      return log;
    }
    exportNdjson() {
      return [...this.raw.keys()].sort().map((key) => this.raw.get(key)).join("\n") + (this.raw.size ? "\n" : "");
    }
    async fold(policy) {
      return foldEvents([...this.raw.values()], policy, this.deliveries);
    }
  }

  const api = {
    CollaborationError,
    EVENT_SCHEMA,
    FAMILIES,
    POLICY_SCHEMA,
    STATE_SCHEMA,
    SignedEventLog,
    canonicalJson,
    eventId,
    foldEvents,
    objectId,
    parseCanonicalEvent,
    policyId,
    repositoryId,
    signingBytes,
  };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  root.ForkMeshCollaboration = api;
}(typeof globalThis !== "undefined" ? globalThis : this));
