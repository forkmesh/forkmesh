import FMAccount
import FMBase
import FMInsights
import Foundation
#if canImport(MetricKit)
import MetricKit
#endif

#if canImport(MetricKit)
final class MetricKitSink: NSObject, MXMetricManagerSubscriber {
    static let shared = MetricKitSink()

    private let store = CrashReportStore.forDevice()
    private weak var stallLog: StallLogFile?
    private var installed = false

    @MainActor
    func install(stallLog: StallLogFile?) {
        self.stallLog = stallLog
        if installed { return }
        installed = true
        MXMetricManager.shared.add(self)
        receive(MXMetricManager.shared.pastDiagnosticPayloads)
    }

    func didReceive(_ payloads: [MXMetricPayload]) {}

    func didReceive(_ payloads: [MXDiagnosticPayload]) {
        receive(payloads)
    }

    private func receive(_ payloads: [MXDiagnosticPayload]) {
        let reports = payloads.flatMap(MetricKitSink.reports(from:))
        if reports.isEmpty { return }
        store.append(reports)
        for report in reports {
            stallLog?.append(report.stallLogLine)
            Log.warning("metrickit", report.stallLogLine)
        }
    }

    static func reports(from payload: MXDiagnosticPayload) -> [CrashReport] {
        let received = Int64(payload.timeStampEnd.timeIntervalSince1970 * 1000)
        var out: [CrashReport] = []
        for diagnostic in payload.crashDiagnostics ?? [] {
            var parts: [String] = []
            if let type = diagnostic.exceptionType { parts.append("exception \(type)") }
            if let code = diagnostic.exceptionCode { parts.append("code \(code)") }
            if let signal = diagnostic.signal { parts.append("signal \(signal)") }
            if let reason = diagnostic.terminationReason { parts.append(reason) }
            out.append(report(.crash, diagnostic, received, summary: parts.isEmpty ? "The app crashed" : parts.joined(separator: ", "),
                              stacks: diagnostic.callStackTree))
        }
        for diagnostic in payload.hangDiagnostics ?? [] {
            let seconds = diagnostic.hangDuration.converted(to: .seconds).value
            out.append(report(.hang, diagnostic, received, summary: String(format: "The main thread hung for %.1f s", seconds),
                              stacks: diagnostic.callStackTree))
        }
        for diagnostic in payload.cpuExceptionDiagnostics ?? [] {
            let cpu = diagnostic.totalCPUTime.converted(to: .seconds).value
            let sampled = diagnostic.totalSampledTime.converted(to: .seconds).value
            out.append(report(.cpu, diagnostic, received,
                              summary: String(format: "%.0f s of CPU in %.0f s", cpu, sampled), stacks: diagnostic.callStackTree))
        }
        for diagnostic in payload.diskWriteExceptionDiagnostics ?? [] {
            let bytes = Int64(diagnostic.totalWritesCaused.converted(to: .bytes).value)
            out.append(report(.diskWrite, diagnostic, received,
                              summary: "\(StatsFormat.compactBytes(bytes)) written to disk", stacks: diagnostic.callStackTree))
        }
        if #available(iOS 16.0, *) {
            for diagnostic in payload.appLaunchDiagnostics ?? [] {
                let seconds = diagnostic.launchDuration.converted(to: .seconds).value
                out.append(report(.launch, diagnostic, received, summary: String(format: "Launch took %.1f s", seconds),
                                  stacks: diagnostic.callStackTree))
            }
        }
        return out
    }

    private static func report(_ kind: CrashReport.Kind, _ diagnostic: MXDiagnostic, _ received: Int64, summary: String,
                               stacks: MXCallStackTree) -> CrashReport {
        let metadata = diagnostic.metaData
        let raw = stacks.jsonRepresentation()
        let detail = CrashReport.attributedStackJSON(raw) ?? String(data: raw, encoding: .utf8) ?? ""
        return CrashReport(
            kind: kind, receivedAt: received, appVersion: diagnostic.applicationVersion,
            osVersion: metadata.osVersion, summary: summary, detail: detail)
    }
}
#endif
