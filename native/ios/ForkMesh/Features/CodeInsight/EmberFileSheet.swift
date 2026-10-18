import Charts
import FMCodeInsight
import SwiftUI
import UIKit

struct EmberFileSheet: View {
    let model: EmberPageModel
    let context: CodeInsightContext
    @Environment(\.fmPalette) private var fm
    @State private var cardTab = 0
    @State private var copied = false

    var body: some View {
        NavigationStack {
            Group {
                if model.selected >= 0 && model.selected < model.snapshot.files.count {
                    content(model.selected)
                } else {
                    Color.clear
                }
            }
            .background(fm.sheetSurface)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { model.select(-1, zoom: false) }
                }
            }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
        .presentationBackgroundInteraction(.enabled(upThrough: .medium))
    }

    private func content(_ file: Int) -> some View {
        let path = model.snapshot.files[file].path
        let card = Ember.card(model.snapshot, file: file, rank: model.rank(of: file), now: Int64(Date().timeIntervalSince1970))
        let refs = file < model.coupling.count ? model.coupling[file] : []
        return ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text(card.folder).fmText(.bodySmall, color: fm.textTertiary).lineLimit(2)
                    Text(card.name).fmText(.titleMedium).textSelection(.enabled)
                }
                actions(path)
                FmEdgeFadeScroll {
                    HStack(spacing: FmSpace.x2) {
                        EmberChip(label: "Metrics", active: cardTab == 0) { cardTab = 0 }
                        EmberChip(label: "Complexity trend", active: cardTab == 1) { cardTab = 1 }
                        EmberChip(label: Ember.couplingTabTitle(refs.count), active: cardTab == 2) { cardTab = 2 }
                    }
                }
                switch cardTab {
                case 1:
                    EmberTrendView(model: model, path: path, load: context.loadTrend)
                case 2:
                    coupling(refs)
                default:
                    metrics(card, path: path)
                }
            }
            .padding(FmSpace.x4)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle(card.name)
        .onChange(of: file) { (_: Int, _: Int) in copied = false }
    }

    private func actions(_ path: String) -> some View {
        HStack(spacing: FmSpace.x2) {
            FmFilledButton("Open file", icon: "open_in_new") { open(path, line: 0) }
                .accessibilityIdentifier("ember-open-file")
            FmTextButton(copied ? "Copied" : "Copy path", icon: copied ? "check" : "content_copy") {
                UIPasteboard.general.string = path
                FmHaptics.tap()
                copied = true
            }
            ShareLink(item: path, subject: Text(Ember.fileNameOf(path))) {
                Image(systemName: "square.and.arrow.up")
                    .font(.system(size: 17, weight: .medium))
                    .foregroundStyle(fm.accent)
                    .frame(width: 44, height: 44)
            }
            .accessibilityLabel("Share path")
        }
    }

    private func open(_ path: String, line: Int) {
        model.select(-1, zoom: false)
        context.openFile(path, line)
    }

    private func metrics(_ card: EmberCard, path: String) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            HStack(spacing: FmSpace.x4) {
                EmberHealthRing(health: card.health)
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text("Code health").fmText(.labelMedium, color: fm.textSecondary)
                    FmStatusBadge(label: card.band.name, color: Ember.bandColor(card.band).color)
                    if !card.note.isEmpty {
                        Text(card.note).fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
            }
            VStack(alignment: .leading, spacing: 0) {
                ForEach(card.metrics) { (metric: EmberMetric) in
                    if metric.line > 0 {
                        Button { open(path, line: metric.line) } label: {
                            FmKeyValueRow(label: metric.label, value: metric.value)
                        }
                        .buttonStyle(.plain)
                        .accessibilityHint("Opens the file at line \(metric.line)")
                    } else {
                        FmKeyValueRow(label: metric.label, value: metric.value)
                    }
                }
            }
            FmSectionHeader(title: "Health issues")
            if card.issues.isEmpty {
                Text("No issues found.").fmText(.bodyMedium, color: fm.textSecondary)
            } else {
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    ForEach(Array(card.issues.enumerated()), id: \.offset) { (item: (offset: Int, element: String)) in
                        Text("• " + item.element).fmText(.bodyMedium)
                    }
                }
            }
        }
    }

    private func coupling(_ refs: [EmberCouplingRef]) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(Ember.couplingIntro).fmText(.bodySmall, color: fm.textSecondary)
            if refs.isEmpty {
                Text(Ember.couplingEmptyText(history: model.history)).fmText(.bodyMedium, color: fm.textSecondary)
                    .padding(.top, FmSpace.x2)
            } else {
                ForEach(Ember.couplingRows(model.snapshot, refs)) { (row: EmberRow) in
                    EmberRowView(row: row) { model.select(row.id, zoom: true) }
                }
            }
        }
    }
}

struct EmberTrendView: View {
    let model: EmberPageModel
    let path: String
    let load: (String) async -> [InsightTrendPoint]
    @Environment(\.fmPalette) private var fm
    @State private var points: [InsightTrendPoint]?
    @State private var picked: Int?

