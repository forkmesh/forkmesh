import Foundation

public final class AgentTokenTank {
    public static let leakSeconds = 12.0
    public static let pourSeconds = 1.0
    public static let stepsPerSecond = 60.0
    public static let framesPerSecond = 30.0
    public static let maxAdvanceSeconds = 0.1
    public static let gravity = 180.0
    public static let flatBias = 0.25
    public static let maxPull = 3.0
    public static let rainSpeed = 40.0
    public static let radius = 2.0
    public static let restDensity = 1.2
    public static let stiffness = 0.3
    public static let nearStiffness = 0.5
    public static let damping = 0.6
    public static let shadeCount = 4
    public static let neighborLimit = 48

    public static func scale(_ values: [Double], floor: Double) -> Double {
        max(floor, values.max() ?? 0)
    }

    public let width: Double
    public let height: Double
    public let capacity: Int
    private var fluid: AgentTokenFluid

    public init(width: Double, height: Double, seed: UInt64 = 1, leakSeconds: Double = AgentTokenTank.leakSeconds) {
        self.width = width
        self.height = height
        let capacity = max(0, Int(width.rounded(.down)) * Int(height.rounded(.down)))
        self.capacity = capacity
        fluid = AgentTokenFluid(width: width, height: height, capacity: capacity, seed: seed, leakSeconds: leakSeconds)
    }

    public var xs: [Double] { fluid.xs }
    public var ys: [Double] { fluid.ys }
    public var shades: [Int] { fluid.shades }
    public var grains: Int { fluid.grains }
    public var gravityX: Double { fluid.gravityX }
    public var gravityY: Double { fluid.gravityY }
    public private(set) var stepsTaken = 0

    public var idle: Bool { fluid.grains == 0 && fluid.owed < 1 && fluid.rainCarry < 1 }

    public func pour(tokens: Double, scale: Double) {
        guard capacity > 0, tokens > 0, scale > 0 else { return }
        drop(min(1, tokens / scale) * Double(capacity) / fluid.leakSeconds)
    }

    public func drop(_ count: Double) {
        guard count > 0 else { return }
        fluid.owed += count
        fluid.pourRate = fluid.owed / AgentTokenTank.pourSeconds
    }

    public func tilt(x: Double, y: Double) {
        let scale = AgentTokenTank.gravity / (1 + AgentTokenTank.flatBias)
        var gx = x * scale
        var gy = (y + AgentTokenTank.flatBias) * scale
        let length = (gx * gx + gy * gy).squareRoot()
        let most = AgentTokenTank.gravity * AgentTokenTank.maxPull
        if length > most {
            gx *= most / length
            gy *= most / length
        }
        fluid.gravityX = gx
        fluid.gravityY = gy
    }

    public func advance(_ seconds: Double) {
        guard capacity > 0 else { return }
        stepsTaken += fluid.advance(seconds)
    }
}

struct AgentTokenFluid {
    let width: Double
    let height: Double
    let capacity: Int
    let leakSeconds: Double
    var xs: [Double]
    var ys: [Double]
    var shades: [Int]
    var grains = 0
    var gravityX = 0.0
    var gravityY = AgentTokenTank.gravity
    var owed = 0.0
    var pourRate = 0.0
    var rainCarry = 0.0

    private var vxs: [Double]
    private var vys: [Double]
    private var lastXs: [Double]
    private var lastYs: [Double]
    private let gridColumns: Int
    private let gridRows: Int
    private var cellStart: [Int]
    private var fill: [Int]
    private var cellOf: [Int]
    private var sorted: [Int]
    private var neighbors = [Int](repeating: 0, count: AgentTokenTank.neighborLimit)
    private var nearXs = [Double](repeating: 0, count: AgentTokenTank.neighborLimit)
    private var nearYs = [Double](repeating: 0, count: AgentTokenTank.neighborLimit)
    private var nearQs = [Double](repeating: 0, count: AgentTokenTank.neighborLimit)
    private var random: UInt64
    private var leakCarry = 0.0
    private var stepCarry = 0.0

