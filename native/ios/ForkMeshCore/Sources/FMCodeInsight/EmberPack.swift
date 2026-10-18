import Foundation

public struct EmberCircle: Sendable, Equatable {
    public var x: Double
    public var y: Double
    public var r: Double

    public init(x: Double = 0, y: Double = 0, r: Double = 0) {
        self.x = x
        self.y = y
        self.r = r
    }
}

public enum Ember {}

extension Ember {
    static func place(_ b: EmberCircle, _ a: EmberCircle, _ c: inout EmberCircle) {
        let dx = b.x - a.x
        let dy = b.y - a.y
        let d2 = dx * dx + dy * dy
        if d2 > 0 {
            var a2 = a.r + c.r
            a2 *= a2
            var b2 = b.r + c.r
            b2 *= b2
            if a2 > b2 {
                let x = (d2 + b2 - a2) / (2 * d2)
                let y = (max(0.0, b2 / d2 - x * x)).squareRoot()
                c.x = b.x - x * dx - y * dy
                c.y = b.y - x * dy + y * dx
            } else {
                let x = (d2 + a2 - b2) / (2 * d2)
                let y = (max(0.0, a2 / d2 - x * x)).squareRoot()
                c.x = a.x + x * dx - y * dy
                c.y = a.y + x * dy + y * dx
            }
        } else {
            c.x = a.x + c.r
            c.y = a.y
        }
    }

    static func intersects(_ a: EmberCircle, _ b: EmberCircle) -> Bool {
        let dr = a.r + b.r - 1e-6
        let dx = b.x - a.x
        let dy = b.y - a.y
        return dr > 0 && dr * dr > dx * dx + dy * dy
    }

    static func enclosesNot(_ a: EmberCircle, _ b: EmberCircle) -> Bool {
        let dr = a.r - b.r
        let dx = b.x - a.x
        let dy = b.y - a.y
        return dr < 0 || dr * dr < dx * dx + dy * dy
    }

    static func enclosesWeak(_ a: EmberCircle, _ b: EmberCircle) -> Bool {
        let dr = a.r - b.r + max(a.r, b.r, 1.0) * 1e-9
        let dx = b.x - a.x
        let dy = b.y - a.y
        return dr > 0 && dr * dr > dx * dx + dy * dy
    }

    static func enclosesWeakAll(_ a: EmberCircle, _ basis: [EmberCircle]) -> Bool {
        for b in basis where !enclosesWeak(a, b) { return false }
        return true
    }

    static func encloseBasis2(_ a: EmberCircle, _ b: EmberCircle) -> EmberCircle {
        let x21 = b.x - a.x
        let y21 = b.y - a.y
        let r21 = b.r - a.r
        let l = (x21 * x21 + y21 * y21).squareRoot()
        if l <= 0 { return a.r >= b.r ? a : b }
        return EmberCircle(x: (a.x + b.x + x21 / l * r21) / 2, y: (a.y + b.y + y21 / l * r21) / 2, r: (l + a.r + b.r) / 2)
    }

    static func encloseBasis3(_ a: EmberCircle, _ b: EmberCircle, _ c: EmberCircle) -> EmberCircle {
        let x1 = a.x, y1 = a.y, r1 = a.r
        let x2 = b.x, y2 = b.y, r2 = b.r
        let x3 = c.x, y3 = c.y, r3 = c.r
        let a2 = x1 - x2
        let a3 = x1 - x3
        let b2 = y1 - y2
        let b3 = y1 - y3
        let c2 = r2 - r1
        let c3 = r3 - r1
        let d1 = x1 * x1 + y1 * y1 - r1 * r1
        let d2 = d1 - x2 * x2 - y2 * y2 + r2 * r2
        let d3 = d1 - x3 * x3 - y3 * y3 + r3 * r3
        let ab = a3 * b2 - a2 * b3
        let xa = (b2 * d3 - b3 * d2) / (ab * 2) - x1
        let xb = (b3 * c2 - b2 * c3) / ab
        let ya = (a3 * d2 - a2 * d3) / (ab * 2) - y1
        let yb = (a2 * c3 - a3 * c2) / ab
        let qa = xb * xb + yb * yb - 1
        let qb = 2 * (r1 + xa * xb + ya * yb)
        let qc = xa * xa + ya * ya - r1 * r1
        let r = -(abs(qa) > 1e-6 ? (qb + max(0.0, qb * qb - 4 * qa * qc).squareRoot()) / (2 * qa) : qc / qb)
        return EmberCircle(x: x1 + xa + xb * r, y: y1 + ya + yb * r, r: r)
    }

    static func encloseBasis(_ basis: [EmberCircle]) -> EmberCircle {
        if basis.count == 1 { return basis[0] }
        if basis.count == 2 { return encloseBasis2(basis[0], basis[1]) }
        return encloseBasis3(basis[0], basis[1], basis[2])
    }