    var body: some View {
        Group {
            if !model.history {
                message(Ember.trendNeedsHistory)
            } else if let points {
                if points.isEmpty {
                    message(Ember.trendEmpty)
                } else {
                    charts(points)
                }
            } else {
                HStack(spacing: FmSpace.x2) {
                    ProgressView()
                    Text(Ember.trendLoading(path)).fmText(.bodySmall, color: fm.textSecondary)
                }
                .padding(.vertical, FmSpace.x4)
            }
        }
        .task(id: path) {
            picked = nil
            guard model.history else { return }
            if let cached = model.trends[path] {
                points = cached
                return
            }
            points = nil
            let loaded = await load(path)
            guard !Task.isCancelled else { return }
            model.trends[path] = loaded
            points = loaded
        }
    }

    private func message(_ text: String) -> some View {
        Text(text).fmText(.bodyMedium, color: fm.textSecondary).padding(.vertical, FmSpace.x3)
    }

    private func date(_ time: Int64) -> Date { Date(timeIntervalSince1970: TimeInterval(time)) }

    private func domain(_ points: [InsightTrendPoint]) -> ClosedRange<Date> {
        let first = date(points.first?.time ?? 0)
        let last = date(points.last?.time ?? 0)
        return last > first ? first...last : first.addingTimeInterval(-43_200)...first.addingTimeInterval(43_200)
    }

    private func nearest(_ points: [InsightTrendPoint], to when: Date) -> Int {
        var best = 0
        var gap = Double.infinity
        for (i, point) in points.enumerated() {
            let d = abs(date(point.time).timeIntervalSince(when))
            if d < gap {
                gap = d
                best = i
            }
        }
        return best
    }

    private func charts(_ points: [InsightTrendPoint]) -> some View {
        let shown = min(points.count - 1, picked ?? points.count - 1)
        let range = domain(points)
        return VStack(alignment: .leading, spacing: FmSpace.x3) {
            chart(points, title: "Lines of code", color: fm.dark ? EmberStyle.trendLines : EmberStyle.trendLinesLight,
                  range: range, shown: shown) { Double($0.codeLines) }
            chart(points, title: "Complexity", color: EmberStyle.trendComplexity, range: range, shown: shown) { Double($0.complexity) }
            Text(Ember.trendInfo(points[shown]))
                .fmText(.bodySmall, color: picked == nil ? fm.textSecondary : fm.textPrimary)
                .accessibilityLabel("Selected version: " + Ember.trendInfo(points[shown]).replacingOccurrences(of: "\n", with: ", "))
        }
    }

    private func chart(_ points: [InsightTrendPoint], title: String, color: Color, range: ClosedRange<Date>, shown: Int,
                       value: @escaping (InsightTrendPoint) -> Double) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack {
                Text(title).fmText(.labelMedium, color: fm.textSecondary)
                Spacer()
                Text(Ember.compact(value(points[shown]))).fmText(.labelMedium, tabular: true)
            }
            Chart {
                ForEach(Array(points.enumerated()), id: \.offset) { (item: (offset: Int, element: InsightTrendPoint)) in
                    LineMark(x: .value("Date", date(item.element.time)), y: .value(title, value(item.element)))
                        .foregroundStyle(color)
                        .lineStyle(StrokeStyle(lineWidth: 2))
                        .interpolationMethod(.monotone)
                    PointMark(x: .value("Date", date(item.element.time)), y: .value(title, value(item.element)))
                        .foregroundStyle(color)
                        .symbolSize(item.offset == shown ? 64 : 24)
                        .accessibilityLabel(Ember.dateText(item.element.time))
                        .accessibilityValue("\(Ember.number(Int(value(item.element)))) \(title.lowercased())")
                }
                if picked != nil {
                    RuleMark(x: .value("Date", date(points[shown].time)))
                        .foregroundStyle(fm.textTertiary)
                        .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
                }
            }
            .chartXScale(domain: range)
            .chartXAxis {
                AxisMarks(values: .automatic(desiredCount: 3)) { (_: AxisValue) in
                    AxisGridLine().foregroundStyle(fm.border)
                    AxisValueLabel(format: .dateTime.month(.abbreviated).year())
                }
            }
            .chartYAxis {
                AxisMarks(values: .automatic(desiredCount: 3)) { (axis: AxisValue) in
                    AxisGridLine().foregroundStyle(fm.border)
                    AxisValueLabel {
                        if let number = axis.as(Double.self) { Text(Ember.compact(number)) }
                    }
                }
            }
            .chartOverlay { (proxy: ChartProxy) in
                GeometryReader { (geometry: GeometryProxy) in
                    Rectangle()
                        .fill(Color.clear)
                        .contentShape(Rectangle())
                        .gesture(
                            DragGesture(minimumDistance: 0)
                                .onChanged { (drag: DragGesture.Value) in
                                    guard let frame = proxy.plotFrame else { return }
                                    let x = drag.location.x - geometry[frame].origin.x
                                    if let when: Date = proxy.value(atX: x) { picked = nearest(points, to: when) }
                                }
                        )
                }
            }
            .frame(height: 120)
        }
    }
}
