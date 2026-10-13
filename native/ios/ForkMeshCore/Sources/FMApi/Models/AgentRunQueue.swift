import FMBase
import Foundation

public struct AgentRunQueueRunner: Equatable, Hashable, Sendable {
    public var name: String
    public var online: Bool
    public var busy: Bool
    public var activeJobs: Int
    public var lastSeenAtMs: Int
    public var nextCheckAtMs: Int
    public var busySinceMs: Int
    public var freeByMs: Int

    public init(
        name: String = "", online: Bool = false, busy: Bool = false, activeJobs: Int = 0, lastSeenAtMs: Int = 0,
        nextCheckAtMs: Int = 0, busySinceMs: Int = 0, freeByMs: Int = 0
    ) {
        self.name = name
        self.online = online
        self.busy = busy
        self.activeJobs = activeJobs
        self.lastSeenAtMs = lastSeenAtMs
        self.nextCheckAtMs = nextCheckAtMs
        self.busySinceMs = busySinceMs
        self.freeByMs = freeByMs
    }

    public init(json: JSONObject) {
        self.init(
            name: json.str("name"), online: json.bool("online"), busy: json.bool("busy"),
            activeJobs: json.int("activeJobs"), lastSeenAtMs: json.int("lastSeenAt"),
            nextCheckAtMs: json.int("nextCheckAt"), busySinceMs: json.int("busySince"), freeByMs: json.int("freeBy"))
    }
}

public struct AgentRunQueue: Equatable, Hashable, Sendable {
    public var position: Int
    public var ahead: Int
    public var waiting: Int
    public var running: Int
    public var repoBusy: Bool
    public var queuedAtMs: Int
    public var checkEveryMs: Int
    public var leaseTimeoutMs: Int
    public var runner: AgentRunQueueRunner?
    public var skewMs: Int

    public init(
        position: Int = 1, ahead: Int = 0, waiting: Int = 1, running: Int = 0, repoBusy: Bool = false,
        queuedAtMs: Int = 0, checkEveryMs: Int = 20_000, leaseTimeoutMs: Int = 7_200_000,
        runner: AgentRunQueueRunner? = nil, skewMs: Int = 0
    ) {
        self.position = position
        self.ahead = ahead
        self.waiting = waiting
        self.running = running
        self.repoBusy = repoBusy
        self.queuedAtMs = queuedAtMs
        self.checkEveryMs = checkEveryMs
        self.leaseTimeoutMs = leaseTimeoutMs
        self.runner = runner
        self.skewMs = skewMs
    }

    public init?(json: JSONObject, receivedAtMs: Int = Int(Date().timeIntervalSince1970 * 1000)) {
        guard json.int("position") > 0 else { return nil }
        let serverNow = json.int("now")
        let every = json.int("checkEveryMs")
        let timeout = json.int("leaseTimeoutMs")
        self.init(
            position: json.int("position"), ahead: json.int("ahead"),
            waiting: max(json.int("position"), json.int("waiting")), running: json.int("running"),
            repoBusy: json.bool("repoBusy"), queuedAtMs: json.int("queuedAt"),
            checkEveryMs: every > 0 ? every : 20_000, leaseTimeoutMs: timeout > 0 ? timeout : 7_200_000,
            runner: json["runner"]?.objectValue.map(AgentRunQueueRunner.init(json:)),
            skewMs: serverNow > 0 ? serverNow - receivedAtMs : 0)
    }

    public func serverNow(_ deviceNowMs: Int) -> Int { deviceNowMs + skewMs }

    public var placeLine: String {
        let others = waiting - 1
        var line = "#\(position) in queue"
        line += others == 0 ? " · no one else waiting" : " · \(others) other\(others == 1 ? "" : "s") waiting"
        if running > 0 { line += " · \(running) running" }
        return line
    }

    public func waitedLine(_ deviceNowMs: Int) -> String {
        guard queuedAtMs > 0 else { return "" }
        return "Queued \(Self.clock(serverNow(deviceNowMs) - queuedAtMs)) ago"
    }

    public func nextCheckAtMs(_ deviceNowMs: Int) -> Int {
        guard let runner, !runner.busy, runner.online, runner.nextCheckAtMs > 0 else { return 0 }
        let now = serverNow(deviceNowMs)
        var next = runner.nextCheckAtMs
        if next < now, checkEveryMs > 0 {
            next += ((now - next) / checkEveryMs + 1) * checkEveryMs
        }
        return next
    }

    public func runnerLine(_ deviceNowMs: Int) -> String {
        guard let runner else { return "" }
        let now = serverNow(deviceNowMs)
        let name = runner.name.isEmpty ? "The runner" : runner.name
        if runner.busy {
            let jobs = runner.activeJobs == 1 ? "a job" : "\(runner.activeJobs) jobs"
            let since = runner.busySinceMs > 0 ? " for \(Self.clock(now - runner.busySinceMs))" : ""
            return "\(name) is busy with \(jobs)\(since). It takes this one as soon as a slot frees."
        }
        if !runner.online {
            guard runner.lastSeenAtMs > 0 else { return "\(name) has not checked in yet." }
            return "\(name) last checked in \(Self.clock(now - runner.lastSeenAtMs)) ago. It may be offline."
        }
        if repoBusy { return "\(name) is waiting for another job on this repository to start." }
        return "\(name) is online and watching the queue."
    }

    public func countdownLabel(_ deviceNowMs: Int) -> String {
        guard let runner else { return "" }
        if runner.busy { return runner.freeByMs > 0 ? "Slot frees by lease timeout in" : "" }
        return nextCheckAtMs(deviceNowMs) > 0 ? "Next claim check in" : ""
    }

    public func countdownMs(_ deviceNowMs: Int) -> Int {
        guard let runner else { return 0 }
        let now = serverNow(deviceNowMs)
        if runner.busy { return runner.freeByMs > 0 ? max(0, runner.freeByMs - now) : 0 }
        let next = nextCheckAtMs(deviceNowMs)
        return next > 0 ? max(0, next - now) : 0
    }

    public func refreshAfterMs(_ deviceNowMs: Int) -> Int {
        let countdown = countdownMs(deviceNowMs)
        let soon = countdown > 0 ? countdown + 1_000 : 15_000
        return min(30_000, max(5_000, soon))
    }

    public static func clock(_ ms: Int) -> String {
        let seconds = max(0, ms) / 1000
        let hours = seconds / 3600
        let minutes = seconds % 3600 / 60
        let secs = seconds % 60
        if hours > 0 { return String(format: "%ldh %02ldm %02lds", hours, minutes, secs) }
        if minutes > 0 { return String(format: "%ldm %02lds", minutes, secs) }
        return "\(secs)s"
    }
}
