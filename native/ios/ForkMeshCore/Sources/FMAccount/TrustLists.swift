// Kotlin counterpart: security/TrustLists.kt.

import FMBase
import FMCrypto
import Foundation

/// A document failed verification; `code` is the stable reason.
public struct TrustListError: Error, Equatable, CustomStringConvertible {
    public let code: String

    public init(_ code: String) { self.code = code }

    public var description: String { "TrustListError(\(code))" }
}

/// Scoped, community-operated trust lists (forkmesh-trust-list-v1).
///
/// An independent port of the reference resolver in `app/src/trust_lists.py` (see
/// `www/docs/design/scoped-trust-lists.md`). The tests pin it to
/// `conformance/trust-lists/v1/vectors.json`, which the Worker, web, desktop and Android
/// implementations also pass: every resolution, rejection code, ordering and telemetry count must
/// be identical.
///
/// Nothing is trusted unless the user follows a list, trust never leaves the role and scope it was
/// issued for, equivocation / forked history / revocation cut a list back to its last good version
/// (`recovered`), and disagreeing lists stay a visible `conflict` that only a local override
/// settles.
///
/// Ed25519 verification is `Ed25519.verifyStrict` (canonical encodings, prime-order subgroup
/// public keys, `S < L`), the rules of the strict verifier in `app/src/collaboration.py`.
///
/// Strings are sorted by UTF-16 code units.
public enum TrustLists {
    public static let listSchema = "forkmesh-trust-list-v1"
    public static let revocationSchema = "forkmesh-trust-list-revocation-v1"
    public static let appealSchema = "forkmesh-trust-list-appeal-v1"
    public static let resolutionSchema = "forkmesh-trust-resolution-v1"
    static let listIdDomain = "forkmesh-trust-list-id-v1"

    static let roles = ["maintainer", "moderator", "mirror-operator", "organization", "reviewer", "security-researcher"]
    static let stances = ["distrust", "trust"]
    static let revocationReasons = ["compromised", "malicious", "retired", "superseded"]
    static let listStates = ["active", "conflicted", "expired", "missing", "recovered", "revoked"]

    static let safeInteger: Int64 = 9_007_199_254_740_991 // 2^53 - 1
    static let maxDocumentBytes = 262_144
    static let maxEntries = 2000
    static let maxEdges = 16
    static let maxCommunities = 64
    static let maxRevocationKeys = 4
    static let maxNote = 280
    static let maxStatement = 1000
    static let maxLifetimeMs: Int64 = 400 * 24 * 60 * 60 * 1000
    static let maxPathLists = 4
    static let futureSkewMs: Int64 = 300_000
    static let maxNesting = 8

    static let listKeys: Set<String> = [
        "schema", "issuer", "name", "version", "previous", "issuedAt", "expiresAt", "roles", "communities",
        "revocationKeys", "entries", "edges", "signature",
    ]
    static let revocationKeys: Set<String> = [
        "schema", "list", "issuer", "name", "fromVersion", "revokedBy", "revokedAt", "reason", "successor", "signature",
    ]
    static let appealKeys: Set<String> = [
        "schema", "subject", "list", "version", "role", "scope", "statement", "issuedAt", "signature",
    ]
    static let entryKeys: Set<String> = ["subject", "role", "scope", "stance", "note"]
    static let edgeKeys: Set<String> = ["kind", "issuer", "name", "roles", "communities", "expiresAt"]
    static let successorKeys: Set<String> = ["issuer", "name"]

    // MARK: Canonical encoding
    //
    // Strings are printable ASCII without quote or backslash, so a plain sorted-key writer produces
    // identical bytes in every language.

    public static func canonicalJson(_ value: JSONValue) throws -> String {
        switch value {
        case .null: return "null"
        case .bool(let b): return b ? "true" : "false"
        case .int(let i):
            if i > safeInteger || i < -safeInteger { throw TrustListError("noncanonical") }
            return String(i)
        case .double: throw TrustListError("noncanonical")
        case .string(let s):
            if !isText(s) { throw TrustListError("noncanonical") }
            return "\"\(s)\""
        case .array(let items):
            return "[" + (try items.map(canonicalJson)).joined(separator: ",") + "]"
        case .object(let map):
            var parts: [String] = []
            for key in sortedStrings(map.keys) {
                if !isText(key) { throw TrustListError("noncanonical") }
                parts.append("\"\(key)\":\(try canonicalJson(map[key] ?? .null))")
            }
            return "{" + parts.joined(separator: ",") + "}"
        }
    }

    public static func canonicalJson(_ object: JSONObject) throws -> String { try canonicalJson(.object(object)) }

    /// `sha256:<hex>` of the raw document's UTF-8 bytes (Python's `surrogatepass`; a Swift string
    /// cannot hold a lone surrogate, see `documentId(utf16:)`).
    public static func documentId(_ raw: String) -> String {
        documentId(utf16: Array(raw.utf16))
    }

