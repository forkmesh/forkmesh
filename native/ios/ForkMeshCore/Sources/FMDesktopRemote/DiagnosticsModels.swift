import FMBase
import Foundation

public enum DesktopHealthLevel: String, Equatable, Sendable {
    case ok
    case busy
    case strained
    case unknown

    public init(wire: String) { self = DesktopHealthLevel(rawValue: wire) ?? .unknown }

    public var label: String {
        switch self {
        case .ok: return "Healthy"
        case .busy: return "Busy"
        case .strained: return "Under pressure"
        case .unknown: return "Unknown"
        }
    }
}

public struct DesktopHealth: Equatable, Sendable {
    public var desktop: String
    public var level: DesktopHealthLevel
    public var cpuPercent: Double
    public var cpuCount: Int
    public var load1: Double
    public var memoryTotal: Double
    public var memoryAvailable: Double
    public var swapTotal: Double
    public var swapFree: Double
    public var appResident: Double
    public var pressureValid: Bool
    public var memoryFull: Double
    public var memorySome: Double
    public var ioFull: Double
    public var diskTotal: Double
    public var diskFree: Double
    public var agentsRunning: Int
    public var agentsQueued: Int
    public var agentsTotal: Int
    public var stalls: Int
    public var openFiles: Int
    public var openFileLimit: Int
    public var threads: Int
    public var onBattery: Bool
    public var batteryPercent: Int
    public var sampledAtMs: Int
    public var pressureLine: String

    public init(json: JSONObject) {
        desktop = json.str("desktop")
        level = DesktopHealthLevel(wire: json.str("level"))
        cpuPercent = json.double("cpuPercent", -1)
        cpuCount = json.int("cpuCount")
        load1 = json.double("load1", -1)
        let memory = json.obj("memory")
        memoryTotal = memory.double("total", -1)
        memoryAvailable = memory.double("available", -1)
        swapTotal = memory.double("swapTotal", -1)
        swapFree = memory.double("swapFree", -1)
        appResident = memory.double("appResident", -1)
        let pressure = json.obj("pressure")
        pressureValid = pressure.bool("valid")
        memoryFull = pressure.double("memoryFull")
        memorySome = pressure.double("memorySome")
        ioFull = pressure.double("ioFull")
        diskTotal = json.obj("disk").double("total", -1)
        diskFree = json.obj("disk").double("free", -1)
        agentsRunning = json.obj("agents").int("running")
        agentsQueued = json.obj("agents").int("queued")
        agentsTotal = json.obj("agents").int("total")
        stalls = json.int("stalls")
        openFiles = json.obj("process").int("openFiles", -1)
        openFileLimit = json.obj("process").int("openFileLimit", -1)
        threads = json.obj("process").int("threads", -1)
        onBattery = json.obj("power").bool("onBattery")
        batteryPercent = json.obj("power").int("percent", -1)
        sampledAtMs = json.int("sampledAtMs")
        pressureLine = json.str("pressureLine")
    }

    public var memoryUsedShare: Double {
        guard memoryTotal > 0, memoryAvailable >= 0 else { return 0 }
        return min(1, max(0, (memoryTotal - memoryAvailable) / memoryTotal))
    }

    public var diskUsedShare: Double {
        guard diskTotal > 0, diskFree >= 0 else { return 0 }
        return min(1, max(0, (diskTotal - diskFree) / diskTotal))
    }

    public var memoryLine: String {
        guard memoryTotal > 0 else { return "—" }
        return "\(RemoteFormat.bytes(memoryTotal - max(0, memoryAvailable))) of \(RemoteFormat.bytes(memoryTotal)) used"
    }

    public var diskLine: String {
        guard diskTotal > 0 else { return "—" }
        return "\(RemoteFormat.bytes(max(0, diskFree))) free of \(RemoteFormat.bytes(diskTotal))"
    }

    public var batteryShare: Double {
        guard batteryPercent >= 0 else { return 0 }
        return min(1, Double(batteryPercent) / 100)
    }

    public var batteryLine: String {
        batteryPercent >= 0 ? "\(min(100, batteryPercent))% left" : "On battery"
    }

    public var agentsLine: String {
        var parts = ["\(agentsRunning) running"]
        if agentsQueued > 0 { parts.append("\(agentsQueued) queued") }
        return parts.joined(separator: " · ")
    }

    public var loadLine: String {
        guard load1 >= 0 else { return "—" }
        return cpuCount > 0 ? String(format: "%.1f on %d CPUs", load1, cpuCount) : String(format: "%.1f", load1)
    }
}

public enum LogSeverity: String, CaseIterable, Equatable, Sendable, Identifiable {
    case all
    case warning
    case error
    case info

    public var id: String { rawValue }

    public init(wire: String) { self = LogSeverity(rawValue: wire) ?? .info }

    public var title: String {
        switch self {
        case .all: return "All"
        case .warning: return "Warnings"
        case .error: return "Errors"
        case .info: return "Info"
        }
    }
}

public struct DesktopLogLine: Equatable, Sendable, Identifiable {
    public var offset: Int
    public var timeMs: Int
    public var severity: LogSeverity
    public var text: String

    public var id: Int { offset }

    public init(json: JSONObject) {
        offset = json.int("offset")
        timeMs = json.int("t")
        severity = LogSeverity(wire: json.str("severity"))
        text = json.str("text")
    }

