import FMAgents
import FMApi
import FMCode
import FMControl
import SwiftUI

struct MergeCleanupSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State private var model: MergeCleanupModel
    @State private var fix = ConflictFixModel()
    private let onMerged: (String) -> Void
    private let onFixConflicts: (() async throws -> Void)?

    init(
        client: AgentOpsClient, sessionId: Int, onFixConflicts: (() async throws -> Void)? = nil,
        onMerged: @escaping (String) -> Void = { _ in }
    ) {
        _model = State(initialValue: MergeCleanupModel(client: client, sessionId: sessionId))
        self.onFixConflicts = onFixConflicts
        self.onMerged = onMerged
    }

    var body: some View {
        NavigationStack {
            Group {
                if let branch = model.mergedBranch {
                    MergeLandedReport(
                        title: "Merged \(branch) and cleaned up.",
                        subtitle: model.mergeStats.map { $0.base.isEmpty ? "" : "\(branch) is part of \($0.base) now." } ?? "",
                        notice: model.setAsideNote,
                        stats: model.mergeStats)
                } else {
                    form
                }
            }
            .navigationTitle(model.mergedBranch == nil ? "Merge & clean up" : "Merged")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(model.mergedBranch == nil ? "Close" : "Done") { dismiss() }
                }
            }
            .task { await run() }
        }
    }

    private var form: some View {
        Form {
            if model.busy {
                Section {
                    HStack {
                        Text("Merging and cleaning up on the desktop…")
                        Spacer()
                        ProgressView()
                    }
                }
            }
            if !model.errorText.isEmpty {
                Section {
                    Label(model.errorText, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fmMessageTools(model.errorText)
                    if !model.conflicted && !model.busy {
                        Button("Try again") { Task { await run() } }
                    }
                }
            }
            if model.canSetAside && !model.busy {
                Section {
                    Button {
                        Task { await run(setAside: true) }
                    } label: {
                        Label("Set aside and merge", systemImage: "shippingbox")
                    }
                    .accessibilityIdentifier("merge-set-aside")
                } footer: {
                    Text(MergeFailureNote.setAsideExplanation)
                }
            }
            if model.conflicted, let onFixConflicts {
                ConflictFixSection(fix: fix) {
                    if await fix.send(onFixConflicts) {
                        FmHaptics.commit()
                        onMerged(ConflictResolution.fixSent)
                        dismiss()
                    } else {
                        FmHaptics.failure()
                    }
                }
            }
        }
    }

    private func run(setAside: Bool = false) async {
        if await model.mergeNow(setAside: setAside) {
            FmHaptics.commit()
            onMerged(model.outcome.message())
        } else if !model.errorText.isEmpty {
            FmHaptics.failure()
        }
    }
}

extension MergeCleanupSheet {
    static func desktopFix(sessionId: Int, base: String = "") -> (() async throws -> Void)? {
        guard let center = HostsFeature.agents else { return nil }
        return {
            let known = center.rows.first {
                $0.isDesktop && !$0.outside && !$0.elsewhere && $0.localId == sessionId
            }?.baseBranch ?? ""
            try await center.promptDesktop(sessionId, ConflictResolution.sessionPrompt(base: base.isEmpty ? known : base))
        }
    }
}

struct MergeCleanupButton: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @State private var rumble = FmRumble()
    @State private var started: Date?
    let working: Bool
    let action: () -> Void

    var body: some View {
        Button {
            guard !working else { return }
            FmHaptics.tap(.forLabel("Merge & clean up", fallback: .click))
            action()
        } label: {
            Group {
                if working && reduceMotion {
                    ProgressView()
                        .controlSize(.small)
                        .tint(Color.white)
                } else if let started, working {
                    TimelineView(.animation) { context in
                        let elapsed = context.date.timeIntervalSince(started)
                        let level = FmRumble.level(elapsed)
                        let t = context.date.timeIntervalSinceReferenceDate
                        let x = (sin(t * 71) + 0.6 * sin(t * 113 + 1.3)) / 1.6
                        let y = (sin(t * 89 + 0.7) + 0.5 * sin(t * 131 + 2.1)) / 1.5
                        let turn = sin(t * 97 + 0.4)
                        icon
                            .scaleEffect(1 + 0.12 * level)
                            .shadow(color: Color.white.opacity(0.7 * level), radius: 2 + 6 * level)
                            .rotationEffect(.degrees(turn * (2 + 12 * level)))
                            .offset(x: x * (0.6 + 3.4 * level), y: y * (0.4 + 2.6 * level))
                    }
                } else {
                    icon
                }
            }
            .frame(width: 42, height: 30)
            .modifier(MergeGlass(tint: fm.success, reduceTransparency: reduceTransparency))
            .contentShape(RoundedRectangle(cornerRadius: MergeGlass.radius, style: .continuous))
        }
        .buttonStyle(.plain)
        .onChange(of: working, initial: true) { _, now in
            if now {
                started = Date()
                rumble.start()
            } else {
                started = nil
                rumble.stop()
            }
        }
        .onDisappear { rumble.stop() }
        .accessibilityLabel(Text(working ? "Merging and cleaning up" : "Merge & clean up"))
    }

    private var icon: some View {
        FmIcon("call_merge", size: 19, color: Color.white)
            .shadow(color: Color.black.opacity(0.18), radius: 1, y: 0.5)
    }
}

