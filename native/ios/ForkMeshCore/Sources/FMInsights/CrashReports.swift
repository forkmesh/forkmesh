import FMBase
import Foundation
import Observation

public struct CrashReport: Codable, Equatable, Sendable, Identifiable {
    public enum Kind: String, Codable, Sendable, CaseIterable {
        case crash, hang, cpu, diskWrite, launch

        public var title: String {
            switch self {
            case .crash: return "Crash"
            case .hang: return "Hang"
            case .cpu: return "CPU exception"
            case .diskWrite: return "Disk writes"
            case .launch: return "Slow launch"
            }
        }

        public var symbol: String {
            switch self {
            case .crash: return "exclamationmark.triangle"
            case .hang: return "hourglass"
            case .cpu: return "cpu"
            case .diskWrite: return "internaldrive"
            case .launch: return "timer"
            }
        }
    }

    public static let summaryLimit = 300
    public static let detailLimit = 16 * 1024

    public var id: String
    public var kind: Kind
    public var receivedAt: Int64
    public var appVersion: String
    public var osVersion: String
    public var summary: String
    public var detail: String

    public init(id: String = UUID().uuidString, kind: Kind, receivedAt: Int64, appVersion: String = "",
                osVersion: String = "", summary: String, detail: String = "") {
        self.id = id
        self.kind = kind
        self.receivedAt = receivedAt
        self.appVersion = String(appVersion.prefix(40))
        self.osVersion = String(osVersion.prefix(60))
        self.summary = String(summary.prefix(CrashReport.summaryLimit))
        self.detail = String(detail.prefix(CrashReport.detailLimit))
    }

    public var stallLogLine: String {
        var line = "[metrickit] \(kind.rawValue) \(summary)"
        if !appVersion.isEmpty { line += " app=\(appVersion)" }
        if !osVersion.isEmpty { line += " os=\(osVersion)" }
        return line
    }

    public func topFrames(_ limit: Int) -> [String] {
        CrashReport.topFrames(detail, limit)
    }

    public static func topFrames(_ json: String, _ limit: Int) -> [String] {
        guard limit > 0, let data = json.data(using: .utf8), let stack = attributed(data)?.stack else { return [] }
        var frame = (stack["callStackRootFrames"] as? [[String: Any]])?.first
        var out: [String] = []
        while let current = frame, out.count < limit {
            let binary = current["binaryName"] as? String ?? "?"
            let offset = (current["offsetIntoBinaryTextSegment"] as? NSNumber)?.int64Value
            out.append(offset.map { "\(binary)+0x\(String($0, radix: 16))" } ?? binary)
            frame = (current["subFrames"] as? [[String: Any]])?.first
        }
        return out
    }

    public static func attributedStackJSON(_ data: Data) -> String? {
        guard let found = attributed(data) else { return nil }
        var slim = found.tree
        slim["callStacks"] = [found.stack]
        guard let out = try? JSONSerialization.data(withJSONObject: slim, options: [.sortedKeys]) else { return nil }
        return String(data: out, encoding: .utf8)
    }

    static func attributed(_ data: Data) -> (tree: [String: Any], stack: [String: Any])? {
        guard let tree = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let stacks = tree["callStacks"] as? [[String: Any]], let first = stacks.first
        else { return nil }
        return (tree, stacks.first { ($0["threadAttributed"] as? Bool) == true } ?? first)
    }

    public func agentPrompt() -> String {
        var lines = [
            "The ForkMesh iOS app reported a \(kind.title.lowercased()) through MetricKit. Find the cause in native/ios and fix it.",
            "",
            "Summary: \(summary)",
        ]
        if !appVersion.isEmpty { lines.append("App version: \(appVersion)") }
        if !osVersion.isEmpty { lines.append("OS: \(osVersion)") }
        if !detail.isEmpty {
            lines.append("")
            lines.append("Call stacks (MetricKit JSON):")
            lines.append(String(detail.prefix(8 * 1024)))
        }
        return lines.joined(separator: "\n")
    }
}

public final class CrashReportStore: @unchecked Sendable {
    public static let fileName = "metrickit.json"
    public static let maxReports = 20

    private let directory: @Sendable () throws -> URL
    private let lock = NSLock()

    public init(directory: @escaping @Sendable () throws -> URL) {
        self.directory = directory
    }

    public static func forDevice() -> CrashReportStore {
        CrashReportStore(directory: {
            let base = try FileManager.default.url(
                for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            return base.appendingPathComponent("diagnostics", isDirectory: true)
        })
    }

    private func fileURL() throws -> URL {
        let folder = try directory()
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder.appendingPathComponent(CrashReportStore.fileName)
    }

    public func load() -> [CrashReport] {
        lock.lock()
        defer { lock.unlock() }
        return read()
    }

    private func read() -> [CrashReport] {
        guard let url = try? fileURL(), let data = try? Data(contentsOf: url) else { return [] }
        return (try? JSONDecoder().decode([CrashReport].self, from: data)) ?? []
    }

    private func write(_ reports: [CrashReport]) {
        guard let url = try? fileURL(), let data = try? JSONEncoder().encode(reports) else { return }
        try? data.write(to: url, options: [.atomic])
    }

    @discardableResult
    public func append(_ incoming: [CrashReport]) -> [CrashReport] {
        lock.lock()
        defer { lock.unlock() }
        var reports = read()
        let known = Set(reports.map(\.id))
        reports.append(contentsOf: incoming.filter { !known.contains($0.id) })
        reports.sort { $0.receivedAt > $1.receivedAt }
        if reports.count > CrashReportStore.maxReports { reports = Array(reports.prefix(CrashReportStore.maxReports)) }
        write(reports)
        return reports
    }

    public func remove(_ id: String) {
        lock.lock()
        defer { lock.unlock() }
        write(read().filter { $0.id != id })
    }

    public func clear() {
        lock.lock()
        defer { lock.unlock() }
        write([])
    }
}

@MainActor
@Observable
public final class CrashReportsModel {
    @ObservationIgnored private let store: CrashReportStore
    public private(set) var reports: [CrashReport] = []

    public init(store: CrashReportStore) {
        self.store = store
    }

    public func reload() {
        reports = store.load()
    }

    public func remove(_ id: String) {
        store.remove(id)
        reload()
    }

    public func clear() {
        store.clear()
        reload()
    }

    public struct KindCount: Equatable, Sendable, Identifiable {
        public var kind: CrashReport.Kind
        public var count: Int
        public var id: String { kind.rawValue }
    }

    public var counts: [KindCount] {
        CrashReport.Kind.allCases.compactMap { kind in
            let count = reports.filter { $0.kind == kind }.count
            return count > 0 ? KindCount(kind: kind, count: count) : nil
        }
    }
}