    /// `documentId` over UTF-16 code units, encoding lone surrogates as three-byte sequences.
    public static func documentId(utf16 units: [UInt16]) -> String {
        "sha256:" + Sha256.hex(utf8SurrogatePass(units))
    }

    static func utf8SurrogatePass(_ units: [UInt16]) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(units.count)
        var i = 0
        while i < units.count {
            var c = UInt32(units[i])
            if c >= 0xD800 && c <= 0xDBFF && i + 1 < units.count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF {
                i += 1
                c = 0x10000 + ((c - 0xD800) << 10) + (UInt32(units[i]) - 0xDC00)
            }
            if c < 0x80 {
                out.append(UInt8(c))
            } else if c < 0x800 {
                out.append(UInt8(0xC0 | (c >> 6)))
                out.append(UInt8(0x80 | (c & 0x3F)))
            } else if c < 0x10000 {
                out.append(UInt8(0xE0 | (c >> 12)))
                out.append(UInt8(0x80 | ((c >> 6) & 0x3F)))
                out.append(UInt8(0x80 | (c & 0x3F)))
            } else {
                out.append(UInt8(0xF0 | (c >> 18)))
                out.append(UInt8(0x80 | ((c >> 12) & 0x3F)))
                out.append(UInt8(0x80 | ((c >> 6) & 0x3F)))
                out.append(UInt8(0x80 | (c & 0x3F)))
            }
            i += 1
        }
        return out
    }

    /// `urn:forkmesh:trust-list:sha256:<hex>` of `issuer` and `name`.
    public static func listId(_ issuer: String, _ name: String) -> String {
        "urn:forkmesh:trust-list:sha256:" + Sha256.hex(Array("\(listIdDomain)\n\(issuer)\n\(name)".utf8))
    }

    static func signingText(_ document: JSONObject) throws -> String {
        var unsigned = JSONObject()
        for (key, value) in document where key != "signature" { unsigned[key] = value }
        return "\(document["schema"]?.text ?? "")\n\(try canonicalJson(unsigned))"
    }

    // MARK: Character classes

    /// `^[\x20-\x21\x23-\x5b\x5d-\x7e]*$`.
    static func isText(_ s: String) -> Bool {
        s.utf16.allSatisfy { ($0 >= 0x20 && $0 <= 0x7e) && $0 != 0x22 && $0 != 0x5c }
    }

    private static func ascii(_ s: String) -> [UInt8]? {
        var out: [UInt8] = []
        for unit in s.utf16 {
            if unit > 0x7f { return nil }
            out.append(UInt8(unit))
        }
        return out
    }

    private static func isLowerAlnum(_ c: UInt8) -> Bool { (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) }
    private static func isAlnum(_ c: UInt8) -> Bool { isLowerAlnum(c) || (c >= 0x41 && c <= 0x5a) }

    /// `^[a-z0-9][a-z0-9-]{0,63}$`.
    static func isSlug(_ s: String) -> Bool { leadingPattern(s, max: 64) }

    /// `^[a-z0-9][a-z0-9-]{0,38}$`.
    static func isOwner(_ s: String) -> Bool { leadingPattern(s, max: 39) }

    private static func leadingPattern(_ s: String, max: Int) -> Bool {
        guard let b = ascii(s), let first = b.first, isLowerAlnum(first), b.count <= max else { return false }
        return b.dropFirst().allSatisfy { isLowerAlnum($0) || $0 == 0x2d }
    }

    /// `^[A-Za-z0-9._-]{1,100}$`.
    static func isRepo(_ s: String) -> Bool {
        guard let b = ascii(s), (1...100).contains(b.count) else { return false }
        return b.allSatisfy { isAlnum($0) || $0 == 0x2e || $0 == 0x5f || $0 == 0x2d }
    }

    /// `^[A-Za-z0-9_-]{n}$`.
    private static func isB64Url(_ s: String, length: Int) -> Bool {
        guard let b = ascii(s), b.count == length else { return false }
        return b.allSatisfy { isAlnum($0) || $0 == 0x5f || $0 == 0x2d }
    }

    private static func isLowerHex(_ s: Substring, length: Int) -> Bool {
        guard s.utf16.count == length else { return false }
        return s.utf8.allSatisfy { ($0 >= 0x30 && $0 <= 0x39) || ($0 >= 0x61 && $0 <= 0x66) }
    }

    /// `^sha256:[0-9a-f]{64}$`.
    static func isDigest(_ s: String) -> Bool {
        s.hasPrefix("sha256:") && isLowerHex(s.dropFirst(7), length: 64)
    }

    /// `^urn:forkmesh:trust-list:sha256:[0-9a-f]{64}$`.
    static func isListUrn(_ s: String) -> Bool {
        let prefix = "urn:forkmesh:trust-list:sha256:"
        return s.hasPrefix(prefix) && isLowerHex(s.dropFirst(prefix.count), length: 64)
    }

    // MARK: Scopes

    static func isValidScope(_ value: JSONValue?) -> Bool {
        guard let s = value?.stringValue else { return false }
        if s.hasPrefix("repo:") {
            let rest = s.dropFirst(5)
            guard let slash = rest.firstIndex(of: "/") else { return false }
            let owner = String(rest[rest.startIndex..<slash])
            let repo = String(rest[rest.index(after: slash)...])
            return isOwner(owner) && isRepo(repo) && repo != "." && repo != ".."
        }
        if s.hasPrefix("org:") { return isOwner(String(s.dropFirst(4))) }
        if s.hasPrefix("community:") { return isSlug(String(s.dropFirst(10))) }
        return false
    }

    /// True when trust in `outer` includes `inner`.
    static func covers(_ outer: String, _ inner: String) -> Bool {
        if outer == inner { return true }
        return outer.hasPrefix("org:") && inner.hasPrefix("repo:\(outer.dropFirst(4))/")
    }

    static func covered<S: Sequence>(_ scopes: S, _ scope: String) -> Bool where S.Element == String {
        scopes.contains { covers($0, scope) }
    }

    /// UTF-16 code unit order.
    static func dartLess(_ a: String, _ b: String) -> Bool {
        a.utf16.lexicographicallyPrecedes(b.utf16)
    }

    static func sortedStrings<S: Sequence>(_ values: S) -> [String] where S.Element == String {
        values.sorted(by: dartLess)
    }

    static func sortedSet<S: Sequence>(_ values: S) -> [String] where S.Element == String {
        sortedStrings(Set(values))
    }

    static func normalizeScopes(_ scopes: [String]) -> [String] {
        let unique = sortedSet(scopes)
        return unique.filter { s in !unique.contains { $0 != s && covers($0, s) } }
    }

    static func intersectScopes(_ left: [String], _ right: [String]) -> [String] {
        let kept = left.filter { covered(right, $0) } + right.filter { covered(left, $0) }
        return normalizeScopes(kept)
    }

    static func sortedIntersection(_ left: [String], _ right: [String]) -> [String] {
        let other = Set(right)
        return sortedSet(left.filter(other.contains))
    }

    static func strings(_ value: JSONValue?) -> [String] {
        value?.arrayValue?.compactMap(\.stringValue) ?? []
    }

    // MARK: Field checks

    static func isSafeInt(_ value: JSONValue?, _ minimum: Int64 = 0) -> Bool {
        guard case .int(let v)? = value else { return false }
        return minimum <= v && v <= safeInteger
    }

    static func sameKeys(_ map: JSONObject, _ keys: Set<String>) -> Bool {
        map.count == keys.count && keys.allSatisfy(map.contains)
    }

    static func decodeFixed(_ value: JSONValue?, length: Int, padding: String) -> [UInt8]? {
        guard let text = value?.stringValue, isB64Url(text, length: length),
              let decoded = B64.decodeOrNil(text + padding) else { return nil }
        let bytes = Array(decoded)
        // Only the canonical encoding of the bytes (zero trailing bits) is accepted.
        return B64.encodeUrlNoPadding(bytes) == text ? bytes : nil
    }

    static func keyBytes(_ value: JSONValue?) -> [UInt8]? { decodeFixed(value, length: 43, padding: "=") }

    static func signatureBytes(_ value: JSONValue?) -> [UInt8]? { decodeFixed(value, length: 86, padding: "==") }

    static func sortedUnique(_ value: JSONValue?, maximum: Int, empty: Bool, _ check: (JSONValue) -> Bool) -> Bool {
        guard let values = value?.arrayValue, values.count <= maximum else { return false }
        if values.isEmpty && !empty { return false }
        if !values.allSatisfy(check) { return false }
        let texts = values.map { $0.stringValue ?? "" }
        for i in 0..<max(0, texts.count - 1) where !dartLess(texts[i], texts[i + 1]) {
            return false
        }
        return true
    }

    static func validRoles(_ value: JSONValue?) -> Bool {
        sortedUnique(value, maximum: roles.count, empty: false) { $0.stringValue.map(roles.contains) ?? false }
    }

    static func validCommunities(_ value: JSONValue?) -> Bool {
        guard sortedUnique(value, maximum: maxCommunities, empty: false, { isValidScope($0) }) else { return false }
        let values = strings(value)
        return normalizeScopes(values) == values
    }

    static func verifySignature(_ document: JSONObject, _ signer: JSONValue?) throws {
        guard let signature = signatureBytes(document["signature"]) else { throw TrustListError("invalid_signature_encoding") }
        if !Ed25519.isCanonicalScalar(signature, offset: 32) { throw TrustListError("invalid_signature") }
        let text = try signingText(document)
        guard let publicKey = keyBytes(signer),
              Ed25519.verifyStrict(publicKey: publicKey, message: Array(text.utf8), signature: signature) else {
            throw TrustListError("invalid_signature")
        }
    }

    static func validateList(_ doc: JSONObject) throws {
        if keyBytes(doc["issuer"]) == nil { throw TrustListError("invalid_issuer") }
        guard doc["name"]?.stringValue.map(isSlug) == true else { throw TrustListError("invalid_name") }
        if !isSafeInt(doc["version"], 1) { throw TrustListError("invalid_version") }
        let previous = doc["previous"]
        if doc["version"] == .int(1) {
            if previous != .null { throw TrustListError("invalid_previous") }
        } else if !(previous?.stringValue.map(isDigest) ?? false) {
            throw TrustListError("invalid_previous")
        }
        guard isSafeInt(doc["issuedAt"]), isSafeInt(doc["expiresAt"]),
              let issued = doc["issuedAt"]?.int64Value, let expires = doc["expiresAt"]?.int64Value,
              expires > issued, expires - issued <= maxLifetimeMs else {
            throw TrustListError("invalid_time")
        }
        if !validRoles(doc["roles"]) || !validCommunities(doc["communities"]) { throw TrustListError("invalid_scope") }
        let keys = doc["revocationKeys"]
        if !sortedUnique(keys, maximum: maxRevocationKeys, empty: true, { keyBytes($0) != nil })
            || (keys?.arrayValue ?? []).contains(doc["issuer"] ?? .null) {
            throw TrustListError("invalid_revocation_keys")
        }
        let docRoles = strings(doc["roles"])
        let docCommunities = strings(doc["communities"])

        guard let entries = doc["entries"]?.arrayValue, entries.count <= maxEntries else { throw TrustListError("invalid_entry") }
        var previousKey: String?
        for item in entries {
            guard let entry = item.objectValue, sameKeys(entry, entryKeys), keyBytes(entry["subject"]) != nil,
                  entry["subject"] != doc["issuer"],
                  let role = entry["role"]?.stringValue, roles.contains(role),
                  isValidScope(entry["scope"]), let scope = entry["scope"]?.stringValue,
                  let stance = entry["stance"]?.stringValue, stances.contains(stance),
                  let note = entry["note"]?.stringValue, note.utf16.count <= maxNote else {
                throw TrustListError("invalid_entry")
            }
            if !docRoles.contains(role) || !covered(docCommunities, scope) { throw TrustListError("entry_outside_scope") }
            let key = "\(entry["subject"]?.text ?? "")\n\(role)\n\(scope)"
            if let previousKey, !dartLess(previousKey, key) { throw TrustListError("unsorted_entries") }
            previousKey = key
        }

        guard let edges = doc["edges"]?.arrayValue, edges.count <= maxEdges else { throw TrustListError("invalid_edge") }
        previousKey = nil
        for item in edges {
            guard let edge = item.objectValue, sameKeys(edge, edgeKeys) else { throw TrustListError("invalid_edge") }
            let kind = edge["kind"]
            guard kind == .string("delegate") || kind == .string("include"),
                  keyBytes(edge["issuer"]) != nil,
                  let edgeName = edge["name"]?.stringValue, isSlug(edgeName),
                  validRoles(edge["roles"]), validCommunities(edge["communities"]),
                  !(kind == .string("delegate") && !isSafeInt(edge["expiresAt"])),
                  !(kind == .string("include") && edge["expiresAt"] != .null),
                  !(edge["issuer"] == doc["issuer"] && edge["name"] == doc["name"]) else {
                throw TrustListError("invalid_edge")
            }
            if !strings(edge["roles"]).allSatisfy(docRoles.contains)
                || !strings(edge["communities"]).allSatisfy({ covered(docCommunities, $0) }) {
                throw TrustListError("edge_outside_scope")
            }
            let key = "\(edge["issuer"]?.text ?? "")\n\(edgeName)"
            if let previousKey, !dartLess(previousKey, key) { throw TrustListError("unsorted_edges") }
            previousKey = key
        }

        try verifySignature(doc, doc["issuer"])
    }

    static func validateRevocation(_ doc: JSONObject) throws {
        guard keyBytes(doc["issuer"]) != nil, let issuer = doc["issuer"]?.stringValue else { throw TrustListError("invalid_issuer") }
        guard let name = doc["name"]?.stringValue, isSlug(name) else { throw TrustListError("invalid_name") }
        if doc["list"] != .string(listId(issuer, name)) { throw TrustListError("list_mismatch") }
        if !isSafeInt(doc["fromVersion"], 1) { throw TrustListError("invalid_version") }
        if keyBytes(doc["revokedBy"]) == nil { throw TrustListError("invalid_revoker") }
        if !isSafeInt(doc["revokedAt"]) { throw TrustListError("invalid_time") }
        if !(doc["reason"]?.stringValue.map(revocationReasons.contains) ?? false) { throw TrustListError("invalid_reason") }
        let successor = doc["successor"] ?? .null
        if successor != .null {
            guard let map = successor.objectValue, sameKeys(map, successorKeys), keyBytes(map["issuer"]) != nil,
                  let successorName = map["name"]?.stringValue, isSlug(successorName),
                  !(map["issuer"] == doc["issuer"] && map["name"] == doc["name"]) else {
                throw TrustListError("invalid_successor")
            }
        }
        try verifySignature(doc, doc["revokedBy"])
    }

    static func validateAppeal(_ doc: JSONObject) throws {
        if keyBytes(doc["subject"]) == nil { throw TrustListError("invalid_subject") }
        guard let list = doc["list"]?.stringValue, isListUrn(list) else { throw TrustListError("invalid_list") }
        if !isSafeInt(doc["version"], 1) { throw TrustListError("invalid_version") }
        if !(doc["role"]?.stringValue.map(roles.contains) ?? false) || !isValidScope(doc["scope"]) {
            throw TrustListError("invalid_scope")
        }
        guard let statement = doc["statement"]?.stringValue, !statement.isEmpty, statement.utf16.count <= maxStatement else {
            throw TrustListError("invalid_statement")
        }
        if !isSafeInt(doc["issuedAt"]) { throw TrustListError("invalid_time") }
        try verifySignature(doc, doc["subject"])
    }

    /// Deepest bracket nesting outside strings, found before any parser runs.
    static func nestingDepth(_ raw: String) -> Int {
        var depth = 0
        var deepest = 0
        var inString = false
        var escaped = false
        for unit in raw.utf16 {
            if inString {
                if escaped {
                    escaped = false
                } else if unit == 0x5C {
                    escaped = true
                } else if unit == 0x22 {
                    inString = false
                }
            } else if unit == 0x22 {
                inString = true
            } else if unit == 0x5B || unit == 0x7B {
                depth += 1
                deepest = max(deepest, depth)
            } else if unit == 0x5D || unit == 0x7D {
                depth -= 1
            }
        }
        return deepest
    }

    /// Parse and verify one raw document, throwing `TrustListError`.
    public static func verifyDocument(_ raw: String) throws -> JSONObject {
        if raw.utf8.count > maxDocumentBytes { throw TrustListError("too_large") }
        if nestingDepth(raw) > maxNesting { throw TrustListError("invalid_json") }
        let document: JSONValue
        do {
            document = try JSON.parse(raw)
        } catch {
            throw TrustListError("invalid_json")
        }
        if try canonicalJson(document) != raw { throw TrustListError("noncanonical") }
        guard let map = document.objectValue else { throw TrustListError("unsupported_schema") }
        let keys: Set<String>
        let validate: (JSONObject) throws -> Void
        switch map["schema"] {
        case .string(listSchema)?:
            keys = listKeys
            validate = validateList
        case .string(revocationSchema)?:
            keys = revocationKeys
            validate = validateRevocation
        case .string(appealSchema)?:
            keys = appealKeys
            validate = validateAppeal
        default:
            throw TrustListError("unsupported_schema")
        }
        if !sameKeys(map, keys) { throw TrustListError("invalid_keys") }
        try validate(map)
        return map
    }

    // MARK: Resolution

    struct Doc {
        let id: String
        let doc: JSONObject
        var version: Int64 { doc["version"]?.int64Value ?? 0 }
        func int(_ key: String) -> Int64 { doc[key]?.int64Value ?? 0 }
        func str(_ key: String) -> String { doc[key]?.stringValue ?? "" }
    }

    struct Group {
        let state: String
        let head: JSONObject?
        let headId: String?
        let cutoff: Int64?
        let reasons: [String]
        let successor: JSONObject?
        let conflicts: [JSONObject]
        let heldVersions: [Int64]
    }

    static func listGroupState(_ docs: [Doc], _ revocations: [Doc], _ now: Int64) -> Group {
        var byVersion: [Int64: [Doc]] = [:]
        for item in docs { byVersion[item.version, default: []].append(item) }
        let versions = byVersion.keys.sorted()
        var cutoffs: [Int64] = []
        var reasons = Set<String>()
        var conflicts: [JSONObject] = []
        let lid: JSONValue = docs.first.map { .string(listId($0.str("issuer"), $0.str("name"))) } ?? .null
        for version in versions {
            let atVersion = byVersion[version] ?? []
            if atVersion.count > 1 {
                cutoffs.append(version)
                reasons.insert("equivocation")
                conflicts.append(["kind": "equivocation", "listId": lid, "version": .int(version)])
                continue
            }
            if let prior = byVersion[version - 1], prior.count == 1, atVersion[0].doc["previous"] != .string(prior[0].id) {
                cutoffs.append(version)
                reasons.insert("history_fork")
                conflicts.append(["kind": "history_fork", "listId": lid, "version": .int(version)])
            }
        }
        var successor: JSONObject?
        if !revocations.isEmpty {
            reasons.insert("revoked")
            let ordered = revocations.sorted { a, b in
                let fa = a.int("fromVersion"), fb = b.int("fromVersion")
                return fa != fb ? fa < fb : dartLess(a.id, b.id)
            }
            cutoffs.append(ordered[0].int("fromVersion"))
            for revocation in ordered {
                if let next = revocation.doc["successor"]?.objectValue {
                    successor = next
                    break
                }
            }
        }
        let cutoff = cutoffs.min()
        var headId: String?
        var head: JSONObject?
        for version in versions.reversed() {
            if let cutoff, version >= cutoff { continue }
            if let atVersion = byVersion[version], atVersion.count == 1 {
                headId = atVersion[0].id
                head = atVersion[0].doc
                break
            }
        }
        let state: String
        if let head {
            if now >= (head["expiresAt"]?.int64Value ?? 0) {
                state = "expired"
            } else if cutoff != nil {
                state = "recovered"
            } else {
                state = "active"
            }
        } else {
            state = !revocations.isEmpty ? "revoked" : (!docs.isEmpty ? "conflicted" : "missing")
        }
        return Group(
            state: state, head: head, headId: headId, cutoff: cutoff, reasons: sortedSet(reasons), successor: successor,
            conflicts: conflicts, heldVersions: versions)
    }

    /// Verify, compose and merge trust lists for one local client (`resolveTrustLists`).
    /// `follows` are `{issuer, name, roles?, communities?}`; `overrides` are
    /// `{subject, role, scope, stance}`.
    public static func resolve(
        _ documents: [String],
        now: Int64,
        follows: [JSONObject] = [],
        overrides: [JSONObject] = []
    ) -> JSONObject {
        var unique: [String: String] = [:]
        for raw in documents {
            let id = documentId(raw)
            if unique[id] == nil { unique[id] = raw }
        }
        let duplicates = documents.count - unique.count

        var rejected: [(documentId: String, code: String)] = []
        var lists: [Doc] = []
        var revocations: [Doc] = []
        var appeals: [Doc] = []
        for docId in sortedStrings(unique.keys) {
            let doc: JSONObject
            do {
                doc = try verifyDocument(unique[docId] ?? "")
            } catch let error as TrustListError {
                rejected.append((docId, error.code))
                continue
            } catch {
                rejected.append((docId, "invalid_json"))
                continue
            }
            let schema = doc["schema"]?.stringValue ?? ""
            if schema != revocationSchema && (doc["issuedAt"]?.int64Value ?? 0) > now + futureSkewMs {
                rejected.append((docId, "issued_in_future"))
            } else if schema == listSchema {
                lists.append(Doc(id: docId, doc: doc))
            } else if schema == revocationSchema {
                revocations.append(Doc(id: docId, doc: doc))
            } else {
                appeals.append(Doc(id: docId, doc: doc))
            }
        }

        var docsByList: [String: [Doc]] = [:]
        var names: [String: [String]] = [:]
        for item in lists {
            let issuer = item.str("issuer")
            let name = item.str("name")
            let lid = listId(issuer, name)
            docsByList[lid, default: []].append(item)
            names[lid] = [issuer, name]
        }

        var revocationsByList: [String: [Doc]] = [:]
        for item in revocations {
            let lid = item.str("list")
            let revokedBy = item.doc["revokedBy"] ?? .null
            let fromVersion = item.int("fromVersion")
            let authorized = revokedBy == item.doc["issuer"]
                || (docsByList[lid] ?? []).contains { listed in
                    listed.version < fromVersion && (listed.doc["revocationKeys"]?.arrayValue ?? []).contains(revokedBy)
                }
            if !authorized {
                rejected.append((item.id, "unauthorized_revoker"))
                continue
            }
            revocationsByList[lid, default: []].append(item)
            if names[lid] == nil { names[lid] = [item.str("issuer"), item.str("name")] }
        }

        var groups: [String: Group] = [:]
        func group(_ lid: String) -> Group {
            if let known = groups[lid] { return known }
            let computed = listGroupState(docsByList[lid] ?? [], revocationsByList[lid] ?? [], now)
            groups[lid] = computed
            return computed
        }
        for lid in Set(docsByList.keys).union(revocationsByList.keys) { _ = group(lid) }

        func usable(_ lid: String) -> JSONObject? {
            let info = group(lid)
            return info.state == "active" || info.state == "recovered" ? info.head : nil
        }

        var followed = Set<String>()
        var reached = Set<String>()
        var expiredDelegations = 0
        var depthLimited = 0
        var cycles = 0
        // tuple key -> listId -> best source (shortest path, then smallest path).
        var sources: [String: [String: JSONObject]] = [:]

        func admit(_ lid: String, _ path: [String], _ roles: [String], _ communities: [String]) {
            reached.insert(lid)
            guard let head = usable(lid) else { return }
            for raw in head["entries"]?.arrayValue ?? [] {
                guard let entry = raw.objectValue else { continue }
                let subject = entry["subject"]?.stringValue ?? ""
                let role = entry["role"]?.stringValue ?? ""
                let scope = entry["scope"]?.stringValue ?? ""
                if !roles.contains(role) || !covered(communities, scope) { continue }
                let tupleKey = "\(subject)\n\(role)\n\(scope)"
                let candidate: JSONObject = [
                    "listId": .string(lid),
                    "version": head["version"] ?? .null,
                    "stance": entry["stance"] ?? .null,
                    "note": entry["note"] ?? .null,
                    "path": JSONValue(path),
                ]
                if let current = sources[tupleKey]?[lid] {
                    let currentPath = strings(current["path"])
                    if path.count < currentPath.count
                        || (path.count == currentPath.count
                            && dartLess(path.joined(separator: "\n"), currentPath.joined(separator: "\n"))) {
                        sources[tupleKey, default: [:]][lid] = candidate
                    }
                } else {
                    sources[tupleKey, default: [:]][lid] = candidate
                }
            }
            for raw in head["edges"]?.arrayValue ?? [] {
                guard let edge = raw.objectValue else { continue }
                if edge["kind"] == .string("delegate") && now >= (edge["expiresAt"]?.int64Value ?? 0) {
                    expiredDelegations += 1
                    continue
                }
                let edgeIssuer = edge["issuer"]?.stringValue ?? ""
                let edgeName = edge["name"]?.stringValue ?? ""
                let child = listId(edgeIssuer, edgeName)
                if names[child] == nil { names[child] = [edgeIssuer, edgeName] }
                if path.contains(child) {
                    cycles += 1
                    continue
                }
                if path.count >= maxPathLists {
                    depthLimited += 1
                    continue
                }
                var childRoles = sortedIntersection(roles, strings(edge["roles"]))
                var childCommunities = intersectScopes(communities, strings(edge["communities"]))
                if let childHead = usable(child) {
                    childRoles = sortedIntersection(childRoles, strings(childHead["roles"]))
                    childCommunities = intersectScopes(childCommunities, strings(childHead["communities"]))
                }
                if childRoles.isEmpty || childCommunities.isEmpty {
                    reached.insert(child)
                    continue
                }
                admit(child, path + [child], childRoles, childCommunities)
            }
        }

        var followIds: [String] = []
        for follow in follows {
            let issuer = follow["issuer"]?.stringValue ?? ""
            let name = follow["name"]?.stringValue ?? ""
            let lid = listId(issuer, name)
            if names[lid] == nil { names[lid] = [issuer, name] }
            followIds.append(lid)
        }
        // Stable by list id, like Python's sorted().
        let followOrder = followIds.indices.sorted { a, b in
            followIds[a] != followIds[b] ? dartLess(followIds[a], followIds[b]) : a < b
        }
        for index in followOrder {
            let lid = followIds[index]
            let follow = follows[index]
            followed.insert(lid)
            guard let head = usable(lid) else {
                reached.insert(lid)
                continue
            }
            var roles = strings(head["roles"])
            var communities = strings(head["communities"])
            if let requested = follow["roles"], requested != .null {
                roles = sortedIntersection(roles, strings(requested))
            }
            if let requested = follow["communities"], requested != .null {
                communities = intersectScopes(communities, strings(requested))
            }
            if !roles.isEmpty && !communities.isEmpty {
                admit(lid, [lid], roles, communities)
            } else {
                reached.insert(lid)
            }
        }

        var overrideMap: [String: String] = [:]
        for o in overrides {
            let key = "\(o["subject"]?.text ?? "")\n\(o["role"]?.text ?? "")\n\(o["scope"]?.text ?? "")"
            overrideMap[key] = o["stance"]?.stringValue ?? ""
        }
        var appealStatusCounts: [String: Int64] = ["open": 0, "resolved": 0, "unattached": 0]
        var assertions: [JSONObject] = []
        var assertionIndex: [String: Int] = [:]
        var conflicts: [JSONObject] = []
        for tupleKey in sortedSet(Set(sources.keys).union(overrideMap.keys)) {
            let parts = tupleKey.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
            let subject = parts[0], role = parts[1], scope = parts[2]
            let perList = sources[tupleKey] ?? [:]
            let tupleSources = sortedStrings(perList.keys).compactMap { perList[$0] }
            let stanceSet = Set(tupleSources.map { $0["stance"]?.text ?? "" })
            let stance = stanceSet.isEmpty ? "none" : (stanceSet.count == 1 ? stanceSet.first! : "conflict")
            let override = overrideMap[tupleKey]
            if stance == "conflict" {
                conflicts.append([
                    "kind": "stance", "subject": .string(subject), "role": .string(role), "scope": .string(scope),
                    "overridden": .bool(override != nil),
                ])
            }
            assertionIndex[tupleKey] = assertions.count
            assertions.append([
                "subject": .string(subject),
                "role": .string(role),
                "scope": .string(scope),
                "stance": .string(stance),
                "override": JSONValue(override),
                "effective": .string(override ?? stance),
                "sources": .array(tupleSources.map(JSONValue.object)),
                "appeals": .array([]),
            ])
        }

        for item in appeals {
            let appeal = item.doc
            let appealList = item.str("list")
            let appealVersion = item.version
            let tupleKey = "\(appeal["subject"]?.text ?? "")\n\(appeal["role"]?.text ?? "")\n\(appeal["scope"]?.text ?? "")"
            guard let index = assertionIndex[tupleKey],
                  (assertions[index]["sources"]?.arrayValue ?? []).contains(where: { $0["listId"] == .string(appealList) }) else {
                appealStatusCounts["unattached", default: 0] += 1
                continue
            }
            var status = "open"
            if let head = usable(appealList), (head["version"]?.int64Value ?? 0) > appealVersion,
               !(head["entries"]?.arrayValue ?? []).contains(where: { raw in
                   raw["subject"] == appeal["subject"] && raw["role"] == appeal["role"]
                       && raw["scope"] == appeal["scope"] && raw["stance"] == .string("distrust")
               }) {
                status = "resolved"
            }
            appealStatusCounts[status, default: 0] += 1
            var existing = assertions[index]["appeals"]?.arrayValue ?? []
            existing.append(.object([
                "appealId": .string(item.id),
                "listId": .string(appealList),
                "version": .int(appealVersion),
                "status": .string(status),
            ]))
            assertions[index]["appeals"] = .array(existing)
        }

        var listRows: [JSONValue] = []
        var stateCounts: [String: Int64] = [:]
        for state in listStates { stateCounts[state] = 0 }
        for lid in sortedSet(Set(groups.keys).union(followed).union(reached)) {
            let info = group(lid)
            conflicts.append(contentsOf: info.conflicts)
            let name = names[lid] ?? ["", ""]
            stateCounts[info.state, default: 0] += 1
            listRows.append(.object([
                "listId": .string(lid),
                "issuer": .string(name[0]),
                "name": .string(name[1]),
                "state": .string(info.state),
                "followed": .bool(followed.contains(lid)),
                "reachable": .bool(reached.contains(lid)),
                "headVersion": info.head?["version"] ?? .null,
                "headDocumentId": JSONValue(info.headId),
                "expiresAt": info.head?["expiresAt"] ?? .null,
                "cutoffVersion": info.cutoff.map(JSONValue.int) ?? .null,
                "cutoffReasons": JSONValue(info.reasons),
                "successor": info.successor.map(JSONValue.object) ?? .null,
                "heldVersions": .array(info.heldVersions.map(JSONValue.int)),
            ]))
        }

        let conflictKeys = conflicts.map { (try? canonicalJson($0)) ?? "" }
        let sortedConflicts = conflicts.indices.sorted { conflictKeys[$0] != conflictKeys[$1] ? dartLess(conflictKeys[$0], conflictKeys[$1]) : $0 < $1 }
            .map { conflicts[$0] }
        rejected.sort { dartLess("\($0.documentId)\n\($0.code)", "\($1.documentId)\n\($1.code)") }
        var rejectionCodes = JSONObject()
        for item in rejected {
            rejectionCodes[item.code] = .int((rejectionCodes[item.code]?.int64Value ?? 0) + 1)
        }

        let appealCounts: JSONObject = [
            "open": .int(appealStatusCounts["open"] ?? 0),
            "resolved": .int(appealStatusCounts["resolved"] ?? 0),
            "unattached": .int(appealStatusCounts["unattached"] ?? 0),
        ]
        var listStateCounts = JSONObject()
        for state in listStates { listStateCounts[state] = .int(stateCounts[state] ?? 0) }

        return [
            "schema": .string(resolutionSchema),
            "now": .int(now),
            "lists": .array(listRows),
            "assertions": .array(assertions.map(JSONValue.object)),
            "conflicts": .array(sortedConflicts.map(JSONValue.object)),
            "rejected": .array(rejected.map { .object(["documentId": .string($0.documentId), "code": .string($0.code)]) }),
            "telemetry": .object([
                "documents": .int(Int64(unique.count)),
                "duplicateDocuments": .int(Int64(duplicates)),
                "rejectedDocuments": .int(Int64(rejected.count)),
                "rejectionCodes": .object(rejectionCodes),
                "revocations": .int(Int64(revocationsByList.values.reduce(0) { $0 + $1.count })),
                "listStates": .object(listStateCounts),
                "followedLists": .int(Int64(followed.count)),
                "reachableLists": .int(Int64(reached.count)),
                "assertions": .int(Int64(assertions.count)),
                "stanceConflicts": .int(Int64(sortedConflicts.filter { $0["kind"] == .string("stance") }.count)),
                "overrides": .int(Int64(overrideMap.count)),
                "appeals": .object(appealCounts),
                "expiredDelegations": .int(Int64(expiredDelegations)),
                "depthLimitedEdges": .int(Int64(depthLimited)),
                "cycleEdges": .int(Int64(cycles)),
            ]),
        ]
    }
}
