import Foundation
import FMBase

public struct Repository: Equatable, Hashable, Sendable {
    /// The catalog route owner: the account whose node published this row. It
    /// keys every read, signature and owner check, so it never changes to the
    /// public name below.
    public var owner: String
    public var name: String
    public var description: String
    public var language: String
    public var stars: Int
    public var forks: Int
    public var mirrors: Int
    public var defaultBranch: String
    public var isPrivate: Bool
    public var liveHost: Bool
    public var cloneOnline: Bool
    public var updatedMs: Int
    /// The organization the relay resolved this exact row to (`logicalOwners`).
    public var organization: String
    /// The name the repository is known by publicly, when that differs from the
    /// serving node: an organization alias, or the upstream a mirror copies.
    /// Set by `collapseRepositoryMirrors`; empty means `owner` is the public name.
    public var publicOwner: String
    public var rootCommit: String
    public var forkedFrom: String
    public var source: String
    public var cloneUrl: String

    public init(
        owner: String, name: String, description: String = "", language: String = "", stars: Int = 0,
        forks: Int = 0, mirrors: Int = 1, defaultBranch: String = "main", isPrivate: Bool = false,
        liveHost: Bool = false, cloneOnline: Bool = false, updatedMs: Int = 0,
        organization: String = "", publicOwner: String = "", rootCommit: String = "", forkedFrom: String = "",
        source: String = "", cloneUrl: String = ""
    ) {
        self.owner = owner
        self.name = name
        self.description = description
        self.language = language
        self.stars = stars
        self.forks = forks
        self.mirrors = mirrors
        self.defaultBranch = defaultBranch
        self.isPrivate = isPrivate
        self.liveHost = liveHost
        self.cloneOnline = cloneOnline
        self.updatedMs = updatedMs
        self.organization = organization
        self.publicOwner = publicOwner
        self.rootCommit = rootCommit
        self.forkedFrom = forkedFrom
        self.source = source
        self.cloneUrl = cloneUrl
    }

    public init(json j: JSONObject) {
        self.init(
            owner: j.str("owner"),
            name: j.str("name"),
            description: j.str("description"),
            language: j.str("language"),
            stars: ModelJSON.int(j["stars"]),
            forks: ModelJSON.int(j["forks"]),
            mirrors: ModelJSON.int(ModelJSON.coalesce(j["mirrors"]) ?? .int(1)),
            defaultBranch: j.str("defaultBranch", "main"),
            isPrivate: j.bool("isPrivate"),
            liveHost: j.bool("liveHost"),
            cloneOnline: j.bool("cloneOnline"),
            updatedMs: ModelJSON.int(ModelJSON.coalesce(j["updatedAt"], j["publishedAt"]) ?? .int(0)),
            organization: Self.catalogOrganization(j),
            rootCommit: j.str("rootCommit").dartTrimmed,
            forkedFrom: j.str("forkedFrom").dartTrimmed.lowercased(),
            source: j.str("source").dartTrimmed,
            cloneUrl: j.str("cloneUrl").dartTrimmed
        )
    }

    public var displayOwner: String { publicOwner.isEmpty ? owner : publicOwner }
    public var fullName: String { "\(displayOwner)/\(name)" }

    /// A fork keeps its parent's root commit and clone URL; neither may fold it
    /// back into the parent.
    var isFork: Bool {
        forkedFrom.contains("/") && forkedFrom != "\(owner.lowercased())/\(name.lowercased())"
    }

    func merged(publicOwner: String, copies: [Repository]) -> Repository {
        func most(_ field: (Repository) -> Int) -> Int { copies.map(field).reduce(0) { $0 > $1 ? $0 : $1 } }
        func first(_ field: (Repository) -> String) -> String { copies.map(field).first { !$0.isEmpty } ?? "" }
        let mirrorCount = most { $0.mirrors }
        var out = self
        out.description = description.isEmpty ? first { $0.description } : description
        out.language = language.isEmpty ? first { $0.language } : language
        out.stars = most { $0.stars }
        out.forks = most { $0.forks }
        out.mirrors = copies.count > mirrorCount ? copies.count : mirrorCount
        out.liveHost = copies.contains { $0.liveHost }
        out.cloneOnline = copies.contains { $0.cloneOnline || $0.liveHost }
        out.updatedMs = most { $0.updatedMs }
        out.publicOwner = publicOwner.lowercased() == owner.lowercased() ? "" : publicOwner
        return out
    }

