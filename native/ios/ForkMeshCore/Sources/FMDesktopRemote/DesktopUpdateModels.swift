import FMBase
import FMChat
import FMControl
import Foundation
import Observation

public struct DesktopUpdateState: Equatable, Sendable {
    public var run = 0
    public var running = false
    public var kind = ""
    public var percent = 0
    public var phase = ""
    public var activity = ""
    public var status = ""
    public var error = ""
    public var relaunching = false

    public init() {}

    public init(json: JSONObject) {
        run = json.int("run")
        running = json.bool("running")
        kind = json.str("kind")
        percent = min(100, max(0, json.int("percent")))
        phase = json.str("phase")
        activity = json.str("activity")
        status = json.str("status")
        error = json.str("error")
        relaunching = json.bool("relaunching")
    }

    public var headline: String {
        if !activity.isEmpty { return activity }
        if !phase.isEmpty { return "\(percent)% · \(phase)" }
        return status
    }
}

public enum DesktopUpdateStage: Equatable, Sendable {
    case idle
    case running
    case restarting
    case back
    case failed
}

@MainActor
@Observable
public final class DesktopUpdateStore {
    public static let action = "updateRestart"
    public static let computerRestartAction = "computerRestart"
    public static let maxLines = 400
    public static let restartWaitMs = 20 * 60 * 1000
    public static let restartPollMs = 3000

    @ObservationIgnored public let family: DesktopControlFamily
    @ObservationIgnored private let channel: DesktopControlCalling
    @ObservationIgnored private let now: () -> Int
    @ObservationIgnored private let sleep: (Int) async -> Void
    @ObservationIgnored private var familyTap: UUID?
    @ObservationIgnored private var closeTap: UUID?
    @ObservationIgnored private var startedAtMs: Int64 = 0
    @ObservationIgnored private var restartFrom: Int64 = 0
    @ObservationIgnored private var tracking = false
    @ObservationIgnored private var waiting = false

    public private(set) var state = DesktopUpdateState()
    public private(set) var lines: [String] = []
    public private(set) var stage: DesktopUpdateStage = .idle
    public private(set) var version = ""
    public private(set) var previousVersion = ""
    public private(set) var error = ""
    public private(set) var unsupported = false
    public private(set) var loaded = false
    public private(set) var revision = 0

    public init(
        channel: DesktopControlCalling,
        now: @escaping () -> Int = { Int(Date().timeIntervalSince1970 * 1000) },
        sleep: @escaping (Int) async -> Void = { ms in try? await Task.sleep(nanoseconds: UInt64(ms) * 1_000_000) }
    ) {
        family = DesktopControlFamily(.update, channel: channel)
        self.channel = channel
        self.now = now
        self.sleep = sleep
        familyTap = family.addEventTap { [weak self] suffix, data in
            if suffix == "progress" { self?.onProgress(data) }
        }
        closeTap = channel.addEventTap { [weak self] event, _ in
            if event == "__closed" { self?.onConnectionLost() }
        }
    }

    public var busy: Bool { stage == .running || stage == .restarting }

    public func refresh() async {
        do {
            apply(status: try await family.call("status"))
        } catch {
            if tracking {
                onConnectionLost()
                return
            }
            unsupported = DesktopControlFamily.isUnsupported(error)
            self.error = RemoteErrorText.describe(error)
        }
    }

    public func prepare() async throws -> RemoteConfirmation {
        try await family.prepare(["action": .string(Self.action)])
    }

    public func prepareComputerRestart() async throws -> RemoteConfirmation {
        try await family.prepare(["action": .string(Self.computerRestartAction)])
    }

    @discardableResult
    public func run(_ confirmation: RemoteConfirmation) async throws -> String {
        let started = try await family.run(confirmation)
        lines = []
        state = DesktopUpdateState()
        state.running = true
        state.kind = "update"
        error = ""
        beginTracking()
        stage = .running
        revision &+= 1
        return started.message
    }

    public func restartComputer(_ confirmation: RemoteConfirmation) async throws -> String {
        let started = try await family.run(confirmation)
        lines = ["==> Restarting computer", "$ loginctl reboot"]
        state = DesktopUpdateState()
        state.running = true
        state.kind = Self.computerRestartAction
        state.status = "Restarting computer…"
        state.relaunching = true
        error = ""
        beginTracking()
        stage = .restarting
        revision &+= 1
        return started.message
    }

    public func unwatch() async {
        if tracking { return }
        _ = try? await family.call("unwatch", timeout: 10)
    }

    public func stop() {
        if let familyTap { family.removeEventTap(familyTap) }
        if let closeTap { channel.removeEventTap(closeTap) }
        familyTap = nil
        closeTap = nil
    }

    func apply(status json: JSONObject) {
        let started = json.int64("startedAtMs")
        let restarted = tracking && restartFrom != 0 && started != 0 && started != restartFrom
        version = json.str("version")
        startedAtMs = started
        loaded = true
        unsupported = false
        error = ""
        let next = DesktopUpdateState(json: json)
        lines = Array(json.strings("lines").suffix(Self.maxLines))
        state = next
        if restarted && !next.running {
            tracking = false
            stage = .back
        } else {
            settleStage()
        }
        revision &+= 1
    }

    func onProgress(_ data: JSONObject) {
        let next = DesktopUpdateState(json: data)
        if next.run != state.run { lines = [] }
        let batch = data.strings("lines")
        if !batch.isEmpty {
            let skipped = data.int64("skipped")
            if skipped > 0 {
                lines.append("… \(skipped) earlier line\(skipped == 1 ? "" : "s") not shown")
            }
            lines.append(contentsOf: batch)
            if lines.count > Self.maxLines { lines.removeFirst(lines.count - Self.maxLines) }
        }
        state = next
        settleStage()
        revision &+= 1
        if next.relaunching { onConnectionLost() }
    }

    private func beginTracking() {
        if !tracking {
            previousVersion = version
            restartFrom = startedAtMs
        }
        tracking = true
    }

    private func settleStage() {
        if state.running {
            beginTracking()
            stage = state.relaunching || state.kind == Self.computerRestartAction ? .restarting : .running
        } else if !state.error.isEmpty {
            tracking = false
            stage = .failed
        } else if stage != .back {
            tracking = false
            stage = .idle
        }
    }

    func onConnectionLost() {
        guard tracking, !waiting else { return }
        stage = .restarting
        revision &+= 1
        Task { await self.waitForRestart() }
    }

    func waitForRestart() async {
        if waiting { return }
        waiting = true
        defer { waiting = false }
        let deadline = now() + Self.restartWaitMs
        while tracking && now() < deadline {
            await sleep(Self.restartPollMs)
            guard let reply = try? await family.call("status", timeout: 15) else { continue }
            let started = reply.int64("startedAtMs")
            let sameProcess = restartFrom != 0 && started == restartFrom
            if sameProcess && DesktopUpdateState(json: reply).running { continue }
            apply(status: reply)
            if !sameProcess || stage != .restarting { return }
        }
        if tracking {
            tracking = false
            stage = .failed
            error = "The desktop has not come back after the update. Check it on the desktop."
            revision &+= 1
        }
    }
}
