import FMBase
import SwiftUI

struct TodayActivityCard: View {
    @Environment(\.fmPalette) private var fm
    let counts: [Int]
    let nowSlot: Int
    var shown = true
    var otherKinds: [String] = []
    var live = false
    @State private var synced: [Int]?
    @State private var syncedKinds: [String]?

    init(counts: [Int], nowSlot: Int, shown: Bool = true, otherKinds: [String] = []) {
        self.counts = counts
        self.nowSlot = nowSlot
        self.shown = shown
        self.otherKinds = otherKinds
    }

    @MainActor init(shown: Bool = true) {
        let log = FmUserActivity.shared.log
        self.init(counts: log.counts(), nowSlot: log.nowSlot, shown: shown, otherKinds: log.otherKinds())
        live = true
    }

    private static let light: [UInt32] = [0xFFEB_EDF0, 0xFF9B_E9A8, 0xFF40_C463, 0xFF30_A14E, 0xFF21_6E39]
    private static let dark: [UInt32] = [0xFF2D_333B, 0xFF0E_4429, 0xFF00_6D32, 0xFF26_A641, 0xFF39_D353]

    static func levelColor(_ level: Int, dark: Bool) -> Color {
        let ramp = dark ? Self.dark : Self.light
        return Color(fmARGB: ramp[min(max(level, 0), ramp.count - 1)])
    }

    var body: some View {
        let counts = synced ?? self.counts
        let kinds = syncedKinds ?? otherKinds
        let summary = ActivitySummary(counts)
        FmCard(padding: EdgeInsets.fmAll(FmSpace.x3), radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: FmSpace.x1) {
                    Image(systemName: "hand.tap.fill")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(fm.success)
                    Text("TODAY'S ACTIVITY").fmText(.labelSmall)
                    Spacer()
                    Text("\(summary.total.formatted()) \(summary.total == 1 ? "interaction" : "interactions")")
                        .fmText(.labelSmall, color: fm.textTertiary, tabular: true)
                }
                if !kinds.isEmpty {
                    Text("On \(ActivityGrid.sourcesText(self: FmUserActivity.kind, others: kinds))")
                        .fmText(.labelSmall, color: fm.textTertiary)
                }
                ActivityHeatGrid(counts: counts, nowSlot: nowSlot, peak: summary.peak)
                    .mask(alignment: .leading) {
                        Rectangle().scaleEffect(x: shown ? 1 : 0.001, anchor: .leading)
                    }
                if summary.total > 0 {
                    ViewThatFits(in: .horizontal) {
                        footer(summary, legend: true)
                        footer(summary, legend: false)
                        Text(
                            "Active \(summary.activeText) · busiest hour \(ActivityGrid.label(summary.peakHour * ActivityGrid.rows)) (\(summary.peakHourTotal.formatted())) · busiest 5 min \(ActivityGrid.label(summary.peakSlot)) (\(summary.peak.formatted()))"
                        )
                        .fmText(.labelSmall, color: fm.textSecondary, tabular: true)
                        .fixedSize(horizontal: false, vertical: true)
                    }
                } else {
                    Text("Taps, scrolls and typing show up here as you use the app.")
                        .fmText(.labelSmall, color: fm.textSecondary)
                }
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(spoken(summary)))
        .accessibilityIdentifier("merge-today-activity")
        .task {
            guard live else { return }
            let activity = FmUserActivity.shared
            if await activity.syncNow(force: true) {
                synced = activity.log.counts()
                syncedKinds = activity.log.otherKinds()
            }
        }
    }

    private func footer(_ summary: ActivitySummary, legend: Bool) -> some View {
        HStack(spacing: FmSpace.x1) {
            chip("timer", summary.activeText, color: fm.success)
            chip("flame.fill", "\(ActivityGrid.label(summary.peakHour * ActivityGrid.rows)) ×\(summary.peakHourTotal.formatted())", color: fm.warning)
            chip("bolt.fill", "\(ActivityGrid.label(summary.peakSlot)) ×\(summary.peak.formatted())", color: fm.accent)
            if legend {
                Spacer(minLength: FmSpace.x1)
                HStack(spacing: 2) {
                    ForEach(0..<ActivityGrid.levels, id: \.self) { level in
                        RoundedRectangle(cornerRadius: 2, style: .continuous)
                            .fill(Self.levelColor(level, dark: fm.dark))
                            .frame(width: 8, height: 8)
                    }
                }
            }
        }
    }

    private func chip(_ icon: String, _ text: String, color: Color) -> some View {
        HStack(spacing: 3) {
            Image(systemName: icon)
                .font(.system(size: 9, weight: .bold))
                .foregroundStyle(color)
            Text(text).fmText(.labelSmall, color: fm.textSecondary, tracking: 0, tabular: true)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(Capsule().fill(fm.bgOverlay.opacity(0.8)))
        .fixedSize()
    }

    private func spoken(_ summary: ActivitySummary) -> String {
        guard summary.total > 0 else { return "Today's activity: nothing recorded yet." }
        return "Today's activity: \(summary.total) interactions, active \(summary.activeText), busiest hour \(ActivityGrid.label(summary.peakHour * ActivityGrid.rows)) with \(summary.peakHourTotal)."
    }
}