struct MergeGlass: ViewModifier {
    static let radius: CGFloat = 10

    let tint: Color
    let reduceTransparency: Bool

    private var shape: RoundedRectangle { RoundedRectangle(cornerRadius: MergeGlass.radius, style: .continuous) }

    func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.glassEffect(.regular.tint(tint.opacity(0.8)).interactive(), in: shape)
        } else {
            material(content)
        }
        #else
        material(content)
        #endif
    }

    @ViewBuilder private func material(_ content: Content) -> some View {
        if reduceTransparency {
            content.background(shape.fill(tint))
        } else {
            content
                .background {
                    ZStack {
                        shape.fill(.ultraThinMaterial)
                        shape.fill(
                            LinearGradient(
                                colors: [tint.opacity(0.92), tint.opacity(0.62)],
                                startPoint: UnitPoint.top,
                                endPoint: UnitPoint.bottom))
                    }
                }
                .overlay {
                    shape.strokeBorder(
                        LinearGradient(
                            colors: [Color.white.opacity(0.7), Color.white.opacity(0.05), Color.white.opacity(0.3)],
                            startPoint: UnitPoint.topLeading,
                            endPoint: UnitPoint.bottomTrailing),
                        lineWidth: 1)
                }
                .shadow(color: tint.opacity(0.35), radius: 4, y: 1)
        }
    }
}

struct DiffBranchTitle: View {
    @Environment(\.fmPalette) private var fm
    let base: String
    let branch: String
    var added: Int?
    var removed: Int?
    var review: DiffReviewModel?

    var body: some View {
        VStack(spacing: 2) {
            HStack(spacing: 6) {
                Text(branch)
                    .fmMono(size: 13, color: fm.textPrimary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .accessibilityLabel(Text("\(base.isEmpty ? "base" : base)...\(branch)"))
                if let added, let removed {
                    Text("+\(added)")
                        .fmMono(size: 13, weight: FmType.emphasis, color: fm.success)
                        .fixedSize()
                    Text("−\(removed)")
                        .fmMono(size: 13, weight: FmType.emphasis, color: fm.danger)
                        .fixedSize()
                }
            }
            .accessibilityElement(children: .combine)
            .accessibilityIdentifier("diff-branch-title")
            .offset(y: -8)
            if let review, !review.files.isEmpty {
                ViewThatFits(in: .horizontal) {
                    DiffHealthPills(model: review, compact: true)
                    ScrollView(.horizontal, showsIndicators: false) {
                        DiffHealthPills(model: review, compact: true)
                    }
                }
                .accessibilityIdentifier("diff-branch-health")
                .offset(y: 1)
            }
        }
    }
}

struct MergeToolbar<Title: View>: ViewModifier {
    let merged: Bool
    let showMerge: Bool
    let merging: Bool
    let mergeIdentifier: String
    let onMerge: () -> Void
    @ViewBuilder let title: () -> Title

    func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.toolbar {
                titleItem
                if showMerge && !merged {
                    mergeItem.sharedBackgroundVisibility(.hidden)
                }
            }
        } else {
            content.toolbar {
                titleItem
                if showMerge && !merged {
                    mergeItem
                }
            }
        }
        #else
        content.toolbar {
            titleItem
            if showMerge && !merged {
                mergeItem
            }
        }
        #endif
    }

    @ToolbarContentBuilder private var titleItem: some ToolbarContent {
        if !merged {
            ToolbarItem(placement: .principal) {
                title()
            }
        }
    }

    private var mergeItem: some ToolbarContent {
        ToolbarItem(placement: .topBarTrailing) {
            MergeCleanupButton(working: merging, action: onMerge)
                .accessibilityIdentifier(mergeIdentifier)
        }
    }
}

