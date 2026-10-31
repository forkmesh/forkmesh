import Foundation

public struct CallFaceQualityLevel: Equatable, Sendable {
    public let segment: Int
    public let hair: Int
    public let guide: Int
    public let fastSegmentation: Bool

    public func runsSegment(frame: Int) -> Bool { frame % max(1, segment) == 0 }
    public func runsHair(frame: Int) -> Bool { frame % max(1, hair) == 0 }
}

public enum CallFaceThermal: Int, Sendable, Comparable {
    case nominal = 0, fair, serious, critical

    public static func < (a: CallFaceThermal, b: CallFaceThermal) -> Bool { a.rawValue < b.rawValue }
}

public enum CallFaceQuality {
    public static let levels: [CallFaceQualityLevel] = [
        CallFaceQualityLevel(segment: 1, hair: 1, guide: 320, fastSegmentation: false),
        CallFaceQualityLevel(segment: 1, hair: 2, guide: 320, fastSegmentation: false),
        CallFaceQualityLevel(segment: 2, hair: 3, guide: 256, fastSegmentation: true),
        CallFaceQualityLevel(segment: 3, hair: 4, guide: 192, fastSegmentation: true),
    ]
    public static let slowMs = 28.0
    public static let fastMs = 16.0
    public static let hold = 24
    public static let costShare = 0.1

    public static func startLevel(appleFamily: Int) -> Int {
        switch appleFamily {
        case 7...: return 0
        case 6: return 1
        case 5: return 2
        default: return levels.count - 1
        }
    }

    public static func floor(_ thermal: CallFaceThermal) -> Int {
        switch thermal {
        case .nominal, .fair: return 0
        case .serious: return 2
        case .critical: return levels.count - 1
        }
    }

    public static func capture(_ thermal: CallFaceThermal, fps: Int) -> (fps: Int, scale: Double) {
        switch thermal {
        case .nominal, .fair: return (fps, 1)
        case .serious: return (min(fps, 24), 0.75)
        case .critical: return (min(fps, 15), 0.5)
        }
    }
}

public final class CallFaceQualityGovernor {
    public private(set) var adaptive: Int
    public private(set) var cost = 0.0
    public private(set) var thermal = CallFaceThermal.nominal
    private var steady = 0

    public init(start: Int = 0) {
        adaptive = min(max(start, 0), CallFaceQuality.levels.count - 1)
    }

    public var index: Int { max(adaptive, CallFaceQuality.floor(thermal)) }
    public var level: CallFaceQualityLevel { CallFaceQuality.levels[index] }

    @discardableResult
    public func note(costMs: Double, thermal next: CallFaceThermal) -> Int {
        thermal = next
        cost = cost > 0 ? cost * (1 - CallFaceQuality.costShare) + costMs * CallFaceQuality.costShare : costMs
        steady += 1
        guard steady >= CallFaceQuality.hold else { return index }
        let last = CallFaceQuality.levels.count - 1
        let step = cost > CallFaceQuality.slowMs ? min(adaptive + 1, last) : cost < CallFaceQuality.fastMs ? max(adaptive - 1, 0) : adaptive
        if step != adaptive {
            adaptive = step
            steady = 0
        }
        return index
    }
}
