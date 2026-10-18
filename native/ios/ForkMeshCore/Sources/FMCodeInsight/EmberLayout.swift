import Foundation

public struct EmberNode: Sendable, Equatable {
    public var name = ""
    public var path = ""
    public var parent = -1
    public var depth = 0
    public var file = -1
    public var children: [Int] = []
    public var x: Double = 0
    public var y: Double = 0
    public var r: Double = 0
    public var codeLines: Int64 = 0
    public var files = 0
    public var health: Double = 10
    public var commits = 0

    public init() {}

    public var isFile: Bool { file >= 0 }
}

public struct EmberLayout: Sendable, Equatable {
    public var nodes: [EmberNode] = []
    public var nodeOfFile: [Int] = []
    public var maxDepth = 0

    public init() {}

    public var isEmpty: Bool { nodes.isEmpty }

    public func isFolder(_ node: Int) -> Bool {
        node >= 0 && node < nodes.count && nodes[node].file < 0
    }

    public func contains(_ node: Int, x: Double, y: Double) -> Bool {
        guard node >= 0 && node < nodes.count else { return false }
        let n = nodes[node]
        let dx = x - n.x
        let dy = y - n.y
        return dx * dx + dy * dy <= n.r * n.r
    }

    public func nodeAt(x: Double, y: Double) -> Int {
        if nodes.isEmpty || !contains(0, x: x, y: y) { return -1 }
        var current = 0
        while true {
            var next = -1
            for child in nodes[current].children where contains(child, x: x, y: y) {
                next = child
                break
            }
            if next < 0 { return current }
            current = next
        }
    }

    public func chain(_ node: Int) -> [Int] {
        var out: [Int] = []
        var at = node
        while at >= 0 && at < nodes.count {
            out.append(at)
            at = nodes[at].parent
        }
        return out.reversed()
    }

    public func isAncestor(_ ancestor: Int, _ node: Int) -> Bool {
        var at = node
        while at >= 0 && at < nodes.count {
            if at == ancestor { return true }
            at = nodes[at].parent
        }
        return false
    }

    public func fileNode(_ file: Int) -> Int {
        file >= 0 && file < nodeOfFile.count ? nodeOfFile[file] : -1
    }
}

public struct EmberLayoutOptions: Sendable, Equatable {
    public var minLines: Double = 12
    public var padding: Double = 2.5
    public var rimShare: Double = 0.035
    public var collapseChains = true

    public init() {}
}

public struct EmberView: Sendable, Equatable {
    public var cx: Double
    public var cy: Double
    public var k: Double

    public init(cx: Double = 0, cy: Double = 0, k: Double = 1) {
        self.cx = cx
        self.cy = cy
        self.k = k
    }
}

public enum EmberTap: Sendable, Equatable {
    case none
    case selectFile(Int)
    case focus(Int)
    case zoomOut
    case clearSelection
}

public struct EmberCrumb: Sendable, Equatable, Hashable {
    public var name: String
    public var node: Int
    public var current: Bool

    public init(name: String, node: Int, current: Bool = false) {
        self.name = name
        self.node = node
        self.current = current
    }
}

extension Ember {
    struct BuildNode {
        var name = ""
        var path = ""
        var file = -1
        var children: [Int] = []
        var folders: [String: Int] = [:]
    }