struct MergeLandedScreen: View {
    let title: String
    var subtitle: String = ""
    var notice: String = ""
    let stats: MergeStats?
    var actionsRepo: RepoRef? = nil
    var actionsCommit: String = ""
    let onBack: () -> Void
    @State private var left = false

    var body: some View {
        MergeLandedReport(
            title: title, subtitle: subtitle, notice: notice, stats: stats, actionsRepo: actionsRepo,
            actionsCommit: actionsCommit, onBack: leave)
            .navigationBarBackButtonHidden(true)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    FmIconButton(icon: "arrow_back", label: "Back", onPressed: leave)
                        .accessibilityIdentifier("merge-landed-top-back")
                }
            }
    }

    private func leave() {
        guard !left else { return }
        left = true
        onBack()
    }
}

struct MergeFailureNote: View {
    static let setAsideExplanation =
        "The desktop saves all uncommitted work in its checkout on a wip/ branch, sets aside only the files this merge touches, merges, then puts those edits back. If they clash with the merge, the files get conflict markers and the wip/ branch keeps the untouched copy."

    @Environment(\.fmPalette) private var fm
    let errorText: String
    var notice: String = ""
    let fix: ConflictFixModel
    let onFix: (() async -> Void)?
    var onSetAside: (() -> Void)? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if !notice.isEmpty {
                Text(notice)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .fmMessageTools(notice)
            }
            if !errorText.isEmpty {
                Label(errorText, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(fm.danger)
                    .fixedSize(horizontal: false, vertical: true)
                    .fmMessageTools(errorText)
            }
            if let onSetAside {
                FmToneButton(
                    label: "Set aside and merge", icon: "inventory_2_outlined", tone: .accent, height: 32,
                    onPressed: onSetAside)
                    .accessibilityIdentifier("merge-set-aside")
                Text(Self.setAsideExplanation)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let onFix {
                HStack(spacing: FmSpace.x2) {
                    FmToneButton(
                        label: "Fix with agent", icon: "auto_awesome_outlined", tone: .accent, height: 32,
                        onPressed: fix.sending || fix.sent ? nil : { Task { await onFix() } })
                        .accessibilityIdentifier("merge-fix-conflicts")
                    if fix.sending {
                        ProgressView().controlSize(.small)
                    }
                }
                Text("The agent merges the base branch into its own branch, resolves each conflict and commits. Merge again once it finishes.")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                if !fix.errorText.isEmpty {
                    Text(fix.errorText)
                        .fmText(.bodySmall, color: fm.danger)
                        .fmMessageTools(fix.errorText)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityIdentifier("merge-failure")
    }
}

struct ConflictFixSection: View {
    let fix: ConflictFixModel
    let action: () async -> Void

    var body: some View {
        Section {
            Button {
                Task { await action() }
            } label: {
                HStack {
                    Label("Fix with agent", systemImage: "sparkles")
                    Spacer()
                    if fix.sending { ProgressView() }
                }
            }
            .disabled(fix.sending || fix.sent)
            .accessibilityIdentifier("merge-fix-conflicts")
            if !fix.errorText.isEmpty {
                Text(fix.errorText)
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(fix.errorText)
            }
        } footer: {
            Text("The agent merges the base branch into its own branch, resolves each conflict and commits. Merge again once it finishes.")
        }
    }
}

struct MergeLandedReport: View {
    @Environment(\.fmPalette) private var fm
    let title: String
    var subtitle: String = ""
    var notice: String = ""
    let stats: MergeStats?
    var actionsRepo: RepoRef? = nil
    var actionsCommit: String = ""
    var onBack: (() -> Void)?
    @State private var shown = false
    @State private var countdown = 5
    @State private var stay = false
    @State private var showActions = false

    var body: some View {
        let rows = stats?.velocity.count ?? 0
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                MergeHeroCard(title: title, subtitle: subtitle, notice: notice, stats: stats, shown: shown)
                    .zIndex(1)
                if let stats, !stats.velocity.isEmpty {
                    MergeVelocityCard(stats: stats, shown: shown)
                        .mergeReveal(shown, step: 1)
                }
                TodayActivityCard(shown: shown)
                    .mergeReveal(shown, step: 2 + rows)
                if actionsRepo != nil {
                    FmOutlinedButton("Actions", icon: "play_arrow", fillWidth: true) {
                        stay = true
                        showActions = true
                    }
                    .accessibilityIdentifier("merge-landed-actions")
                    .padding(.top, FmSpace.x1)
                    .mergeReveal(shown, step: 3 + rows)
                }
                if let onBack {
                    FmFilledButton(
                        stay ? "Go back to agent list" : "Go back to agent list (\(countdown))", icon: "arrow_back",
                        fillWidth: true, onPressed: onBack)
                        .accessibilityIdentifier("merge-landed-back")
                        .padding(.top, FmSpace.x1)
                        .mergeReveal(shown, step: 3 + rows)
                }
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
        }
        .navigationDestination(isPresented: $showActions) {
            if let actionsRepo {
                RepoActionsScreen(repo: actionsRepo, commit: actionsCommit)
            }
        }
        .scrollBounceBehavior(.basedOnSize)
        .background(fm.canvas)
        .onAppear { shown = true }
        .task { await leaveAfterCountdown() }
        .accessibilityIdentifier("merge-landed-report")
    }

    private func leaveAfterCountdown() async {
        guard let onBack else { return }
        while countdown > 0 {
            do { try await Task.sleep(for: .seconds(1)) } catch { return }
            if stay { return }
            countdown -= 1
        }
        if stay { return }
        onBack()
    }
}

struct MergeHeroCard: View {
    @Environment(\.fmPalette) private var fm
    let title: String
    var subtitle: String = ""
    var notice: String = ""
    let stats: MergeStats?
    var shown = true

    private var shape: RoundedRectangle { RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous) }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(alignment: .center, spacing: FmSpace.x3) {
                MergeBadge(shown: shown)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .fmText(.titleSmall)
                        .lineLimit(2)
                        .minimumScaleFactor(0.85)
                        .fixedSize(horizontal: false, vertical: true)
                    if !subtitle.isEmpty {
                        Text(subtitle)
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            if !notice.isEmpty {
                Text(notice)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .fmMessageTools(notice)
            }
            if let stats, stats.hasLanding {
                landing(stats)
                    .mergeReveal(shown, step: 1)
            }
        }
        .padding(FmSpace.x3)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background {
            ZStack {
                shape.fill(fm.bgRaised)
                shape.fill(
                    LinearGradient(
                        colors: [fm.success.opacity(fm.dark ? 0.22 : 0.16), fm.success.opacity(0.02)],
                        startPoint: UnitPoint.topLeading,
                        endPoint: UnitPoint.bottomTrailing))
            }
        }
        .overlay { shape.strokeBorder(fm.success.opacity(0.3), lineWidth: 1) }
        .overlay(alignment: .topLeading) {
            MergeConfetti(fired: shown)
                .frame(width: 280, height: 220)
                .offset(x: FmSpace.x3 + MergeBadge.size / 2 - 140, y: FmSpace.x3 + MergeBadge.size / 2 - 110)
        }
        .mergeReveal(shown, step: 0)
    }

    private func landing(_ stats: MergeStats) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x1) {
                if let commits = stats.commits, commits > 0 {
                    MergeChip(icon: "point.topleft.down.to.point.bottomright.curvepath", value: "\(commits)", unit: commits == 1 ? "commit" : "commits")
                }
                MergeChip(icon: "doc.on.doc", value: "\(stats.files)", unit: stats.files == 1 ? "file" : "files")
                Spacer(minLength: FmSpace.x1)
                Text(stats.insertionsText).fmText(.titleSmall, color: fm.success, tabular: true)
                Text(stats.deletionsText).fmText(.titleSmall, color: fm.danger, tabular: true)
            }
            HStack(spacing: FmSpace.x2) {
                if let share = stats.insertionShare {
                    MergeChurnBar(share: share)
                        .frame(height: 6)
                        .scaleEffect(x: shown ? 1 : 0.001, anchor: .leading)
                } else {
                    Spacer(minLength: 0)
                }
                Text(stats.shortCommit).fmMono(size: 11, color: fm.textTertiary)
            }
        }
        .accessibilityElement(children: .combine)
    }
}

