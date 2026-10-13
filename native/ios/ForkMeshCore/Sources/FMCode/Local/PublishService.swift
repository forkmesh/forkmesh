// Publishes an on-device repository to the mesh: push the branch, then sign
// and post the catalog record. Publishing and pushing are deliberately **not**
// gated behind user presence — mobile-node-contract §6 reserves that for
// destructive actions.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMDiff
import FMGit
import Foundation

/// How a publish ended, mapped from the Worker's answer.
public enum PublishStatus: String, Sendable {
    /// The canonical record was written (or refreshed).
    case published
    /// Accepted as a subordinate device mirror (`202 mirror:true`). Another
    /// node holds the source authority for this name — the contract says to
    /// ask the user to rename (mobile-node-contract §7).
    case mirrored
    /// The presented key may not publish for this owner.
    case unauthorized
    /// A newer record is already stored (`409 stale_update`).
    case stale
    /// The Worker requires a newer client (`426`).
    case upgradeRequired
    /// Rate limited or over the per-owner record cap.
    case limited
    /// Anything else (network, 5xx, malformed).
    case failed
}

public struct PublishOutcome: Equatable, Sendable {
    public let status: PublishStatus
    public let message: String
    /// `owner/name` on the network when the publish landed.
    public let publishedAs: String
    /// Node that owns the canonical record when `status` is `.mirrored`.
    public let canonicalNodeId: String

    public init(status: PublishStatus, message: String, publishedAs: String = "", canonicalNodeId: String = "") {
        self.status = status
        self.message = message
        self.publishedAs = publishedAs
        self.canonicalNodeId = canonicalNodeId
    }

    public var isSuccess: Bool { status == .published }
}

/// Head-of-branch facts a catalog record advertises.
public struct PublishHead: Equatable, Sendable {
    public let commit: String
    public let rootCommit: String
    public let subject: String
    public let authorName: String
    public let at: Date?
    public let commitCount: Int
    public let branchCount: Int

    public init(
        commit: String = "", rootCommit: String = "", subject: String = "", authorName: String = "",
        at: Date? = nil, commitCount: Int = 0, branchCount: Int = 0
    ) {
        self.commit = commit
        self.rootCommit = rootCommit
        self.subject = subject
        self.authorName = authorName
        self.at = at
        self.commitCount = commitCount
        self.branchCount = branchCount
    }
}

/// Publishes an on-device repository to the mesh.
@MainActor
public final class PublishService {
    /// Preferences key holding this install's stable catalog node id.
    nonisolated public static let nodeIdKey = "node/catalogNodeId"

    /// Longest repository name the catalog accepts (`MAX_REPO_SEGMENT`).
    nonisolated public static let maxPublishNameLength = 80

    let api: ApiService
    let identity: DeviceIdentity
    let prefs: KeyValueStore
    let clock: Clock
    /// A catalog node id belongs to one instance's catalog.
    let scope: LocalRepoScope
    private var cachedNodeId: String?
    public var mirrorLookup: (@MainActor (String, String) async throws -> [RepoMirror])?

    public init(
        api: ApiService, identity: DeviceIdentity, prefs: KeyValueStore, clock: Clock = SystemClock(),
        scope: LocalRepoScope = LegacyLocalRepoScope()
    ) {
        self.api = api
        self.identity = identity
        self.prefs = prefs
        self.clock = clock
        self.scope = scope
    }

    /// Why `name` cannot be published, or nil when it is fine.
    nonisolated public static func validatePublishName(_ name: String) -> String? {
        if let local = LocalRepoStore.validateName(name) { return local }
        if name.utf16.count > maxPublishNameLength {
            return "Published names are at most \(maxPublishNameLength) characters."
        }
        return nil
    }

    /// Stable per-install id used as the catalog record's `nodeId`.
    ///
    /// Deliberately not the device pubkey: a revoked device rotates its key
    /// (contract §4) and would otherwise lose the source authority for every
    /// repository it published.
    public func nodeId() -> String {
        if let cachedNodeId { return cachedNodeId }
        let key = scope.key(PublishService.nodeIdKey)
        var value = prefs.string(forKey: key) ?? ""
        if !PublishService.isNodeId(value) {
            value = Hex.encode(Bytes.random(16))
            prefs.set(value, forKey: key)
        }
        cachedNodeId = value
        return value
    }