    public static func buildLayout(_ snapshot: InsightSnapshot, options: EmberLayoutOptions = EmberLayoutOptions()) -> EmberLayout {
        var layout = EmberLayout()
        var build: [BuildNode] = []
        build.reserveCapacity(snapshot.files.count * 2 + 1)
        var root = BuildNode()
        root.name = snapshot.name.isEmpty ? "Repository" : snapshot.name
        build.append(root)
        for (i, file) in snapshot.files.enumerated() {
            let parts = file.path.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
            if parts.isEmpty { continue }
            var current = 0
            var prefix = ""
            for p in 0..<(parts.count - 1) {
                prefix = prefix.isEmpty ? parts[p] : prefix + "/" + parts[p]
                if let found = build[current].folders[parts[p]] {
                    current = found
                    continue
                }
                var folder = BuildNode()
                folder.name = parts[p]
                folder.path = prefix
                build.append(folder)
                let index = build.count - 1
                build[current].folders[parts[p]] = index
                build[current].children.append(index)
                current = index
            }
            var leaf = BuildNode()
            leaf.name = parts[parts.count - 1]
            leaf.path = file.path
            leaf.file = i
            build.append(leaf)
            build[current].children.append(build.count - 1)
        }

        layout.nodeOfFile = [Int](repeating: -1, count: snapshot.files.count)
        layout.nodes.reserveCapacity(build.count)
        var stack: [(build: Int, parent: Int, depth: Int)] = [(0, -1, 0)]
        while let item = stack.popLast() {
            var source = item.build
            var node = EmberNode()
            node.name = build[source].name
            node.path = build[source].path
            node.file = build[source].file
            node.parent = item.parent
            node.depth = item.depth
            if options.collapseChains && item.parent >= 0 && node.file < 0 {
                while build[source].children.count == 1 && build[build[source].children[0]].file < 0 {
                    source = build[source].children[0]
                    node.name += "/" + build[source].name
                    node.path = build[source].path
                }
            }
            layout.nodes.append(node)
            let index = layout.nodes.count - 1
            layout.maxDepth = max(layout.maxDepth, node.depth)
            if item.parent >= 0 { layout.nodes[item.parent].children.append(index) }
            if node.file >= 0 { layout.nodeOfFile[node.file] = index }
            for child in build[source].children.reversed() {
                stack.append((child, index, item.depth + 1))
            }
        }

        for i in stride(from: layout.nodes.count - 1, through: 0, by: -1) {
            if layout.nodes[i].file >= 0 {
                let file = snapshot.files[layout.nodes[i].file]
                layout.nodes[i].codeLines = Int64(file.codeLines)
                layout.nodes[i].files = 1
                layout.nodes[i].health = file.health
                layout.nodes[i].commits = file.commits
                layout.nodes[i].r = max(Double(file.codeLines), options.minLines).squareRoot()
                continue
            }
            var node = layout.nodes[i]
            var weight = 0.0
            var lines: Int64 = 0
            for child in node.children {
                let c = layout.nodes[child]
                node.codeLines += c.codeLines
                node.files += c.files
                node.commits += c.commits
                let l = max(Int64(1), c.codeLines)
                lines += l
                weight += c.health * Double(l)
            }
            node.health = lines > 0 ? weight / Double(lines) : 10
            var keyed: [(index: Int, r: Double, name: String)] = []
            keyed.reserveCapacity(node.children.count)
            for child in node.children { keyed.append((child, layout.nodes[child].r, layout.nodes[child].name)) }
            keyed.sort { a, b in
                if a.r != b.r { return a.r > b.r }
                return U.less(a.name, b.name)
            }
            node.children = keyed.map(\.index)
            var circles = keyed.map { EmberCircle(x: 0, y: 0, r: $0.r + options.padding) }
            let enclosing = packSiblings(&circles)
            for (c, child) in node.children.enumerated() {
                layout.nodes[child].x = circles[c].x
                layout.nodes[child].y = circles[c].y
            }
            node.r = enclosing + options.padding + enclosing * options.rimShare
            layout.nodes[i] = node
        }
        if layout.nodes.count > 1 {
            for i in 1..<layout.nodes.count {
                let parent = layout.nodes[layout.nodes[i].parent]
                layout.nodes[i].x += parent.x
                layout.nodes[i].y += parent.y
            }
        }
        return layout
    }

    public static func fitTransform(_ node: EmberNode, width: Double, height: Double, margin: Double = 18) -> EmberView {
        let half = max(10.0, min(width, height) / 2 - margin)
        return EmberView(cx: node.x, cy: node.y, k: node.r > 0 ? half / node.r : 1)
    }