struct MergeBadge: View {
    static let size: CGFloat = 38

    @Environment(\.fmPalette) private var fm
    let shown: Bool

    var body: some View {
        ZStack {
            Circle()
                .fill(
                    LinearGradient(
                        colors: [fm.success, fm.success.opacity(0.75)], startPoint: UnitPoint.top,
                        endPoint: UnitPoint.bottom))
            Image(systemName: "checkmark")
                .font(.system(size: 17, weight: .heavy))
                .foregroundStyle(Color.white)
                .symbolEffect(.bounce, value: shown)
        }
        .frame(width: Self.size, height: Self.size)
        .shadow(color: fm.success.opacity(0.45), radius: 8)
        .scaleEffect(shown ? 1 : 0.2)
        .rotationEffect(.degrees(shown ? 0 : -90))
        .accessibilityHidden(true)
    }
}

struct MergeChip: View {
    @Environment(\.fmPalette) private var fm
    let icon: String
    let value: String
    let unit: String

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: icon)
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(fm.success)
            Text(value).fmText(.labelMedium, tabular: true)
            Text(unit).fmText(.bodySmall, color: fm.textSecondary)
        }
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, 3)
        .background(Capsule().fill(fm.bgOverlay.opacity(0.8)))
        .fixedSize()
    }
}

