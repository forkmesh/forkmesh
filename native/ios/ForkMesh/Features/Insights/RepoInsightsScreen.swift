import Charts
import FMApi
import FMInsights
import SwiftUI

struct RepoInsightsScreen: View {
    let repo: RepoRef
    @State private var model: RepoInsightsModel?

    var body: some View {
        Group {
            if let model {
                RepoInsightsContent(model: model)
            } else {
                InsightsUnavailableView(title: "Insights are unavailable", systemImage: "chart.bar.xaxis",
                                        message: "Sign in to see this repository's activity.")
            }
        }
        .navigationTitle("Insights")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            if model == nil, let api = InsightsFeature.api { model = RepoInsightsModel(repo: repo, api: api) }
        }
    }
}

private struct RepoInsightsContent: View {
    @Bindable var model: RepoInsightsModel

    var body: some View {
        content
            .fmRefreshable { await model.reload() }
            .task { await model.reload() }
    }

    @ViewBuilder private var content: some View {
        switch model.summary {
        case .idle, .loading:
            List { InsightsLoadingRows(rows: 6) }
        case .failed(let error):
            InsightsErrorView(title: "Couldn't load insights", error: error) { await model.reload() }
        case .loaded(let summary):
            List {
                Section {
                    LabeledContent("Open issues", value: "\(summary.openIssues)")
                    LabeledContent("Closed issues", value: "\(summary.closedIssues)")
                    LabeledContent("Open pull requests", value: "\(summary.openPulls)")
                    LabeledContent("Merged pull requests", value: "\(summary.mergedPulls)")
                    if !summary.language.isEmpty { LabeledContent("Language", value: summary.language) }
                    LabeledContent("Stars · forks · mirrors", value: "\(summary.stars) · \(summary.forks) · \(summary.mirrors)")
                }
                Section("Commits, last \(RepoInsightsSummary.weeks) weeks") {
                    Chart(summary.weekly) { week in
                        BarMark(x: .value("Week", InsightsTime.date(week.weekStartMs), unit: .weekOfYear),
                                y: .value("Commits", week.commits))
                    }
                    .frame(height: 160)
                    .accessibilityLabel("\(summary.commitsInWindow) commits in the last \(RepoInsightsSummary.weeks) weeks")
                    Text("\(summary.commitsInWindow) of the \(summary.commitsSeen) commits the server listed")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if !summary.contributors.isEmpty {
                    Section("Top contributors") {
                        ForEach(summary.contributors) { contributor in
                            LabeledContent(contributor.name, value: "\(contributor.commits)")
                        }
                    }
                }
                if !summary.burnup.isEmpty {
                    Section("Issue burnup") {
                        IssueBurnupChart(points: summary.burnup)
                    }
                }
                Section {
                    NavigationLink {
                        RepoQualityScreen(repo: model.repo)
                            .shellPageClearance()
                    } label: {
                        Label("Quality and ratchet (from your desktop)", systemImage: "checkmark.seal")
                    }
                }
            }
        }
    }
}

struct IssueBurnupChart: View {
    let points: [IssueBurnupPoint]

    var body: some View {
        Chart {
            ForEach(points) { point in
                AreaMark(x: .value("Date", InsightsTime.date(point.timestampMs)), y: .value("Issues", point.closedCount),
                         stacking: .standard)
                    .foregroundStyle(by: .value("State", "Closed"))
                AreaMark(x: .value("Date", InsightsTime.date(point.timestampMs)), y: .value("Issues", point.openCount),
                         stacking: .standard)
                    .foregroundStyle(by: .value("State", "Open"))
            }
        }
        .chartForegroundStyleScale(["Closed": Color.purple, "Open": Color.green])
        .frame(height: 180)
        .accessibilityLabel(accessibilityText)
    }

