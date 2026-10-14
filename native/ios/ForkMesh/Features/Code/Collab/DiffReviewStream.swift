import FMApi
import FMAssistant
import FMBase
import FMCode
import FMDiff
import Foundation
import Observation
import SwiftUI
import UIKit

private struct DiffMetrics {
    let fontSize: CGFloat
    let digits: Int
    let longest: Int

    var charWidth: CGFloat { fontSize * 0.602 }
    var numberWidth: CGFloat { CGFloat(digits) * charWidth * 0.9 + 1 }
    var gutterPad: CGFloat { 4 }
    var linePad: CGFloat { 1 }
    var signWidth: CGFloat { charWidth * 1.6 }

    var unifiedWidth: CGFloat {
        numberWidth * 2 + gutterPad * 3 + signWidth + charWidth * CGFloat(longest + 2) + FmSpace.x3
    }

    var sideWidth: CGFloat {
        numberWidth + gutterPad * 3 + charWidth * CGFloat(longest + 2) + FmSpace.x2
    }
}

private struct DiffJump: Equatable {
    let row: Int
    let serial: Int
    let belowHeader: Bool
    var atBottom: Bool = false
}

private struct DiffRowEdges: Equatable {
    let pastDivider: Bool
    let aboveBottom: Bool
}

private struct DiffSweep: Equatable {
    let file: Int
    let serial: Int
}

private enum DiffStreamIds {
    static let leading: Int = -1
    static let viewport: String = "diff-viewport"
}

@MainActor
@Observable
private final class DiffScrollAnchor {
    var top: Int?
    @ObservationIgnored var settle: Task<Void, Never>?
    @ObservationIgnored var suppress: Bool = false
}

@MainActor
private final class DiffPageMarks {
    private var above: Set<Int> = []

    func mark(_ row: Int, above now: Bool) {
        if now {
            above.insert(row)
        } else {
            above.remove(row)
        }
    }

    func bottom(in model: DiffReviewModel) -> Int? {
        above.max { (left: Int, right: Int) -> Bool in
            (model.position(of: left) ?? -1) < (model.position(of: right) ?? -1)
        }
    }
}

@MainActor
private final class DiffLineRatchet {
    private var above: [Int: Bool] = [:]
    private var crossing: [Int] = []
    private var mutedUntil: TimeInterval = 0

    func mute() {
        mutedUntil = ProcessInfo.processInfo.systemUptime + 0.35
    }

    func crossed(_ row: Int, above now: Bool, in model: DiffReviewModel) {
        guard let was = above.updateValue(now, forKey: row), was != now else {
            return
        }
        guard ProcessInfo.processInfo.systemUptime >= mutedUntil else {
            return
        }
        if crossing.isEmpty {
            DispatchQueue.main.async { [weak self, weak model] in
                MainActor.assumeIsolated {
                    guard let self, let model else {
                        return
                    }
                    self.strike(model)
                }
            }
        }
        crossing.append(row)
    }

    private func strike(_ model: DiffReviewModel) {
        let pass: DiffScrollPass = model.scrollPass(crossing: crossing)
        crossing.removeAll(keepingCapacity: true)
        let settings: DiffViewSettings = DiffViewSettings.shared
        FmSoundCues.ratchet(
            pass.lines, changed: pass.changed, flagged: pass.flagged, sound: settings.scrollSound, haptic: settings.scrollHaptic)
    }
}

@MainActor
private struct DiffRatchetTooth: ViewModifier {
    let row: Int
    let divider: CGFloat
    let ratchet: DiffLineRatchet?
    let marks: DiffPageMarks
    let model: DiffReviewModel

    func body(content: Content) -> some View {
        let line: CGFloat = divider
        return content.onGeometryChange(for: DiffRowEdges.self, of: { (proxy: GeometryProxy) -> DiffRowEdges in
            let top: CGFloat = proxy.frame(in: .named(DiffStreamIds.viewport)).minY
            let height: CGFloat = proxy.bounds(of: .named(DiffStreamIds.viewport))?.height ?? 0
            return DiffRowEdges(pastDivider: top < line, aboveBottom: top < height)
        }, action: { (edges: DiffRowEdges) in
            ratchet?.crossed(row, above: edges.pastDivider, in: model)
            marks.mark(row, above: edges.aboveBottom)
        })
        .onDisappear {
            marks.mark(row, above: false)
        }
    }
}

private struct DiffPinnedHeaders<Content: View>: View {
    let anchor: DiffScrollAnchor
    let moved: (Int?, Int?) -> Void
    @ViewBuilder let content: (Int?) -> Content

    var body: some View {
        content(anchor.top)
            .onChange(of: anchor.top) { (old: Int?, new: Int?) in
                moved(old, new)
            }
    }
}

struct DiffInlineReview {
    let session: BranchReviewSession
    let repo: RepoRef
}

private let findColor: Color = Color(red: 0.89, green: 0.70, blue: 0.25).opacity(0.45)
private let findCurrentColor: Color = Color(red: 0.97, green: 0.51, blue: 0.40).opacity(0.65)

