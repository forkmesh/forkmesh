import FMBase
import Foundation

public struct CommitGraphRow: Equatable, Sendable, Identifiable {
    public var id: String { hash }
    public let hash: String
    public let column: Int
    public let lanesIn: [Int]
    public let lanesOut: [Int]
    public let edges: [CommitGraphEdge]
    public let isMerge: Bool
    public let overflow: Bool
}

public struct CommitGraphEdge: Equatable, Sendable {
    public let from: Int
    public let to: Int
}

public enum CommitGraph {
    public static let maxLanes = 6

    public static func parents(_ commit: JSONObject) -> [String] {
        if let list = commit["parents"]?.arrayValue {
            return list.map { ($0.objectValue?.firstStr("sha", "hash") ?? $0.text).dartTrimmed.lowercased() }.filter { !$0.isEmpty }
        }
        return commit.str("parents").split(separator: " ").map { String($0).lowercased() }.filter { !$0.isEmpty }
    }

    public static func hash(_ commit: JSONObject) -> String { commit.firstStr("hash", "sha").dartTrimmed.lowercased() }

    public static func layout(_ commits: [(hash: String, parents: [String])]) -> [CommitGraphRow] {
        var lanes: [String?] = []
        var rows: [CommitGraphRow] = []
        for commit in commits {
            let hash = commit.hash.lowercased()
            let before = lanes.enumerated().compactMap { $0.element == nil ? nil : $0.offset }
            var column = lanes.firstIndex { $0 == hash } ?? -1
            if column < 0 {
                column = lanes.firstIndex { $0 == nil } ?? lanes.count
                if column == lanes.count { lanes.append(nil) }
            }
            var edges: [CommitGraphEdge] = []
            for (index, lane) in lanes.enumerated() where lane == hash && index != column {
                lanes[index] = nil
                edges.append(CommitGraphEdge(from: index, to: column))
            }
            let parents = commit.parents.map { $0.lowercased() }
            lanes[column] = parents.first
            if !parents.isEmpty { edges.append(CommitGraphEdge(from: column, to: column)) }
            for parent in parents.dropFirst() {
                if let existing = lanes.firstIndex(where: { $0 == parent }) {
                    edges.append(CommitGraphEdge(from: column, to: existing))
                    continue
                }
                let slot = lanes.firstIndex { $0 == nil } ?? lanes.count
                if slot == lanes.count { lanes.append(parent) } else { lanes[slot] = parent }
                edges.append(CommitGraphEdge(from: column, to: slot))
            }
            while let last = lanes.last, last == nil { lanes.removeLast() }
            let after = lanes.enumerated().compactMap { $0.element == nil ? nil : $0.offset }
            let overflow = column >= maxLanes || after.contains { $0 >= maxLanes }
            rows.append(CommitGraphRow(
                hash: hash, column: min(column, maxLanes - 1), lanesIn: before.filter { $0 < maxLanes },
                lanesOut: after.filter { $0 < maxLanes },
                edges: edges.filter { $0.from < maxLanes && $0.to < maxLanes }, isMerge: parents.count > 1,
                overflow: overflow))
        }
        return rows
    }

    public static func layout(json commits: [JSONObject]) -> [CommitGraphRow] {
        layout(commits.map { (hash($0), parents($0)) })
    }

    public static func width(_ rows: [CommitGraphRow]) -> Int {
        rows.reduce(1) { max($0, ($1.lanesIn + $1.lanesOut + [$1.column]).max().map { $0 + 1 } ?? 1) }
    }
}