    public static func interpolateView(_ from: EmberView, _ to: EmberView, _ t: Double, width: Double, height: Double) -> EmberView {
        let size = max(1.0, min(width, height))
        let rho = 2.0.squareRoot()
        let rho2 = 2.0
        let rho4 = 4.0
        let w0 = size / from.k
        let w1 = size / to.k
        let dx = to.cx - from.cx
        let dy = to.cy - from.cy
        let d2 = dx * dx + dy * dy
        var out = EmberView()
        if d2 < 1e-12 {
            let s = log(w1 / w0) / rho
            out.cx = from.cx + t * dx
            out.cy = from.cy + t * dy
            out.k = size / (w0 * exp(rho * t * s))
            return out
        }
        let d1 = d2.squareRoot()
        let b0 = (w1 * w1 - w0 * w0 + rho4 * d2) / (2 * w0 * rho2 * d1)
        let b1 = (w1 * w1 - w0 * w0 - rho4 * d2) / (2 * w1 * rho2 * d1)
        let r0 = log((b0 * b0 + 1).squareRoot() - b0)
        let r1 = log((b1 * b1 + 1).squareRoot() - b1)
        let s = t * (r1 - r0) / rho
        let coshr0 = cosh(r0)
        let u = w0 / (rho2 * d1) * (coshr0 * tanh(rho * s + r0) - sinh(r0))
        out.cx = from.cx + u * dx
        out.cy = from.cy + u * dy
        out.k = size / (w0 * coshr0 / cosh(rho * s + r0))
        if !out.cx.isFinite || !out.cy.isFinite || !out.k.isFinite || out.k <= 0 {
            out.cx = from.cx + t * dx
            out.cy = from.cy + t * dy
            out.k = exp(log(from.k) + t * (log(to.k) - log(from.k)))
        }
        if t >= 1 { out = to }
        return out
    }

    public static func easeInOut(_ value: Double) -> Double {
        let t = max(0, min(1, value))
        return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2
    }

    public static func readableFocus(_ layout: EmberLayout, _ node: Int, width: Double, height: Double, minRadius: Double = 48) -> Int {
        guard node >= 0 && node < layout.nodes.count else { return layout.nodes.isEmpty ? -1 : 0 }
        var candidate = layout.isFolder(node) ? node : layout.nodes[node].parent
        if candidate < 0 { return 0 }
        var best = candidate
        while candidate >= 0 {
            let fit = fitTransform(layout.nodes[candidate], width: width, height: height)
            if layout.nodes[node].r * fit.k < minRadius { break }
            best = candidate
            candidate = layout.nodes[candidate].parent
        }
        return best
    }

    public static func toScreen(_ view: EmberView, x: Double, y: Double, width: Double, height: Double) -> (x: Double, y: Double) {
        ((x - view.cx) * view.k + width / 2, (y - view.cy) * view.k + height / 2)
    }

    public static func toWorld(_ view: EmberView, x: Double, y: Double, width: Double, height: Double) -> (x: Double, y: Double) {
        ((x - width / 2) / view.k + view.cx, (y - height / 2) / view.k + view.cy)
    }

    public static func minScale(_ layout: EmberLayout, width: Double, height: Double) -> Double {
        guard let root = layout.nodes.first else { return 1e-6 }
        return fitTransform(root, width: width, height: height).k * 0.6
    }

    public static func maxScale(_ layout: EmberLayout, width: Double, height: Double) -> Double {
        max(minScale(layout, width: width, height: height) * 2, 90)
    }

    public static func zoomAround(_ view: EmberView, scale factor: Double, x: Double, y: Double, width: Double, height: Double,
                                  minK: Double, maxK: Double) -> EmberView {
        let anchor = toWorld(view, x: x, y: y, width: width, height: height)
        let k = max(minK, min(maxK, view.k * factor))
        return EmberView(cx: anchor.x - (x - width / 2) / k, cy: anchor.y - (y - height / 2) / k, k: k)
    }

    public static func pan(_ view: EmberView, dx: Double, dy: Double) -> EmberView {
        EmberView(cx: view.cx - dx / view.k, cy: view.cy - dy / view.k, k: view.k)
    }

    public static func focusFromView(_ layout: EmberLayout, _ view: EmberView, width: Double, height: Double) -> Int {
        guard !layout.isEmpty else { return 0 }
        let need = 0.42 * min(width, height)
        let center = toWorld(view, x: width / 2, y: height / 2, width: width, height: height)
        var focus = 0
        for node in layout.chain(layout.nodeAt(x: center.x, y: center.y)) where layout.isFolder(node) && layout.nodes[node].r * view.k >= need {
            focus = node
        }
        return focus
    }

    public static func zoomOutTarget(_ layout: EmberLayout, focus: Int) -> Int {
        guard focus >= 0 && focus < layout.nodes.count else { return 0 }
        let parent = layout.nodes[focus].parent
        return parent >= 0 ? parent : 0
    }

