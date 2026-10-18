import Foundation

public enum RoomSpiderAct: String, Sendable, CaseIterable {
    case hello, giggle, dizzy, nap, dance, peek, knock, weave, wave, sniff, cheer, swing, hang, climb

    public var seconds: Double {
        switch self {
        case .hello: return 1.6
        case .giggle: return 1.2
        case .dizzy: return 2.4
        case .nap: return 5
        case .dance: return 3
        case .peek: return 3
        case .knock: return 2.4
        case .weave: return 3.5
        case .wave: return 2
        case .sniff: return 2
        case .cheer: return 1.6
        case .swing: return 2.2
        case .hang: return 2.4
        case .climb: return 1.4
        }
    }

    public var line: String {
        switch self {
        case .hello: return "Hello, room!"
        case .giggle: return "Hee hee, that tickles!"
        case .dizzy: return "Whoa… the room is spinning."
        case .nap: return "Comfy. Five more minutes…"
        case .dance: return "Table top dance!"
        case .peek: return "Ooh, what's out there?"
        case .knock: return "Knock knock!"
        case .weave: return "A web would look nice here."
        case .wave: return "Hi there!"
        case .sniff: return "Hmm, what's this?"
        case .cheer: return "Again! Again!"
        case .swing: return "Wheee!"
        case .hang: return "Everything looks better upside down."
        case .climb: return "Wall climbing!"
        }
    }

    public var still: Bool { self != .dance }

    public static func arriving(on kind: RoomSurfaceKind, upright: Bool) -> RoomSpiderAct? {
        switch kind {
        case .seat: return .nap
        case .table: return .dance
        case .window: return .peek
        case .door: return .knock
        case .ceiling: return .hang
        case .wall: return .climb
        case .other: return upright ? .sniff : nil
        case .floor: return nil
        }
    }
}

public struct RoomWeb: Sendable, Equatable, Identifiable {
    public let id: Int
    public var center: RoomPoint
    public var normal: RoomPoint
    public var across: RoomPoint
    public var size: Float
    public var surface: String
}

public struct RoomDice: Sendable {
    private var state: UInt64

    public init(seed: UInt64) { state = seed }

    public mutating func next() -> Float {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z: UInt64 = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        z ^= z >> 31
        return Float(z >> 40) / Float(1 << 24)
    }

    public mutating func between(_ low: Float, _ high: Float) -> Float { low + (high - low) * next() }
}
