import FMBase
import Foundation

public struct CodeMapNode: Equatable, Sendable {
    public let name: String
    public let path: String
    public let size: Int
    public let isFile: Bool
    public let children: [CodeMapNode]

    public init(name: String, path: String, size: Int, isFile: Bool, children: [CodeMapNode] = []) {
        self.name = name
        self.path = path
        self.size = size
        self.isFile = isFile
        self.children = children
    }

    public init(json j: JSONObject, parent: String = "", depth: Int = 0) {
        let name = j.str("name")
        let isFile = j.str("type") == "file"
        let explicit = j.str("path")
        let path = !explicit.isEmpty ? explicit : (parent.isEmpty ? name : (name.isEmpty ? parent : "\(parent)/\(name)"))
        let kids = depth >= CodeMapLayout.maxDepth ? [] :
            j.maps("children").prefix(CodeMapLayout.maxChildren).map { CodeMapNode(json: $0, parent: path, depth: depth + 1) }
        let own = max(0, j.int("size"))
        let total = kids.reduce(0) { $0 + $1.size }
        self.init(name: name, path: path, size: max(own, total), isFile: isFile, children: kids)
    }

    public func find(_ path: String) -> CodeMapNode? {
        if self.path == path { return self }
        for child in children where path == child.path || path.hasPrefix(child.path + "/") || child.path.isEmpty {
            if let found = child.find(path) { return found }
        }
        return nil
    }

    public var fileCount: Int { isFile ? 1 : children.reduce(0) { $0 + $1.fileCount } }
}

public struct CodeMapRect: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var width: Double
    public var height: Double

    public init(x: Double, y: Double, width: Double, height: Double) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }

    public var area: Double { width * height }
    public var shortSide: Double { min(width, height) }
}

public struct CodeMapCell: Equatable, Sendable, Identifiable {
    public var id: String { node.path }
    public let node: CodeMapNode
    public let rect: CodeMapRect
    public let depth: Int
    public let colorSlot: Int
}

public struct CodeMapCrumb: Equatable, Sendable, Identifiable {
    public var id: String { path }
    public let name: String
    public let path: String
}

public enum CodeMapLayout {
    public static let maxDepth = 6
    public static let maxChildren = 400
    public static let maxCells = 1500
    public static let minCellSide = 3.0

    public static func squarify(_ sizes: [Double], in rect: CodeMapRect) -> [CodeMapRect] {
        let total = sizes.reduce(0, +)
        guard total > 0, rect.area > 0 else { return sizes.map { _ in CodeMapRect(x: rect.x, y: rect.y, width: 0, height: 0) } }
        let scale = rect.area / total
        let areas = sizes.map { max(0, $0) * scale }
        var out = [CodeMapRect](repeating: CodeMapRect(x: rect.x, y: rect.y, width: 0, height: 0), count: sizes.count)
        var free = rect
        var start = 0
        while start < areas.count {
            let side = free.shortSide
            if side <= 0 { break }
            var end = start + 1
            var best = worst(areas[start..<end], side)
            while end < areas.count {
                let next = worst(areas[start...end], side)
                if next > best { break }
                best = next
                end += 1
            }
            let rowArea = areas[start..<end].reduce(0, +)
            let horizontal = free.width >= free.height
            let thickness = rowArea / side
            var offset = 0.0
            for i in start..<end {
                let length = thickness > 0 ? areas[i] / thickness : 0
                out[i] = horizontal
                    ? CodeMapRect(x: free.x, y: free.y + offset, width: thickness, height: length)
                    : CodeMapRect(x: free.x + offset, y: free.y, width: length, height: thickness)
                offset += length
            }
            if horizontal {
                free = CodeMapRect(x: free.x + thickness, y: free.y, width: max(0, free.width - thickness), height: free.height)
            } else {
                free = CodeMapRect(x: free.x, y: free.y + thickness, width: free.width, height: max(0, free.height - thickness))
            }
            start = end
        }
        return out
    }

    static func worst(_ row: ArraySlice<Double>, _ side: Double) -> Double {
        let sum = row.reduce(0, +)
        guard sum > 0, side > 0, let maxArea = row.max(), let minArea = row.min(), minArea > 0 else { return .infinity }
        let s2 = side * side
        let sum2 = sum * sum
        return max(s2 * maxArea / sum2, sum2 / (s2 * minArea))
    }

    public static func cells(_ root: CodeMapNode, in rect: CodeMapRect, depth maxLevel: Int = 3, inset: Double = 2) -> [CodeMapCell] {
        var out: [CodeMapCell] = []
        func walk(_ node: CodeMapNode, _ rect: CodeMapRect, _ level: Int, _ slot: Int) {
            let kids = node.children.filter { $0.size > 0 }.sorted { $0.size > $1.size }
            if kids.isEmpty { return }
            let rects = squarify(kids.map { Double($0.size) }, in: rect)
            for (index, child) in kids.enumerated() {
                if out.count >= maxCells { return }
                let r = rects[index]
                if r.shortSide < minCellSide { continue }
                let colorSlot = level == 1 ? min(index, 8) : slot
                out.append(CodeMapCell(node: child, rect: r, depth: level, colorSlot: colorSlot))
                if level < maxLevel && !child.isFile {
                    let pad = r.shortSide > inset * 6 ? inset : 0
                    let header = r.height > 28 ? 14.0 : 0
                    let inner = CodeMapRect(
                        x: r.x + pad, y: r.y + pad + header, width: max(0, r.width - 2 * pad),
                        height: max(0, r.height - 2 * pad - header))
                    walk(child, inner, level + 1, colorSlot)
                }
            }
        }
        walk(root, rect, 1, 8)
        return out
    }

    public static func hit(_ cells: [CodeMapCell], x: Double, y: Double) -> CodeMapCell? {
        cells.last { x >= $0.rect.x && x <= $0.rect.x + $0.rect.width && y >= $0.rect.y && y <= $0.rect.y + $0.rect.height }
    }

    public static func breadcrumb(_ path: String) -> [CodeMapCrumb] {
        var out = [CodeMapCrumb(name: "/", path: "")]
        var current = ""
        for part in path.split(separator: "/") {
            current = current.isEmpty ? String(part) : "\(current)/\(part)"
            out.append(CodeMapCrumb(name: String(part), path: current))
        }
        return out
    }

    public static func sizeLabel(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        let units = ["KB", "MB", "GB"]
        var value = Double(bytes) / 1024
        var unit = 0
        while value >= 1024 && unit < units.count - 1 {
            value /= 1024
            unit += 1
        }
        return value >= 10 ? "\(Int(value.rounded())) \(units[unit])" : "\(String(format: "%.1f", value)) \(units[unit])"
    }
}
