import Charts
import FMApi
import FMInsights
import SwiftUI

struct DesktopTimelineScreen: View {
    @State private var model: DesktopStatsModel?

    var body: some View {
        Group {
            if let model {
                DesktopTimelineContent(model: model)
            } else {
                InsightsUnavailableView(title: "Your desktop is not paired", systemImage: "desktopcomputer",
                                        message: "The log timeline comes from your desktop. Pair it in Desktops.")
            }
        }
        .navigationTitle("Desktop log")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            if model == nil, let client = InsightsFeature.desktopClient { model = DesktopStatsModel(client: client) }
        }
    }
}

private struct DesktopTimelineContent: View {
    @Bindable var model: DesktopStatsModel

    var body: some View {
        List {
            switch model.timeline {
            case .idle, .loading:
                InsightsLoadingRows(rows: 4)
            case .unsupported:
                Text("Your desktop does not offer the timeline yet. Update ForkMesh on the desktop.")
                    .foregroundStyle(.secondary)
                    .fmMessageTools("Your desktop does not offer the timeline yet. Update ForkMesh on the desktop.")
            case .failed(let message):
                InsightsBanner(text: message, systemImage: "exclamationmark.triangle")
            case .loaded(let timeline):
                Section("Last 30 days") {
                    Chart {
                        ForEach(timeline.days) { day in
                            BarMark(x: .value("Day", InsightsTime.date(day.dayTs), unit: .day), y: .value("Lines", day.errors))
                                .foregroundStyle(by: .value("Level", "Errors"))
                            BarMark(x: .value("Day", InsightsTime.date(day.dayTs), unit: .day), y: .value("Lines", day.warnings))
                                .foregroundStyle(by: .value("Level", "Warnings"))
                        }
                    }
                    .chartForegroundStyleScale(["Errors": Color.red, "Warnings": Color.orange])
                    .frame(height: 200)
                    .accessibilityLabel("Errors and warnings per day")
                    if let busiest = timeline.busiest {
                        LabeledContent("Busiest day", value: InsightsTime.date(busiest.dayTs).formatted(date: .abbreviated, time: .omitted))
                    }
                }
                Section {
                    NavigationLink(value: Route.desktops) {
                        Label("Open the desktop's live log", systemImage: "text.alignleft")
                    }
                }
            }
        }
        .fmRefreshable { await model.loadTimeline() }
        .task { await model.loadTimeline() }
    }
}

struct SentryIssuesScreen: View {
    @State private var model: DesktopStatsModel?

    var body: some View {
        Group {
            if let model {
                SentryIssuesContent(model: model)
            } else {
                InsightsUnavailableView(title: "Your desktop is not paired", systemImage: "desktopcomputer",
                                        message: "Sentry issues are read by your desktop's Sentry package.")
            }
        }
        .navigationTitle("Sentry")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            if model == nil, let client = InsightsFeature.desktopClient { model = DesktopStatsModel(client: client) }
        }
    }
}

private struct SentryIssuesContent: View {
    @Bindable var model: DesktopStatsModel

    var body: some View {
        List {
            switch model.sentry {
            case .idle, .loading:
                InsightsLoadingRows(rows: 5)
            case .unsupported:
                Text("Your desktop does not offer Sentry issues yet. Install the Sentry package and update ForkMesh.")
                    .foregroundStyle(.secondary)
                    .fmMessageTools("Your desktop does not offer Sentry issues yet. Install the Sentry package and update ForkMesh.")
            case .failed(let message):
                InsightsBanner(text: message, systemImage: "exclamationmark.triangle")
            case .loaded(let issues):
                if issues.isEmpty {
                    ContentUnavailableView("No unresolved issues", systemImage: "checkmark.circle")
                }
                ForEach(issues) { issue in
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Image(systemName: issue.tone.symbol).foregroundStyle(issue.tone.color)
                            Text(issue.title).lineLimit(3)
                        }
                        Text("\(issue.project) · \(issue.count) events · last \(InsightsTime.relative(issue.lastSeen))")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    .accessibilityElement(children: .combine)
                }
            }
        }
        .fmRefreshable { await model.loadSentry() }
        .task { await model.loadSentry() }
    }
}

struct CrashReportsScreen: View {
    @State private var model = CrashReportsModel(store: InsightsFeature.crashStore)
    @State private var confirmClear = false

    var body: some View {
        List {
            if model.reports.isEmpty {
                ContentUnavailableView(
                    "No reports", systemImage: "checkmark.shield",
                    description: Text("iOS delivers crash and hang reports about once a day. They appear here when it does."))
            } else {
                Section {
                    ForEach(model.counts) { entry in
                        LabeledContent(entry.kind.title, value: "\(entry.count)")
                    }
                }
                Section("Reports") {
                    ForEach(model.reports) { report in
                        NavigationLink {
                            CrashReportDetail(report: report) { model.remove(report.id) }
                                .shellPageClearance()
                        } label: {
                            Label {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(report.summary).lineLimit(2)
                                    Text("\(report.kind.title) · \(InsightsTime.relative(report.receivedAt))")
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                            } icon: {
                                Image(systemName: report.kind.symbol)
                            }
                        }
                    }
                    .onDelete { offsets in
                        let ids: [String] = offsets.map { model.reports[$0].id }
                        for id in ids { model.remove(id) }
                    }
                }
            }
        }
        .navigationTitle("Crash reports")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            if !model.reports.isEmpty {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button("Delete all", role: .destructive) { confirmClear = true }
                    } label: {
                        Image(systemName: "ellipsis.circle").accessibilityLabel("More")
                    }
                }
            }
        }
        .confirmationDialog("Delete every report?", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("Delete all", role: .destructive) { model.clear() }
        }
        .onAppear { model.reload() }
        .fmRefreshable { model.reload() }
    }
}

struct CrashReportDetail: View {
    let report: CrashReport
    let onDelete: () -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var sent = false

    var body: some View {
        List {
            Section {
                LabeledContent("Kind", value: report.kind.title)
                LabeledContent("Received", value: InsightsTime.relative(report.receivedAt))
                if !report.appVersion.isEmpty { LabeledContent("App", value: report.appVersion) }
                if !report.osVersion.isEmpty { LabeledContent("OS", value: report.osVersion) }
                Text(report.summary)
            }
            Section {
                NavigationLink(value: Route.compose(ComposeTarget(text: report.agentPrompt()))) {
                    Label("Send to agent", systemImage: "sparkles")
                }
                Button {
                    Task {
                        guard let api = InsightsFeature.api else { return }
                        let result = await api.submitBugReport(
                            message: report.agentPrompt(), screen: "crash-reports", severity: "high", platform: "ios",
                            appVersion: report.appVersion, osVersion: report.osVersion)
                        sent = result.ok
                    }
                } label: {
                    Label(sent ? "Reported" : "Report to ForkMesh", systemImage: sent ? "checkmark" : "paperplane")
                }
                .disabled(sent)
                ShareLink(item: report.agentPrompt()) { Label("Share", systemImage: "square.and.arrow.up") }
            }
            if !report.detail.isEmpty {
                Section("Call stacks") {
                    Text(report.detail).font(.caption2.monospaced()).textSelection(.enabled)
                }
            }
            Section {
                Button("Delete report", role: .destructive) {
                    onDelete()
                    dismiss()
                }
            }
        }
        .navigationTitle(report.kind.title)
        .navigationBarTitleDisplayMode(.inline)
    }
}