    init(width: Double, height: Double, capacity: Int, seed: UInt64, leakSeconds: Double) {
        self.width = width
        self.height = height
        self.capacity = capacity
        self.leakSeconds = leakSeconds
        xs = [Double](repeating: 0, count: capacity)
        ys = [Double](repeating: 0, count: capacity)
        shades = [Int](repeating: 0, count: capacity)
        vxs = [Double](repeating: 0, count: capacity)
        vys = [Double](repeating: 0, count: capacity)
        lastXs = [Double](repeating: 0, count: capacity)
        lastYs = [Double](repeating: 0, count: capacity)
        gridColumns = max(1, Int((width / AgentTokenTank.radius).rounded(.up)))
        gridRows = max(1, Int((height / AgentTokenTank.radius).rounded(.up)))
        cellStart = [Int](repeating: 0, count: gridColumns * gridRows + 1)
        fill = cellStart
        cellOf = [Int](repeating: 0, count: capacity)
        sorted = [Int](repeating: 0, count: capacity)
        random = seed == 0 ? 0x9E37_79B9_7F4A_7C15 : seed
    }

    mutating func advance(_ seconds: Double) -> Int {
        stepCarry += min(max(seconds, 0), AgentTokenTank.maxAdvanceSeconds) * AgentTokenTank.stepsPerSecond
        var steps = 0
        while stepCarry >= 1 {
            step(1 / AgentTokenTank.stepsPerSecond)
            stepCarry -= 1
            steps += 1
        }
        return steps
    }

    private mutating func step(_ dt: Double) {
        rain(dt)
        leak(dt)
        guard grains > 0 else { return }
        let damping = 1 - AgentTokenTank.damping * dt
        for i in 0..<grains {
            vxs[i] = (vxs[i] + gravityX * dt) * damping
            vys[i] = (vys[i] + gravityY * dt) * damping
            lastXs[i] = xs[i]
            lastYs[i] = ys[i]
            xs[i] += vxs[i] * dt
            ys[i] += vys[i] * dt
        }
        relax()
        for i in 0..<grains {
            contain(i)
            vxs[i] = (xs[i] - lastXs[i]) / dt
            vys[i] = (ys[i] - lastYs[i]) / dt
        }
    }

    private mutating func rain(_ dt: Double) {
        guard owed > 0 else { return }
        let due = min(owed, pourRate * dt)
        owed -= due
        rainCarry += due
        while rainCarry >= 1 {
            rainCarry -= 1
            spawn()
        }
    }

    private mutating func leak(_ dt: Double) {
        guard grains > 0 else {
            leakCarry = 0
            return
        }
        leakCarry += Double(grains) * dt / leakSeconds
        while leakCarry >= 1, grains > 0 {
            leakCarry -= 1
            remove(lowest())
        }
    }

    private mutating func spawn() {
        guard grains < capacity else { return }
        let i = grains
        let along = next()
        let length = (gravityX * gravityX + gravityY * gravityY).squareRoot()
        let ux = length > 0 ? gravityX / length : 0
        let uy = length > 0 ? gravityY / length : 1
        if abs(uy) >= abs(ux) {
            xs[i] = 0.5 + along * (width - 1)
            ys[i] = uy >= 0 ? 0.5 : height - 0.5
        } else {
            xs[i] = ux >= 0 ? 0.5 : width - 0.5
            ys[i] = 0.5 + along * (height - 1)
        }
        vxs[i] = ux * AgentTokenTank.rainSpeed
        vys[i] = uy * AgentTokenTank.rainSpeed
        shades[i] = 1 + min(Int(next() * Double(AgentTokenTank.shadeCount)), AgentTokenTank.shadeCount - 1)
        grains += 1
    }

    private func lowest() -> Int {
        var best = 0
        var bestDepth = -Double.infinity
        for i in 0..<grains {
            let depth = xs[i] * gravityX + ys[i] * gravityY
            if depth > bestDepth {
                bestDepth = depth
                best = i
            }
        }
        return best
    }

    private mutating func remove(_ i: Int) {
        let last = grains - 1
        xs[i] = xs[last]
        ys[i] = ys[last]
        vxs[i] = vxs[last]
        vys[i] = vys[last]
        shades[i] = shades[last]
        grains = last
    }

