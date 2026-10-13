// The Diagnostics view behind the telemetry strip. Pick a range, read each metric as a square chart
// (drag across one to read any point), or filter the log by family. History is this launch only.
// State: FMTools' TelemetryScreenModel.

import FMBase
import FMTools
import SwiftUI
import UIKit

struct TelemetryScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: TelemetryScreenModel?
    @State private var notice: FmNotice?

    init(focusMetric: TelemetryMetric? = nil, logCategory: LogCategory? = nil) {
        if let telemetry = Services.get(AppTelemetry.self) {
            _model = State(initialValue: TelemetryScreenModel(telemetry: telemetry, focusMetric: focusMetric, logCategory: logCategory))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(TelemetryScreenModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    FmIconButton(icon: "copy_all_outlined", label: "Copy the logs shown", onPressed: copyLogs)
                }
            }
            .fmPreGlassBackground(fm.bgBase)
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            VStack(spacing: 0) {
                TelemetryRangeBar(model: model)
                FmSegmentTabBar(
                    tabs: [FmSegmentTab(label: "Stats"), FmSegmentTab(label: "Logs")],
                    index: model.tab,
                    onChanged: { (index: Int) in model.tab = index })
                if notice != nil {
                    FmNoticeLine($notice)
                        .padding(.horizontal, FmSpace.x4)
                }
                if model.tab == 0 {
                    TelemetryStatsView(model: model)
                } else {
                    TelemetryLogsView(model: model, notice: $notice)
                }
            }
        } else {
            FmEmptyState(icon: "speed_rounded", title: "Diagnostics are unavailable")
        }
    }

    private func copyLogs() {
        guard let model = model else {
            return
        }
        let copied: (text: String, confirmation: String) = model.copyText()
        UIPasteboard.general.string = copied.text
        notice = FmNotice(copied.confirmation)
    }
}

/// Every range in one full-width segmented row, so none hides off the edge of a narrow phone.
private struct TelemetryRangeBar: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: TelemetryScreenModel

    var body: some View {
        HStack(spacing: 0) {
            ForEach(TelemetryRange.allCases, id: \.self) { (range: TelemetryRange) in
                segment(range)
            }
        }
        .padding(2)
        .frame(height: 36)
        .shellGlass(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x1)
    }

    private func segment(_ range: TelemetryRange) -> some View {
        let selected: Bool = range == model.range
        return Button(action: { model.range = range }) {
            Text(range.label)
                .fmText(.labelMedium, color: selected ? fm.textPrimary : fm.textTertiary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(selected ? fm.bgRaised : Color.clear)
                .clipShape(FmRoundedShape(radius: FmRadius.sm + 2))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
        .accessibilityIdentifier("telemetry-range-\(range.label)")
    }
}

private struct TelemetryStatsView: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: TelemetryScreenModel

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            ScrollView {
                grid(width: proxy.size.width - FmSpace.x4 * 2)
                    .padding(FmSpace.x4)
            }
        }
    }

    private func grid(width: CGFloat) -> some View {
        let gap: CGFloat = FmSpace.x3
        let columns: Int = TelemetryScreenModel.columns(width: Double(width))
        let side: CGFloat = max(40, (width - gap * CGFloat(columns - 1)) / CGFloat(columns))
        let items: [GridItem] = Array(repeating: GridItem(.fixed(side), spacing: gap), count: columns)
        return VStack(alignment: .leading, spacing: gap) {
            Text(model.uptimeLine)
                .fmText(.bodySmall, color: fm.textTertiary)
            if let focus = model.focus {
                TelemetryMetricCard(model: model, metric: focus, large: true, selected: false) {
                    model.focus = nil
                }
                .frame(height: 240)
            }
            LazyVGrid(columns: items, alignment: .leading, spacing: gap) {
                ForEach(TelemetryMetric.allCases, id: \.self) { (metric: TelemetryMetric) in
                    TelemetryMetricCard(model: model, metric: metric, large: false, selected: metric == model.focus) {
                        model.focus = metric
                    }
                    .frame(width: side, height: side)
                }
            }
        }
    }
}

/// One metric: its title, the latest (or scrubbed) value, the chart and a summary line.
private struct TelemetryMetricCard: View {
    @Environment(\.fmPalette) private var fm
    let model: TelemetryScreenModel
    let metric: TelemetryMetric
    let large: Bool
    let selected: Bool
    let onTap: () -> Void
    @State private var scrub: Int?

