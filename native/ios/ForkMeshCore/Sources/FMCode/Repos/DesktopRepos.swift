import FMApi
import FMBase
import FMChat
import FMControl
import Foundation
import Observation

public struct DesktopRepo: Equatable, Hashable, Sendable, Identifiable {
    public var owner: String
    public var name: String
    public var description: String
    public var branch: String
    public var checkout: Bool
    public var isPrivate: Bool
    public var published: Bool
    public var runningAgents: Int
    public var totalAgents: Int

    public var id: String { slug.lowercased() }
    public var slug: String { "\(owner)/\(name)" }

    public init(
        owner: String, name: String, description: String = "", branch: String = "", checkout: Bool = false,
        isPrivate: Bool = false, published: Bool = false, runningAgents: Int = 0, totalAgents: Int = 0
    ) {
        self.owner = owner
        self.name = name
        self.description = description
        self.branch = branch
        self.checkout = checkout
        self.isPrivate = isPrivate
        self.published = published
        self.runningAgents = runningAgents
        self.totalAgents = totalAgents
    }

    public init?(json: JSONObject) {
        var owner = json.str("owner").dartTrimmed
        var name = json.str("name").dartTrimmed
        if owner.isEmpty || name.isEmpty, let ref = RepoRef(slug: json.str("repo").dartTrimmed) {
            owner = ref.owner
            name = ref.name
        }
        if owner.isEmpty || name.isEmpty { return nil }
        let agents = json["agents"]?.objectValue ?? [:]
        self.init(
            owner: owner, name: name, description: json.str("description"), branch: json.str("branch"),
            checkout: json.bool("checkout"), isPrivate: json.bool("private"), published: json.bool("published"),
            runningAgents: agents.int("running"), totalAgents: agents.int("total"))
    }

    public var repository: Repository {
        Repository(owner: owner, name: name, description: description, mirrors: 0, isPrivate: isPrivate, liveHost: true)
    }

    public var detail: String {
        var parts: [String] = []
        if !branch.isEmpty { parts.append(branch) } else if !checkout { parts.append("No checkout") }
        if runningAgents > 0 { parts.append("\(runningAgents) running") }
        parts.append(isPrivate ? "Private" : "Public")
        if !published { parts.append("Not published") }
        return parts.joined(separator: " · ")
    }
}

@MainActor
@Observable
public final class DesktopReposModel {
    public private(set) var repos: [DesktopRepo] = []
    public private(set) var desktopName = ""
    public private(set) var loading = false
    public private(set) var problem: String?

    @ObservationIgnored private let family: DesktopControlFamily?
    @ObservationIgnored private let fallback: @MainActor () -> [LocalAgentRepository]
    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var started = false

    public init(
        channel: DesktopControlCalling?, desktopName: String = "",
        fallback: @escaping @MainActor () -> [LocalAgentRepository] = { [] }
    ) {
        family = channel.map { DesktopControlFamily(.repos, channel: $0) }
        self.desktopName = desktopName
        self.fallback = fallback
    }

    public var available: Bool { family != nil }

    public func start() async {
        if started { return }
        started = true
        if let family {
            tapId = family.addEventTap { [weak self] suffix, _ in
                guard suffix == "changed", let self else { return }
                Task { await self.load() }
            }
        }
        await load()
    }

    public func stop() {
        if let tapId { family?.removeEventTap(tapId) }
        tapId = nil
        started = false
    }

    public func load() async {
        guard let family else { return }
        loading = true
        defer { loading = false }
        do {
            let reply = try await family.call("list")
            repos = DesktopReposModel.sorted(reply.maps("repos").compactMap(DesktopRepo.init(json:)))
            let name = reply.str("desktop").dartTrimmed
            if !name.isEmpty { desktopName = name }
            problem = nil
        } catch {
            let older = fallback().map { DesktopRepo(owner: $0.owner, name: $0.name, checkout: true, published: true) }
            if !older.isEmpty {
                repos = DesktopReposModel.sorted(older)
                problem = nil
            } else {
                problem = DesktopReposModel.explain(error)
            }
        }
    }

    public nonisolated static func sorted(_ repos: [DesktopRepo]) -> [DesktopRepo] {
        var seen = Set<String>()
        return repos.filter { seen.insert($0.id).inserted }.sorted { left, right in
            if (left.runningAgents > 0) != (right.runningAgents > 0) { return left.runningAgents > 0 }
            return left.id < right.id
        }
    }

    public nonisolated static func explain(_ error: Error) -> String {
        switch (error as? LocalControlException)?.code ?? "" {
        case "not_paired": return "Pair this phone with your desktop to see its repositories."
        case "not_owner": return "The desktop is signed in to another account."
        case "unsupported", "unknown_method": return "Update the desktop to list its repositories here."
        default: return "The desktop is offline. Its repositories show here when it is back."
        }
    }
}