    /// The first organization in a catalog row's `logicalOwners` (or the legacy
    /// single `ownerKind`/`logicalOwner` pair).
    static func catalogOrganization(_ j: JSONObject) -> String {
        let owners: [JSONValue]
        if let values = j["logicalOwners"]?.arrayValue, !values.isEmpty {
            owners = values
        } else {
            owners = [.object(["kind": j["ownerKind"] ?? .null, "owner": j["logicalOwner"] ?? .null])]
        }
        for value in owners {
            guard let entry = value.objectValue else { continue }
            let kind = entry.str("kind").dartTrimmed.lowercased()
            let owner = entry.str("owner").dartTrimmed
            if kind == "organization" && !owner.isEmpty { return owner }
        }
        return ""
    }
}

private let cloneSegment = try! NSRegularExpression(pattern: "^[A-Za-z0-9._:-]+$")

/// The `owner/name` a mirror's clone URL points at, or nil.
func cloneUrlRoute(_ cloneUrl: String) -> (owner: String, name: String)? {
    var path = cloneUrl.dartTrimmed
    if path.isEmpty { return nil }
    if path.hasPrefix("git@"), let colon = path.firstIndex(of: ":") {
        path = String(path[path.index(after: colon)...])
    } else {
        path = URLComponents(string: path)?.percentEncodedPath ?? ""
    }
    let parts = path.split(separator: "/").map(String.init)
    if parts.count < 2 { return nil }
    func decode(_ part: String) -> String { DartCore.decodeComponent(part) ?? part }
    let owner = decode(parts[parts.count - 2])
    var name = decode(parts[parts.count - 1])
    if name.lowercased().hasSuffix(".git") { name = String(name.dropLast(4)) }
    if !DartRegex.matches(cloneSegment, owner) || !DartRegex.matches(cloneSegment, name) { return nil }
    return (owner, name)
}

/// Folds the catalog's physical rows into one entry per repository, named by
/// its public owner (issue #717).
///
/// `/api/repositories` lists every serving copy under the node that publishes
/// it, so the flagship arrived as mirror8/forkmesh, mirror23/forkmesh, ... and
/// never as forkmesh/forkmesh. The relay resolves rows to their organization
/// (`logicalOwners`), but an organization alias is registered on ONE row: the
/// other mirrors are recognised by sharing its root commit, or by a clone URL
/// naming the route they copy. Rows with no public name other than their own
/// owner are left alone. The kept row is the one the alias routes to, so reads
/// and owner-signed actions keep addressing a real catalog row.
public func collapseRepositoryMirrors(_ repos: [Repository]) -> [Repository] {
    func route(_ owner: String, _ name: String) -> String { "\(owner.lowercased())/\(name.lowercased())" }
    var orgByRoot: [String: String] = [:]
    var orgByRoute: [String: String] = [:]
    for repo in repos where !repo.organization.isEmpty {
        let key = route(repo.owner, repo.name)
        if orgByRoute[key] == nil { orgByRoute[key] = repo.organization }
        if !repo.isFork && !repo.rootCommit.isEmpty {
            let rootKey = "\(repo.rootCommit)\n\(repo.name.lowercased())"
            if orgByRoot[rootKey] == nil { orgByRoot[rootKey] = repo.organization }
        }
    }
    func publicOwnerOf(_ repo: Repository) -> String {
        if !repo.organization.isEmpty { return repo.organization }
        if repo.isFork { return repo.owner }
        if !repo.rootCommit.isEmpty, let byRoot = orgByRoot["\(repo.rootCommit)\n\(repo.name.lowercased())"] {
            return byRoot
        }
        if repo.source == "remote-clone", let upstream = cloneUrlRoute(repo.cloneUrl),
           upstream.name.lowercased() == repo.name.lowercased() {
            return orgByRoute[route(upstream.owner, upstream.name)] ?? upstream.owner
        }
        return repo.owner
    }

    var order: [String] = []
    var groups: [String: [Repository]] = [:]
    var publicOwners: [String: String] = [:]
    for repo in repos {
        let publicOwner = publicOwnerOf(repo)
        let key = "\(route(publicOwner, repo.name))\n\(repo.isPrivate)"
        if groups[key] == nil {
            order.append(key)
            publicOwners[key] = publicOwner
        }
        groups[key, default: []].append(repo)
    }
    return order.map { key in
        let copies = groups[key]!
        let publicOwner = publicOwners[key]!
        return groupRepresentative(copies, publicOwner).merged(publicOwner: publicOwner, copies: copies)
    }
}

/// The row an organization alias routes to, else a live host, else the most
/// recently updated copy.
private func groupRepresentative(_ copies: [Repository], _ publicOwner: String) -> Repository {
    let wanted = publicOwner.lowercased()
    if let repo = copies.first(where: { $0.organization.lowercased() == wanted }) { return repo }
    if let repo = copies.first(where: { $0.owner.lowercased() == wanted }) { return repo }
    let live = copies.filter(\.liveHost)
    let pool = live.isEmpty ? copies : live
    return pool.dropFirst().reduce(pool[0]) { a, b in b.updatedMs > a.updatedMs ? b : a }
}
