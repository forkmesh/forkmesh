import Charts
import FMApi
import FMInsights
import SwiftUI

struct StatsDestination: View {
    var body: some View {
        if let model = StatsDestination.makeModel() {
            StatsScreen(model: model)
        } else {
            InsightsUnavailableView(title: "Stats are unavailable", systemImage: "chart.bar",
                                    message: "Sign in to see the network and your desktop at a glance.")
        }
    }
}

extension StatsDestination {
    @MainActor
    static func makeModel() -> StatsBoardModel? {
        guard let api = InsightsFeature.api, let store = InsightsFeature.statsLayoutStore() else { return nil }
        var desktop: (@MainActor () async throws -> DesktopHealth)?
        var codemagic: (@MainActor () async throws -> CodemagicUsage)?
        if let client = InsightsFeature.desktopClient {
            desktop = { @MainActor () async throws -> DesktopHealth in try await client.health() }
            codemagic = { @MainActor () async throws -> CodemagicUsage in try await client.codemagic() }
        }
        return StatsBoardModel(transport: api, layoutStore: store, desktop: desktop, codemagic: codemagic,
                               history: InsightsFeature.statsHistoryStore())
    }
}

struct StatsScreen: View {
    @State var model: StatsBoardModel
    @State private var editing = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                if !model.failures.isEmpty {
                    InsightsBanner(
                        text: "Couldn't load \(model.failures.joined(separator: ", ")). Pull to retry.",
                        systemImage: "exclamationmark.triangle")
                }
                if model.visible.isEmpty {
                    ContentUnavailableView {
                        Label("No tiles", systemImage: "square.grid.2x2")
                    } description: {
                        Text("Your board is empty.")
                    } actions: {
                        Button("Add tiles") { editing = true }.buttonStyle(.borderedProminent)
                    }
                }
                StatsGridLayout(columns: StatsGridLayout.columns, spacing: 6) {
                    ForEach(model.visible) { placement in
                        StatsTileView(entry: StatsCatalog.entry(placement.id), data: model.data(placement.id),
                                      loading: model.isLoading(placement.id), wide: placement.width == 2)
                            .layoutValue(key: StatsSpanKey.self, value: placement.width)
                    }
                }
                if !model.desktopNote.isEmpty {
                    InsightsBanner(text: model.desktopNote, systemImage: "desktopcomputer", tint: .secondary)
                }
                StatsDesktopLinks()
                if let updated = model.updatedAt {
                    Text("Updated \(updated.formatted(date: .omitted, time: .shortened))")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
        }
        .navigationTitle("Stats")
        .navigationBarTitleDisplayMode(.large)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button("Edit") { editing = true }
            }
        }
        .fmRefreshable { await model.reload() }
        .task { await model.reload() }
        .sheet(isPresented: $editing) {
            StatsEditSheet(model: model)
        }
    }
}

struct StatsSpanKey: LayoutValueKey {
    static let defaultValue = 1
}

struct StatsGridLayout: Layout {
    static let columns = 3

    var columns: Int
    var spacing: CGFloat

    private func pack(_ subviews: Subviews) -> [[Int]] {
        var out: [[Int]] = []
        var row: [Int] = []
        var used = 0
        for index in subviews.indices {
            let span = min(max(subviews[index][StatsSpanKey.self], 1), columns)
            if used + span > columns, !row.isEmpty {
                out.append(row)
                row = []
                used = 0
            }
            row.append(index)
            used += span
        }
        if !row.isEmpty { out.append(row) }
        return out
    }

    private func cellWidth(_ total: CGFloat) -> CGFloat {
        max(0, (total - spacing * CGFloat(columns - 1)) / CGFloat(columns))
    }

    private func width(_ span: Int, cell: CGFloat) -> CGFloat {
        cell * CGFloat(span) + spacing * CGFloat(span - 1)
    }

    private func rowHeights(_ rows: [[Int]], _ subviews: Subviews, cell: CGFloat) -> [CGFloat] {
        rows.map { row in
            row.map { index in
                let span = min(max(subviews[index][StatsSpanKey.self], 1), columns)
                return subviews[index].sizeThatFits(ProposedViewSize(width: width(span, cell: cell), height: nil)).height
            }.max() ?? 0
        }
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let total = proposal.width ?? 360
        let rows = pack(subviews)
        let heights = rowHeights(rows, subviews, cell: cellWidth(total))
        let height = heights.reduce(0, +) + spacing * CGFloat(max(rows.count - 1, 0))
        return CGSize(width: total, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let cell = cellWidth(bounds.width)
        let rows = pack(subviews)
        let heights = rowHeights(rows, subviews, cell: cell)
        var y = bounds.minY
        for (row, height) in zip(rows, heights) {
            var x = bounds.minX
            for index in row {
                let span = min(max(subviews[index][StatsSpanKey.self], 1), columns)
                let w = width(span, cell: cell)
                subviews[index].place(at: CGPoint(x: x, y: y), anchor: .topLeading,
                                      proposal: ProposedViewSize(width: w, height: height))
                x += w + spacing
            }
            y += height + spacing
        }
    }
}

struct StatsTileView: View {
    let entry: StatsTileCatalogEntry?
    let data: StatsTileData?
    var loading = false
    var wide = false

