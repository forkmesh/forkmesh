import FMAccount
import FMApi
import FMBase
import FMInsights
import FMTools
import Foundation
import Observation
import SwiftUI

@MainActor @Observable final class PerformanceDiagnostics {
    static let shared: PerformanceDiagnostics = PerformanceDiagnostics()
    static let sentKey: String = "performanceDiagnosticsSentAt"
    static let refreshSeconds: Double = 5

    private(set) var alert: PerformanceAlert = .none

    var lastSent: Date? {
        let value: Double = UserDefaults.standard.double(forKey: Self.sentKey)
        return value > 0 ? Date(timeIntervalSince1970: value) : nil
    }

    func refresh() async {
        let events: [PerformanceEvent] = Services.get(AppTelemetry.self)?.monitor?.events ?? []
        let samples: [PerformanceAlert.Sample] = events.map { (event: PerformanceEvent) -> PerformanceAlert.Sample in
            PerformanceAlert.Sample(event)
        }
        let diagnostics: [PerformanceAlert.Diagnostic] = await Task.detached(priority: .utility) {
            PerformanceDiagnostics.metricKitDiagnostics()
        }.value
        alert = PerformanceAlert.evaluate(samples: samples, diagnostics: diagnostics, lastSent: lastSent)
    }

    nonisolated static func metricKitDiagnostics() -> [PerformanceAlert.Diagnostic] {
        CrashReportStore.forDevice().load()
            .filter { (report: CrashReport) -> Bool in report.kind != .diskWrite }
            .map { (report: CrashReport) -> PerformanceAlert.Diagnostic in
                PerformanceAlert.Diagnostic(summary: report.summary, at: Date(epochMillis: report.receivedAt))
            }
    }

    func prompt(router: AppRouter?, includeLogs: Bool) async -> String {
        let lead: String = DiagnosticsBundle.lead(reason: alert.reason)
        let report: String = await PerformanceReportBuilder.report(router: router, draft: lead)
            ?? "The performance report is unavailable in this build."
        let commit: String = String((AppServices.current?.config.build.commit ?? "").prefix(12))
        var header: String = "build: \(AppChangelog.label(infoDictionary: Bundle.main.infoDictionary))"
        if !commit.isEmpty { header += ", commit \(commit)" }
        if alert.detected { header += "\nalert: \(alert.reason)" }
        return DiagnosticsBundle.prompt(
            reason: alert.reason, report: header + "\n" + report, includeLogs: includeLogs,
            limit: ApiService.maxAgentPromptChars)
    }

    func markSent() {
        UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: Self.sentKey)
        alert = .none
    }
}

struct DiagnosticsSendButton: View {
    @Environment(\.fmPalette) private var fm
    let alert: PerformanceAlert
    var compact: Bool = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Label(alert.label, systemImage: alert.detected ? "exclamationmark.triangle.fill" : "stethoscope")
                .labelStyle(.titleAndIcon)
                .fmText(compact ? .labelSmall : .labelMedium, color: alert.detected ? fm.danger : fm.textSecondary)
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, compact ? FmSpace.x2 : FmSpace.x3)
                .padding(.vertical, compact ? FmSpace.x1 : FmSpace.x2)
                .background(Capsule().fill(alert.detected ? fm.danger.opacity(fm.dark ? 0.2 : 0.1) : fm.bgOverlay))
                .frame(minHeight: compact ? 32 : 44)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .accessibilityLabel(Text(alert.detected ? "Send diagnostics to an agent. \(alert.reason)" : "Send diagnostics to an agent"))
        .accessibilityIdentifier("hub-send-diagnostics")
    }
}

struct DiagnosticsSendSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var includeLogs: Bool = true
    @State private var preview: String = ""
    @State private var sending: Bool = false
    @State private var problem: String?
    private var diagnostics: PerformanceDiagnostics { PerformanceDiagnostics.shared }

    let router: AppRouter
    let send: (String) async -> String?
    let onSent: () -> Void

    private static let contents: [String] = [
        "App version, build and commit",
        "iPhone model, iOS version, memory and thermal state",
        "Frame, main-thread and network timings, and the slowest events",
        "Hang and crash reports from iOS, with their stack frames",
        "The screen you are on and your recent screens",
    ]

    var body: some View {
        NavigationStack {
            List {
                Section {
                    summary
                }
                Section("What is sent") {
                    ForEach(Self.contents, id: \.self) { (line: String) in
                        Label {
                            Text(verbatim: line)
                                .fmText(.bodyMedium, color: fm.textPrimary)
                        } icon: {
                            Image(systemName: "checkmark")
                                .foregroundStyle(fm.accent)
                        }
                    }
                    Toggle(isOn: $includeLogs) {
                        Text("Include recent log lines")
                            .fmText(.bodyMedium, color: fm.textPrimary)
                    }
                    .tint(fm.accent)
                    .accessibilityIdentifier("diagnostics-include-logs")
                }
                Section {
                    Text("Tokens, passwords, emails, phone numbers, link queries and message contents are removed on this iPhone first. A coding agent on forkmesh/forkmesh reads the report, fixes the cause on a branch and adds tests.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                Section("Preview") {
                    DisclosureGroup("Show what is sent") {
                        if preview.isEmpty {
                            ProgressView()
                        } else {
                            Text(verbatim: preview)
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(fm.textSecondary)
                                .textSelection(.enabled)
                        }
                    }
                }
                if let problem = problem {
                    Section {
                        Text(verbatim: problem)
                            .fmText(.bodySmall, color: fm.danger)
                    }
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle("Send diagnostics")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    if sending {
                        ProgressView()
                    } else {
                        Button("Send") { submit() }
                            .disabled(preview.isEmpty)
                            .accessibilityIdentifier("diagnostics-send")
                    }
                }
            }
            .task(id: includeLogs) {
                preview = ""
                preview = await diagnostics.prompt(router: router, includeLogs: includeLogs)
            }
        }
        .presentationDetents([.large])
    }

    private var summary: some View {
        let alert: PerformanceAlert = diagnostics.alert
        return Label {
            VStack(alignment: .leading, spacing: 2) {
                Text(alert.detected ? "Performance issue detected" : "No issue detected right now")
                    .fmText(.titleSmall, color: fm.textPrimary)
                Text(verbatim: alert.detected ? alert.reason : "A report still helps an agent find slow spots.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
        } icon: {
            Image(systemName: alert.detected ? "exclamationmark.triangle.fill" : "stethoscope")
                .foregroundStyle(alert.detected ? fm.danger : fm.accent)
        }
        .padding(.vertical, FmSpace.x1)
    }

    private func submit() {
        let text: String = preview
        Task { @MainActor in
            sending = true
            problem = nil
            let failure: String? = await send(text)
            sending = false
            if let failure = failure {
                FmHaptics.error()
                problem = failure
                return
            }
            diagnostics.markSent()
            dismiss()
            onSent()
        }
    }
}
