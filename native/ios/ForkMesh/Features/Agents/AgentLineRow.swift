import FMAgentWatch
import FMAgents
import FMApi
import FMBase
import SwiftUI

struct AgentLineRow: View {
    @Environment(\.fmPalette) private var fm
    let line: AgentLine
    let nowMs: Int
    let loader: AgentAttachmentLoader
    var view: AgentListView = .compact
    var trailing: AnyView? = nil
    var dialog: String = ""
    var eagleFeed: AgentEagleFeed? = nil
    var watch: AgentWatchController? = nil
    var watchKeys: [String] = []

    static let ruleWidth: CGFloat = 0.5

    var body: some View {
        framed(content.padding(.horizontal, 7))
            .contentShape(Rectangle())
            .accessibilityElement(children: .combine)
            .accessibilityLabel(Text(line.accessibilitySummary))
    }

    @ViewBuilder private func framed<Inner: View>(_ inner: Inner) -> some View {
        if view == .eagle {
            inner
                .padding(.vertical, 6)
                .overlay(alignment: .top) { rule }
                .overlay(alignment: .bottom) { rule }
        } else {
            inner
                .padding(.vertical, 4)
                .background(fm.bgOverlay)
                .clipShape(FmRoundedShape(radius: FmRadius.sm))
        }
    }

    private var rule: some View {
        Rectangle()
            .fill(fm.border)
            .frame(height: AgentLineRow.ruleWidth)
    }

    @ViewBuilder private var content: some View {
        switch view {
        case .compact: compact
        case .zen: zen
        case .full: full
        case .eagle: eagle
        }
    }

    private var title: some View {
        Text(line.title)
            .font(.system(size: 12.5, weight: .semibold))
            .foregroundStyle(fm.textPrimary)
            .lineLimit(1)
            .truncationMode(.tail)
            .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func dot(_ side: CGFloat = AgentLineDot.standard) -> some View {
        AgentLineDot(line: line, watch: watch, keys: watchKeys, side: side)
            .overlay(alignment: .topTrailing) {
                if line.unread {
                    AgentUnreadDot()
                        .offset(x: 2, y: -2)
                }
            }
    }

    @ViewBuilder private var ends: some View {
        if let ref = line.thumbnail {
            AgentLineThumbnail(ref: ref, loader: loader)
        }
        if let trailing {
            trailing
        }
    }

    private var zen: some View {
        HStack(spacing: 6) {
            dot(20)
            title
            ends
        }
        .frame(minHeight: 20)
    }

    private var full: some View {
        HStack(spacing: 6) {
            dot()
            VStack(alignment: .leading, spacing: 2) {
                title
                AgentLineChips(chips: AgentLines.chips(line, nowMs: nowMs), line: line)
            }
            ends
        }
    }

    private var eagle: some View {
        HStack(spacing: 6) {
            dot()
            VStack(alignment: .leading, spacing: 2) {
                HStack(alignment: .top, spacing: 4) {
                    title
                    AgentEagleStats(line: line)
                        .fixedSize()
                }
                Group {
                    if let eagleFeed {
                        AgentEagleLine(feed: eagleFeed, line: line)
                    } else {
                        AgentEagleStrip(dialog: dialog, moving: line.isActive)
                    }
                }
                .frame(height: 12)
            }
            ends
        }
    }

    private var compact: some View {
        HStack(spacing: 6) {
            dot()
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    Text(line.title)
                        .font(.system(size: 12.5, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    Text(AgentLines.shortAge(ms: line.updatedAtMs, nowMs: nowMs))
                        .font(.system(size: 9.5, weight: .medium).monospacedDigit())
                        .foregroundStyle(fm.textTertiary)
                }
                AgentLineMeta(line: line)
            }
            ends
        }
    }
}

struct AgentUnreadDot: View {
    @Environment(\.fmPalette) private var fm

    var body: some View {
        Circle()
            .fill(fm.accent)
            .frame(width: 8, height: 8)
            .overlay(Circle().strokeBorder(fm.bgOverlay, lineWidth: 1.5).padding(-1.5))
            .accessibilityHidden(true)
    }
}

struct AgentLineChips: View {
    @Environment(\.fmPalette) private var fm
    let chips: [AgentLineChip]
    let line: AgentLine

