import Charts
import FMApi
import FMInsights
import SwiftUI

struct NetworkDestination: View {
    let section: String?

    var body: some View {
        if let api = InsightsFeature.api {
            NetworkScreen(model: NetworkScreenModel(
                transport: api, section: NetworkSection(route: section), livePeers: { InsightsFeature.livePeers() }))
        } else {
            InsightsUnavailableView(title: "Network is unavailable", systemImage: "point.3.connected.trianglepath.dotted",
                                    message: "Choose a server first.")
        }
    }
}

struct NetworkScreen: View {
    @State var model: NetworkScreenModel

    var body: some View {
        List {
            Section {
                Picker("Section", selection: $model.section) {
                    ForEach(NetworkSection.allCases, id: \.self) { section in
                        Text(section.title).tag(section)
                    }
                }
                .pickerStyle(.segmented)
                .listRowBackground(Color.clear)
            }
            switch model.section {
            case .nodes: nodes
            case .relays: relays
            case .endpoints: endpoints
            case .activity: activity
            }
        }
        .searchable(text: $model.query, prompt: "Search nodes")
        .navigationTitle("Network")
        .navigationBarTitleDisplayMode(.large)
        .fmRefreshable { await model.reload() }
        .task { await model.reload() }
    }

    @ViewBuilder private var nodes: some View {
        if let snapshot = model.snapshot.value {
            Section {
                LabeledContent("Online", value: "\(snapshot.onlineNodes.count)")
                LabeledContent("Hosts", value: "\(snapshot.hosts)")
                LabeledContent("Repositories", value: StatsFormat.compactNumber(Double(snapshot.repos)))
                if model.warningCount > 0 {
                    InsightsBanner(text: "\(model.warningCount) node\(model.warningCount == 1 ? "" : "s") report health warnings",
                                   systemImage: "stethoscope")
                }
            }
        } else if let error = model.snapshot.error {
            Section { InsightsBanner(text: "Couldn't load the relay's list: \(error.message)", systemImage: "exclamationmark.triangle") }
        }
        Section("Nodes") {
            if model.snapshot.isLoading && model.nodes.isEmpty {
                InsightsLoadingRows(rows: 5)
            } else if model.nodes.isEmpty {
                Text(model.query.isEmpty ? "No nodes are online." : "No node matches.")
                    .foregroundStyle(.secondary)
            }
            ForEach(model.nodes) { node in
                NavigationLink {
                    NodeDetailView(node: node)
                        .shellPageClearance()
                } label: {
                    NodeRowView(node: node)
                }
            }
        }
    }

    @ViewBuilder private var relays: some View {
        let rows = InsightsFeature.relayRows()
        Section("Relays on this phone") {
            if rows.isEmpty { Text("No relays.").foregroundStyle(.secondary) }
            ForEach(rows) { relay in
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text(relay.label).font(.body.weight(relay.active ? .semibold : .regular))
                        if relay.active {
                            Text("Active").font(.caption2.weight(.semibold))
                                .padding(.horizontal, 6).padding(.vertical, 2)
                                .background(Capsule().fill(Color.accentColor.opacity(0.15)))
                        }
                    }
                    Text(relay.host).font(.caption.monospaced()).foregroundStyle(.secondary)
                    if relay.verifiedAt > 0 {
                        Text("Verified \(InsightsTime.relative(relay.verifiedAt))").font(.caption).foregroundStyle(.secondary)
                    }
                    if !relay.missing.isEmpty {
                        Text("Not offered: \(relay.missing.joined(separator: ", "))")
                            .font(.caption).foregroundStyle(.orange)
                            .fmMessageTools("Not offered: \(relay.missing.joined(separator: ", "))")
                    }
                }
                .accessibilityElement(children: .combine)
            }
        }
    }

    @ViewBuilder private var endpoints: some View {
        let rows = EndpointRow.forRelay(serverUrl: InsightsFeature.activeServerUrl)
        Section("Endpoints this phone uses") {
            if rows.isEmpty { Text("No server is configured.").foregroundStyle(.secondary) }
            ForEach(rows) { endpoint in
                VStack(alignment: .leading, spacing: 2) {
                    Text(endpoint.name).font(.body.weight(.semibold))
                    Text(endpoint.url).font(.caption.monospaced()).textSelection(.enabled)
                    Text(endpoint.purpose).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    @ViewBuilder private var activity: some View {
        switch model.history {
        case .idle, .loading:
            Section { InsightsLoadingRows(rows: 3) }
        case .failed(let error):
            Section { InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle") }
        case .loaded(let history):
            Section("Node time online, last 24 hours") {
                Chart(history.hours) { hour in
                    BarMark(x: .value("Hour", InsightsTime.date(hour.hourTs), unit: .hour),
                            y: .value("Node minutes", hour.nodeMinutes))
                }
                .frame(height: 180)
                .accessibilityLabel("Node minutes online per hour")
                if let peak = history.peakHour {
                    LabeledContent("Busiest hour",
                                   value: InsightsTime.date(peak.hourTs).formatted(date: .omitted, time: .shortened))
                }
            }
            Section("Most online") {
                ForEach(history.nodes.prefix(10)) { node in
                    LabeledContent(node.label.isEmpty ? node.id : node.label,
                                   value: StatsFormat.compactDuration(Int64(node.totalMinutes) * 60_000))
                }
            }
        }
    }
}

struct NodeRowView: View {
    let node: NodeRow

    var body: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(node.online ? Color.green : Color.gray)
                .frame(width: 8, height: 8)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                HStack {
                    Text(node.name).lineLimit(1)
                    if node.isSelf { Text("this phone").font(.caption2).foregroundStyle(.secondary) }
                }
                Text(node.subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            if node.worst >= .warning {
                Image(systemName: node.worst.tone.symbol).foregroundStyle(node.worst.tone.color)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(node.accessibilityLabel)
    }
}

struct NodeDetailView: View {
    let node: NodeRow

    var body: some View {
        List {
            Section {
                LabeledContent("State", value: node.online ? "Online" : "Offline")
                LabeledContent("Platform", value: node.platformLabel)
                if !node.version.isEmpty { LabeledContent("Version", value: node.version) }
                if !node.owner.isEmpty { LabeledContent("Owner", value: node.owner) }
                LabeledContent("Mirrors", value: "\(node.mirrors)")
            }
            Section("Self-diagnostics") {
                if node.diagnostics.isEmpty {
                    Text("This node has not reported any findings.").foregroundStyle(.secondary)
                }
                ForEach(node.diagnostics) { finding in
                    Label {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(finding.message)
                            Text("\(finding.severity.label) · \(finding.id)").font(.caption).foregroundStyle(.secondary)
                        }
                    } icon: {
                        Image(systemName: finding.severity.tone.symbol).foregroundStyle(finding.severity.tone.color)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(finding.message, alignment: .top)
                }
            }
            if !node.owner.isEmpty {
                Section {
                    NavigationLink(value: Route.profile(name: node.owner)) {
                        Label("Owner's profile", systemImage: "person.crop.circle")
                    }
                }
            }
        }
        .navigationTitle(node.name)
        .navigationBarTitleDisplayMode(.inline)
    }
}
