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
        HStack(spacing: 8) {
            VStack(alignment: .leading, spacing: 2) {
                title
                Group {
                    if let eagleFeed {
                        AgentEagleLine(feed: eagleFeed, line: line)
                    } else {
                        AgentEagleStrip(dialog: dialog, moving: line.isActive)
                    }
                }
                .frame(height: 12)
            }
            if let trailing {
                trailing
            }
            AgentEagleTile(line: line, loader: loader)
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

struct AgentEagleTile: View {
    static let side: CGFloat = 32
    static let ring: CGFloat = 2

    @Environment(\.fmPalette) private var fm
    let line: AgentLine
    let loader: AgentAttachmentLoader

    private var radius: CGFloat { min(FmRadius.lg, Self.side / 4) }

    var body: some View {
        ZStack {
            picture
            AgentTokenRing(line: line, radius: radius)
                .allowsHitTesting(false)
            VStack(spacing: 1.5) {
                if line.filesChanged >= 0 {
                    AgentTileFiles(line: line)
                }
                AgentTileBadges(line: line)
                if line.status == .success && !line.isActive {
                    AgentTileDone()
                } else if line.backgroundTasks > 0 {
                    AgentTileTasks(count: line.backgroundTasks, active: line.isActive)
                }
            }
            .padding(Self.ring + 1)
            .allowsHitTesting(false)
        }
        .frame(width: Self.side, height: Self.side)
        .overlay(alignment: .topTrailing) {
            AgentTileStatusDot(status: line.status)
                .offset(x: 2, y: -2)
                .allowsHitTesting(false)
        }
        .overlay(alignment: .topLeading) {
            if line.unread {
                AgentUnreadDot()
                    .offset(x: -2, y: -2)
            }
        }
    }

    @ViewBuilder private var picture: some View {
        if let ref = line.thumbnail {
            AgentLineThumbnail(ref: ref, loader: loader, size: Self.side)
        } else {
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .fill(fm.bgOverlay)
        }
    }
}

struct AgentChurnRing: View {
    @Environment(\.fmPalette) private var fm
    let added: Int
    let removed: Int
    let radius: CGFloat
    let width: CGFloat

    private var share: CGFloat? {
        let plus = max(added, 0)
        let minus = max(removed, 0)
        guard plus + minus > 0 else { return nil }
        return CGFloat(plus) / CGFloat(plus + minus)
    }

    @ViewBuilder var body: some View {
        if let share {
            ZStack {
                AgentRingArc(radius: radius, inset: width / 2, start: 0, length: share)
                    .stroke(fm.success, style: StrokeStyle(lineWidth: width))
                AgentRingArc(radius: radius, inset: width / 2, start: share, length: 1 - share)
                    .stroke(fm.danger, style: StrokeStyle(lineWidth: width))
            }
        } else {
            RoundedRectangle(cornerRadius: radius, style: .continuous).inset(by: width / 2)
                .stroke(fm.border, lineWidth: 0.5)
        }
    }
}

struct AgentRingArc: Shape {
    let radius: CGFloat
    let inset: CGFloat
    let start: CGFloat
    let length: CGFloat

    func path(in rect: CGRect) -> Path {
        let edge = RoundedRectangle(cornerRadius: radius, style: .continuous).inset(by: inset).path(in: rect)
        let from = start - floor(start)
        let to = from + length
        guard to > 1 else { return edge.trimmedPath(from: from, to: to) }
        var path = edge.trimmedPath(from: from, to: 1)
        path.addPath(edge.trimmedPath(from: 0, to: to - 1))
        return path
    }
}

struct AgentTokenRing: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.shellTabHidden) private var tabHidden
    let line: AgentLine
    let radius: CGFloat
    @State private var sampler = AgentTokenSampler()

    private static let width: CGFloat = 1
    private static let dash: [CGFloat] = [2.5, 2]
    private static let settleAfter: Duration = .seconds(10)

    private var reading: [Int] { [line.tokensIn, line.tokensOut] }

    private var inPace: AgentTokenPace { line.isActive ? sampler.inPace : .idle }

    private var outPace: AgentTokenPace { line.isActive ? sampler.outPace : .idle }

    private var moving: Bool { !reduceMotion && (inPace != .idle || outPace != .idle) }

    var body: some View {
        Group {
            if line.tokensIn > 0 || line.tokensOut > 0 {
                TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: !moving || tabHidden)) {
                    (context: TimelineViewDefaultContext) in
                    let time = moving ? context.date.timeIntervalSinceReferenceDate : 0
                    ZStack {
                        lane(inset: Self.width / 2, pace: inPace, phase: -time * inPace.travel)
                        lane(inset: Self.width * 1.5, pace: outPace, phase: time * outPace.travel)
                    }
                }
            } else {
                RoundedRectangle(cornerRadius: radius, style: .continuous).inset(by: Self.width)
                    .stroke(fm.border, lineWidth: 0.5)
            }
        }
        .onChange(of: reading, initial: true) { (_: [Int], now: [Int]) in
            sampler.sample(tokensIn: now[0], tokensOut: now[1], atMs: Int(Date().timeIntervalSince1970 * 1000))
        }
        .task(id: reading) {
            try? await Task.sleep(for: Self.settleAfter)
            if !Task.isCancelled { sampler.settle() }
        }
    }

    private func lane(inset: CGFloat, pace: AgentTokenPace, phase: Double) -> some View {
        let pattern = Self.dash.reduce(0, +)
        return RoundedRectangle(cornerRadius: radius, style: .continuous).inset(by: inset)
            .stroke(
                tint(pace),
                style: StrokeStyle(
                    lineWidth: Self.width, dash: Self.dash,
                    dashPhase: CGFloat(phase.truncatingRemainder(dividingBy: Double(pattern)))))
    }

    private func tint(_ pace: AgentTokenPace) -> Color {
        switch pace {
        case .idle: return fm.textTertiary
        case .slow: return fm.accent
        case .steady: return fm.success
        case .fast: return fm.warning
        case .blazing: return fm.danger
        }
    }
}

