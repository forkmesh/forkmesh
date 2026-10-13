import FMBase
import FMControl
import Foundation
import Observation

public enum DesktopStrain: String, Equatable, Sendable {
    case calm, busy, strained

    public init(level: String) {
        switch level {
        case "strained": self = .strained
        case "busy": self = .busy
        default: self = .calm
        }
    }

    public var label: String {
        switch self {
        case .calm: return "running smoothly"
        case .busy: return "busy"
        case .strained: return "under pressure"
        }
    }
}

@MainActor
@Observable
public final class DesktopLoadLightModel {
    nonisolated public static let calmFloor: TimeInterval = 15 * 60
    nonisolated public static let busyFloor: TimeInterval = 2 * 60

    public private(set) var load: DesktopStrain?

    @ObservationIgnored private let channel: DesktopControlCalling
    @ObservationIgnored private let linkOpen: @MainActor () -> Bool
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored private var lastLoadedAt: Date?
    @ObservationIgnored private var loading: Bool = false

    public init(
        channel: DesktopControlCalling, linkOpen: @escaping @MainActor () -> Bool,
        now: @escaping () -> Date = Date.init
    ) {
        self.channel = channel
        self.linkOpen = linkOpen
        self.now = now
    }

    public func refresh() async {
        if let last = lastLoadedAt {
            let floor: TimeInterval = load == .calm ? DesktopLoadLightModel.calmFloor : DesktopLoadLightModel.busyFloor
            if now().timeIntervalSince(last) < floor { return }
        }
        await read()
    }

    public func read() async {
        if loading || !linkOpen() { return }
        loading = true
        defer { loading = false }
        do {
            let reply: JSONObject = try await channel.request(DesktopStatsMethod.health, [:], timeout: 15)
            load = DesktopStrain(level: reply.str("level"))
            lastLoadedAt = now()
        } catch {
            if DesktopControlFamily.isUnsupported(error) {
                load = nil
                lastLoadedAt = now()
            }
        }
    }
}
