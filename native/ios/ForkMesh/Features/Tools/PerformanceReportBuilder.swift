import FMAccount
import FMAgents
import FMApi
import FMBase
import FMInsights
import FMTools
import Foundation
import UIKit
import WatchConnectivity
import os

@MainActor
enum PerformanceReportBuilder {
    nonisolated static let metricKitReports = 2
    nonisolated static let metricKitFrames = 8
    static let lead = "Find and fix what makes the ForkMesh iOS app slow or crash, using this report."

    static func prompt(adding report: String, to draft: String) -> String {
        let typed: String = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        return (typed.isEmpty ? lead : typed) + "\n\n" + report
    }

    static func budget(for draft: String) -> Int {
        let typed: String = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        let used: Int = (typed.isEmpty ? lead : typed).utf16.count + 2
        return min(PerformanceReport.maxChars, ApiService.maxAgentPromptChars - used)
    }

    static func report(router: AppRouter?, draft: String) async -> String? {
        guard let telemetry = Services.get(AppTelemetry.self) else { return nil }
        let limit: Int = budget(for: draft)
        guard limit >= PerformanceReport.minChars else { return nil }
        let stallLog: String = await telemetry.monitor?.stallLog?.tail(maxBytes: PerformanceReport.stallLogBytes) ?? ""
        let metricKit: [String] = await Task.detached(priority: .userInitiated) {
            PerformanceReportBuilder.metricKitLines()
        }.value
        return PerformanceReport.text(
            telemetry: telemetry, context: context(router), metricKit: metricKit, stallLog: stallLog, limit: limit)
    }

    nonisolated static func metricKitLines() -> [String] {
        let reports: [CrashReport] = CrashReportStore.forDevice().load()
            .sorted { (a: CrashReport, b: CrashReport) -> Bool in a.receivedAt > b.receivedAt }
        return reports.prefix(metricKitReports).map { (report: CrashReport) -> String in
            var line: String = "\(report.kind.title) \(DartDateFormat.iso8601(Date(epochMillis: report.receivedAt))): \(report.summary)"
            if !report.appVersion.isEmpty { line += " app=\(report.appVersion)" }
            if !report.osVersion.isEmpty { line += " os=\(report.osVersion)" }
            let frames: [String] = report.kind == .diskWrite ? [] : report.topFrames(metricKitFrames)
            if !frames.isEmpty {
                line += "\nstack: " + frames.joined(separator: " < ")
            }
            return line
        }
    }

    static func context(_ router: AppRouter?) -> [(String, String)] {
        let info: ProcessInfo = ProcessInfo.processInfo
        let device: UIDevice = UIDevice.current
        let megabyte: UInt64 = 1024 * 1024
        var out: [(String, String)] = []
        if let build = AppServices.current?.config.build {
            out.append(("app", "\(build.version) (\(AppServices.isDebugBuild ? "debug" : "release"))"))
        }
        out.append(("os", "\(device.systemName) \(device.systemVersion)"))
        out.append(("device", "\(machine()) (\(device.model))"))
        out.append((
            "memory", "\(info.physicalMemory / megabyte) MB, \(UInt64(os_proc_available_memory()) / megabyte) MB left for the app"))
        out.append(("thermal state", thermal(info.thermalState)))
        out.append(("low power mode", info.isLowPowerModeEnabled ? "on" : "off"))
        if let screen = UIApplication.shared.connectedScenes.compactMap({ ($0 as? UIWindowScene)?.screen }).first {
            out.append(("display", "max \(screen.maximumFramesPerSecond) fps"))
        }
        if let router = router {
            out.append(("tab", router.selectedTab.rawValue))
            let path: [String] = router.currentPath.map(\.title)
            out.append(("screen", path.isEmpty ? "tab root" : path.joined(separator: " > ")))
            if !router.recents.isEmpty {
                out.append(("recent screens", router.recents.prefix(5).map(\.title).joined(separator: ", ")))
            }
        }
        out.append(("agents", agents()))
        out.append(("watch", watch()))
        return out
    }

    static func agents() -> String {
        let counts: ShellCounts = ShellCounts.shared
        let settings: SettingsService? = Services.get(SettingsService.self)
        let view: String = Services.get(KeyValueStore.self)?.string(forKey: AgentListView.storageKey) ?? AgentListView.eagle.rawValue
        return "\(counts.agentsTotal) listed, \(counts.agents) running, view \(view), "
            + "dots \(settings?.showAgentDots == true ? "on" : "off"), aquarium \(settings?.aquariumOn == true ? "on" : "off")"
    }

    static func watch() -> String {
        guard WCSession.isSupported() else { return "unsupported" }
        let session: WCSession = WCSession.default
        guard session.activationState == .activated else { return "not activated" }
        if !session.isPaired { return "none paired" }
        return "paired, app \(session.isWatchAppInstalled ? "installed" : "not installed")\(session.isReachable ? ", reachable" : "")"
    }

    static func thermal(_ state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal: return "nominal"
        case .fair: return "fair"
        case .serious: return "serious (iOS is throttling the CPU)"
        case .critical: return "critical (iOS is throttling hard)"
        @unknown default: return "unknown"
        }
    }

    static func machine() -> String {
        var info: utsname = utsname()
        uname(&info)
        return withUnsafeBytes(of: &info.machine) { (raw: UnsafeRawBufferPointer) -> String in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }
}