    var body: some View {
        let card: MetricCardState = model.card(metric)
        return VStack(alignment: .leading, spacing: 0) {
            Text(metric.title)
                .fmText(.labelMedium, color: fm.textSecondary)
                .lineLimit(1)
            HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1) {
                Text(card.value(scrub: scrub))
                    .fmText(large ? .titleLarge : .titleMedium)
                    .lineLimit(1)
                Text(card.when(scrub: scrub))
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            if large {
                Text(metric.description)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .lineLimit(1)
            }
            chart(card)
                .padding(.vertical, FmSpace.x2)
            Text(card.footer)
                .fmText(.bodySmall, color: fm.textTertiary)
                .lineLimit(2)
        }
        .padding(FmSpace.x3)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .overlay {
            FmRoundedShape(radius: FmRadius.md).strokeBorder(selected ? fm.accent : fm.border, lineWidth: 1)
        }
        .contentShape(Rectangle())
        .onTapGesture(perform: onTap)
        .accessibilityIdentifier(large ? "telemetry-focus-\(metric.label)" : "telemetry-card-\(metric.label)")
    }

    private func chart(_ card: MetricCardState) -> some View {
        GeometryReader { (proxy: GeometryProxy) in
            TelemetryChart(
                values: card.points, slots: card.slots, floor: metric.floor,
                line: fm.accent, fill: fm.accentSubtle, grid: fm.border,
                scrubIndex: scrub, surface: fm.bgRaised)
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 4)
                        .onChanged { (value: DragGesture.Value) in
                            scrub = card.scrubIndex(x: Double(value.location.x), width: Double(proxy.size.width))
                        }
                        .onEnded { (_: DragGesture.Value) in
                            scrub = nil
                        }
                )
        }
    }
}

private struct TelemetryLogsView: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: TelemetryScreenModel
    @Binding var notice: FmNotice?

    var body: some View {
        let entries: [AppLogEntry] = model.visibleEntries
        return VStack(spacing: 0) {
            chips
            captionRow
            if entries.isEmpty {
                Text(model.emptyText)
                    .fmText(.bodyMedium, color: fm.textTertiary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    ForEach(Array(entries.enumerated()), id: \.offset) { (pair: (offset: Int, element: AppLogEntry)) in
                        TelemetryLogRow(entry: pair.element, notice: $notice)
                            .listRowInsets(EdgeInsets.fmSymmetric(horizontal: FmSpace.x4, vertical: 6))
                            .listRowSeparator(.hidden)
                            .listRowBackground(Color.clear)
                    }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
            }
        }
    }

    private var chips: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                chip(label: model.allChipLabel, icon: nil, ink: fm.textPrimary, selected: model.allSelected) {
                    model.selectAll()
                }
                ForEach(LogCategory.allCases, id: \.self) { (category: LogCategory) in
                    chip(
                        label: model.chipLabel(category), icon: category.icon,
                        ink: LogCategoryInk.of(category, count: model.counts[category.index]).color(fm),
                        selected: model.isSelected(category)
                    ) {
                        model.setCategory(category, on: !model.isSelected(category))
                    }
                }
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x1)
        }
        .frame(height: 48)
    }

    private func chip(label: String, icon: String?, ink: Color, selected: Bool, onTap: @escaping () -> Void) -> some View {
        Button(action: onTap) {
            HStack(spacing: FmSpace.x1) {
                if let icon = icon {
                    FmIcon(icon, size: 16, color: ink)
                }
                Text(label)
                    .fmText(.labelMedium, color: fm.textPrimary)
            }
            .padding(.horizontal, FmSpace.x3)
            .frame(height: 32)
            .background(selected ? fm.accentSubtle : Color.clear, in: RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
            .shellGlass(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
            .overlay {
                FmRoundedShape(radius: FmRadius.md).strokeBorder(selected ? fm.accent : Color.clear, lineWidth: 1)
            }
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
    }

    private var captionRow: some View {
        HStack(spacing: FmSpace.x2) {
            Text(model.caption)
                .fmText(.bodySmall, color: fm.textTertiary)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmIconButton(icon: model.paused ? "play_arrow_rounded" : "pause_rounded", label: model.pauseLabel) {
                model.togglePause()
            }
        }
        .padding(.leading, FmSpace.x4)
        .padding(.trailing, FmSpace.x2)
        .padding(.bottom, FmSpace.x1)
    }
}

private struct TelemetryLogRow: View {
    @Environment(\.fmPalette) private var fm
    let entry: AppLogEntry
    @Binding var notice: FmNotice?

    private var iconColor: Color {
        let ink: LogCategoryInk = LogCategoryInk.of(entry.category, count: 1)
        return ink == .primary ? fm.textSecondary : ink.color(fm)
    }

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            Text(entry.stamp())
                .fmMono(size: FmType.meta, color: fm.textTertiary)
            FmIcon(entry.category.icon, size: 14, color: iconColor, label: entry.category.label)
                .padding(.top, 1)
            Text(entry.message)
                .fmMono(size: FmType.meta, color: fm.textPrimary)
                .lineLimit(8)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .contentShape(Rectangle())
        .onLongPressGesture {
            UIPasteboard.general.string = entry.description
            notice = FmNotice("Copied log line")
        }
    }
}