    nonisolated static func isNodeId(_ value: String) -> Bool {
        let bytes = Array(value.utf8)
        return bytes.count == 32 && bytes.allSatisfy { (0x30...0x39).contains($0) || (0x61...0x66).contains($0) }
    }

    /// Signing material for `{origin}/{owner}/{repo}`.
    public func credentials(owner: String, repo: String, signed: Bool = true) -> ForkMeshRemoteCredentials {
        ForkMeshRemoteCredentials(
            origin: api.gitOrigin,
            owner: owner,
            repo: repo,
            seed: signed ? identity.exportSeed() : nil,
            pubkeyB64url: signed ? identity.publicKeyB64url : "")
    }

    /// Push `branch` to the mesh remote. Never forces, never deletes.
    public func push(
        workspace: GitWorkspace,
        owner: String,
        repo: String,
        branch: String,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalPushResult {
        try await workspace.push(credentials: credentials(owner: owner, repo: repo), branch: branch, onProgress: onProgress)
    }

    /// Fetch the mesh remote into `refs/remotes/origin/*`.
    public func fetch(
        workspace: GitWorkspace,
        owner: String,
        repo: String,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalFetchResult {
        try await withMirrorFailover(owner: owner, repo: repo, action: "fetch") { credentials, route in
            try await workspace.fetch(credentials: credentials, route: route, onProgress: onProgress)
        }
    }

    public func pull(
        workspace: GitWorkspace,
        owner: String,
        repo: String,
        branch: String,
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> LocalPullResult {
        try await withMirrorFailover(owner: owner, repo: repo, action: "pull") { credentials, route in
            try await workspace.pull(credentials: credentials, branch: branch, route: route, onProgress: onProgress)
        }
    }

    func mirrors(_ owner: String, _ repo: String) async throws -> [RepoMirror] {
        if let mirrorLookup { return try await mirrorLookup(owner, repo) }
        return try await api.freshMirrorSet(owner, repo).mirrors
    }

    func withMirrorFailover<T: Sendable>(
        owner: String,
        repo: String,
        action: String,
        _ attempt: @MainActor (ForkMeshRemoteCredentials, GitRemoteRoute) async throws -> T
    ) async throws -> T {
        let relay = credentials(owner: owner, repo: repo)
        let route = GitRemoteRoute()
        var earlier = [CloneAttempt]()
        var tried = Set<String>()
        do {
            return try await attempt(relay, route)
        } catch {
            if CloneEndpoints.isFinal(error) || !PublishService.mirrorsMayHelp(error) { throw error }
            let message = LocalRepoScreenModel.message(error)
            if let mirror = route.mirrorUrl {
                tried.insert(mirror.lowercased())
                let node = route.mirrorHost ?? ""
                earlier.append(CloneAttempt(endpoint: CloneEndpoint(kind: .mirror(node: node, healthy: false), url: mirror), message: message))
            } else {
                earlier.append(CloneAttempt(endpoint: CloneEndpoint(kind: .relay, url: relay.relayUrl), message: message))
            }
        }
        var found = [RepoMirror]()
        var lookupProblem: String?
        do {
            found = try await mirrors(owner, repo)
        } catch {
            lookupProblem = LocalRepoScreenModel.message(error)
        }
        let endpoints = CloneEndpoints.plan(owner: owner, name: repo, mirrors: found, relay: relay)
            .filter { !$0.isRelay && !tried.contains($0.url.lowercased()) }
        return try await CloneEndpoints.run(
            repository: "\(owner)/\(repo)", endpoints: endpoints, lookupProblem: lookupProblem,
            action: action, earlier: earlier
        ) { endpoint in
            try await attempt(endpoint.credentials(relay), GitRemoteRoute())
        }
    }

    nonisolated static func mirrorsMayHelp(_ error: Error) -> Bool {
        guard let error = error as? GitWorkspaceException, let code = error.code else { return false }
        switch code {
        case .networkUnavailable, .tlsRejected, .authenticationFailed, .remoteRejected, .protocolUnsupported:
            return true
        default:
            return false
        }
    }

    /// Push then publish. `session` must carry `publish_repo` + `owner_sign`.
    /// A push failure throws (`GitWorkspaceException`); everything after it is
    /// an outcome.
    public func publish(
        session: AuthSession?,
        workspace: GitWorkspace,
        repo: String,
        branch: String,
        isPublic: Bool,
        description: String = "",
        head: PublishHead = PublishHead(),
        forkedFrom: String = "",
        onProgress: (@Sendable (GitTransferProgress) -> Void)? = nil
    ) async throws -> PublishOutcome {
        if let refusal = PublishService.describeSession(session) {
            return PublishOutcome(status: .unauthorized, message: refusal)
        }
        let owner = session!.nodeName
        let push = try await self.push(workspace: workspace, owner: owner, repo: repo, branch: branch, onProgress: onProgress)
        if let rejected = push.refs.first(where: { $0.isRejected }) {
            return PublishOutcome(
                status: .failed,
                message: rejected.message.isEmpty
                    ? "ForkMesh refused the push for \(rejected.branch)."
                    : "ForkMesh refused \(rejected.branch): \(rejected.message)")
        }
        return await publishCatalogRecord(
            session: session!, repo: repo, branch: branch, isPublic: isPublic,
            description: description, head: head, forkedFrom: forkedFrom)
    }

    /// Sign and POST the catalog record for an already-pushed repository.
    ///
    /// `forkedFrom` is the `owner/name` this repository was copied from, when
    /// it was. A fork edge is always a claim the publisher makes about their
    /// *own* repository — which is why it rides inside the signed record — and
    /// the relay indexes it into the parent's fork list.
    public func publishCatalogRecord(
        session: AuthSession,
        repo: String,
        branch: String,
        isPublic: Bool,
        description: String = "",
        head: PublishHead = PublishHead(),
        forkedFrom: String = ""
    ) async -> PublishOutcome {
        let owner = session.nodeName
        let record = buildCatalogRecord(
            owner: owner,
            name: repo,
            description: description,
            isPublic: isPublic,
            branch: branch,
            maintainer: identity.publicKeyB64url,
            nodeId: nodeId(),
            updatedAt: clock.now,
            head: head,
            machineName: session.thisDevice?.label ?? "",
            cloneUrl: "\(api.gitOrigin)/\(owner)/\(repo)",
            forkedFrom: forkedFrom)
        var signable = record
        signable.removeValue(forKey: "signature")
        let signature: String
        do {
            let canonical = CatalogCanonical.v2Canonical(try catalogValues(signable))
            signature = identity.sign(Array(canonical.utf8))
        } catch {
            return PublishOutcome(status: .failed, message: "Could not reach ForkMesh to publish (\(error)).")
        }
        var payload = record
        payload["catalogSigVersion"] = .int(2)
        payload["catalogSig"] = .string(signature)
        // Lifts an owner's delete tombstone only for an explicit, in-app
        // publish (never for an automatic heartbeat).
        payload["publishIntent"] = .string("user")
        let answer: ApiJsonResult
        do {
            answer = try await api.publishRepository(payload)
        } catch {
            return PublishOutcome(status: .failed, message: "Could not reach ForkMesh to publish (\(dartErrorText(error))).")
        }
        return PublishService.interpret(answer, owner: owner, repo: repo)
    }

    nonisolated static func interpret(_ answer: ApiJsonResult, owner: String, repo: String) -> PublishOutcome {
        let publishedAs = "\(owner)/\(repo)"
        if answer.ok && answer.body["mirror"] == .bool(true) {
            return PublishOutcome(
                status: .mirrored,
                message: "Another node already publishes \(publishedAs), so this device was "
                    + "recorded as a mirror. Publish under a different name to make "
                    + "this phone the source.",
                publishedAs: publishedAs,
                canonicalNodeId: answer.body["canonicalNodeId"].map { $0.isNull ? "" : DartCore.toString($0) } ?? "")
        }
        if answer.ok {
            return PublishOutcome(status: .published, message: "Published \(publishedAs).", publishedAs: publishedAs)
        }
        switch answer.statusCode {
        case 409:
            return PublishOutcome(
                status: .stale,
                message: "ForkMesh already has a newer record for this repository. Fetch and try again.")
        case 426:
            return PublishOutcome(
                status: .upgradeRequired,
                message: "This repository needs owner-sealed encryption, which the "
                    + "mobile app cannot do yet. Publish it as public instead.")
        case 429:
            return PublishOutcome(
                status: .limited,
                message: "ForkMesh is rate-limiting publications for this account, or "
                    + "the account has too many repositories.")
        case 401, 403:
            let message: String
            switch answer.error {
            case "account_required":
                message = "This account has no key that may publish. Sign in again on this device."
            case "maintainer_mismatch":
                message = "ForkMesh does not recognise this device key for \(owner)."
            case "blocked":
                message = "This repository name is blocked on ForkMesh."
            default:
                message = "ForkMesh refused this publication (\(answer.error))."
            }
            return PublishOutcome(status: .unauthorized, message: message)
        case 410:
            return PublishOutcome(
                status: .failed,
                message: "This repository was deleted on ForkMesh and cannot be republished from here.")
        default:
            return PublishOutcome(
                status: .failed,
                message: answer.error.isEmpty
                    ? "ForkMesh could not publish this repository (HTTP \(answer.statusCode))."
                    : "ForkMesh could not publish this repository (\(answer.error)).")
        }
    }

    /// Why `session` cannot publish, or nil when it can.
    nonisolated public static func describeSession(_ session: AuthSession?) -> String? {
        guard let session, !session.nodeName.isEmpty else {
            return "Sign in to publish a repository to ForkMesh."
        }
        if !session.canPublish || !session.canOwnerSign {
            return "This sign-in is not a mobile node, so it cannot publish. Sign "
                + "out and sign in again on this device to register it as a node."
        }
        return nil
    }
}

/// `JSONValue` to the canonicalizer's value type; a null or a double has no
/// place in a catalog record and throws.
func catalogValues(_ object: JSONObject) throws -> [String: CatalogValue] {
    var out: [String: CatalogValue] = [:]
    for (key, value) in object { out[key] = try catalogValue(value) }
    return out
}

func catalogValue(_ value: JSONValue) throws -> CatalogValue {
    switch value {
    case .string(let text): return .string(text)
    case .int(let number): return .int(Int(number))
    case .bool(let flag): return .bool(flag)
    case .array(let items): return .array(try items.map(catalogValue))
    case .object(let members): return .object(try catalogValues(members))
    case .null, .double: throw CatalogCanonicalError("not canonicalizable JSON")
    }
}

/// Build the record exactly as the Worker's `safe_catalog_record` normalizes
/// it, so `catalogSigVersion: 2` verifies.
///
/// Every always-present field is emitted with the value the Worker would
/// derive; optional fields (`machineName`, `commitSubject`, ...) are included
/// only when non-empty, because the Worker keeps them absent rather than empty
/// and an extra key would change the hash.
///
/// Deliberately omitted: `stateHash` / `stateSig` values. They are optional,
/// and the relay *pins* whatever it is given — it then refuses to serve any
/// mirror whose live advertisement does not hash to that value. A phone can
/// only guess the served ref set, so a wrong fingerprint would make the
/// repository unclonable. The desktop node keeps publishing them.
public func buildCatalogRecord(
    owner: String,
    name: String,
    description: String,
    isPublic: Bool,
    branch: String,
    maintainer: String,
    nodeId: String,
    updatedAt: Date,
    head: PublishHead = PublishHead(),
    machineName: String = "",
    cloneUrl: String = "",
    platform: String = "mobile",
    version: String = "",
    forkedFrom: String = ""
) -> JSONObject {
    func clean(_ value: String, _ max: Int) -> JSONValue { .string(CatalogCanonical.cleanString(value, maxLength: max)) }
    let updatedMs = String(Int64((updatedAt.timeIntervalSince1970 * 1000).rounded(.down)))
    let commitAt = head.at.map { String(Int64(($0.timeIntervalSince1970 * 1000).rounded(.down)) / 1000) } ?? ""
    var record: JSONObject = [:]
    record["owner"] = .string(owner)
    record["name"] = .string(name)
    record["visibility"] = .string(isPublic ? "public" : "private")
    record["mirrorEncryption"] = ""
    record["opaqueRepoId"] = ""
    record["keyEpoch"] = .int(0)
    record["encryptedManifestHash"] = ""
    record["encryptedManifestSig"] = ""
    record["sizeBytes"] = .int(0)
    record["description"] = clean(description, 240)
    record["logoMetadata"] = .object([
        "description": "",
        "languages": .object([:]),
        "topics": .array([]),
        "fileStructure": .array([]),
        "frameworks": .array([]),
        "projectCategory": "",
    ])
    record["cloneUrl"] = clean(cloneUrl, 2048)
    record["solana"] = ""
    // The Worker's default when `channel` is absent; sent explicitly so the
    // signed record and the stored record are the same object.
    record["channel"] = clean("#\(owner)-\(name)", 120)
    record["hostedSince"] = ""
    record["lastSync"] = .string(updatedMs)
    record["updatedAt"] = .string(updatedMs)
    record["rootCommit"] = clean(head.rootCommit, 64)
    record["source"] = "local-node"
    record["commit"] = clean(head.commit, 64)
    record["branch"] = clean(branch, 120)
    record["issueCount"] = ""
    record["issueMaxNumber"] = ""
    record["commitCount"] = .string(head.commitCount > 0 ? "\(head.commitCount)" : "")
    record["branchCount"] = .string(head.branchCount > 0 ? "\(head.branchCount)" : "")
    record["pullCount"] = ""
    record["discussionCount"] = ""
    record["activityWeeks"] = .array(Array(repeating: .int(0), count: 52))
    record["worktreeCount"] = ""
    record["artifactCount"] = ""
    record["platform"] = clean(platform, 16)
    record["version"] = clean(version, 32)
    record["nodeId"] = clean(nodeId, 64)
    record["clonesServed"] = ""
    record["websiteServed"] = ""
    record["maintainer"] = clean(maintainer, 120)
    // Legacy v1 field: stored, but popped before the v2 record is hashed.
    record["signature"] = ""
    record["stateHash"] = ""
    record["stateSig"] = ""
    if let parent = normalizeForkedFrom(forkedFrom, owner: owner, name: name) {
        record["forkedFrom"] = .string(parent)
    }
    let machine = CatalogCanonical.cleanString(machineName, maxLength: 63)
    if !machine.isEmpty { record["machineName"] = .string(machine) }
    let subject = CatalogCanonical.cleanString(head.subject, maxLength: 120)
    if !subject.isEmpty { record["commitSubject"] = .string(subject) }
    let author = CatalogCanonical.cleanString(head.authorName, maxLength: 64)
    if !author.isEmpty { record["commitAuthorName"] = .string(author) }
    if !commitAt.isEmpty { record["commitAt"] = .string(commitAt) }
    return record
}

/// The `forkedFrom` value the Worker would store for `value`, or nil when it
/// would store nothing.
///
/// This has to agree with `app/src/catalog.py` exactly: send a `forkedFrom`
/// the Worker normalizes differently — or at all when it would drop it — and
/// the two hashes diverge, so the publish fails signature verification. The
/// key is **absent** (nil, never "") when a repository is not a fork, and a
/// self-referential edge is dropped, mirroring the Worker.
public func normalizeForkedFrom(_ value: String, owner: String, name: String) -> String? {
    // clean_string(value, 130).strip().lower().partition("/")
    let cleaned = CatalogCanonical.cleanString(value, maxLength: 130).lowercased()
    let bytes = Array(cleaned.utf8)
    guard let slash = bytes.firstIndex(of: 0x2f) else { return nil }
    guard let parentOwner = safeSegment(String(decoding: bytes[0..<slash], as: UTF8.self)),
          let parentName = safeSegment(String(decoding: bytes[(slash + 1)...], as: UTF8.self)) else {
        return nil
    }
    if parentOwner == owner.dartTrimmed.lowercased() && parentName == name.dartTrimmed.lowercased() {
        return nil
    }
    return "\(parentOwner)/\(parentName)"
}

/// `safe_segment` from `app/src/catalog.py`: non-empty, at most
/// `MAX_REPO_SEGMENT` characters, and matching `ROOM_NAME_RE`. A second slash
/// fails the pattern, which is what keeps `a/b/c` from parsing as a parent.
private func safeSegment(_ value: String) -> String? {
    let trimmed = value.dartTrimmed
    if trimmed.isEmpty || trimmed.utf16.count > 80 { return nil }
    let ok = trimmed.utf8.allSatisfy { byte in
        (0x41...0x5a).contains(byte) || (0x61...0x7a).contains(byte) || (0x30...0x39).contains(byte)
            || byte == 0x2e || byte == 0x5f || byte == 0x3a || byte == 0x2d
    }
    return ok ? trimmed : nil
}