    static func extendBasis(_ basis: [EmberCircle], _ p: EmberCircle) -> [EmberCircle]? {
        if enclosesWeakAll(p, basis) { return [p] }
        for b in basis where enclosesNot(p, b) && enclosesWeakAll(encloseBasis2(b, p), basis) {
            return [b, p]
        }
        if basis.count >= 2 {
            for i in 0..<(basis.count - 1) {
                for j in (i + 1)..<basis.count {
                    if enclosesNot(encloseBasis2(basis[i], basis[j]), p)
                        && enclosesNot(encloseBasis2(basis[i], p), basis[j])
                        && enclosesNot(encloseBasis2(basis[j], p), basis[i])
                        && enclosesWeakAll(encloseBasis3(basis[i], basis[j], p), basis) {
                        return [basis[i], basis[j], p]
                    }
                }
            }
        }
        return nil
    }

    static func boundingFallback(_ circles: [EmberCircle]) -> EmberCircle {
        var cx = 0.0
        var cy = 0.0
        for c in circles {
            cx += c.x
            cy += c.y
        }
        cx /= Double(circles.count)
        cy /= Double(circles.count)
        var r = 0.0
        for c in circles { r = max(r, hypot(c.x - cx, c.y - cy) + c.r) }
        return EmberCircle(x: cx, y: cy, r: r)
    }

    static func chainScore(_ circles: [EmberCircle], _ next: [Int], _ node: Int) -> Double {
        let a = circles[node]
        let b = circles[next[node]]
        let ab = a.r + b.r
        let dx = (a.x * b.r + b.x * a.r) / ab
        let dy = (a.y * b.r + b.y * a.r) / ab
        return dx * dx + dy * dy
    }

    @discardableResult
    public static func packSiblings(_ circles: inout [EmberCircle]) -> Double {
        let n = circles.count
        if n == 0 { return 0 }
        circles[0].x = 0
        circles[0].y = 0
        if n == 1 { return circles[0].r }
        circles[0].x = -circles[1].r
        circles[1].x = circles[0].r
        circles[1].y = 0
        if n == 2 {
            let e = encloseBasis2(circles[0], circles[1])
            for i in circles.indices {
                circles[i].x -= e.x
                circles[i].y -= e.y
            }
            return e.r
        }
        place(circles[1], circles[0], &circles[2])
        var next = [Int](repeating: -1, count: n)
        var previous = [Int](repeating: -1, count: n)
        var a = 0
        var b = 1
        var c = 2
        next[a] = b
        previous[c] = b
        next[b] = c
        previous[a] = c
        next[c] = a
        previous[b] = a

        var i = 3
        while i < n {
            place(circles[a], circles[b], &circles[i])
            c = i
            var j = next[b]
            var k = previous[a]
            var sj = circles[b].r
            var sk = circles[a].r
            var retry = false
            repeat {
                if sj <= sk {
                    if intersects(circles[j], circles[c]) {
                        b = j
                        next[a] = b
                        previous[b] = a
                        retry = true
                        break
                    }
                    sj += circles[j].r
                    j = next[j]
                } else {
                    if intersects(circles[k], circles[c]) {
                        a = k
                        next[a] = b
                        previous[b] = a
                        retry = true
                        break
                    }
                    sk += circles[k].r
                    k = previous[k]
                }
            } while j != next[k]
            if retry { continue }
            previous[c] = a
            next[c] = b
            next[a] = c
            previous[b] = c
            b = c
            var aa = chainScore(circles, next, a)
            c = next[c]
            while c != b {
                let ca = chainScore(circles, next, c)
                if ca < aa {
                    a = c
                    aa = ca
                }
                c = next[c]
            }
            b = next[a]
            i += 1
        }

        var front = [circles[b]]
        var it = next[b]
        while it != b {
            front.append(circles[it])
            it = next[it]
        }
        let e = encloseCircles(front)
        for index in circles.indices {
            circles[index].x -= e.x
            circles[index].y -= e.y
        }
        return e.r
    }

    public static func encloseCircles(_ input: [EmberCircle]) -> EmberCircle {
        if input.isEmpty { return EmberCircle() }
        var circles = input
        var state: UInt64 = 1
        func random() -> Double {
            state = (1664525 &* state &+ 1013904223) % 4294967296
            return Double(state) / 4294967296.0
        }
        var m = circles.count
        while m > 0 {
            let i = Int(random() * Double(m))
            m -= 1
            circles.swapAt(m, i)
        }
        var basis: [EmberCircle] = []
        var e = EmberCircle()
        var haveE = false
        var guardCount = 0
        let limit = max(1000, circles.count * 64)
        var i = 0
        while i < circles.count {
            let p = circles[i]
            if haveE && enclosesWeak(e, p) {
                i += 1
                continue
            }
            guardCount += 1
            guard let extended = extendBasis(basis, p), guardCount <= limit else { return boundingFallback(input) }
            basis = extended
            e = encloseBasis(basis)
            haveE = true
            i = 0
        }
        return e
    }
}