struct DiffReviewStream<Header: View>: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private let model: DiffReviewModel
    private let extra: [DiffMenuAction]
    private let emptyMessage: String
    private let showsHealth: Bool
    private let refresh: (() async -> Void)?
    private let inline: DiffInlineReview?
    private let fileActions: (DiffFile) -> [DiffMenuAction]
    private let header: () -> Header

    @State private var anchor: DiffScrollAnchor = DiffScrollAnchor()
    @State private var ratchet: DiffLineRatchet = DiffLineRatchet()
    @State private var pageMarks: DiffPageMarks = DiffPageMarks()
    @State private var pinnedHeight: CGFloat = 36
    @State private var jump: DiffJump?
    @State private var finding: Bool = false
    @State private var showFiles: Bool = false
    @State private var sweeps: [Int: Int] = [:]
    @State private var sweepSerial: Int = 0
    @State private var leaving: DiffSweep?
    @State private var pinchBase: Double?

    init(
        model: DiffReviewModel, extra: [DiffMenuAction] = [], emptyMessage: String = DiffFileListLogic.defaultEmpty,
        showsHealth: Bool = true, refresh: (() async -> Void)? = nil, inline: DiffInlineReview? = nil,
        fileActions: @escaping (DiffFile) -> [DiffMenuAction] = { (_: DiffFile) in [] },
        @ViewBuilder header: @escaping () -> Header
    ) {
        self.model = model
        self.extra = extra
        self.emptyMessage = emptyMessage
        self.showsHealth = showsHealth
        self.refresh = refresh
        self.inline = inline
        self.fileActions = fileActions
        self.header = header
    }

    private var settings: DiffViewSettings { DiffViewSettings.shared }

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            if finding {
                findBar
            }
            if showsHealth && !model.files.isEmpty {
                healthStrip
            }
            CollabNoticeLine(horizontalPadding: FmSpace.x3)
            GeometryReader { (proxy: GeometryProxy) in
                ScrollViewReader { (reader: ScrollViewProxy) in
                    scroller(viewport: proxy.size.width)
                        .onChange(of: jump) { (_: DiffJump?, request: DiffJump?) in
                            guard let request else {
                                return
                            }
                            let offset: CGFloat = min(0.5, 40 / max(proxy.size.height, 1))
                            let place: UnitPoint = request.atBottom ? .bottom : (request.belowHeader ? UnitPoint(x: 0, y: offset) : .top)
                            reader.scrollTo(request.row, anchor: place)
                        }
                }
                .coordinateSpace(.named(DiffStreamIds.viewport))
                .simultaneousGesture(pinch)
                .overlay(alignment: .top) {
                    DiffPinnedHeaders(anchor: anchor, moved: { (old: Int?, new: Int?) in
                        scrolled(from: old, to: new)
                    }) { (top: Int?) in
                        pinnedHeaders(top: top, width: proxy.size.width)
                    }
                }
            }
        }
        .background(fm.readingSurface)
        .genieTurnsPages { (page: GeniePageTurn) -> String in
            turn(page)
        }
        .onAppear {
            settings.attach(Services.get(KeyValueStore.self))
            model.split = settings.split
        }
        .onChange(of: settings.split) { (_: Bool, split: Bool) in
            model.split = split
        }
        .sheet(isPresented: $showFiles, onDismiss: { FmSoundCues.play(.close) }) {
            DiffFilesSheet(model: model) { (index: Int) in
                go(DiffReviewRow.headerId(index))
            }
        }
    }

    @ViewBuilder private func pinnedHeaders(top: Int?, width: CGFloat) -> some View {
        ZStack(alignment: .top) {
            if let file = stickyFile(top) {
                fileHeader(file, pinned: true)
                    .id(file)
                    .frame(width: width)
                    .onGeometryChange(for: CGFloat.self, of: { (proxy: GeometryProxy) -> CGFloat in
                        proxy.size.height.rounded()
                    }, action: { (height: CGFloat) in
                        if height > 0 && height != pinnedHeight {
                            ratchet.mute()
                            pinnedHeight = height
                        }
                    })
                    .shadow(color: Color.black.opacity(0.12), radius: 3, y: 1)
            }
            if let leaving {
                fileHeader(leaving.file, pinned: true, celebrate: true)
                    .id(leaving.serial)
                    .frame(width: width)
                    .shadow(color: Color.black.opacity(0.12), radius: 3, y: 1)
                    .allowsHitTesting(false)
                    .transition(AnyTransition.asymmetric(
                        insertion: AnyTransition.identity,
                        removal: AnyTransition.offset(y: -14).combined(with: AnyTransition.opacity)))
            }
        }
    }

    private func stickyFile(_ top: Int?) -> Int? {
        guard let top, let position = model.position(of: top) else {
            return nil
        }
        switch model.rows[position] {
        case .section, .header:
            return nil
        default:
            return model.fileAt(rowId: top)
        }
    }

    private func go(_ row: Int?, _ cue: FmCue? = .navigate) {
        guard let row else {
            return
        }
        if let cue {
            FmHaptics.selection(cue)
        }
        ratchet.mute()
        anchor.settle?.cancel()
        anchor.suppress = true
        jump = DiffJump(row: row, serial: (jump?.serial ?? 0) + 1, belowHeader: !startsFile(row))
    }

    private func startsFile(_ row: Int) -> Bool {
        model.position(of: row).map { (position: Int) -> Bool in
            if case .header = model.rows[position] {
                return true
            }
            return false
        } ?? true
    }

    private func scrolled(from old: Int?, to new: Int?) {
        anchor.settle?.cancel()
        if anchor.suppress {
            anchor.suppress = false
            return
        }
        guard old != nil, settings.autoViewed else {
            return
        }
        let passed: [Int] = model.autoMarkPassed(from: old, to: new)
        if let last = passed.last {
            diffViewedFeedback(model, marked: true)
            celebrate(passed, leaving: last)
        }
        let model: DiffReviewModel = self.model
        anchor.settle = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 400_000_000)
            if Task.isCancelled {
                return
            }
            let marked: [Int] = model.settleScroll()
            if !marked.isEmpty {
                diffViewedFeedback(model, marked: true)
                celebrate(marked, leaving: nil)
            }
        }
    }

    private func celebrate(_ files: [Int], leaving last: Int?) {
        guard !reduceMotion else {
            return
        }
        sweepSerial += 1
        let serial: Int = sweepSerial
        for file in files {
            sweeps[file] = serial
        }
        if let last, stickyFile(anchor.top) != last {
            leaving = DiffSweep(file: last, serial: serial)
        }
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 520_000_000)
            if leaving?.serial == serial {
                withAnimation(.easeIn(duration: 0.24)) {
                    leaving = nil
                }
            }
            try? await Task.sleep(nanoseconds: 400_000_000)
            for file in files where sweeps[file] == serial {
                sweeps.removeValue(forKey: file)
            }
        }
    }

    private func toggleViewed(_ file: Int) {
        ratchet.mute()
        let next: Int? = model.toggleViewed(file)
        let marked: Bool = model.isViewed(file)
        diffViewedFeedback(model, marked: marked)
        if marked {
            celebrate([file], leaving: nil)
        }
        guard let next else {
            return
        }
        let done: Bool = model.seen == model.files.count
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 320_000_000)
            go(DiffReviewRow.headerId(next), done ? nil : .navigate)
        }
    }

    private func turn(_ page: GeniePageTurn) -> String {
        let top: Int? = anchor.top
        switch page {
        case .next:
            guard let row = model.pageDown(top: top, bottom: pageMarks.bottom(in: model)) else {
                return DiffReviewText.diffEnd
            }
            flip(row, atBottom: false)
            return DiffReviewText.nextPage
        case .previous:
            guard let row = model.pageUp(top: top) else {
                return DiffReviewText.diffTop
            }
            flip(row, atBottom: true)
            return DiffReviewText.previousPage
        case .nextFile:
            guard let file = model.nextFile(from: top) else {
                return DiffReviewText.lastFile
            }
            go(DiffReviewRow.headerId(file))
            return DiffReviewText.nextFile(model.files[file].file)
        case .previousFile:
            guard let file = model.previousFile(from: top) else {
                return DiffReviewText.firstFile
            }
            go(DiffReviewRow.headerId(file))
            return DiffReviewText.previousFile(model.files[file].file)
        }
    }

    private func flip(_ row: Int, atBottom: Bool) {
        FmHaptics.selection(.navigate)
        ratchet.mute()
        jump = DiffJump(row: row, serial: (jump?.serial ?? 0) + 1, belowHeader: !startsFile(row), atBottom: atBottom)
    }

    private func submitFind() {
        if let row = model.stepMatch(true) {
            go(row)
        } else if !model.query.isEmpty {
            FmHaptics.warning()
        }
    }


    private var toolbar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 2) {
                Button(action: {
                    FmHaptics.selection(.open)
                    showFiles = true
                }) {
                    HStack(spacing: 4) {
                        FmIcon("format_list_bulleted", size: 16, color: fm.textSecondary)
                        Text("\(model.seen)/\(model.files.count)")
                            .fmMono(size: 12, weight: FmType.emphasis,
                                    color: model.seen == model.files.count && !model.files.isEmpty ? fm.success : fm.textPrimary)
                    }
                    .padding(.horizontal, 6)
                    .frame(height: 30)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("\(model.viewedLabel). Files"))
                .accessibilityIdentifier("diff-files")
                .accessibilityActions {
                    if settings.canGrow {
                        Button("Larger diff text") { settings.setFontSize(settings.fontSize + 1) }
                    }
                    if settings.canShrink {
                        Button("Smaller diff text") { settings.setFontSize(settings.fontSize - 1) }
                    }
                }
                separator
                tiny("search", "Find in diff", active: finding, cue: finding ? .close : .open, id: "diff-find") {
                    finding.toggle()
                    if !finding { model.query = "" }
                }
                tiny(
                    settings.layoutToggleIcon, settings.layoutToggleLabel, active: settings.split, cue: .toggle(!settings.split),
                    id: "diff-layout-toggle"
                ) {
                    settings.split.toggle()
                }
                tiny("wrap_text", "Wrap lines", active: settings.wrap, cue: .toggle(!settings.wrap), id: "diff-wrap") {
                    settings.wrap.toggle()
                }
                tiny("code", "Syntax highlighting", active: settings.syntax, cue: .toggle(!settings.syntax), id: "diff-syntax") {
                    settings.syntax.toggle()
                }
                tiny(
                    "flag_outlined", "Show findings on lines", active: settings.findings, cue: .toggle(!settings.findings),
                    id: "diff-findings"
                ) {
                    settings.findings.toggle()
                }
                tiny(
                    settings.autoViewed ? "visibility_outlined" : "visibility_off_outlined",
                    "Automatically mark files as viewed while scrolling", active: settings.autoViewed,
                    cue: .toggle(!settings.autoViewed), id: "diff-auto-viewed"
                ) {
                    settings.autoViewed.toggle()
                }
                tiny(
                    settings.scrollSound ? "volume_up_outlined" : "volume_off_outlined", "Click sound while scrolling",
                    active: settings.scrollSound, cue: .toggle(!settings.scrollSound), id: "diff-scroll-sound"
                ) {
                    settings.scrollSound.toggle()
                }
                tiny(
                    settings.scrollHaptic ? "vibration_outlined" : "mobile_off_outlined", "Haptic while scrolling",
                    active: settings.scrollHaptic, cue: .toggle(!settings.scrollHaptic), id: "diff-scroll-haptic"
                ) {
                    settings.scrollHaptic.toggle()
                }
                if !extra.isEmpty {
                    separator
                    Menu {
                        ForEach(extra.indices, id: \.self) { (index: Int) in
                            Button(action: extra[index].action) {
                                Label(extra[index].label, systemImage: extra[index].systemImage)
                            }
                        }
                    } label: {
                        FmIcon("more_horiz", size: 18, color: fm.textSecondary)
                            .frame(width: 30, height: 30)
                    }
                    .accessibilityLabel(Text("More"))
                }
            }
            .padding(.horizontal, 4)
        }
        .frame(height: 36)
        .shellGlass(Capsule())
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, FmSpace.x1)
        .accessibilityIdentifier("diff-toolbar")
    }

    private var pinch: some Gesture {
        MagnifyGesture()
            .onChanged { (value: MagnifyGesture.Value) in
                let base: Double = pinchBase ?? settings.fontSize
                pinchBase = base
                let before: Double = settings.fontSize
                settings.setFontSize((base * Double(value.magnification) * 2).rounded() / 2)
                guard settings.fontSize != before else {
                    return
                }
                ratchet.mute()
                if settings.fontSize.rounded() == settings.fontSize {
                    FmHaptics.selection(.select)
                }
            }
            .onEnded { (_: MagnifyGesture.Value) in
                pinchBase = nil
            }
    }

    private var separator: some View {
        Rectangle()
            .fill(fm.border)
            .frame(width: 1, height: 18)
            .padding(.horizontal, 3)
    }

    private func tiny(
        _ icon: String, _ label: String, active: Bool = false, enabled: Bool = true, cue: FmCue = .click, id: String,
        _ action: @escaping () -> Void
    ) -> some View {
        Button(action: {
            FmHaptics.selection(cue)
            ratchet.mute()
            action()
        }) {
            FmIcon(icon, size: 18, color: active ? fm.accent : (enabled ? fm.textSecondary : fm.textDisabled))
                .frame(width: 30, height: 30)
                .background {
                    RoundedRectangle(cornerRadius: 6)
                        .fill(active ? fm.accent.opacity(0.14) : Color.clear)
                }
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityLabel(Text(label))
        .accessibilityAddTraits(active ? .isSelected : [])
        .accessibilityIdentifier(id)
    }


    private var findBar: some View {
        let model: DiffReviewModel = self.model
        let query: Binding<String> = Binding(
            get: { model.query },
            set: { (value: String) in
                model.query = value
                if let match = model.currentMatch {
                    go(match.row, .select)
                }
            })
        return HStack(spacing: 4) {
            FmTextField(text: query, hint: DiffReviewText.findHint, autofocus: true, onSubmit: { submitFind() })
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
            Text(model.query.isEmpty ? "" : model.findLabel)
                .fmMono(size: 11, color: fm.textSecondary)
                .accessibilityIdentifier("diff-find-count")
            tiny("keyboard_arrow_up", "Previous match", enabled: !model.matches.isEmpty, id: "diff-find-prev") {
                go(model.stepMatch(false))
            }
            tiny("keyboard_arrow_down", "Next match", enabled: !model.matches.isEmpty, id: "diff-find-next") {
                go(model.stepMatch(true))
            }
            tiny("close", "Close find bar", cue: .close, id: "diff-find-close") {
                finding = false
                model.query = ""
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 2)
        .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, FmSpace.x1)
    }


    private var healthStrip: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 6) {
                Text("+\(model.additions)")
                    .fmMono(size: 11, weight: FmType.emphasis, color: fm.success)
                Text("−\(model.deletions)")
                    .fmMono(size: 11, weight: FmType.emphasis, color: fm.danger)
                DiffHealthPills(model: model)
            }
            .padding(.horizontal, 8)
        }
        .frame(height: 30)
        .shellGlass(Capsule())
        .padding(.horizontal, FmSpace.x2)
        .padding(.bottom, FmSpace.x1)
    }


    private var metrics: DiffMetrics {
        DiffMetrics(fontSize: CGFloat(settings.fontSize), digits: model.gutterDigits, longest: model.longestColumns)
    }

    private func canvasWidth(_ viewport: CGFloat) -> CGFloat {
        let wanted: CGFloat = settings.split ? metrics.sideWidth * 2 + 1 : metrics.unifiedWidth
        return max(wanted, viewport)
    }

    @ViewBuilder private func scroller(viewport: CGFloat) -> some View {
        if settings.wrap {
            withRefresh(
                ScrollView(.vertical) {
                    stack(viewport: viewport)
                        .frame(width: viewport)
                }
                .scrollPosition(id: Bindable(anchor).top))
        } else {
            let width: CGFloat = canvasWidth(viewport)
            ScrollView(.horizontal) {
                withRefresh(
                    ScrollView(.vertical) {
                        stack(viewport: viewport)
                            .frame(width: width, alignment: .leading)
                    }
                    .scrollPosition(id: Bindable(anchor).top))
                .frame(width: width)
            }
        }
    }

    @ViewBuilder private func withRefresh<Content: View>(_ content: Content) -> some View {
        if let refresh {
            content.fmRefreshable {
                await refresh()
            }
        } else {
            content
        }
    }

    private func stack(viewport: CGFloat) -> some View {
        let metrics: DiffMetrics = self.metrics
        return LazyVStack(alignment: .leading, spacing: 0) {
            VStack(spacing: 0) {
                header()
                if model.files.isEmpty {
                    FmEmptyState(icon: "check_circle_outline", title: DiffReviewText.emptyTitle, message: emptyMessage)
                        .frame(minHeight: 280)
                }
            }
            .frame(width: viewport)
            .id(DiffStreamIds.leading)
            ForEach(model.rows) { (row: DiffReviewRow) in
                rowView(row, metrics, viewport: viewport)
            }
        }
        .scrollTargetLayout()
        .padding(.bottom, 24)
    }

    @ViewBuilder private func rowView(_ row: DiffReviewRow, _ metrics: DiffMetrics, viewport: CGFloat) -> some View {
        switch row {
        case .section(let index):
            sectionRow(index)
                .frame(width: viewport)
        case .header(let file):
            VStack(spacing: 0) {
                fileHeader(file, pinned: false)
                if !model.isFolded(file) {
                    threadStack(file, top: true, anchors: [], width: nil)
                }
            }
            .frame(width: viewport)
        case .notice(_, _, let text):
            Text(text)
                .fmText(.bodySmall, color: fm.textSecondary)
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, FmSpace.x2)
                .frame(width: viewport, alignment: .leading)
                .background(fm.bgOverlay)
        case .hunk(let file, let hunk):
            hunkRow(file, hunk)
                .modifier(tooth(row.id))
        case .line(let file, let line):
            VStack(alignment: .leading, spacing: 0) {
                unifiedLine(file, line, row: row.id, metrics)
                threadStack(file, top: false, anchors: DiffThreadAnchors.unified(line), width: viewport)
            }
            .modifier(tooth(row.id))
        case .pair(let file, let pair):
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: 0) {
                    splitSide(file, pair.left, left: true, row: row.id, metrics)
                    Rectangle()
                        .fill(fm.border)
                        .frame(width: 1)
                    splitSide(file, pair.right, left: false, row: row.id, metrics)
                }
                .fixedSize(horizontal: false, vertical: true)
                threadStack(file, top: false, anchors: DiffThreadAnchors.split(pair), width: viewport)
            }
            .modifier(tooth(row.id))
        case .preview(let file):
            DiffImagePreview(model: model, file: file)
                .frame(width: viewport)
        case .end(let file):
            Color.clear
                .frame(height: 10)
                .onAppear { model.endAppeared(file) }
                .onDisappear { model.endDisappeared(file) }
                .modifier(tooth(row.id, clicks: false))
        }
    }

    @ViewBuilder private func threadStack(_ file: Int, top: Bool, anchors: [(DiffSide, Int)], width: CGFloat?) -> some View {
        if let inline {
            let path: String = model.files[file].file.path
            let threads: [ReviewThread] = top
                ? inline.session.topThreads(path: path)
                : inline.session.threads(path: path, at: anchors)
            if !threads.isEmpty {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    ForEach(threads) { (thread: ReviewThread) in
                        ReviewThreadCard(session: inline.session, repo: inline.repo, thread: thread) { (notice: CollabNotice) in
                            CollabMessenger.shared.show(notice)
                        }
                    }
                }
                .padding(.horizontal, FmSpace.x2)
                .padding(.vertical, FmSpace.x2)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(fm.canvas)
                .frame(width: width)
            }
        }
    }

    private func isSelected(_ file: Int, _ line: DiffLine) -> Bool {
        guard let inline else {
            return false
        }
        return inline.session.isSelected(path: model.files[file].file.path, line: line)
    }

    private func selectLabel(_ selected: Bool) -> String {
        selected ? "Clear line selection" : "Select line to comment"
    }

    private var canComment: Bool { inline?.session.canComment ?? false }

    private func selectLine(_ file: Int, _ line: DiffLine) {
        guard let inline, inline.session.canComment else {
            return
        }
        FmHaptics.selection(.select)
        inline.session.tap(path: model.files[file].file.path, line: line)
    }

    private func tooth(_ row: Int, clicks: Bool = true) -> DiffRatchetTooth {
        DiffRatchetTooth(row: row, divider: pinnedHeight, ratchet: clicks ? ratchet : nil, marks: pageMarks, model: model)
    }

    private func sectionRow(_ index: Int) -> some View {
        Text(model.sections[index].title.uppercased())
            .fmText(.labelSmall, color: fm.textSecondary, weight: FmType.emphasis)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, FmSpace.x3)
            .padding(.top, FmSpace.x3)
            .padding(.bottom, FmSpace.x1)
            .background(fm.canvas)
    }

    private func fileHeader(_ index: Int, pinned: Bool, celebrate: Bool = false) -> some View {
        let entry: DiffReviewFile = model.files[index]
        let file: DiffFile = entry.file
        let viewed: Bool = model.isViewed(index)
        let folded: Bool = model.isFolded(index)
        let detail: String = DiffFileRowText.detail(file)
        let name: Text = Text(DiffFileRowText.name(file))
            .font(FmFonts.monoFont(size: 13).weight(.semibold))
            .foregroundColor(viewed ? fm.textSecondary : fm.textPrimary)
        let path: Text = detail.isEmpty
            ? name
            : name + Text("  " + detail).font(FmFonts.monoFont(size: 11)).foregroundColor(fm.textTertiary)
        return HStack(spacing: 6) {
            if viewed {
                FmIcon(folded ? "chevron_right" : "expand_more", size: 18, color: fm.textSecondary)
                    .accessibilityLabel(Text(DiffReviewText.foldHint(folded)))
            }
            FmChangeBadge(kind: diffChangeKind(file.status).fmKind, size: 18)
            path
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: .infinity, alignment: .leading)
            if file.binary {
                Text("BIN")
                    .fmMono(size: 11, color: fm.textTertiary)
            } else {
                HStack(spacing: 3) {
                    Text("+\(file.additions)")
                        .fmMono(size: 11, weight: FmType.emphasis, color: fm.success)
                    Text("−\(file.deletions)")
                        .fmMono(size: 11, weight: FmType.emphasis, color: fm.danger)
                }
                .fixedSize()
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text(DiffReviewText.countsTooltip(file)))
            }
            Menu {
                fileMenu(index, file: file, viewed: viewed)
            } label: {
                FmIcon("more_horiz", size: 18, color: fm.textSecondary)
                    .frame(width: 36, height: 36)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel(Text("File actions"))
            .accessibilityIdentifier("diff-file-actions")
            .layoutPriority(1)
            DiffViewedButton(viewed: viewed, path: file.path, celebrate: celebrate) { toggleViewed(index) }
                .layoutPriority(1)
        }
        .padding(.horizontal, 8)
        .frame(minHeight: 36)
        .background(viewed ? fm.bgOverlay : fm.bgRaised)
        .overlay {
            if let serial = sweeps[index] {
                DiffHeaderSweep(tint: fm.success)
                    .id(serial)
            }
        }
        .overlay(alignment: .bottom) {
            FmHairline()
        }
        .overlay(alignment: .top) {
            FmHairline()
        }
        .contentShape(Rectangle())
        .onTapGesture {
            if viewed {
                FmHaptics.selection(folded ? .open : .close)
                ratchet.mute()
                model.toggleFold(index)
            } else if pinned {
                go(DiffReviewRow.headerId(index))
            }
        }
        .contextMenu {
            fileMenu(index, file: file, viewed: viewed)
        }
        .accessibilityIdentifier(pinned ? "diff-sticky-header" : "diff-file-\(file.path)")
    }

    @ViewBuilder private func fileMenu(_ index: Int, file: DiffFile, viewed: Bool) -> some View {
        let actions: [DiffMenuAction] = fileActions(file)
        ForEach(actions.indices, id: \.self) { (action: Int) in
            Button(action: actions[action].action) {
                Label(actions[action].label, systemImage: actions[action].systemImage)
            }
        }
        Button(action: { toggleViewed(index) }) {
            Label(viewed ? "Mark as not viewed" : "Mark as viewed", systemImage: viewed ? "square" : "checkmark.square")
        }
        Button(action: { diffCopy(file.path, "Path") }) {
            Label("Copy path", systemImage: "link")
        }
        Button(action: { diffCopy(file.raw, "Diff") }) {
            Label("Copy file diff", systemImage: "doc.on.doc")
        }
    }

    private func hunkRow(_ file: Int, _ index: Int) -> some View {
        let layout: DiffFileLayout = model.layout(file)
        let label: String = layout.hunkLabel(index)
        let text: Text = (label.isEmpty ? Text("") : Text("\(label)   ").foregroundColor(fm.textTertiary))
            + Text(layout.hunkHeader(index)).foregroundColor(fm.accent)
        return text
            .font(FmFonts.monoFont(size: CGFloat(settings.fontSize) * 0.92))
            .lineLimit(settings.wrap ? nil : 1)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, CGFloat(settings.fontSize) * 0.3)
            .background(fm.accent.opacity(0.09))
    }

    private func unifiedLine(_ file: Int, _ line: DiffLine, row: Int, _ metrics: DiffMetrics) -> some View {
        let flags: [DiffLineFlag] = settings.findings ? model.flags(file, line) : []
        let selected: Bool = isSelected(file, line)
        return HStack(alignment: .top, spacing: 0) {
            HStack(alignment: .top, spacing: metrics.gutterPad) {
                number(line.oldNumber, metrics)
                number(line.newNumber, metrics)
            }
            .padding(.horizontal, metrics.gutterPad)
            .padding(.vertical, metrics.linePad)
            .frame(maxHeight: .infinity, alignment: .top)
            .background(selected ? fm.accent.opacity(0.35) : flags.isEmpty ? gutterColor(line.kind) : flagColor(flags).opacity(0.35))
            .overlay(alignment: .topLeading) { lineMarks(file, line, metrics, backed: false) }
            .contentShape(Rectangle())
            .onTapGesture { selectLine(file, line) }
            .accessibilityHidden(true)
            Text(DiffFileLayout.sign(line.kind))
                .font(FmFonts.monoFont(size: CGFloat(settings.fontSize)))
                .foregroundColor(signColor(line.kind))
                .frame(width: metrics.signWidth)
                .padding(.top, metrics.linePad)
                .frame(maxHeight: .infinity, alignment: .top)
                .contentShape(Rectangle())
                .onTapGesture { selectLine(file, line) }
                .accessibilityHidden(true)
            code(file, line, row: row, right: true, flags: flags)
                .padding(.vertical, metrics.linePad)
                .padding(.trailing, FmSpace.x2)
                .frame(maxWidth: .infinity, alignment: .leading)
                .accessibilityActions {
                    if canComment {
                        Button(selectLabel(selected)) { selectLine(file, line) }
                    }
                }
        }
        .fixedSize(horizontal: false, vertical: true)
        .background(selected ? fm.accent.opacity(0.16) : lineColor(line.kind))
    }

    @ViewBuilder private func splitSide(_ file: Int, _ line: DiffLine?, left: Bool, row: Int, _ metrics: DiffMetrics) -> some View {
        if let line {
            let flags: [DiffLineFlag] = settings.findings ? model.flags(file, line) : []
            let selected: Bool = isSelected(file, line)
            HStack(alignment: .top, spacing: 0) {
                number(left ? line.oldNumber : line.newNumber, metrics)
                    .padding(.horizontal, metrics.gutterPad)
                    .padding(.vertical, metrics.linePad)
                    .frame(maxHeight: .infinity, alignment: .top)
                    .background(selected ? fm.accent.opacity(0.35) : flags.isEmpty ? gutterColor(line.kind) : flagColor(flags).opacity(0.35))
                    .overlay(alignment: .topLeading) { lineMarks(file, line, metrics, backed: true) }
                    .contentShape(Rectangle())
                    .onTapGesture { selectLine(file, line) }
                    .accessibilityHidden(true)
                code(file, line, row: row, right: !left, flags: flags)
                    .padding(.leading, metrics.gutterPad)
                    .padding(.vertical, metrics.linePad)
                    .padding(.trailing, FmSpace.x1)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .accessibilityActions {
                        if canComment {
                            Button(selectLabel(selected)) { selectLine(file, line) }
                        }
                    }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(selected ? fm.accent.opacity(0.16) : lineColor(line.kind))
        } else {
            fm.bgOverlay.opacity(0.55)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    @ViewBuilder private func lineMarks(_ file: Int, _ line: DiffLine, _ metrics: DiffMetrics, backed: Bool) -> some View {
        let marks: [DiffLineMark] = model.marks(file, line)
        if !marks.isEmpty {
            let size: CGFloat = CGFloat(settings.fontSize) * 0.62
            HStack(spacing: 1) {
                ForEach(marks, id: \.metric) { (mark: DiffLineMark) in
                    Image(systemName: diffHealthSymbol(mark.metric))
                        .font(.system(size: size, weight: .bold))
                        .foregroundStyle(mark.severe ? fm.danger : fm.warning)
                }
            }
            .padding(.horizontal, 1)
            .background(
                RoundedRectangle(cornerRadius: 2)
                    .fill(backed ? fm.editorGutter : Color.clear)
                    .overlay(RoundedRectangle(cornerRadius: 2).fill(backed ? gutterColor(line.kind) : Color.clear))
            )
            .padding(.leading, backed ? 1 : metrics.gutterPad - 1)
            .padding(.top, metrics.linePad + CGFloat(settings.fontSize) * 0.12)
            .fixedSize()
        }
    }

    private func number(_ value: Int, _ metrics: DiffMetrics) -> some View {
        Text(value > 0 ? String(value) : "")
            .font(FmFonts.monoFont(size: CGFloat(settings.fontSize) * 0.85))
            .foregroundColor(fm.textTertiary)
            .frame(width: metrics.numberWidth, alignment: .trailing)
    }

    private func code(_ file: Int, _ line: DiffLine, row: Int, right: Bool, flags: [DiffLineFlag]) -> some View {
        var text: AttributedString = AttributedString()
        let emphasis: Color = line.kind == .added ? fm.success.opacity(0.32) : fm.danger.opacity(0.30)
        for run in model.layout(file).runs(line, syntax: settings.syntax) {
            var part: AttributedString = AttributedString(run.text)
            part[AttributeScopes.SwiftUIAttributes.ForegroundColorAttribute.self] = fm.codeColor(run.kind)
            if run.kind.isItalic {
                part.inlinePresentationIntent = InlinePresentationIntent.emphasized
            } else if run.kind.isEmphasized {
                part.inlinePresentationIntent = InlinePresentationIntent.stronglyEmphasized
            }
            if run.emphasized {
                part[AttributeScopes.SwiftUIAttributes.BackgroundColorAttribute.self] = emphasis
            }
            text.append(part)
        }
        if !model.query.isEmpty {
            let current: DiffFindMatch? = model.currentMatch
            for match in model.matches(in: row, right: right) {
                let nsRange: NSRange = NSRange(location: match.start, length: match.length)
                guard let range = Range<String.Index>(nsRange, in: line.text) else {
                    continue
                }
                let start: Int = line.text.distance(from: line.text.startIndex, to: range.lowerBound)
                let count: Int = line.text.distance(from: range.lowerBound, to: range.upperBound)
                guard start + count <= text.characters.count else {
                    continue
                }
                let lower: AttributedString.Index = text.index(text.startIndex, offsetByCharacters: start)
                let upper: AttributedString.Index = text.index(lower, offsetByCharacters: count)
                text[lower..<upper][AttributeScopes.SwiftUIAttributes.BackgroundColorAttribute.self] =
                    match == current ? findCurrentColor : findColor
            }
        }
        if line.noNewlineAtEnd {
            var mark: AttributedString = AttributedString(" ⏎̸")
            mark[AttributeScopes.SwiftUIAttributes.ForegroundColorAttribute.self] = fm.danger
            text.append(mark)
        }
        if !flags.isEmpty {
            var tag: AttributedString = AttributedString("  ⚑ " + flags.map(\.label).joined(separator: " · "))
            tag[AttributeScopes.SwiftUIAttributes.ForegroundColorAttribute.self] = flagColor(flags)
            tag.inlinePresentationIntent = InlinePresentationIntent.stronglyEmphasized
            text.append(tag)
        }
        return Text(text)
            .font(FmFonts.monoFont(size: CGFloat(settings.fontSize)))
            .lineSpacing(CGFloat(settings.fontSize) * 0.15)
            .lineLimit(settings.wrap ? nil : 1)
            .fixedSize(horizontal: false, vertical: true)
            .textSelection(.enabled)
    }

    private func flagColor(_ flags: [DiffLineFlag]) -> Color {
        flags.contains(where: \.severe) ? fm.danger : fm.warning
    }

    private func lineColor(_ kind: DiffLineKind) -> Color {
        switch kind {
        case .added: return fm.success.opacity(0.12)
        case .removed: return fm.danger.opacity(0.11)
        case .context: return Color.clear
        }
    }

    private func gutterColor(_ kind: DiffLineKind) -> Color {
        switch kind {
        case .added: return fm.success.opacity(0.20)
        case .removed: return fm.danger.opacity(0.18)
        case .context: return fm.readingGutter
        }
    }

    private func signColor(_ kind: DiffLineKind) -> Color {
        switch kind {
        case .added: return fm.success
        case .removed: return fm.danger
        case .context: return fm.textTertiary
        }
    }
}

@MainActor private func diffViewedFeedback(_ model: DiffReviewModel, marked: Bool) {
    if !marked {
        FmHaptics.selection(.toggleOff)
    } else if model.seen == model.files.count {
        FmHaptics.success()
    } else {
        FmHaptics.tap(.toggleOn)
    }
}

private struct DiffViewedPop {
    var scale: CGFloat = 1
    var tilt: Double = 0
    var ring: CGFloat = 1
    var glow: Double = 0
}

private struct DiffViewedButton: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let viewed: Bool
    let path: String
    var celebrate: Bool = false
    let action: () -> Void

    @State private var pops: Int = 0

    var body: some View {
        let tint: Color = fm.success
        Button(action: action) {
            HStack(spacing: 3) {
                FmIcon(viewed ? "check_box" : "check_box_outline_blank", size: 16, color: viewed ? fm.success : fm.textTertiary)
                    .keyframeAnimator(initialValue: DiffViewedPop(), trigger: pops) { icon, pop in
                        icon
                            .scaleEffect(pop.scale)
                            .rotationEffect(.degrees(pop.tilt))
                    } keyframes: { (_: DiffViewedPop) in
                        KeyframeTrack(\.scale) {
                            SpringKeyframe(0.7, duration: 0.07)
                            SpringKeyframe(1.45, duration: 0.15)
                            SpringKeyframe(1.0, duration: 0.3)
                        }
                        KeyframeTrack(\.tilt) {
                            CubicKeyframe(-14, duration: 0.12)
                            SpringKeyframe(0, duration: 0.35, spring: .bouncy)
                        }
                    }
                Text("Viewed")
                    .fmText(.labelSmall, color: viewed ? fm.success : fm.textSecondary, weight: FmType.emphasis)
                    .lineLimit(1)
            }
            .padding(.horizontal, 7)
            .frame(height: 26.5)
            .background(RoundedRectangle(cornerRadius: 7).fill(viewed ? fm.success.opacity(0.12) : Color.clear))
            .overlay(RoundedRectangle(cornerRadius: 7).stroke(viewed ? fm.success.opacity(0.7) : fm.border, lineWidth: 1))
            .keyframeAnimator(initialValue: DiffViewedPop(), trigger: pops) { badge, pop in
                badge
                    .scaleEffect(pop.scale)
                    .overlay {
                        RoundedRectangle(cornerRadius: 7)
                            .stroke(tint.opacity(pop.glow), lineWidth: 2)
                            .scaleEffect(pop.ring)
                    }
            } keyframes: { (_: DiffViewedPop) in
                KeyframeTrack(\.scale) {
                    SpringKeyframe(1.08, duration: 0.12)
                    SpringKeyframe(1.0, duration: 0.3)
                }
                KeyframeTrack(\.ring) {
                    LinearKeyframe(1.0, duration: 0.05)
                    CubicKeyframe(1.16, duration: 0.4)
                }
                KeyframeTrack(\.glow) {
                    LinearKeyframe(0.9, duration: 0.05)
                    CubicKeyframe(0, duration: 0.4)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .fixedSize()
        .onChange(of: viewed) { (_: Bool, now: Bool) in
            if now && !reduceMotion {
                pops += 1
            }
        }
        .onAppear {
            if celebrate && viewed && !reduceMotion {
                pops += 1
            }
        }
        .accessibilityLabel(Text(viewed ? "Viewed" : "Not viewed"))
        .accessibilityHint(Text(DiffReviewText.viewedHint(viewed)))
        .accessibilityIdentifier("diff-viewed-\(path)")
    }
}

private struct DiffHeaderSweep: View {
    let tint: Color

    @State private var progress: CGFloat = 0

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let band: CGFloat = max(proxy.size.width * 0.45, 80)
            ZStack(alignment: .leading) {
                tint.opacity(0.22)
                    .opacity(Double(1 - progress))
                LinearGradient(
                    colors: [tint.opacity(0), tint.opacity(0.42), tint.opacity(0)],
                    startPoint: UnitPoint.leading,
                    endPoint: UnitPoint.trailing)
                    .frame(width: band)
                    .offset(x: -band + (proxy.size.width + band) * progress)
            }
            .frame(width: proxy.size.width, height: proxy.size.height, alignment: .leading)
            .clipped()
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .onAppear {
            withAnimation(.easeOut(duration: 0.7)) {
                progress = 1
            }
        }
    }
}

private func diffHealthSymbol(_ key: String) -> String {
    switch key {
    case "merge": return "arrow.triangle.merge"
    case "complexity": return "point.3.connected.trianglepath.dotted"
    case "quality": return "checkmark.seal"
    case "security": return "lock.shield"
    case "network": return "network"
    case "practices": return "list.bullet.clipboard"
    default: return "circle.fill"
    }
}

struct DiffHealthPills: View {
    @Environment(\.fmPalette) private var fm

    let model: DiffReviewModel
    var compact: Bool = false

    @State private var metric: DiffHealthMetric?

    var body: some View {
        HStack(spacing: compact ? 4 : 6) {
            ForEach(model.health) { (shown: DiffHealthMetric) in
                pill(shown)
            }
        }
        .alert(
            metric?.label ?? "",
            isPresented: Binding(get: { metric != nil }, set: { if !$0 { metric = nil } }),
            presenting: metric
        ) { (shown: DiffHealthMetric) in
            Button("Copy") { UIPasteboard.general.string = shown.explanation }
            Button("Send to agent") { FmMessageHandoff.send(shown.explanation) }
            Button("OK", role: .cancel) {}
        } message: { (shown: DiffHealthMetric) in
            Text(shown.explanation)
        }
    }

    private func toneColor(_ tone: DiffHealthTone) -> Color {
        switch tone {
        case .green: return fm.success
        case .orange: return fm.warning
        case .red: return fm.danger
        }
    }

    private func pill(_ shown: DiffHealthMetric) -> some View {
        let color: Color = toneColor(shown.tone)
        let size: CGFloat = compact ? 9.5 : 11
        let radius: CGFloat = compact ? 6 : 8
        return Button(action: {
            FmHaptics.selection(.open)
            metric = shown
        }) {
            HStack(spacing: compact ? 2 : 3) {
                Image(systemName: diffHealthSymbol(shown.key))
                    .font(.system(size: size, weight: .semibold))
                    .foregroundStyle(color)
                Text(String(shown.score))
                    .fmMono(size: size, weight: .bold, color: color)
            }
            .padding(.horizontal, compact ? 4 : 6)
            .frame(height: compact ? 16 : 20)
            .background(RoundedRectangle(cornerRadius: radius).fill(color.opacity(0.12)))
            .overlay(RoundedRectangle(cornerRadius: radius).stroke(color.opacity(0.55), lineWidth: compact ? 0.75 : 1))
            .fixedSize()
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(shown.accessibility))
        .accessibilityHint(Text(shown.detail))
        .accessibilityIdentifier("diff-health-\(shown.key)")
    }
}

private struct DiffFilesSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var filter: String = ""

    let model: DiffReviewModel
    let onOpen: (Int) -> Void

    var body: some View {
        let wanted: Set<String> = Set(DiffFileListLogic.filter(model.allFiles, query: filter).map(\.path))
        let shown: [DiffReviewFile] = model.files.filter { wanted.contains($0.file.path) }
        NavigationStack {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    HStack(spacing: FmSpace.x3) {
                        FmLinearProgressIndicator(value: model.progress, color: fm.success)
                            .clipShape(Capsule())
                        Text(model.viewedLabel)
                            .fmText(.labelMedium, color: fm.textSecondary)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
                    Text("\(model.kindsLabel) · \(model.remainingLabel)")
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.bottom, FmSpace.x1)
                    if DiffFileListLogic.showsFilter(model.files.count) {
                        FmTextField(text: $filter, hint: DiffFileListLogic.filterHint)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled(true)
                            .padding(.horizontal, FmSpace.x4)
                            .padding(.vertical, FmSpace.x1)
                    }
                    ForEach(shown, id: \.index) { (entry: DiffReviewFile) in
                        row(entry)
                        FmRowDivider()
                    }
                    if shown.isEmpty && !filter.isEmpty {
                        Text(DiffFileListLogic.noMatch(filter))
                            .fmText(.bodyMedium, color: fm.textSecondary)
                            .frame(maxWidth: .infinity)
                            .padding(FmSpace.x5)
                    }
                }
            }
            .scrollDismissesKeyboard(.interactively)
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Files")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    private func row(_ entry: DiffReviewFile) -> some View {
        let viewed: Bool = model.isViewed(entry.index)
        return HStack(spacing: FmSpace.x3) {
            Button(action: {
                dismiss()
                onOpen(entry.index)
            }) {
                HStack(spacing: FmSpace.x3) {
                    FmChangeBadge(kind: diffChangeKind(entry.file.status).fmKind, size: 20)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(DiffFileRowText.name(entry.file))
                            .fmText(.bodyMedium, color: viewed ? fm.textTertiary : fm.textPrimary, weight: FmType.emphasis)
                            .lineLimit(1)
                        if !DiffFileRowText.detail(entry.file).isEmpty {
                            Text(DiffFileRowText.detail(entry.file))
                                .fmMono(color: fm.textTertiary)
                                .lineLimit(1)
                                .truncationMode(.head)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    if entry.file.binary {
                        Text("BIN")
                            .fmMono(color: fm.textTertiary)
                    } else {
                        DiffStatSummary(additions: entry.file.additions, deletions: entry.file.deletions, showBar: false)
                    }
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            FmIconButton(label: DiffFileRowText.viewedLabel(viewed), onPressed: {
                _ = model.toggleViewed(entry.index)
                diffViewedFeedback(model, marked: model.isViewed(entry.index))
            }) {
                FmIcon(viewed ? "check_circle" : "radio_button_unchecked", size: 20, color: viewed ? fm.success : fm.textTertiary)
            }
            .frame(width: 40, height: 40)
        }
        .padding(.leading, FmSpace.x4)
        .padding(.trailing, FmSpace.x1)
        .padding(.vertical, FmSpace.x2)
        .accessibilityIdentifier("diff-file-row-\(entry.file.path)")
    }
}
