import AppIntents
import FMApi
import FMBase
import FMSurfaces
import Foundation

@MainActor
enum SurfaceEntitySource {
    static let limit = 30

    static func recents(_ kind: SurfaceEntityKind) -> [SurfaceRecent] {
        SurfaceRecentsStore.shared()?.all(of: kind) ?? []
    }

    static func title(_ ref: SurfaceEntityRef) -> String {
        SurfaceRecentsStore.shared()?.find(ref)?.title ?? ref.defaultTitle
    }

    static func agents() -> AgentsSurface {
        if let coordinator = SurfacesCoordinator.shared {
            return coordinator.currentAgents()
        }
        return SurfaceSnapshotStore.shared()?.read()?.agents ?? .empty
    }

    static func repoSlugs() async -> [String] {
        var slugs: [String] = recents(.repo).map(\.ref.id)
        slugs += agents().runs.map(\.repo)
        if let api = AppServices.current?.api, api.hasSession,
           let repositories = try? await api.repositories() {
            slugs += repositories.map { "\($0.owner)/\($0.name)" }
        }
        var seen: Set<String> = []
        return slugs.filter { RepoRef(slug: $0) != nil && seen.insert($0.lowercased()).inserted }
            .prefix(limit).map { $0 }
    }
}

struct RepositoryEntity: AppEntity {
    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Repository"
    static let defaultQuery = RepositoryQuery()

    let id: String

    var displayRepresentation: DisplayRepresentation {
        DisplayRepresentation(title: "\(id)", image: .init(systemName: "folder"))
    }

    var repo: RepoRef? { RepoRef(slug: id) }
}

struct RepositoryQuery: EntityStringQuery {
    func entities(for identifiers: [String]) async throws -> [RepositoryEntity] {
        identifiers.filter { RepoRef(slug: $0) != nil }.map(RepositoryEntity.init(id:))
    }

    @MainActor
    func suggestedEntities() async throws -> [RepositoryEntity] {
        await SurfaceEntitySource.repoSlugs().map(RepositoryEntity.init(id:))
    }

    @MainActor
    func entities(matching string: String) async throws -> [RepositoryEntity] {
        let needle = string.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        var found = await SurfaceEntitySource.repoSlugs().filter { needle.isEmpty || $0.lowercased().contains(needle) }
        if let typed = RepoRef(slug: needle), !found.contains(where: { $0.lowercased() == typed.slug }) {
            found.insert(typed.slug, at: 0)
        }
        return found.map(RepositoryEntity.init(id:))
    }
}

struct IssueEntity: AppEntity {
    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Issue"
    static let defaultQuery = IssueQuery()

    let id: String
    let title: String

    var displayRepresentation: DisplayRepresentation {
        DisplayRepresentation(title: "\(title)", subtitle: "\(id)", image: .init(systemName: "exclamationmark.circle"))
    }

    var route: Route? { SurfaceEntityRef(kind: .issue, id: id).route }
}

struct IssueQuery: EntityStringQuery {
    @MainActor
    func entities(for identifiers: [String]) async throws -> [IssueEntity] {
        identifiers.compactMap { (id: String) -> IssueEntity? in
            let ref = SurfaceEntityRef(kind: .issue, id: id)
            guard ref.route != nil else { return nil }
            return IssueEntity(id: id, title: SurfaceEntitySource.title(ref))
        }
    }

    @MainActor
    func suggestedEntities() async throws -> [IssueEntity] {
        SurfaceEntitySource.recents(.issue).prefix(SurfaceEntitySource.limit).map {
            IssueEntity(id: $0.ref.id, title: $0.title)
        }
    }

    @MainActor
    func entities(matching string: String) async throws -> [IssueEntity] {
        SurfaceRecentsStore.matching(SurfaceEntitySource.recents(.issue), query: string)
            .prefix(SurfaceEntitySource.limit).map { IssueEntity(id: $0.ref.id, title: $0.title) }
    }
}

struct NoteEntity: AppEntity {
    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Note"
    static let defaultQuery = NoteQuery()

    let id: String
    let title: String

    var displayRepresentation: DisplayRepresentation {
        DisplayRepresentation(title: "\(title)", image: .init(systemName: "note.text"))
    }
}

struct NoteQuery: EntityStringQuery {
    @MainActor
    func entities(for identifiers: [String]) async throws -> [NoteEntity] {
        identifiers.filter { !$0.isEmpty }.map {
            NoteEntity(id: $0, title: SurfaceEntitySource.title(SurfaceEntityRef(kind: .note, id: $0)))
        }
    }

    @MainActor
    func suggestedEntities() async throws -> [NoteEntity] {
        SurfaceEntitySource.recents(.note).prefix(SurfaceEntitySource.limit).map {
            NoteEntity(id: $0.ref.id, title: $0.title)
        }
    }

    @MainActor
    func entities(matching string: String) async throws -> [NoteEntity] {
        SurfaceRecentsStore.matching(SurfaceEntitySource.recents(.note), query: string)
            .prefix(SurfaceEntitySource.limit).map { NoteEntity(id: $0.ref.id, title: $0.title) }
    }
}

struct AgentRunEntity: AppEntity {
    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Agent run"
    static let defaultQuery = AgentRunQuery()

    let id: String
    let title: String
    let subtitle: String

    init(id: String, title: String, subtitle: String) {
        self.id = id
        self.title = title
        self.subtitle = subtitle
    }

    init(_ candidate: AgentVoiceCandidate) {
        self.init(id: candidate.id, title: candidate.name, subtitle: candidate.detail)
    }

    var displayRepresentation: DisplayRepresentation {
        DisplayRepresentation(title: "\(title)", subtitle: "\(subtitle)", image: .init(systemName: "cpu"))
    }
}

struct AgentRunQuery: EntityStringQuery {
    @MainActor
    func entities(for identifiers: [String]) async throws -> [AgentRunEntity] {
        let known = await AgentVoiceSource.candidates()
        return identifiers.compactMap { (id: String) -> AgentRunEntity? in
            let rowId = AgentRouteDestination.rowId(id)
            if let found = known.first(where: { $0.id == id || $0.id == rowId }) {
                return AgentRunEntity(id: id, title: found.name, subtitle: found.detail)
            }
            if let number = AgentVoice.number(rowId: rowId) {
                return AgentRunEntity(id: id, title: AgentVoice.name(number: number), subtitle: "")
            }
            return id.isEmpty ? nil : AgentRunEntity(id: id, title: "Agent run", subtitle: id)
        }
    }

    @MainActor
    func suggestedEntities() async throws -> [AgentRunEntity] {
        AgentVoice.ordered(await AgentVoiceSource.candidates()).prefix(AgentVoiceSource.suggestionLimit)
            .map { AgentRunEntity($0) }
    }

    @MainActor
    func entities(matching string: String) async throws -> [AgentRunEntity] {
        let found = AgentVoice.match(await AgentVoiceSource.candidates(), query: string)
            .prefix(AgentVoiceSource.suggestionLimit).map { AgentRunEntity($0) }
        if found.isEmpty, let number = AgentVoice.number(in: string) {
            return [AgentRunEntity(id: AgentVoice.rowId(number: number), title: AgentVoice.name(number: number), subtitle: "")]
        }
        return found
    }
}