struct MergeConfetti: View {
    static let duration: TimeInterval = 1.8
    static let pieces = 30

    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let fired: Bool
    @State private var start: Date?
    @State private var done = false

    var body: some View {
        let colors = [fm.success, fm.accent, fm.warning, fm.danger, fm.codeType, fm.codeKeyword]
        let begin = done ? nil : start
        TimelineView(.animation(paused: begin == nil)) { context in
            Canvas { canvas, size in
                guard let begin else { return }
                let t = context.date.timeIntervalSince(begin)
                guard t >= 0, t < Self.duration else { return }
                let fade = 1 - pow(t / Self.duration, 2)
                for index in 0..<Self.pieces {
                    let seed = Double(index)
                    let angle = seed / Double(Self.pieces) * 2 * .pi + 0.35 * sin(seed * 12.9898)
                    let speed = 70 + 60 * abs(sin(seed * 78.233))
                    let x = size.width / 2 + CGFloat(cos(angle) * speed * t)
                    let y = size.height / 2 + CGFloat(sin(angle) * speed * t + 70 * t * t)
                    var piece = canvas
                    piece.opacity = fade
                    piece.translateBy(x: x, y: y)
                    piece.rotate(by: .radians(t * (4 + seed.truncatingRemainder(dividingBy: 5)) + seed))
                    let rect = index % 3 == 0
                        ? CGRect(x: -2.5, y: -2.5, width: 5, height: 5)
                        : CGRect(x: -3.5, y: -1.5, width: 7, height: 3)
                    let path = index % 3 == 0 ? Circle().path(in: rect) : Path(roundedRect: rect, cornerRadius: 1)
                    piece.fill(path, with: .color(colors[index % colors.count]))
                }
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .onChange(of: fired, initial: true) { _, now in
            guard now, start == nil, !reduceMotion else { return }
            start = Date().addingTimeInterval(0.25)
        }
        .task(id: start) {
            guard start != nil else { return }
            do { try await Task.sleep(for: .seconds(Self.duration + 0.3)) } catch { return }
            done = true
        }
    }
}

struct MergeVelocityCard: View {
    @Environment(\.fmPalette) private var fm
    let stats: MergeStats
    var shown = true

    var body: some View {
        FmCard(padding: EdgeInsets.fmAll(FmSpace.x3), radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: FmSpace.x1) {
                    Image(systemName: "bolt.fill")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(fm.warning)
                    Text("MERGE VELOCITY").fmText(.labelSmall)
                    Spacer()
                    if !stats.base.isEmpty {
                        Text("into \(stats.base)").fmText(.labelSmall, color: fm.textTertiary)
                            .lineLimit(1)
                    }
                }
                ForEach(Array(stats.velocity.enumerated()), id: \.element.id) { index, window in
                    MergeVelocityRow(window: window, shown: shown)
                        .mergeReveal(shown, step: 2 + index)
                }
            }
        }
    }
}

struct MergeChurnBar: View {
    @Environment(\.fmPalette) private var fm
    let share: Double

    var body: some View {
        let added = fm.success
        let removed = fm.danger
        Canvas { context, size in
            let both = share > 0 && share < 1
            let gap: CGFloat = both ? 2 : 0
            let room = max(size.width - gap, 0)
            let floor = min(4, room / 2)
            let plus: CGFloat = share <= 0 ? 0 : share >= 1 ? room : min(max(room * share, floor), room - floor)
            let minus = share >= 1 ? 0 : room - plus
            if plus > 0 {
                context.fill(Capsule().path(in: CGRect(x: 0, y: 0, width: plus, height: size.height)), with: .color(added))
            }
            if minus > 0 {
                let rect = CGRect(x: size.width - minus, y: 0, width: minus, height: size.height)
                context.fill(Capsule().path(in: rect), with: .color(removed))
            }
        }
        .accessibilityHidden(true)
    }
}