struct ActivityHeatGrid: View {
    @Environment(\.fmPalette) private var fm
    let counts: [Int]
    let nowSlot: Int
    let peak: Int

    private let gutter: CGFloat = 22
    private let rowPitch: CGFloat = 6.5
    private let axis: CGFloat = 11

    var body: some View {
        let dark = fm.dark
        let ring = fm.textPrimary
        let label = fm.textTertiary
        Canvas { context, size in
            let pitch = (size.width - gutter) / CGFloat(ActivityGrid.hours)
            let gap: CGFloat = 1.5
            let cell = pitch - gap
            let rowCell = rowPitch - gap
            let radius = min(1.5, rowCell / 3)
            func rect(_ slot: Int) -> CGRect {
                CGRect(
                    x: gutter + CGFloat(slot / ActivityGrid.rows) * pitch + gap / 2,
                    y: CGFloat(slot % ActivityGrid.rows) * rowPitch + gap / 2, width: cell, height: rowCell)
            }
            for slot in 0..<ActivityGrid.slots {
                let value = slot < counts.count ? counts[slot] : 0
                var fill = TodayActivityCard.levelColor(ActivityGrid.level(value, peak: peak), dark: dark)
                if slot > nowSlot && value <= 0 { fill = fill.opacity(0.4) }
                context.fill(Path(roundedRect: rect(slot), cornerRadius: radius, style: .continuous), with: .color(fill))
            }
            if (0..<ActivityGrid.slots).contains(nowSlot) {
                let frame = rect(nowSlot).insetBy(dx: -0.75, dy: -0.75)
                context.stroke(
                    Path(roundedRect: frame, cornerRadius: radius + 1, style: .continuous), with: .color(ring), lineWidth: 1.25)
            }
            for row in stride(from: 0, to: ActivityGrid.rows, by: 3) {
                let text = context.resolve(
                    Text(String(format: ":%02d", row * ActivityGrid.slotMinutes)).font(.system(size: 8)).foregroundStyle(label))
                context.draw(text, at: CGPoint(x: gutter - 3, y: (CGFloat(row) + 0.5) * rowPitch), anchor: .trailing)
            }
            for hour in stride(from: 0, to: ActivityGrid.hours, by: 6) {
                let text = context.resolve(
                    Text(String(format: "%02d", hour)).font(.system(size: 8)).foregroundStyle(label))
                context.draw(
                    text, at: CGPoint(x: gutter + (CGFloat(hour) + 0.5) * pitch, y: CGFloat(ActivityGrid.rows) * rowPitch + 1),
                    anchor: .top)
            }
        }
        .frame(height: CGFloat(ActivityGrid.rows) * rowPitch + axis)
        .accessibilityHidden(true)
    }
}
