import Foundation

public enum Synapse {
    public static func stableHash(_ text: String) -> UInt32 {
        var hash: UInt32 = 2166136261
        for unit in text.utf16 {
            hash ^= UInt32(unit)
            hash = hash &* 16777619
        }
        return SynapseMath.mixHash(hash)
    }

    public static func fileName(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return path }
        return String(path[path.index(after: slash)...])
    }

    static func importsOther(_ file: InsightFile, _ index: Int) -> Bool {
        file.dependsOn.contains { $0 != index }
    }

    static func importedByOther(_ file: InsightFile, _ index: Int) -> Bool {
        file.dependedOnBy.contains { $0 != index }
    }

    public static func importCount(_ snapshot: InsightSnapshot) -> Int {
        var total = 0
        for (i, file) in snapshot.files.enumerated() {
            for target in file.dependsOn where target != i { total += 1 }
        }
        return total
    }

    static func byPath(_ snapshot: InsightSnapshot) -> (Int, Int) -> Bool {
        { a, b in U.less(snapshot.files[a].path, snapshot.files[b].path) }
    }

    public static func cyclePath(_ snapshot: InsightSnapshot, _ members: [Int]) -> [Int] {
        guard var start = members.first else { return [] }
        let count = snapshot.files.count
        let inside = Set(members)
        for member in members where member >= 0 && member < count && start >= 0 && start < count {
            if U.less(snapshot.files[member].path, snapshot.files[start].path) { start = member }
        }
        if start < 0 || start >= count { return [] }
        var parent: [Int: Int] = [start: -1]
        var queue = [start]
        var closing = -1
        var head = 0
        let order = byPath(snapshot)
        while head < queue.count && closing < 0 {
            let current = queue[head]
            head += 1
            let next = snapshot.files[current].dependsOn.sorted(by: order)
            for target in next {
                if !inside.contains(target) { continue }
                if target == start {
                    closing = current
                    break
                }
                if parent[target] != nil { continue }
                parent[target] = current
                queue.append(target)
            }
        }
        if closing < 0 { return [] }
        var path: [Int] = []
        var at = closing
        while at >= 0 {
            path.append(at)
            at = parent[at] ?? -1
        }
        path.reverse()
        path.append(start)
        return path
    }

    public static func cycleText(_ snapshot: InsightSnapshot, _ path: [Int]) -> String {
        path.filter { $0 >= 0 && $0 < snapshot.files.count }
            .map { fileName(snapshot.files[$0].path) }
            .joined(separator: " → ")
    }

    public static func cycleInfos(_ snapshot: InsightSnapshot) -> [SynapseCycle] {
        snapshot.cycles.enumerated().map { c, members in
            var info = SynapseCycle()
            info.index = c
            info.files = members
            info.path = cyclePath(snapshot, members)
            return info
        }
    }

    public static func orphans(_ snapshot: InsightSnapshot) -> [Int] {
        var result: [Int] = []
        for (i, file) in snapshot.files.enumerated() where !importsOther(file, i) && !importedByOther(file, i) {
            result.append(i)
        }
        return result.sorted(by: byPath(snapshot))
    }

    public static func leaves(_ snapshot: InsightSnapshot) -> [Int] {
        var result: [Int] = []
        for (i, file) in snapshot.files.enumerated() where !importsOther(file, i) && importedByOther(file, i) {
            result.append(i)
        }
        return result
    }

    static func ranked(_ snapshot: InsightSnapshot, _ limit: Int, incoming: Bool) -> [Int] {
        var result: [Int] = []
        for (i, file) in snapshot.files.enumerated() where !(incoming ? file.dependedOnBy : file.dependsOn).isEmpty {
            result.append(i)
        }
        result.sort { a, b in
            let fa = snapshot.files[a]
            let fb = snapshot.files[b]
            let ca = (incoming ? fa.dependedOnBy : fa.dependsOn).count
            let cb = (incoming ? fb.dependedOnBy : fb.dependsOn).count
            if ca != cb { return ca > cb }
            return U.less(fa.path, fb.path)
        }
        if limit >= 0 && result.count > limit { result.removeSubrange(limit...) }
        return result
    }

    public static func mostImported(_ snapshot: InsightSnapshot, _ limit: Int) -> [Int] {
        ranked(snapshot, limit, incoming: true)
    }

    public static func mostImporting(_ snapshot: InsightSnapshot, _ limit: Int) -> [Int] {
        ranked(snapshot, limit, incoming: false)
    }

    public static func mostConnected(_ snapshot: InsightSnapshot) -> Int {
        var best = -1
        var bestScore = 0.0
        for (i, file) in snapshot.files.enumerated() {
            let incoming = Double(file.dependedOnBy.count)
            let outgoing = Double(file.dependsOn.count)
            let score = Foundation.log1p(incoming) * Foundation.log1p(outgoing) + 0.05 * Foundation.log1p(incoming + outgoing)
                + (file.cycle >= 0 ? 0.5 : 0.0)
            if score > bestScore
                || (score == bestScore && best >= 0 && score > 0 && U.less(file.path, snapshot.files[best].path)) {
                best = i
                bestScore = score
            }
        }
        return best
    }

    public static func neighbourhood(_ snapshot: InsightSnapshot, _ file: Int, hops: Int, direction: SynapseDirection) -> SynapseNeighbourhood {
        var result = SynapseNeighbourhood()
        let count = snapshot.files.count
        if file < 0 || file >= count || hops <= 0 { return result }
        func walk(_ forward: Bool) -> [Int: Int] {
            var reached: [Int: Int] = [:]
            var frontier = [file]
            var depth = 1
            while depth <= hops && !frontier.isEmpty {
                var next: [Int] = []
                for current in frontier {
                    let info = snapshot.files[current]
                    for target in forward ? info.dependsOn : info.dependedOnBy {
                        if target == file || reached[target] != nil || target < 0 || target >= count { continue }
                        reached[target] = depth
                        next.append(target)
                    }
                }
                frontier = next
                depth += 1
            }
            return reached
        }
        if direction != .importers { result.imports = walk(true) }
        if direction != .imports { result.importers = walk(false) }
        return result
    }

    public static func grouped(_ value: Int) -> String { U.grouped(value) }

    public static func plural(_ count: Int, _ one: String, _ many: String, format: (Int) -> String = Synapse.grouped) -> String {
        "\(format(count)) \(count == 1 ? one : many)"
    }

    public static func summary(_ snapshot: InsightSnapshot, format: (Int) -> String = Synapse.grouped) -> String {
        [
            plural(importCount(snapshot), "import", "imports", format: format),
            plural(snapshot.cycles.count, "circular dependency", "circular dependencies", format: format),
            plural(orphans(snapshot).count, "orphan", "orphans", format: format),
        ].joined(separator: " · ")
    }

    public static func cycleOrder(_ graph: SynapseGraph) -> [Int] {
        Array(graph.cycles.indices).sorted { a, b in
            let sa = graph.cycles[a].files.count
            let sb = graph.cycles[b].files.count
            return sa != sb ? sa > sb : a < b
        }
    }

    public static func sortedByPath(_ snapshot: InsightSnapshot, _ files: [Int]) -> [Int] {
        var seen = Set<Int>()
        return files.filter { $0 >= 0 && $0 < snapshot.files.count && seen.insert($0).inserted }.sorted(by: byPath(snapshot))
    }

    public static func externalPackages(_ file: InsightFile) -> [String] {
        var seen = Set<String>()
        let unique = file.externalImports.filter { seen.insert($0).inserted }
        return unique.enumerated().sorted { x, y in
            let a = x.element.lowercased()
            let b = y.element.lowercased()
            if a != b { return U.less(a, b) }
            return x.offset < y.offset
        }.map(\.element)
    }

    public static func connectedCount(_ graph: SynapseGraph) -> Int {
        graph.nodes.reduce(0) { $0 + ($1.fanIn + $1.fanOut > 0 ? 1 : 0) }
    }

    public static func matches(_ snapshot: InsightSnapshot, _ query: String, limit: Int) -> [Int] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        if needle.isEmpty || limit <= 0 { return [] }
        var exact: [Int] = []
        var named: [Int] = []
        var rest: [Int] = []
        for (i, file) in snapshot.files.enumerated() {
            guard file.path.range(of: needle, options: .caseInsensitive) != nil else { continue }
            if file.path.caseInsensitiveCompare(needle) == .orderedSame {
                exact.append(i)
            } else if fileName(file.path).range(of: needle, options: .caseInsensitive) != nil {
                named.append(i)
            } else {
                rest.append(i)
            }
        }
        return Array((exact + named + rest).prefix(limit))
    }
}
