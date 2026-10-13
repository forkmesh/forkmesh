import FMBase
import FMControl
import Foundation
import Observation

public enum BuildMacMethod {
    public static let status = "diag.buildMac"
}

public enum BuildMacState: String, Equatable, Sendable {
    case idle, building, offline
}

public struct BuildMacLight: Equatable, Sendable {
    public var state: BuildMacState
    public var job: String
    public var sinceMs: Int64

    public init(state: BuildMacState, job: String = "", sinceMs: Int64 = 0) {
        self.state = state
        self.job = job
        self.sinceMs = sinceMs
    }

    public init?(json: JSONObject) {
        guard json.bool("configured"), let state = BuildMacState(rawValue: json.str("state")) else { return nil }
        self.init(state: state, job: state == .building ? json.str("job") : "", sinceMs: json.int64("sinceMs"))
    }

    public var jobLabel: String {
        let words: [Substring] = job.split(separator: " ", maxSplits: 1)
        let version: String = words.count > 1 ? " \(words[1])" : ""
        switch words.first ?? "" {
        case "desktop-release": return "the macOS release\(version)"
        case "ios-release": return "the iOS release\(version)"
        default: return job.hasPrefix("ci/") ? "an iOS ad hoc build" : job.isEmpty ? "a job" : job
        }
    }

    public var healthy: Bool { state != .offline }

    public var summary: String {
        switch state {
        case .idle: return "idle"
        case .building: return "building \(jobLabel)"
        case .offline: return "not reachable from the desktop"
        }
    }

    public var label: String { "Office MacBook, \(summary)" }
}

@MainActor
@Observable
public final class BuildMacLightModel {
    nonisolated public static let idleFloor: TimeInterval = 15 * 60
    nonisolated public static let busyFloor: TimeInterval = 2 * 60

    public private(set) var light: BuildMacLight?

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
            let floor: TimeInterval = light?.state == .idle ? BuildMacLightModel.idleFloor : BuildMacLightModel.busyFloor
            if now().timeIntervalSince(last) < floor { return }
        }
        await load()
    }

    public func load() async {
        if loading || !linkOpen() { return }
        loading = true
        defer { loading = false }
        do {
            let reply: JSONObject = try await channel.request(BuildMacMethod.status, [:], timeout: 20)
            light = BuildMacLight(json: reply)
            lastLoadedAt = now()
        } catch {
            if DesktopControlFamily.isUnsupported(error) {
                light = nil
                lastLoadedAt = now()
            }
        }
    }
}
