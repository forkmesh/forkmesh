import FMApi
import FMInsights
import SwiftUI

struct StatusDestination: View {
    var body: some View {
        if let api = InsightsFeature.api {
            StatusScreen(model: StatusScreenModel(transport: api))
        } else {
            InsightsUnavailableView(title: "Status is unavailable", systemImage: "waveform.path.ecg",
                                    message: "Choose a server first.")
        }
    }
}

struct StatusScreen: View {
    @State var model: StatusScreenModel
    @State private var showMirrors = false

    var body: some View {
        content
            .navigationTitle("Status")
            .navigationBarTitleDisplayMode(.large)
            .fmRefreshable { await model.reload() }
            .task { await model.reload() }
    }

    @ViewBuilder private var content: some View {
        switch model.report {
        case .idle, .loading:
            List { InsightsLoadingRows(rows: 6) }
        case .failed(let error):
            InsightsErrorView(title: "Couldn't load status", error: error) { await model.reload() }
        case .loaded(let report):
            List {
                Section {
                    HStack(spacing: 12) {
                        HStack(spacing: 12) {
                            Image(systemName: report.overall.tone.symbol)
                                .font(.title)
                                .foregroundStyle(report.overall.tone.color)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(report.headline).font(.headline)
                                Text(InsightsFeature.hostLabel).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        .accessibilityElement(children: .combine)
                        Spacer(minLength: 0)
                        FmMessageTools(report.headline)
                    }
                    StatusCurrentGrid(current: report.current)
                }
                Section("Systems") {
                    ForEach(report.serviceSystems) { system in
                        NavigationLink {
                            StatusSystemDetail(system: system)
                                .shellPageClearance()
                        } label: {
                            StatusSystemRow(system: system)
                        }
                    }
                }
                if !report.nodeSystems.isEmpty {
                    Section("Nodes") {
                        ForEach(report.nodeSystems) { system in
                            NavigationLink {
                                StatusSystemDetail(system: system)
                                    .shellPageClearance()
                            } label: {
                                StatusSystemRow(system: system)
                            }
                        }
                    }
                }
                if !report.mirrorSystems.isEmpty {
                    Section {
                        DisclosureGroup("Mirrors (\(report.mirrorSystems.count))", isExpanded: $showMirrors) {
                            ForEach(report.mirrorSystems) { system in
                                NavigationLink {
                                    StatusSystemDetail(system: system)
                                        .shellPageClearance()
                                } label: {
                                    StatusSystemRow(system: system)
                                }
                            }
                        }
                    }
                }
                if InsightsFeature.isAdmin {
                    Section {
                        NavigationLink(value: Route.admin(section: AdminSection.errors.rawValue)) {
                            Label("Error console", systemImage: "exclamationmark.octagon")
                        }
                    }
                }
            }
        }
    }
}

struct StatusCurrentGrid: View {
    let current: StatusCurrent

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            row("Repositories", current.catalogRepos.map(String.init) ?? "–")
            row("Nodes online", current.onlineNodes.map(String.init) ?? "–")
            row("Errors, 24 h", current.errors24h.map(String.init) ?? "–")
            if let online = current.mainnodeOnline {
                row("Verified mirror", online ? "online" : "offline")
            }
            if current.lastSampleTs > 0 {
                row("Last check", InsightsTime.relative(current.lastSampleTs))
            }
        }
        .font(.subheadline)
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).foregroundStyle(.secondary)
            Spacer()
            Text(value).monospacedDigit()
        }
        .accessibilityElement(children: .combine)
    }
}

struct StatusSystemRow: View {
    let system: StatusSystem

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Image(systemName: system.state.tone.symbol).foregroundStyle(system.state.tone.color)
                Text(system.label).lineLimit(2)
                Spacer()
                Text(StatsFormat.percent(system.uptime24h, digits: 2))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
            StatusMinuteStrip(minutes: system.minutes)
            if !system.reason.isEmpty {
                Text(system.reason).font(.caption).foregroundStyle(.red).lineLimit(2)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(system.label), \(system.state.label), \(StatsFormat.percent(system.uptime24h, digits: 2)) uptime today")
    }
}

struct StatusMinuteStrip: View {
    let minutes: [StatusMinute]

    var body: some View {
        HStack(spacing: 1) {
            ForEach(minutes) { minute in
                RoundedRectangle(cornerRadius: 1)
                    .fill(color(minute.state))
                    .frame(height: 10)
            }
        }
        .accessibilityHidden(true)
    }

    private func color(_ state: ServiceState) -> Color {
        switch state {
        case .operational: return .green
        case .degraded: return .orange
        case .down: return .red
        case .unknown: return Color(.systemGray4)
        case .future: return Color(.systemGray6)
        }
    }
}

struct StatusSystemDetail: View {
    let system: StatusSystem

    var body: some View {
        List {
            Section {
                LabeledContent("State", value: system.state.label)
                LabeledContent("Uptime, 24 h", value: StatsFormat.percent(system.uptime24h, digits: 2))
                if !system.isNode {
                    LabeledContent("Uptime, 30 days", value: StatsFormat.percent(system.uptime30d, digits: 2))
                }
                LabeledContent("Checks, 24 h", value: "\(system.checks24h)")
                LabeledContent("Failures, 24 h", value: "\(system.failures24h)")
                if system.observedAt > 0 {
                    LabeledContent("Observed", value: InsightsTime.relative(system.observedAt))
                }
            }
            if !system.reason.isEmpty {
                Section("Latest failure") {
                    Text(system.reason)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(system.reason)
                    if system.reasonTs > 0 {
                        Text(InsightsTime.relative(system.reasonTs)).font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            if !system.detail.isEmpty {
                Section("What is checked") { Text(system.detail).font(.footnote) }
            }
            Section("Last hour") { StatusMinuteStrip(minutes: system.minutes) }
        }
        .navigationTitle(system.label)
        .navigationBarTitleDisplayMode(.inline)
    }
}