    public var body: String {
        guard timeMs > 0, text.count > 21 else { return text }
        return String(text.dropFirst(19)).trimmingCharacters(in: .whitespaces)
    }
}

public struct DesktopLogPage: Equatable, Sendable {
    public var lines: [DesktopLogLine]
    public var start: Int
    public var end: Int
    public var more: Bool

    public init(json: JSONObject) {
        lines = json.maps("lines").map(DesktopLogLine.init(json:))
        start = json.int("start")
        end = json.int("end")
        more = json.bool("more")
    }
}

public struct StallRecord: Equatable, Sendable, Identifiable {
    public var index: Int
    public var when: String
    public var summary: String
    public var lines: Int
    public var bytes: Int
    public var text: String
    public var truncated: Bool

    public var id: Int { index }

    public init(json: JSONObject) {
        index = json.int("index")
        when = json.str("when")
        summary = json.str("summary")
        lines = json.int("lines")
        bytes = json.int("bytes")
        text = json.str("text")
        truncated = json.bool("truncated")
    }
}

public struct StallList: Equatable, Sendable {
    public var stalls: [StallRecord]
    public var canSend: Bool

    public init(json: JSONObject) {
        stalls = json.maps("stalls").map(StallRecord.init(json:))
        canSend = json.bool("canSend")
    }
}

public struct ProcessAgent: Equatable, Sendable {
    public var session: Int
    public var label: String
    public var stoppable: Bool
}

public struct DesktopProcess: Equatable, Sendable, Identifiable {
    public var pid: Int
    public var name: String
    public var user: String
    public var rssBytes: Double
    public var percent: Double
    public var command: String
    public var agent: ProcessAgent?

    public var id: Int { pid }

    public init(json: JSONObject) {
        pid = json.int("pid")
        name = json.str("name")
        user = json.str("user")
        rssBytes = json.double("rssBytes")
        percent = json.double("percent")
        command = json.str("command")
        let agent = json.obj("agent")
        self.agent = agent.int("session") > 0
            ? ProcessAgent(session: agent.int("session"), label: agent.str("label"), stoppable: agent.bool("stoppable"))
            : nil
    }
}

public struct ProcessList: Equatable, Sendable {
    public var processes: [DesktopProcess]
    public var memoryTotal: Double

    public init(json: JSONObject) {
        processes = json.maps("processes").map(DesktopProcess.init(json:))
        memoryTotal = json.double("memoryTotal")
    }

    public var agentProcesses: [DesktopProcess] { processes.filter { $0.agent != nil } }

    public func share(of process: DesktopProcess) -> Double {
        memoryTotal > 0 ? min(1, process.rssBytes / memoryTotal) : process.percent / 100
    }
}

public enum DiagnosticsAction: Equatable, Sendable {
    case stopAgent(session: Int)
    case sendStalls
    case sendStall(index: Int)

    public var params: JSONObject {
        switch self {
        case .stopAgent(let session): return ["action": "stopAgent", "session": .int(Int64(session))]
        case .sendStalls: return ["action": "sendStalls"]
        case .sendStall(let index): return ["action": "sendStall", "index": .int(Int64(index))]
        }
    }
}

public enum DesktopBackupResult: String, Equatable, Sendable {
    case ok
    case running
    case failed
    case none
    case off
    case unknown

    public init(wire: String) { self = DesktopBackupResult(rawValue: wire) ?? .unknown }

    public var label: String {
        switch self {
        case .ok: return "Backed up"
        case .running: return "Backing up now"
        case .failed: return "Last backup failed"
        case .none: return "No backup yet"
        case .off: return "Hourly backups are off"
        case .unknown: return "Unknown"
        }
    }
}

public struct DesktopBackupStatus: Equatable, Sendable {
    public var enabled: Bool
    public var keep: Int
    public var running: Bool
    public var result: DesktopBackupResult
    public var location: String
    public var count: Int
    public var totalBytes: Double
    public var lastBackupAtMs: Int64
    public var lastBackupBytes: Double
    public var nextDueAtMs: Int64
    public var error: String
    public var failedAtMs: Int64

    public init(json: JSONObject) {
        enabled = json.bool("enabled")
        keep = json.int("keep")
        running = json.bool("running")
        result = DesktopBackupResult(wire: json.str("result"))
        location = json.str("location")
        count = json.int("count")
        totalBytes = json.double("totalBytes")
        lastBackupAtMs = json.int64("lastBackupAtMs")
        lastBackupBytes = json.double("lastBackupBytes")
        nextDueAtMs = json.int64("nextDueAtMs")
        error = json.str("error")
        failedAtMs = json.int64("failedAtMs")
    }

    public var hasBackup: Bool { lastBackupAtMs > 0 }

    public var sizeLine: String {
        guard count > 0 else { return "—" }
        let copies = count == 1 ? "1 copy" : "\(count) copies"
        return "\(RemoteFormat.bytes(lastBackupBytes)) · \(copies), \(RemoteFormat.bytes(totalBytes)) in all"
    }

    public var errorText: String {
        switch error {
        case "": return ""
        case "tar_missing": return "The desktop has no tar tool to write backups with."
        case "nothing_to_back_up": return "There was nothing to back up yet."
        case "rename_failed": return "The backup was written but could not be saved in place."
        default: return "The backup could not be written. Open Settings, Data on the desktop for details."
        }
    }
}
