import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieClearance {
    public nonisolated static let body: CGFloat = 0.36
    public nonisolated static let gap: CGFloat = 4
    public nonisolated static let step: CGFloat = 6
    public nonisolated static let touching: Double = 0.04
    public nonisolated static let better: Double = 0.5

    public static func footprint(_ point: CGPoint, side: CGFloat) -> CGRect {
        let half: CGFloat = side * body + gap
        return CGRect(x: point.x - half, y: point.y - half, width: half * 2, height: half * 2)
    }

    public static func covered(_ point: CGPoint, side: CGFloat, buttons: [CGRect]) -> Double {
        let box: CGRect = footprint(point, side: side)
        let area: Double = Double(box.width * box.height)
        guard area > 0 else { return 0 }
        let total: Double = buttons.reduce(0) { (sum: Double, button: CGRect) -> Double in
            let overlap: CGRect = button.intersection(box)
            return overlap.isNull || overlap.isEmpty ? sum : sum + Double(overlap.width * overlap.height)
        }
        return min(total / area, 1)
    }

    public static func covers(_ point: CGPoint, side: CGFloat, buttons: [CGRect]) -> Bool {
        covered(point, side: side, buttons: buttons) > touching
    }

    public static func spots(_ arena: GenieArena) -> [(CGPoint, GenieEdge)] {
        let box: CGRect = arena.box
        var out: [(CGPoint, GenieEdge)] = []
        for edge in GenieEdge.allCases {
            let across: Bool = edge == .top || edge == .bottom
            let length: CGFloat = across ? box.width : box.height
            let count: Int = max(Int((length / step).rounded(.up)), 1)
            for index in 0...count {
                let along: CGFloat = length * CGFloat(index) / CGFloat(count)
                let near: CGPoint = across ? CGPoint(x: box.minX + along, y: box.midY) : CGPoint(x: box.midX, y: box.minY + along)
                out.append((arena.on(edge, near: near), edge))
            }
        }
        return out
    }

    public static func place(
        _ point: CGPoint, on surface: GenieEdge, arena: GenieArena, side: CGFloat, buttons: [CGRect]
    ) -> (CGPoint, GenieEdge) {
        let here: Double = covered(point, side: side, buttons: buttons)
        guard here > touching else { return (point, surface) }
        let ring: Double = arena.perimeter
        let from: Double = arena.distance(point, on: surface)
        let found: (CGPoint, GenieEdge)? = pick(spots(arena), here: here, side: side, buttons: buttons) {
            (spot: CGPoint, edge: GenieEdge) -> Double in
            let apart: Double = abs(arena.distance(spot, on: edge) - from)
            return min(apart, ring - apart)
        }
        return found ?? (point, surface)
    }

    public static func tucked(
        _ edge: GenieEdge, near point: CGPoint, full: CGRect, safe: CGRect, side: CGFloat, buttons: [CGRect]
    ) -> CGPoint {
        let start: CGPoint = GenieCrawl.parked(edge, near: point, full: full, safe: safe, side: side)
        let here: Double = covered(start, side: side, buttons: buttons)
        guard here > touching else { return start }
        let across: Bool = edge == .top || edge == .bottom
        let length: CGFloat = across ? full.width : full.height
        let count: Int = max(Int((length / step).rounded(.up)), 1)
        let options: [(CGPoint, GenieEdge)] = (0...count).map { (index: Int) -> (CGPoint, GenieEdge) in
            let along: CGFloat = length * CGFloat(index) / CGFloat(count)
            let near: CGPoint = across ? CGPoint(x: full.minX + along, y: start.y) : CGPoint(x: start.x, y: full.minY + along)
            return (GenieCrawl.parked(edge, near: near, full: full, safe: safe, side: side), edge)
        }
        let found: (CGPoint, GenieEdge)? = pick(options, here: here, side: side, buttons: buttons) {
            (spot: CGPoint, _: GenieEdge) -> Double in
            Double(hypot(spot.x - start.x, spot.y - start.y))
        }
        return found?.0 ?? start
    }

    static func pick(
        _ options: [(CGPoint, GenieEdge)], here: Double, side: CGFloat, buttons: [CGRect],
        distance: (CGPoint, GenieEdge) -> Double
    ) -> (CGPoint, GenieEdge)? {
        let scored: [(CGPoint, GenieEdge, Double)] = options.map { ($0.0, $0.1, covered($0.0, side: side, buttons: buttons)) }
        guard let least = scored.map({ $0.2 }).min(), least <= here * better else { return nil }
        let enough: Double = max(least + 0.02, touching)
        return scored.filter { $0.2 <= enough }
            .min { distance($0.0, $0.1) < distance($1.0, $1.1) }
            .map { ($0.0, $0.1) }
    }
}