    private var bySlot: [AgentLineChipSlot: AgentLineChip] {
        Dictionary(chips.map { ($0.slot, $0) }, uniquingKeysWith: { first, _ in first })
    }

    var body: some View {
        columns
            .font(.system(size: 9.5, weight: .medium).monospacedDigit())
            .foregroundStyle(fm.textSecondary)
            .frame(height: 12)
    }

    private var columns: some View {
        let shown = bySlot
        let slots = AgentLineChipSlot.allCases
        return ViewThatFits(in: .horizontal) {
            ForEach(Array(stride(from: slots.count, through: 3, by: -1)), id: \.self) { (count: Int) in
                HStack(spacing: 7) {
                    ForEach(slots.prefix(count), id: \.self) { (slot: AgentLineChipSlot) in
                        column(slot, chip: shown[slot])
                    }
                }
                .fixedSize()
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func column(_ slot: AgentLineChipSlot, chip: AgentLineChip?) -> some View {
        HStack(spacing: 2) {
            if slot != .status {
                ZStack {
                    if chip != nil {
                        glyph(slot)
                    }
                }
                .frame(width: 10, height: 10)
            }
            ZStack(alignment: .leading) {
                ForEach(slot.widestWords, id: \.self) { (word: String) in
                    Text(word).hidden()
                }
                if let chip {
                    Text(chip.word).lineLimit(1)
                }
            }
        }
    }

    @ViewBuilder private func glyph(_ slot: AgentLineChipSlot) -> some View {
        let amber = Color.fmHex("#e3742f") ?? fm.warning
        switch slot {
        case .status:
            EmptyView()
        case .provider:
            symbol("terminal", fm.textSecondary)
        case .host:
            symbol(!line.runner.isEmpty ? "cpu" : line.cloud ? "cloud.fill" : line.outside ? "terminal" : "desktopcomputer",
                   line.cloud || !line.runner.isEmpty ? fm.accent : fm.textSecondary)
        case .age:
            symbol("clock.arrow.circlepath", fm.textSecondary)
        case .sync:
            symbol(line.conflicted ? "exclamationmark.triangle.fill" : "arrow.down", amber)
        case .files:
            symbol("doc", Color.fmHex("#58a6ff") ?? fm.accent)
        case .churn:
            symbol("plusminus", fm.textSecondary)
        case .branch:
            symbol(line.hasWorktree ? "folder.fill" : "arrow.triangle.branch", fm.success)
        }
    }

    private func symbol(_ name: String, _ color: Color) -> some View {
        Image(systemName: name)
            .font(.system(size: 8.5, weight: .semibold))
            .foregroundStyle(color)
    }
}

struct AgentEagleStats: View {
    @Environment(\.fmPalette) private var fm
    let line: AgentLine

    private var churned: Bool { line.linesAdded > 0 || line.linesRemoved > 0 }

    var body: some View {
        if line.filesChanged >= 0 || churned {
            HStack(alignment: .bottom, spacing: 2) {
                if line.filesChanged >= 0 {
                    Text(AgentLines.compactCount(line.filesChanged))
                        .font(.system(size: 8.5, weight: .semibold).monospacedDigit())
                        .foregroundStyle(fm.textSecondary)
                        .lineLimit(1)
                }
                if churned {
                    AgentChurnBars(added: line.linesAdded, removed: line.linesRemoved)
                        .scaleEffect(0.8, anchor: .bottom)
                }
            }
            .frame(height: 10)
            .padding(.top, 1)
        }
    }
}

struct AgentEagleLine: View {
    let feed: AgentEagleFeed
    let line: AgentLine

    var body: some View {
        AgentEagleStrip(dialog: feed.dialog(line), moving: line.isActive)
            .task(id: line.isActive) { feed.recall(line) }
    }
}

struct AgentEagleStrip: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.shellTabHidden) private var tabHidden
    let dialog: String
    var moving = true
    @State private var train: [String] = []
    @State private var widths: [String: CGFloat] = [:]
    @State private var showing = false
    @State private var laneWidth: CGFloat = 0
    @State private var cycleStart = Date.distantFuture

    private static let speed: CGFloat = 80
    private static let gap: CGFloat = 5
    private static let hold: Double = 0.5
    private static let revealTime: Double = 0.35
    private static let reveal = Animation.spring(duration: revealTime, bounce: 0.15)

    var body: some View {
        ZStack(alignment: .leading) {
            if showing {
                lane
                    .transition(.opacity)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .frame(height: showing ? 12 : 0)
        .clipped()
        .onChange(of: dialog, initial: true) { (_: String, next: String) in
            offer(next)
        }
        .onChange(of: moving) { (_: Bool, now: Bool) in
            restart(now)
        }
    }

    private func width(_ text: String) -> CGFloat { widths[text] ?? 0 }

    private var headWidth: CGFloat { train.first.map { width($0) } ?? 0 }

    private var scrolls: Bool { moving && !reduceMotion && headWidth > 0 }

    private var distance: CGFloat { headWidth + Self.gap }

    private var cars: [String] {
        guard scrolls else { return Array(train.prefix(1)) }
        var out = train
        guard let last = train.last, width(last) > 0 else { return out }
        var span = train.reduce(CGFloat(0)) { $0 + width($1) + Self.gap }
        let need = distance + laneWidth
        while span < need, out.count < 60 {
            out.append(last)
            span += width(last) + Self.gap
        }
        return out
    }

    private var lane: some View {
        Color.clear
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .onGeometryChange(for: CGFloat.self, of: { $0.size.width }) { (width: CGFloat) in
                laneWidth = width
            }
            .overlay(alignment: .leading) {
                TimelineView(.animation(minimumInterval: 1.0 / 60.0, paused: !scrolls || tabHidden)) { (context: TimelineViewDefaultContext) in
                    let elapsed: Double = scrolls ? context.date.timeIntervalSince(cycleStart) : 0
                    let due: Bool = scrolls && elapsed * Double(Self.speed) >= Double(distance)
                    HStack(spacing: Self.gap) {
                        ForEach(Array(cars.enumerated()), id: \.offset) { (car: (offset: Int, element: String)) in
                            words(car.element)
                                .onGeometryChange(for: CGFloat.self, of: { $0.size.width }) { (width: CGFloat) in
                                    widths[car.element] = width
                                }
                        }
                    }
                    .fixedSize()
                    .offset(x: -min(CGFloat(max(elapsed, 0)) * Self.speed, distance))
                    .onChange(of: due, initial: true) { (_: Bool, isDue: Bool) in
                        if isDue { wrap(at: context.date) }
                    }
                }
            }
            .clipped()
            .mask {
                LinearGradient(
                    stops: [
                        .init(color: .black.opacity(0.35), location: 0),
                        .init(color: .black, location: 0.02),
                        .init(color: .black, location: 0.95),
                        .init(color: .clear, location: 1),
                    ],
                    startPoint: .leading, endPoint: .trailing)
            }
    }

    private func words(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 10, weight: .medium))
            .foregroundStyle(fm.textPrimary)
            .lineLimit(1)
            .fixedSize()
    }

    private func offer(_ next: String) {
        if next.isEmpty {
            settle()
            return
        }
        guard next != train.last else { return }
        if train.isEmpty {
            train = [next]
            if showing {
                cycleStart = Date().addingTimeInterval(Self.hold)
                return
            }
            cycleStart = Date().addingTimeInterval(Self.revealTime + Self.hold)
            withAnimation(Self.reveal) {
                showing = true
            }
            return
        }
        if !scrolls {
            var still = Transaction()
            still.disablesAnimations = true
            withTransaction(still) {
                train = [next]
                prune()
                cycleStart = Date().addingTimeInterval(Self.hold)
            }
            return
        }
        let current = cars
        var x = -min(max(CGFloat(Date().timeIntervalSince(cycleStart)), 0) * Self.speed, distance)
        var cut = current.count
        for (index, text) in current.enumerated() {
            if index > 0, x > laneWidth {
                cut = index
                break
            }
            x += width(text) + Self.gap
        }
        train = Array(current.prefix(cut)) + [next]
        prune()
    }

    private func wrap(at frame: Date) {
        let now = max(frame, Date())
        let over = max(now.timeIntervalSince(cycleStart) - Double(distance / Self.speed), 0)
        var rest = Array(cars.dropFirst())
        if rest.isEmpty, let head = train.first { rest = [head] }
        guard let next = rest.first else { return }
        while rest.count > 1, rest[rest.count - 1] == rest[rest.count - 2] {
            rest.removeLast()
        }
        let lap = Double((width(next) + Self.gap) / Self.speed)
        var still = Transaction()
        still.disablesAnimations = true
        withTransaction(still) {
            train = rest
            prune()
            cycleStart = now.addingTimeInterval(-over.truncatingRemainder(dividingBy: max(lap, 0.1)))
        }
    }

    private func restart(_ now: Bool) {
        var still = Transaction()
        still.disablesAnimations = true
        withTransaction(still) {
            if !now, let latest = train.last { train = [latest] }
            prune()
            cycleStart = Date().addingTimeInterval(Self.hold)
        }
    }

    private func prune() {
        let kept = Set(train)
        if widths.keys.contains(where: { !kept.contains($0) }) {
            widths = widths.filter { kept.contains($0.key) }
        }
    }

    private func settle() {
        guard showing || !train.isEmpty else { return }
        cycleStart = .distantFuture
        withAnimation(Self.reveal) {
            showing = false
            train = []
        }
    }
}

struct AgentLineMeta: View {
    @Environment(\.fmPalette) private var fm
    let line: AgentLine