struct AgentTileStatusDot: View {
    @Environment(\.fmPalette) private var fm
    let status: AgentLineStatus

    private var tint: Color {
        switch status {
        case .running: return fm.accent
        case .waiting: return AgentDotsPainter.paused(fm.warning)
        case .success: return fm.success
        case .merged: return Color.fmHex("#a371f7") ?? fm.accent
        case .failed: return fm.danger
        case .queued, .stopped, .other: return fm.textTertiary
        }
    }

    var body: some View {
        Circle()
            .fill(tint)
            .frame(width: 8, height: 8)
            .overlay(Circle().strokeBorder(fm.bgOverlay, lineWidth: 1.5).padding(-1.5))
            .accessibilityHidden(true)
    }
}

struct AgentTileFiles: View {
    let line: AgentLine

    private static let height: CGFloat = 9

    var body: some View {
        Text(AgentLines.compactCount(line.filesChanged))
            .font(.system(size: 7, weight: .bold).monospacedDigit())
            .foregroundStyle(Color.white)
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, 3)
            .frame(height: Self.height)
            .background(Capsule().fill(Color.black.opacity(0.45)))
            .overlay(
                AgentChurnRing(
                    added: line.linesAdded, removed: line.linesRemoved, radius: Self.height / 2, width: 0.75))
    }
}

struct AgentTileBadges: View {
    @Environment(\.fmPalette) private var fm
    let line: AgentLine

    private static let shown = 4

    var body: some View {
        let badges = AgentLineBadge.of(line).prefix(Self.shown)
        if !badges.isEmpty {
            HStack(spacing: 1) {
                ForEach(Array(badges), id: \.self) { (badge: AgentLineBadge) in
                    Image(systemName: symbol(badge))
                        .font(.system(size: 4.5, weight: .bold))
                        .foregroundStyle(tint(badge))
                        .frame(width: 5, height: 5)
                }
            }
            .padding(.horizontal, 1.5)
            .frame(height: 7)
            .background(Capsule().fill(Color.black.opacity(0.45)))
            .accessibilityHidden(true)
        }
    }

    private func symbol(_ badge: AgentLineBadge) -> String {
        switch badge {
        case .conflicted: return "exclamationmark.triangle.fill"
        case .behind: return "arrow.down"
        case .worktree: return "folder.fill"
        case .branch: return "arrow.triangle.branch"
        case .ahead: return "smallcircle.filled.circle"
        case .pull: return "arrow.triangle.pull"
        case .runner: return "cpu"
        case .cloud: return "cloud.fill"
        case .outside: return "terminal"
        }
    }

    private func tint(_ badge: AgentLineBadge) -> Color {
        switch badge {
        case .conflicted, .behind: return Color.fmHex("#e3742f") ?? fm.warning
        case .worktree, .runner, .cloud: return fm.accent
        case .branch, .ahead, .pull, .outside: return Color.white
        }
    }
}

struct AgentTileDone: View {
    @Environment(\.fmPalette) private var fm

    var body: some View {
        Text("done")
            .font(.system(size: 5.5, weight: .heavy))
            .foregroundStyle(Color.white)
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, 3)
            .frame(height: 7)
            .background(Capsule().fill(fm.success))
            .accessibilityHidden(true)
    }
}

struct AgentTileTasks: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.shellTabHidden) private var tabHidden
    let count: Int
    let active: Bool

    private static let shown = 10
    private static let kept = 7
    private static let period = 1.4

    private var blinking: Bool { active && !reduceMotion }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 20.0, paused: !blinking || tabHidden)) {
            (context: TimelineViewDefaultContext) in
            dots.opacity(blinking ? Self.glow(context.date) : 1)
        }
        .accessibilityHidden(true)
    }

    private static func glow(_ date: Date) -> Double {
        let phase = date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: period) / period
        return 0.6 + 0.4 * cos(phase * 2 * .pi)
    }

    private var drawn: Int { count > Self.shown ? Self.kept : count }

    private var dots: some View {
        HStack(spacing: 1) {
            ForEach(0..<drawn, id: \.self) { _ in
                Capsule().fill(Color.white).frame(width: 1, height: 4)
            }
            if count > drawn {
                Text("+\(count - drawn)")
                    .font(.system(size: 5.5, weight: .bold).monospacedDigit())
                    .foregroundStyle(Color.white)
                    .fixedSize()
            }
        }
        .padding(.horizontal, 2)
        .frame(height: 6)
        .background(Capsule().fill(Color.black.opacity(0.45)))
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
    var size: CGFloat = 26
    @State private var viewing: AgentAttachmentViewerItem?

    var body: some View {
        Button {
            viewing = AgentAttachmentViewerItem(refs: [ref], start: ref.id)
        } label: {
            AgentAttachmentThumbnail(ref: ref, loader: loader, size: size)
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
        NavigationLink {
            UsageScreen(usage: model)
                .shellPageClearance()
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
        .accessibilityHint(Text("Opens Usage: accounts, sign-in and new terminals"))
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