    public static func focusTarget(_ layout: EmberLayout, _ node: Int) -> Int {
        guard node >= 0 && node < layout.nodes.count else { return 0 }
        return layout.isFolder(node) ? node : max(0, layout.nodes[node].parent)
    }

    public static func tapAt(_ layout: EmberLayout, view: EmberView, focus: Int, selected: Int, x: Double, y: Double,
                             width: Double, height: Double, fileRadius: Double = 2.5, folderRadius: Double = 14) -> EmberTap {
        guard !layout.isEmpty else { return .none }
        let world = toWorld(view, x: x, y: y, width: width, height: height)
        let deepest = layout.nodeAt(x: world.x, y: world.y)
        if deepest < 0 {
            if focus != 0 { return .zoomOut }
            return selected >= 0 ? .clearSelection : .none
        }
        if !layout.isFolder(deepest) && layout.nodes[deepest].r * view.k >= fileRadius {
            return .selectFile(layout.nodes[deepest].file)
        }
        var target = -1
        for node in layout.chain(deepest) where node != focus && layout.isFolder(node) && layout.isAncestor(focus, node)
            && layout.nodes[node].r * view.k >= folderRadius {
            target = node
        }
        if target >= 0 { return .focus(target) }
        if !layout.isAncestor(focus, deepest) && focus != 0 { return .zoomOut }
        return selected >= 0 ? .clearSelection : .none
    }

    public static func revealTarget(_ layout: EmberLayout, view: EmberView, file: Int, width: Double, height: Double,
                                    inset: Double = 40, minRadius: Double = 20) -> Int? {
        let node = layout.fileNode(file)
        guard node >= 0 else { return nil }
        let n = layout.nodes[node]
        let r = n.r * view.k
        let p = toScreen(view, x: n.x, y: n.y, width: width, height: height)
        let visible = r >= minRadius && p.x >= inset && p.x <= width - inset && p.y >= inset && p.y <= height - inset
        return visible ? nil : readableFocus(layout, node, width: width, height: height)
    }

    public static func visibleNodes(_ layout: EmberLayout, view: EmberView, width: Double, height: Double, minRadius: Double = 0.6) -> [Int] {
        guard !layout.isEmpty else { return [] }
        var out: [Int] = []
        var stack = [0]
        let diagonal = hypot(width, height)
        while let index = stack.popLast() {
            let node = layout.nodes[index]
            let c = toScreen(view, x: node.x, y: node.y, width: width, height: height)
            let r = node.r * view.k
            if r < minRadius { continue }
            if c.x + r < -2 || c.x - r > width + 2 || c.y + r < -2 || c.y - r > height + 2 { continue }
            if node.file < 0 && r > 4 * diagonal && hypot(c.x - width / 2, c.y - height / 2) + diagonal < r {
                stack.append(contentsOf: node.children.reversed())
                continue
            }
            out.append(index)
            if node.file < 0 { stack.append(contentsOf: node.children.reversed()) }
        }
        return out
    }

    public static func crumbs(_ layout: EmberLayout, focus: Int, selectedFile: Int) -> [EmberCrumb] {
        guard !layout.isEmpty else { return [] }
        let chain = layout.chain(focus)
        var parts: [EmberCrumb] = []
        if chain.count > 6 {
            parts.append(EmberCrumb(name: layout.nodes[chain[0]].name, node: chain[0]))
            parts.append(EmberCrumb(name: "…", node: -1))
            for i in (chain.count - 4)..<chain.count { parts.append(EmberCrumb(name: layout.nodes[chain[i]].name, node: chain[i])) }
        } else {
            for node in chain { parts.append(EmberCrumb(name: layout.nodes[node].name, node: node)) }
        }
        let node = layout.fileNode(selectedFile)
        if node >= 0 {
            let fileChain = layout.chain(node)
            if let last = chain.last, let at = fileChain.firstIndex(of: last) {
                var i = at + 1
                while i + 1 < fileChain.count {
                    parts.append(EmberCrumb(name: layout.nodes[fileChain[i]].name, node: fileChain[i]))
                    i += 1
                }
            } else {
                parts.append(EmberCrumb(name: "…", node: -1))
            }
            parts.append(EmberCrumb(name: layout.nodes[node].name, node: node, current: true))
        }
        return parts
    }
}