    private var status: StatsTileStatus {
        guard let data, data.available else { return StatsTileStatus.none }
        return data.status
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            HStack(spacing: 3) {
                Image(systemName: entry?.symbol ?? "square")
                    .font(.system(size: 9, weight: .semibold))
                Text(entry?.title ?? "")
                    .font(.caption2.weight(.semibold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.75)
                Spacer(minLength: 0)
                if loading && data != nil {
                    ProgressView().controlSize(.mini)
                } else if status != StatsTileStatus.none {
                    Image(systemName: status.symbol)
                        .font(.system(size: 9, weight: .bold))
                        .foregroundStyle(status.color)
                }
            }
            .foregroundStyle(.secondary)
            if let data, data.available {
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .firstTextBaseline, spacing: 4) {
                        valueText(data)
                        trendChip(data)
                    }
                    VStack(alignment: .leading, spacing: 1) {
                        valueText(data)
                        trendChip(data)
                    }
                }
                if !data.note.isEmpty {
                    Text(data.note)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .minimumScaleFactor(0.85)
                }
                Spacer(minLength: 0)
                if data.series.values.count >= 2 {
                    StatsSparkline(series: data.series, status: data.status)
                        .frame(height: wide ? 24 : 16)
                        .padding(.top, 2)
                }
            } else if data == nil && loading {
                ProgressView()
                    .controlSize(.small)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.top, 4)
                Spacer(minLength: 0)
            } else {
                Text("No data")
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(.tertiary)
                let note = data?.note ?? ""
                Text(note.isEmpty ? (data == nil ? "could not load" : "nothing yet") : note)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .minimumScaleFactor(0.85)
                Spacer(minLength: 0)
            }
        }
        .padding(.leading, status == StatsTileStatus.none ? 7 : 9)
        .padding(.trailing, 6)
        .padding(.vertical, 6)
        .frame(maxWidth: .infinity, minHeight: 66, maxHeight: .infinity, alignment: .topLeading)
        .background {
            ZStack(alignment: .leading) {
                Color(.secondarySystemGroupedBackground)
                if status != StatsTileStatus.none {
                    status.color.opacity(0.1)
                    status.color.frame(width: 3)
                }
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 9, style: .continuous))
        .overlay {
            if status != StatsTileStatus.none {
                RoundedRectangle(cornerRadius: 9, style: .continuous)
                    .strokeBorder(status.color.opacity(0.55), lineWidth: 1)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(accessibilityText)
    }

    private func valueText(_ data: StatsTileData) -> some View {
        Text(data.value.isEmpty ? "\u{2013}" : data.value)
            .font(.system(.callout, design: .rounded).weight(.bold).monospacedDigit())
            .lineLimit(1)
            .minimumScaleFactor(0.6)
    }

    @ViewBuilder
    private func trendChip(_ data: StatsTileData) -> some View {
        if let percent = data.shownTrend {
            let tone = data.trendTone == StatsTileStatus.none ? Color.secondary : data.trendTone.color
            Text(StatsFormat.trendText(percent))
                .font(.system(size: 9, weight: .semibold).monospacedDigit())
                .foregroundStyle(tone)
                .padding(.horizontal, 4)
                .padding(.vertical, 1)
                .background(Capsule().fill(tone.opacity(0.16)))
                .fixedSize()
        }
    }

    private var accessibilityText: String {
        var parts = [entry?.title ?? ""]
        if let data, data.available {
            parts.append(data.value)
            if !data.note.isEmpty { parts.append(data.note) }
            if !data.status.spoken.isEmpty { parts.append(data.status.spoken) }
            if let percent = data.shownTrend {
                parts.append(String(format: "trend %@ %.0f percent", percent >= 0 ? "up" : "down", abs(percent)))
            }
        } else if loading {
            parts.append("loading")
        } else {
            parts.append("no data yet")
            if let note = data?.note, !note.isEmpty { parts.append(note) }
        }
        return parts.joined(separator: ", ")
    }
}

struct StatsSparkline: View {
    let series: StatsTileSeries
    let status: StatsTileStatus