    private var glyphColor: Color { fm.textSecondary }

    var body: some View {
        HStack(spacing: 5) {
            if !line.runner.isEmpty {
                HStack(spacing: 1.5) {
                    glyph("cpu", color: fm.accent)
                    Text(line.runner)
                        .foregroundStyle(fm.accent)
                        .lineLimit(1)
                }
                .accessibilityLabel(line.runnerBadge)
            } else if line.cloud {
                glyph("cloud.fill", color: fm.accent)
            } else if line.outside {
                glyph("terminal", color: glyphColor)
            }
            if !line.repoName.isEmpty {
                Text(line.repoName)
                    .foregroundStyle(fm.textTertiary)
                    .lineLimit(1)
                    .layoutPriority(1)
            }
            if !line.branch.isEmpty {
                HStack(spacing: 1.5) {
                    glyph("arrow.triangle.branch", color: glyphColor)
                    Text(line.branch)
                        .foregroundStyle(glyphColor)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
            if line.hasWorktree {
                glyph("folder.fill", color: fm.accent)
            }
            if line.ahead > 0 {
                counted("smallcircle.filled.circle", AgentLines.count(line.ahead), color: glyphColor)
            }
            if line.conflicted {
                glyph("exclamationmark.triangle.fill", color: Color.fmHex("#e3742f") ?? fm.warning)
            } else if line.behind > 0 {
                counted("arrow.down", AgentLines.count(line.behind), color: Color.fmHex("#e3742f") ?? fm.warning)
            }
            if line.filesChanged >= 0 {
                counted("doc", AgentLines.count(line.filesChanged), color: glyphColor)
            }
            if line.linesAdded > 0 || line.linesRemoved > 0 {
                AgentChurnBars(added: line.linesAdded, removed: line.linesRemoved)
            }
            if line.prNumber > 0 {
                counted("arrow.triangle.pull", "\(line.prNumber)", color: glyphColor)
            }
            Spacer(minLength: 0)
        }
        .font(.system(size: 9.5, weight: .medium).monospacedDigit())
        .frame(height: 12)
    }

    private func glyph(_ name: String, color: Color) -> some View {
        Image(systemName: name)
            .font(.system(size: 8.5, weight: .semibold))
            .foregroundStyle(color)
    }

    private func counted(_ name: String, _ value: String, color: Color) -> some View {
        HStack(spacing: 1) {
            glyph(name, color: color)
            Text(value)
                .foregroundStyle(color)
                .fixedSize()
        }
        .layoutPriority(2)
    }
}

struct AgentChurnBars: View {
    @Environment(\.fmPalette) private var fm
    let added: Int
    let removed: Int

    private static func height(_ n: Int) -> CGFloat {
        guard n > 0 else { return 0 }
        return min(10, max(2, CGFloat(log1p(Double(n)) / log1p(800.0)) * 10))
    }

    var body: some View {
        HStack(alignment: .bottom, spacing: 1.5) {
            Capsule().fill(fm.success).frame(width: 2.5, height: Self.height(added))
            Capsule().fill(fm.danger).frame(width: 2.5, height: Self.height(removed))
        }
        .frame(height: 10, alignment: .bottom)
    }
}

struct AgentLineThumbnail: View {
    @Environment(\.fmPalette) private var fm
    let ref: AgentAttachmentRef
    let loader: AgentAttachmentLoader
    @State private var viewing: AgentAttachmentViewerItem?

    var body: some View {
        Button {
            viewing = AgentAttachmentViewerItem(refs: [ref], start: ref.id)
        } label: {
            AgentAttachmentThumbnail(ref: ref, loader: loader, size: 26)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text("Photo \(ref.name). Opens full screen."))
        .fullScreenCover(item: $viewing) { (item: AgentAttachmentViewerItem) in
            AgentAttachmentViewer(refs: item.refs, start: item.start, loader: loader)
        }
    }
}

struct AgentUsageBars: View {
    @Environment(\.fmPalette) private var fm
    let client: AgentOpsClient
    @State private var model: AgentUsageModel
    @State private var showing: Bool = false
    @State private var runnerAccounts: RunnerAccountsModel? = Services.get(RunnerAccountsModel.self)

    init(client: AgentOpsClient) {
        self.client = client
        _model = State(initialValue: AgentUsageModel(client: client))
    }

    private var meter: UsageProviderMeter? {
        model.summary?.activeMeter
    }

    private var credits: UsageCredits? {
        meter?.credits ?? model.summary?.providers.first { $0.credits != nil }?.credits
    }

    var body: some View {
        Button {
            showing = true
        } label: {
            HStack(alignment: .bottom, spacing: 3) {
                if let meter = meter {
                    AgentUsageBar(percent: meter.sessionWindow?.usedPercent, state: meter.sessionWindow?.state ?? .unknown)
                    AgentUsageBar(percent: meter.weeklyWindow?.usedPercent, state: meter.weeklyWindow?.state ?? .unknown)
                    AgentUsageBar(percent: credits?.usedPercent, state: credits?.state ?? .unknown, credit: true)
                } else {
                    Image(systemName: "gauge.with.dots.needle.33percent")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(fm.textSecondary)
                }
            }
            .fixedSize()
            .padding(.horizontal, 9)
            .modifier(AgentToolChip())
        }
        .buttonStyle(ShellBarButtonStyle())
        .accessibilityLabel(Text(label))
        .accessibilityIdentifier("agents-usage")
        .accessibilityHint(Text("Accounts, usage, sign-in and new terminals"))
        .sheet(isPresented: $showing) {
            AgentAccountsSheet(usage: model, runnerAccounts: runnerAccounts)
        }
        .task { await load() }
    }

    private var label: String {
        guard let meter = meter else { return "Usage" }
        let parts: [String] = [meter.sessionWindow, meter.weeklyWindow].compactMap { $0 }.map { (window: UsageWindowMeter) in
            "\(window.label) " + (window.usedPercent.map { "\($0)% used" } ?? "no reading")
        }
        let extra: String = credits.map { "usage credits \($0.caption)" } ?? "usage credits off"
        return "\(meter.label): " + (parts + [extra]).joined(separator: ", ")
    }

    private func load() async {
        await model.refresh()
        await AgentOpsFeature.scheduleUsageReminders(model.reminders, nowMs: model.nowMs)
    }
}

struct AgentUsageBar: View {
    @Environment(\.fmPalette) private var fm
    let percent: Int?
    let state: UsageState
    var credit: Bool = false

    private var tint: Color {
        switch state {
        case .exhausted: return fm.danger
        case .high: return fm.warning
        case .ok: return credit ? fm.accent : fm.success
        case .unknown: return fm.textTertiary
        }
    }

    var body: some View {
        let track: CGFloat = 18
        let used: CGFloat = CGFloat(percent ?? 0) / 100
        return ZStack(alignment: .bottom) {
            Capsule().fill((credit ? fm.accent : fm.textPrimary).opacity(0.15))
            if percent != nil {
                Capsule().fill(tint).frame(height: max(used > 0 ? 2 : 0, track * used))
            }
        }
        .frame(width: 4, height: track)
    }
}