    private mutating func contain(_ i: Int) {
        xs[i] = min(max(xs[i], 0.5), width - 0.5)
        ys[i] = min(max(ys[i], 0.5), height - 0.5)
    }

    private func cell(_ x: Double, _ y: Double) -> Int {
        let column = min(max(Int(x / AgentTokenTank.radius), 0), gridColumns - 1)
        let row = min(max(Int(y / AgentTokenTank.radius), 0), gridRows - 1)
        return row * gridColumns + column
    }

    private mutating func relax() {
        let radius = AgentTokenTank.radius
        for c in cellStart.indices { cellStart[c] = 0 }
        for i in 0..<grains {
            let c = cell(xs[i], ys[i])
            cellOf[i] = c
            cellStart[c + 1] += 1
        }
        for c in 0..<(gridColumns * gridRows) { cellStart[c + 1] += cellStart[c] }
        for c in cellStart.indices { fill[c] = cellStart[c] }
        for i in 0..<grains {
            sorted[fill[cellOf[i]]] = i
            fill[cellOf[i]] += 1
        }
        for i in 0..<grains {
            let column = cellOf[i] % gridColumns
            let row = cellOf[i] / gridColumns
            var density = 0.0
            var near = 0.0
            var found = 0
            for r in max(0, row - 1)...min(gridRows - 1, row + 1) {
                for c in max(0, column - 1)...min(gridColumns - 1, column + 1) {
                    let cellIndex = r * gridColumns + c
                    for k in cellStart[cellIndex]..<cellStart[cellIndex + 1] {
                        let j = sorted[k]
                        if j == i || found == AgentTokenTank.neighborLimit { continue }
                        var dx = xs[j] - xs[i]
                        var dy = ys[j] - ys[i]
                        let square = dx * dx + dy * dy
                        if square >= radius * radius { continue }
                        var distance = square.squareRoot()
                        if distance < 1e-6 {
                            let angle = next() * 2 * Double.pi
                            dx = cos(angle) * 1e-3
                            dy = sin(angle) * 1e-3
                            distance = 1e-3
                        }
                        let q = 1 - distance / radius
                        density += q * q
                        near += q * q * q
                        neighbors[found] = j
                        nearXs[found] = dx / distance
                        nearYs[found] = dy / distance
                        nearQs[found] = q
                        found += 1
                    }
                }
            }
            let pressure = AgentTokenTank.stiffness * (density - AgentTokenTank.restDensity)
            let nearPressure = AgentTokenTank.nearStiffness * near
            var moveX = 0.0
            var moveY = 0.0
            for n in 0..<found {
                let q = nearQs[n]
                let push = (pressure * q + nearPressure * q * q) / 2
                let px = nearXs[n] * push
                let py = nearYs[n] * push
                xs[neighbors[n]] += px
                ys[neighbors[n]] += py
                moveX -= px
                moveY -= py
            }
            xs[i] += moveX
            ys[i] += moveY
        }
    }

    private mutating func next() -> Double {
        var x = random
        x ^= x >> 12
        x ^= x << 25
        x ^= x >> 27
        random = x
        return Double((x &* 2_685_821_657_736_338_717) >> 11) / 9_007_199_254_740_992
    }
}

public final class AgentTokenTanks {
    public static let frameSeconds = 1 / AgentTokenTank.framesPerSecond

    public let input: AgentTokenTank
    public let output: AgentTokenTank
    private var last: Date?

    public init(width: Double, height: Double) {
        input = AgentTokenTank(width: width, height: height, seed: 1)
        output = AgentTokenTank(width: width, height: height, seed: 2)
    }

    public var idle: Bool { input.idle && output.idle }
    public var wantsMotion: Bool { !idle }

    public func advance(to date: Date) {
        if let last {
            let seconds = date.timeIntervalSince(last)
            input.advance(seconds)
            output.advance(seconds)
        }
        last = date
    }

    public func tilt(x: Double, y: Double) {
        input.tilt(x: x, y: y)
        output.tilt(x: x, y: y)
    }
}