    var body: some View {
        let values = series.values
        let last = values.count - 1
        Chart {
            if series.categorical {
                ForEach(values.indices, id: \.self) { index in
                    BarMark(x: .value("Point", index), y: .value("Value", values[index]))
                        .foregroundStyle(Color.accentColor.gradient)
                }
            } else {
                ForEach(values.indices, id: \.self) { index in
                    AreaMark(x: .value("Point", index), yStart: .value("Base", baseline),
                             yEnd: .value("Value", values[index]))
                        .foregroundStyle(Color.accentColor.opacity(0.18))
                    LineMark(x: .value("Point", index), y: .value("Value", values[index]))
                        .foregroundStyle(Color.secondary)
                        .lineStyle(StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                }
                PointMark(x: .value("Point", last), y: .value("Value", values[last]))
                    .symbolSize(14)
                    .foregroundStyle(status == StatsTileStatus.none ? Color.accentColor : status.color)
            }
        }
        .chartXAxis(.hidden)
        .chartYAxis(.hidden)
        .chartYScale(domain: domain)
        .chartLegend(.hidden)
        .accessibilityHidden(true)
    }

    private var baseline: Double { series.categorical ? 0 : domain.lowerBound }

    private var domain: ClosedRange<Double> {
        let low = series.values.min() ?? 0
        let high = series.values.max() ?? 1
        if series.categorical { return 0...max(high, 1) }
        if high - low < 1e-9 { return (low - 1)...(high + 1) }
        let pad = (high - low) * 0.1
        return (low - pad)...(high + pad)
    }
}

struct StatsEditSheet: View {
    @Bindable var model: StatsBoardModel
    @Environment(\.dismiss) private var dismiss
    @State private var confirmReset = false

    var body: some View {
        NavigationStack {
            List {
                Section("On the board") {
                    ForEach(model.visible) { placement in
                        HStack {
                            Label(StatsCatalog.entry(placement.id)?.title ?? placement.id,
                                  systemImage: StatsCatalog.entry(placement.id)?.symbol ?? "square")
                            Spacer()
                            Button(placement.width == 2 ? "Wide" : "Square") { model.toggleWide(placement.id) }
                                .buttonStyle(.bordered)
                                .font(.caption)
                                .accessibilityLabel(placement.width == 2 ? "Make square" : "Make wide")
                        }
                    }
                    .onMove { model.move(fromOffsets: $0, toOffset: $1) }
                    .onDelete { offsets in
                        let ids = offsets.map { model.visible[$0].id }
                        for id in ids { model.remove(id) }
                    }
                }
                if !model.addable.isEmpty {
                    Section("Add") {
                        ForEach(model.addable) { entry in
                            Button {
                                model.add(entry.id)
                            } label: {
                                VStack(alignment: .leading, spacing: 2) {
                                    Label(entry.title, systemImage: entry.symbol)
                                    Text(entry.hint).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
                Section {
                    Button("Reset to defaults", role: .destructive) { confirmReset = true }
                }
            }
            .environment(\.editMode, .constant(.active))
            .navigationTitle("Edit tiles")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
            .confirmationDialog("Reset the board?", isPresented: $confirmReset, titleVisibility: .visible) {
                Button("Reset", role: .destructive) { model.reset() }
            }
        }
    }
}

struct StatsDesktopLinks: View {
    var body: some View {
        VStack(spacing: 0) {
            NavigationLink {
                DesktopTimelineScreen()
                    .shellPageClearance()
            } label: {
                linkRow("Desktop log, 30 days", "chart.xyaxis.line")
            }
            Divider()
            NavigationLink {
                SentryIssuesScreen()
                    .shellPageClearance()
            } label: {
                linkRow("Sentry issues", "ladybug")
            }
            Divider()
            NavigationLink {
                CrashReportsScreen()
                    .shellPageClearance()
            } label: {
                linkRow("Crash and hang reports", "exclamationmark.bubble")
            }
            Divider()
            NavigationLink(value: Route.network(section: nil)) {
                linkRow("Network and nodes", "point.3.connected.trianglepath.dotted")
            }
            Divider()
            NavigationLink(value: Route.status) {
                linkRow("Status", "waveform.path.ecg")
            }
        }
        .background(RoundedRectangle(cornerRadius: 14).fill(Color(.secondarySystemGroupedBackground)))
    }

    private func linkRow(_ title: String, _ symbol: String) -> some View {
        HStack {
            Label(title, systemImage: symbol)
            Spacer()
            Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
        }
        .padding(.horizontal, 14)
        .frame(minHeight: 44)
        .contentShape(Rectangle())
    }
}
