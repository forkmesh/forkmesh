import Foundation

public struct DjPace: Equatable, Sendable {
    public struct Range: Equatable, Sendable {
        public var minimum: Float
        public var maximum: Float
        public var preferred: Float

        public init(minimum: Float, maximum: Float, preferred: Float) {
            self.minimum = minimum
            self.maximum = maximum
            self.preferred = preferred
        }
    }

    public static let full = 60
    public static let eased = 30
    public static let people = 24
    public static let idle = Range(minimum: 8, maximum: 15, preferred: 15)

    public var hot: Bool
    public var lowPower: Bool
    public var displayFps: Int

    public init(hot: Bool = false, lowPower: Bool = false, displayFps: Int = 60) {
        self.hot = hot
        self.lowPower = lowPower
        self.displayFps = displayFps
    }

    public var strained: Bool { hot || lowPower }

    var screen: Int { max(Self.eased, displayFps) }

    func capped(_ fps: Int) -> Int { min(fps, screen) }

    public var motionFps: Int { capped(strained ? Self.eased : Self.full) }

    public var touchFps: Int { strained ? capped(Self.full) : screen }

    public var peopleFps: Int { strained ? Self.people / 2 : Self.people }

    public var steady: Range {
        let fps = Float(motionFps)
        return Range(minimum: fps / 2, maximum: fps, preferred: fps)
    }

    public var touched: Range {
        let fps = Float(touchFps)
        return Range(minimum: min(fps, Float(Self.full)), maximum: fps, preferred: fps)
    }

    public var motionInterval: Double { Self.interval(motionFps) }
    public var peopleInterval: Double { Self.interval(peopleFps) }

    public static func interval(_ fps: Int) -> Double { 0.9 / Double(max(1, fps)) }

    public var label: String {
        let why = hot && lowPower ? " (eased: hot, Low Power Mode)" : hot ? " (eased: hot)" : lowPower ? " (eased: Low Power Mode)" : ""
        return "steady record and waveform \(motionFps) Hz, touched record \(touchFps) Hz, people \(peopleFps) Hz\(why)"
    }
}