    private var accessibilityText: String {
        guard let last = points.last else { return "No issues" }
        return "Burnup: \(last.openCount) open and \(last.closedCount) closed issues now"
    }
}

struct RepoQualityScreen: View {
    let repo: RepoRef
    @State private var model: DesktopStatsModel?
    @State private var confirming = false

    var body: some View {
        Group {
            if let model {
                RepoQualityContent(model: model, repo: repo)
            } else {
                InsightsUnavailableView(title: "Your desktop is not paired", systemImage: "desktopcomputer",
                                        message: "Quality and ratchet are computed by your desktop. Pair it in Desktops.")
            }
        }
        .navigationTitle("Quality")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            if model == nil, let client = InsightsFeature.desktopClient { model = DesktopStatsModel(client: client) }
        }
    }
}

private struct RepoQualityContent: View {
    @Bindable var model: DesktopStatsModel
    let repo: RepoRef

    var body: some View {
        List {
            Section("Repository stats") {
                switch model.repoStats {
                case .idle, .loading:
                    InsightsLoadingRows(rows: 3)
                case .unsupported:
                    Text("Your desktop does not offer this yet. Update ForkMesh on the desktop.").foregroundStyle(.secondary)
                        .fmMessageTools("Your desktop does not offer this yet. Update ForkMesh on the desktop.")
                case .failed(let message):
                    InsightsBanner(text: message, systemImage: "exclamationmark.triangle")
                case .loaded(let stats):
                    LabeledContent("Commits", value: StatsFormat.compactNumber(Double(stats.commits)))
                    LabeledContent("Files", value: StatsFormat.compactNumber(Double(stats.files)))
                    LabeledContent("Lines", value: StatsFormat.compactNumber(Double(stats.lines)))
                    Toggle("Ratchet mode", isOn: Binding(
                        get: { stats.ratchetEnabled },
                        set: { enabled in Task { await model.prepareRatchet(repo.slug, enabled: enabled) } }))
                    if !stats.ratchetNote.isEmpty { Text(stats.ratchetNote).font(.caption).foregroundStyle(.secondary) }
                }
                if !model.ratchetError.isEmpty {
                    InsightsBanner(text: model.ratchetError, systemImage: "exclamationmark.triangle")
                }
            }
            Section("Code health") {
                switch model.quality {
                case .idle, .loading:
                    InsightsLoadingRows(rows: 4)
                case .unsupported:
                    Text("Your desktop does not offer this yet.").foregroundStyle(.secondary)
                        .fmMessageTools("Your desktop does not offer this yet.")
                case .failed(let message):
                    InsightsBanner(text: message, systemImage: "exclamationmark.triangle")
                case .loaded(let quality):
                    HStack {
                        Text("Score").foregroundStyle(.secondary)
                        Spacer()
                        Text("\(quality.score)\(quality.grade.isEmpty ? "" : " · \(quality.grade)")")
                            .font(.headline.monospacedDigit())
                            .foregroundStyle(quality.tone.color)
                    }
                    ForEach(quality.files.prefix(50)) { file in
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Text(file.path).font(.caption.monospaced()).lineLimit(1).truncationMode(.middle)
                                Spacer()
                                Text("\(file.score)").font(.caption.monospacedDigit())
                            }
                            ForEach(file.findings, id: \.self) { finding in
                                Text(finding).font(.caption2).foregroundStyle(.secondary)
                            }
                        }
                        .accessibilityElement(children: .combine)
                    }
                }
            }
        }
        .fmRefreshable { await model.loadRepo(repo.slug) }
        .task { await model.loadRepo(repo.slug) }
        .confirmationDialog(
            model.pendingRatchet?.summary ?? "Change ratchet mode?",
            isPresented: Binding(get: { model.pendingRatchet != nil }, set: { if !$0 { model.cancelRatchet() } }),
            titleVisibility: .visible
        ) {
            Button("Confirm on desktop") { Task { await model.confirmRatchet() } }
            Button("Cancel", role: .cancel) { model.cancelRatchet() }
        }
    }
}
