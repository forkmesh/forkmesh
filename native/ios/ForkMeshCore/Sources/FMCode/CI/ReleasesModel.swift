import FMAccount
import FMApi
import FMBase
import FMCrypto
import Foundation
import Observation

public struct RepoArtifactRow: Equatable, Hashable, Sendable, Identifiable {
    public var tag: String
    public var asset: RepoReleaseAsset
    public var id: String { "\(tag):\(asset.sha256):\(asset.name)" }
}

public struct ReleaseAnnouncementDraft: Equatable, Sendable {
    public var title: String
    public var body: String

    public init(release: RepoRelease, repo: RepoRef) {
        title = "\(repo.name) \(release.tag)"
        var lines = ["\(repo.slug) \(release.tag) is out."]
        if !release.assets.isEmpty {
            let platforms = Array(Set(release.assets.map(\.platformLabel))).sorted()
            lines.append("Downloads: \(platforms.joined(separator: ", ")).")
        }
        body = lines.joined(separator: "\n")
    }

    public var validationError: String? {
        if title.trimmingCharacters(in: .whitespaces).isEmpty { return "Give the announcement a title." }
        if title.count > 240 { return "Keep the title under 240 characters." }
        if body.count > 4000 { return "Keep the text under 4000 characters." }
        return nil
    }
}

@MainActor
@Observable
public final class RepoReleasesModel {
    public let repo: RepoRef
    public private(set) var releases: [RepoRelease] = []
    public private(set) var loading = false
    public private(set) var error: String?
    public private(set) var busy = false

    @ObservationIgnored private let client: CIClient

    public init(client: CIClient, repo: RepoRef) {
        self.client = client
        self.repo = repo
    }

    public func release(tag: String) -> RepoRelease? { releases.first { $0.tag == tag } }

    public var artifacts: [RepoArtifactRow] {
        releases.flatMap { release in release.assets.map { RepoArtifactRow(tag: release.tag, asset: $0) } }
    }

    public var artifactsTotalBytes: Int { artifacts.reduce(0) { $0 + max(0, $1.asset.size) } }

    public func load(refresh: Bool = false) async {
        loading = true
        if refresh { client.clearRepoCache(repo.owner, repo.name) }
        do {
            releases = try await client.releases(repo.owner, repo.name)
            error = nil
        } catch {
            self.error = "\(error)"
        }
        loading = false
    }

    public func announce(_ draft: ReleaseAnnouncementDraft, tag: String, account: String, identity: DeviceIdentity?,
                         nowMillis: Int64) async -> String {
        if let problem = draft.validationError { return problem }
        guard let identity, !account.isEmpty else { return "Sign in on this device to announce a release." }
        if busy { return "" }
        busy = true
        defer { busy = false }
        do {
            try await client.announceRelease(
                repo, tag: tag, title: draft.title, body: draft.body, account: account.lowercased(), identity: identity,
                nowMillis: nowMillis)
            return "Announced on the Fediverse."
        } catch {
            return "\(error)"
        }
    }
}

public struct RepoMirrorNode: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var node: String
    public var machineName: String
    public var status: String
    public var lastSeenMs: Int
    public var syncAgeMs: Int
    public var behind: Int
    public var cloneAvailable: Bool
    public var branch: String
    public var commit: String
    public var version: String
    public var platform: String
    public var region: String
    public var latencyMs: Int
    public var directVerified: Bool
    public var directReason: String
    public var artifactCount: Int
    public var actionsState: String

    public init(json: JSONObject) {
        node = json.firstStr("node", "owner")
        id = json.str("id").isEmpty ? node : json.str("id")
        machineName = json.str("machineName")
        status = json.str("status")
        lastSeenMs = ModelJSON.int(json["lastSeen"])
        syncAgeMs = ModelJSON.int(json["syncAgeMs"])
        behind = ModelJSON.int(json["behind"])
        cloneAvailable = json.bool("cloneAvailable")
        branch = json.str("branch")
        commit = json.str("commit")
        version = json.str("version")
        platform = json.str("platform")
        region = json.str("region")
        latencyMs = ModelJSON.int(json["latencyMs"])
        directVerified = json.bool("directVerified")
        directReason = json.str("directReason")
        artifactCount = ModelJSON.int(json["artifactCount"])
        actionsState = json.str("actionsState")
    }

    public var online: Bool { status == "online" || status == "live" }

    public var light: String {
        if !online { return "offline" }
        if behind > 0 { return "behind" }
        return directVerified || cloneAvailable ? "current" : "unverified"
    }

    public var subtitle: String {
        var parts: [String] = []
        if !branch.isEmpty { parts.append(commit.isEmpty ? branch : "\(branch) @ \(commit.prefix(8))") }
        if behind > 0 { parts.append("\(behind) behind") }
        if latencyMs > 0 { parts.append("\(latencyMs) ms") }
        if !region.isEmpty { parts.append(region) }
        if !version.isEmpty { parts.append("v\(version)") }
        return parts.joined(separator: " · ")
    }
}

@MainActor
@Observable
public final class RepoMirrorsModel {
    public let repo: RepoRef
    public private(set) var nodes: [RepoMirrorNode] = []
    public private(set) var sync: JSONObject?
    public private(set) var loading = false
    public private(set) var error: String?
    public var onlineOnly = false

    @ObservationIgnored private let client: CIClient

    public init(client: CIClient, repo: RepoRef) {
        self.client = client
        self.repo = repo
    }

    public var shown: [RepoMirrorNode] { onlineOnly ? nodes.filter(\.online) : nodes }
    public var syncLabel: String { sync?.str("label") ?? "" }
    public var syncSummary: String { sync?.str("summary") ?? "" }
    public var syncSeverity: String { sync?.str("severity") ?? "" }
    public var totalArtifacts: Int { nodes.map(\.artifactCount).max() ?? 0 }

    public func load() async {
        loading = true
        do {
            let payload = try await client.mirrorsPayload(repo)
            nodes = payload.maps("mirrors").prefix(200).map(RepoMirrorNode.init(json:)).sorted {
                $0.online != $1.online ? $0.online : $0.node < $1.node
            }
            sync = SyncStatus.mirrorsPayloadStatus(payload)
            error = nil
        } catch {
            self.error = "\(error)"
        }
        loading = false
    }
}