struct MergeVelocityRow: View {
    @Environment(\.fmPalette) private var fm
    let window: MergeVelocityWindow
    var shown = true

    private var shortLabel: String { window.id.isEmpty || window.id.count > 4 ? window.label : window.id }

    private var isRecord: Bool { window.count > 0 && window.count >= window.top }

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            Text(shortLabel)
                .fmText(.labelMedium, color: fm.textSecondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                .frame(width: 34, alignment: .leading)
            MergeVelocityChart(window: window)
                .frame(height: 20)
                .scaleEffect(x: 1, y: shown ? 1 : 0.001, anchor: .bottom)
            HStack(spacing: 2) {
                if isRecord {
                    Image(systemName: "flame.fill")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(fm.warning)
                        .symbolEffect(.bounce, value: shown)
                }
                Text("\(shown ? window.count : 0)").fmText(.titleSmall, tabular: true)
                    .contentTransition(.numericText(value: Double(shown ? window.count : 0)))
                    .lineLimit(1)
            }
            .frame(minWidth: 52, alignment: .trailing)
            VStack(alignment: .trailing, spacing: -1) {
                Text("top \(window.record)")
                Text("avg \(window.avgText)")
            }
            .font(.system(size: 9, weight: .medium).monospacedDigit())
            .foregroundStyle(fm.textTertiary)
            .lineLimit(1)
            .frame(width: 50, alignment: .trailing)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(window.spoken)
    }
}

struct MergeVelocityChart: View {
    @Environment(\.fmPalette) private var fm
    let window: MergeVelocityWindow

    var body: some View {
        let bar = fm.success
        let track = fm.bgOverlay
        let tick = fm.textSecondary
        let baseline = fm.border
        let series = window.series
        let peak = CGFloat(max(window.peak, 1))
        let share = window.share
        let avgShare = window.avgShare
        Canvas { context, size in
            if series.isEmpty {
                let height: CGFloat = 6
                let y = (size.height - height) / 2
                let trackRect = CGRect(x: 0, y: y, width: size.width, height: height)
                context.fill(Capsule().path(in: trackRect), with: .color(track))
                if share > 0 {
                    let fill = CGRect(x: 0, y: y, width: max(size.width * share, height), height: height)
                    context.fill(Capsule().path(in: fill), with: .color(bar))
                }
                if avgShare > 0 {
                    let x = min(max(size.width * avgShare - 1, 0), size.width - 2)
                    let mark = CGRect(x: x, y: y - 4, width: 2, height: height + 8)
                    context.fill(Path(roundedRect: mark, cornerRadius: 1), with: .color(tick))
                }
                return
            }
            let count = CGFloat(series.count)
            let gap: CGFloat = series.count > 30 ? 1 : 2
            let width = max((size.width - gap * (count - 1)) / count, 1)
            context.fill(
                Path(CGRect(x: 0, y: size.height - 1, width: size.width, height: 1)), with: .color(baseline))
            for (index, value) in series.enumerated() where value > 0 {
                let height = max(size.height * CGFloat(value) / peak, 3)
                let x = CGFloat(index) * (width + gap)
                let rect = CGRect(x: x, y: size.height - height, width: width, height: height)
                let radius = min(2, width / 2)
                let shape = UnevenRoundedRectangle(topLeadingRadius: radius, topTrailingRadius: radius)
                let newest = index == series.count - 1
                context.fill(shape.path(in: rect), with: .color(newest ? bar : bar.opacity(0.55)))
            }
        }
        .accessibilityHidden(true)
    }
}

struct MergeReveal: ViewModifier {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let shown: Bool
    let step: Int

    func body(content: Content) -> some View {
        let still = shown || reduceMotion
        content
            .opacity(shown ? 1 : 0)
            .offset(y: still ? 0 : 36)
            .scaleEffect(still ? 1 : 0.9, anchor: .top)
            .blur(radius: still ? 0 : 8)
            .animation(
                reduceMotion
                    ? .easeOut(duration: 0.2)
                    : .spring(response: 0.6, dampingFraction: 0.68).delay(0.1 + 0.11 * Double(step)),
                value: shown)
    }
}

extension View {
    func mergeReveal(_ shown: Bool, step: Int) -> some View {
        modifier(MergeReveal(shown: shown, step: step))
    }
}
