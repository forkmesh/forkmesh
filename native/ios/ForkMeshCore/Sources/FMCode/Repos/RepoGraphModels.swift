import FMApi
import FMBase
import Foundation
import Observation

public enum RepoSurface: Int, CaseIterable, Sendable, Identifiable {
    case desktop, web, mobile

    public var id: Int { rawValue }

    public var label: String {
        switch self {
        case .desktop: return "Desktop"
        case .web: return "Web"
        case .mobile: return "Mobile"
        }
    }
}

public struct RepoSurfaceMark: Equatable, Sendable {
    public let surface: RepoSurface
    public let commit: String

    public init?(surface: RepoSurface, commit raw: String) {
        let hex = raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased().prefix { $0.isHexDigit }
        guard hex.count >= 7 else { return nil }
        self.surface = surface
        self.commit = String(hex)
    }

    public var short: String { RepoDisplay.shortHash(commit) }

    public func matches(_ hash: String) -> Bool {
        let other = hash.lowercased()
        return other.count >= 7 && (other.hasPrefix(commit) || commit.hasPrefix(other))
    }
}

public enum RepoSurfacePlace: Equatable, Sendable {
    case tip
    case behind(Int)
    case outside
    case unknown
}

public struct RepoSurfaceSpot: Equatable, Sendable, Identifiable {
    public let surface: RepoSurface
    public let mark: RepoSurfaceMark?
    public let rowIndex: Int?
    public let place: RepoSurfacePlace

    public var id: Int { surface.rawValue }

    public var detail: String {
        switch place {
        case .tip: return "At the tip"
        case .behind(let count): return count == 1 ? "1 commit behind" : "\(count) commits behind"
        case .outside: return "Older than the commits shown"
        case .unknown: return "Not reported"
        }
    }
}

public struct RepoGraphRow: Equatable, Sendable, Identifiable {
    public let row: RepoCommitRow
    public let graph: CommitGraphRow
    public let marks: [RepoSurface]

    public var id: Int { row.index }
}

public struct RepoGraph: Equatable, Sendable {
    public let rows: [RepoGraphRow]
    public let lanes: Int
    public let spots: [RepoSurfaceSpot]

    public static let empty = RepoGraph(rows: [], lanes: 1, spots: [])

    public var marked: Bool { spots.contains { $0.rowIndex != nil } }

    @MainActor
    public static func build(
        _ commits: [JSONObject], marks: [RepoSurfaceMark], now: Date = Date(), timeZone: TimeZone = .current
    ) -> RepoGraph {
        let commitRows = RepoCommitsTabModel.commitRows(commits, now: now, timeZone: timeZone)
        let layout = CommitGraph.layout(json: commits)
        let hashes = commits.map(CommitGraph.hash)
        var marksAt: [Int: [RepoSurface]] = [:]
        var spots: [RepoSurfaceSpot] = []
        for surface in RepoSurface.allCases {
            guard let mark = marks.first(where: { $0.surface == surface }) else {
                spots.append(RepoSurfaceSpot(surface: surface, mark: nil, rowIndex: nil, place: .unknown))
                continue
            }
            guard let index = hashes.firstIndex(where: mark.matches) else {
                spots.append(RepoSurfaceSpot(surface: surface, mark: mark, rowIndex: nil, place: .outside))
                continue
            }
            marksAt[index, default: []].append(surface)
            let behind = commitsBehind(index, commits: commits, hashes: hashes)
            spots.append(
                RepoSurfaceSpot(surface: surface, mark: mark, rowIndex: index, place: behind == 0 ? .tip : .behind(behind)))
        }
        let rows = zip(commitRows, layout).map { (row: RepoCommitRow, graph: CommitGraphRow) in
            RepoGraphRow(row: row, graph: graph, marks: marksAt[row.index] ?? [])
        }
        return RepoGraph(rows: rows, lanes: CommitGraph.width(layout), spots: spots)
    }

    static func commitsBehind(_ index: Int, commits: [JSONObject], hashes: [String]) -> Int {
        var position: [String: Int] = [:]
        for (offset, hash) in hashes.enumerated() where !hash.isEmpty { position[hash] = offset }
        var reached: Set<Int> = [index]
        var queue = [index]
        while let next = queue.popLast() {
            for parent in CommitGraph.parents(commits[next]) {
                guard let found = position[parent], reached.insert(found).inserted else { continue }
                queue.append(found)
            }
        }
        return commits.count - reached.count
    }
}

@MainActor
@Observable
public final class RepoSurfaceMarksModel {
    public typealias Loader = @MainActor @Sendable () async -> String?

    public private(set) var marks: [RepoSurfaceMark] = []
    public private(set) var loaded = false

    @ObservationIgnored private let loaders: [RepoSurface: Loader]

    public init(loaders: [RepoSurface: Loader]) {
        self.loaders = loaders
    }

    public func load() async {
        var found: [RepoSurfaceMark] = []
        await withTaskGroup(of: RepoSurfaceMark?.self) { group in
            for (surface, loader) in loaders {
                group.addTask { @MainActor in
                    guard let commit = await loader() else { return nil }
                    return RepoSurfaceMark(surface: surface, commit: commit)
                }
            }
            for await mark in group {
                guard let mark else { continue }
                found.append(mark)
                marks = found.sorted { $0.surface.rawValue < $1.surface.rawValue }
            }
        }
        marks = found.sorted { $0.surface.rawValue < $1.surface.rawValue }
        loaded = true
    }

    public static func desktopCommit(_ status: JSONObject) -> String? {
        let commit = status.str("commit")
        return commit.isEmpty ? nil : commit
    }

    public static func webCommit(_ version: JSONObject) -> String? {
        let rev = version.str("rev")
        return rev.isEmpty ? nil : rev
    }
}

extension RepoCommitsTabModel {
    public func graph(marks: [RepoSurfaceMark], now: Date = Date()) -> RepoGraph {
        RepoGraph.build(commits.data ?? [], marks: marks, now: now)
    }

    public static func showsSurfaces(_ graph: RepoGraph, repo: Repository) -> Bool {
        graph.marked || repo.name.lowercased() == "forkmesh"
    }
}
