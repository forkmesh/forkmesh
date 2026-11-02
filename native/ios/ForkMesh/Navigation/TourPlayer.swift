import FMApi
import FMPackages
import SwiftUI

struct TourPlayer: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    @State private var track: TourTrack
    @State private var stops: [TourStop]
    @State private var index: Int
    let onClose: () -> Void
    var onJump: ((TourJump) -> Void)? = nil

    init(track: TourTrack, at start: Int = 0, onClose: @escaping () -> Void, onJump: ((TourJump) -> Void)? = nil) {
        let list: [TourStop] = ShellTour.stops(for: track)
        _track = State(initialValue: track)
        _stops = State(initialValue: list)
        _index = State(initialValue: min(max(0, start), max(0, list.count - 1)))
        self.onClose = onClose
        self.onJump = onJump
    }

    private var finished: Bool { index >= stops.count }
    private var current: TourStop? { stops.indices.contains(index) ? stops[index] : nil }

    private var nextTrack: TourTrack? {
        let done: Set<TourTrack> = ShellTour.done
        return TourTrack.allCases.first { $0.rank > track.rank && !done.contains($0) }
    }

    var body: some View {
        ZStack {
            TourBackdrop(tint: TourPlayer.tint(finished ? nil : current?.scene, fm: fm))
            VStack(spacing: 0) {
                header
                TabView(selection: $index) {
                    ForEach(Array(stops.enumerated()), id: \.element.id) { (entry: (offset: Int, element: TourStop)) in
                        TourPlayerPage(stop: entry.element, active: entry.offset == index)
                            .tag(entry.offset)
                    }
                    TourDonePage(track: track, next: nextTrack, onNext: continueWith)
                        .tag(stops.count)
                }
                .tabViewStyle(.page(indexDisplayMode: .never))
                .onChange(of: index) { (_: Int, now: Int) in
                    if now >= stops.count {
                        complete()
                    } else {
                        FmHaptics.selection()
                        remember()
                    }
                }
                footer
            }
        }
        .preferredColorScheme(fm.dark ? .dark : .light)
        .accessibilityIdentifier("tour-player")
        .onAppear {
            ShellTour.markSeen()
            remember()
        }
    }

    private var header: some View {
        HStack(spacing: FmSpace.x3) {
            Button(action: back) {
                Image(systemName: index == 0 ? "xmark" : "arrow.left")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(fm.textPrimary)
                    .frame(width: 44, height: 44)
                    .contentShape(Circle())
            }
            .buttonStyle(.plain)
            .shellGlass(Circle())
            .accessibilityLabel(Text(index == 0 ? "Close the tour" : "Back"))
            .accessibilityIdentifier("tour-back")
            TourDots(count: stops.count, index: min(index, stops.count - 1), onSelect: go)
            Spacer(minLength: 0)
            if !finished {
                Button("Skip", action: skip)
                    .fmText(.labelLarge, color: fm.textSecondary)
                    .frame(minWidth: 44, minHeight: 44)
                    .accessibilityHint(Text("Closes the tour. Tour stays on Home."))
                    .accessibilityIdentifier("tour-skip")
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x1)
    }

    private var footer: some View {
        VStack(spacing: FmSpace.x1) {
            FmFilledButton(primaryLabel, minHeight: 54, fillWidth: true, onPressed: primary)
                .accessibilityIdentifier("tour-next")
            Group {
                if let stop = current, let jump = stop.jump, canJump(jump) {
                    Button(stop.jumpLabel) { take(jump) }
                        .fmText(.labelLarge, color: fm.textSecondary)
                        .accessibilityIdentifier("tour-show-me")
                } else {
                    Color.clear
                }
            }
            .frame(height: 44)
        }
        .padding(.horizontal, FmSpace.x5)
        .padding(.bottom, FmSpace.x2)
    }

    private var primaryLabel: String {
        if finished { return "Start using ForkMesh" }
        return index == stops.count - 1 ? "Finish" : "Next"
    }

    static func tint(_ scene: TourScene?, fm: FmPalette) -> Color {
        switch scene {
        case .none: return fm.success
        case .some(.code): return Color(fmARGB: 0xFF7B4DFF)
        default: return fm.accent
        }
    }

    private func primary() {
        if finished {
            close()
        } else {
            go(index + 1)
        }
    }

    private func back() {
        if index == 0 {
            skip()
        } else {
            go(index - 1)
        }
    }

    private func go(_ next: Int) {
        guard next >= 0, next <= stops.count else { return }
        withAnimation(reduceMotion ? nil : .smooth(duration: 0.4)) {
            index = next
        }
    }

    private func remember() {
        guard let stop = current else { return }
        ShellTour.remember(TourPlace(track: track, stop: stop.id))
    }

    private func complete() {
        FmHaptics.success()
        ShellTour.markDone(track)
        ShellTour.remember(nil)
    }

    private func continueWith(_ next: TourTrack) {
        FmHaptics.tap()
        let list: [TourStop] = ShellTour.stops(for: next)
        let seen: Set<String> = Set(stops.map(\.id))
        let start: Int = list.firstIndex { !seen.contains($0.id) } ?? 0
        var reset: Transaction = Transaction()
        reset.disablesAnimations = true
        withTransaction(reset) {
            track = next
            stops = list
            index = start
        }
        remember()
    }

    private func skip() {
        close()
    }

    private func close() {
        ShellTour.markSeen()
        onClose()
    }

    private func canJump(_ jump: TourJump) -> Bool {
        if onJump != nil { return true }
        switch jump {
        case .tab, .route: return AppNavigator.shared != nil
        case .home, .hub, .composer, .palette, .projects: return false
        }
    }

    private func take(_ jump: TourJump) {
        ShellTour.markSeen()
        if let onJump = onJump {
            onJump(jump)
            return
        }
        onClose()
        guard let router = AppNavigator.shared?.router else { return }
        switch jump {
        case let .tab(tab): router.select(tab)
        case let .route(route): router.open(route)
        case .home, .hub, .composer, .palette, .projects: return
        }
    }
}

struct TourBackdrop: View {
    @Environment(\.fmPalette) private var fm

    let tint: Color

    var body: some View {
        ZStack {
            (fm.dark ? Color.black : fm.bgBase)
            RadialGradient(
                colors: [tint.opacity(fm.dark ? 0.26 : 0.13), tint.opacity(0)],
                center: UnitPoint(x: 0.5, y: 0.34), startRadius: 0, endRadius: 320)
                .animation(.smooth(duration: 0.6), value: tint)
        }
        .ignoresSafeArea()
        .accessibilityHidden(true)
    }
}

struct TourDots: View {
    @Environment(\.fmPalette) private var fm

    let count: Int
    let index: Int
    let onSelect: (Int) -> Void

    private var dot: CGFloat { count > 12 ? 5 : 6 }

    var body: some View {
        HStack(spacing: count > 12 ? 4 : 5) {
            ForEach(0..<max(count, 0), id: \.self) { (at: Int) in
                Capsule()
                    .fill(at == index ? fm.textPrimary : fm.textTertiary.opacity(0.5))
                    .frame(width: at == index ? dot * 4 : dot, height: dot)
                    .frame(minHeight: 44)
                    .contentShape(Rectangle())
                    .onTapGesture { onSelect(at) }
            }
        }
        .animation(.snappy, value: index)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Stop \(index + 1) of \(count)"))
        .accessibilityAdjustableAction { (direction: AccessibilityAdjustmentDirection) in
            switch direction {
            case .increment: onSelect(index + 1)
            case .decrement: onSelect(index - 1)
            @unknown default: return
            }
        }
    }
}

struct TourPlayerPage: View {
    @Environment(\.fmPalette) private var fm

    let stop: TourStop
    let active: Bool

    var body: some View {
        VStack(spacing: 0) {
            TourStage(scene: stop.scene, title: stop.short.headline, active: active)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            VStack(spacing: FmSpace.x2) {
                TourPlayerPage.headline(stop.short, accent: fm.accent)
                    .fmText(.displaySmall)
                    .multilineTextAlignment(.center)
                    .accessibilityAddTraits(.isHeader)
                Text(stop.fromMarket ? "Not installed yet. Add it from the Market." : stop.short.line)
                    .fmText(.bodyLarge, color: fm.textSecondary, height: FmType.leadingSnug)
                    .multilineTextAlignment(.center)
            }
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, FmSpace.x6)
            .padding(.top, FmSpace.x3)
            .padding(.bottom, FmSpace.x4)
            .accessibilityElement(children: .combine)
            .accessibilityHint(Text(stop.message))
        }
    }

    static func headline(_ short: TourShort, accent: Color) -> Text {
        var out: Text = Text(verbatim: "")
        for (at, word) in short.headline.split(separator: " ").enumerated() {
            if at > 0 {
                out = out + Text(verbatim: " ")
            }
            if let emblem = short.emblem, word == short.emblemBefore ?? "" {
                out = out + Text(Image(systemName: emblem)).foregroundColor(accent) + Text(verbatim: " ")
            }
            out = out + Text(String(word))
        }
        return out + Text(verbatim: ".").foregroundColor(accent)
    }
}

struct TourDonePage: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let track: TourTrack
    let next: TourTrack?
    let onNext: (TourTrack) -> Void

    @State private var drawn: Bool = false

    var body: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 0)
            ZStack {
                RoundedRectangle(cornerRadius: 44, style: .continuous)
                    .fill(fm.bgRaised)
                    .frame(width: 140, height: 140)
                    .shadow(color: Color.black.opacity(fm.dark ? 0.4 : 0.1), radius: 24, y: 12)
                Image(systemName: "checkmark")
                    .font(.system(size: 60, weight: .bold))
                    .foregroundStyle(fm.success)
                    .symbolEffect(.bounce, value: drawn)
            }
            .scaleEffect(drawn || reduceMotion ? 1 : 0.7)
            .opacity(drawn || reduceMotion ? 1 : 0)
            Spacer(minLength: 0)
            VStack(spacing: FmSpace.x2) {
                (Text("You're set") + Text(verbatim: ".").foregroundColor(fm.accent))
                    .fmText(.displaySmall)
                    .accessibilityAddTraits(.isHeader)
                Text("\(track.title) done. Tour stays on Home for later.")
                    .fmText(.bodyLarge, color: fm.textSecondary)
                    .multilineTextAlignment(.center)
            }
            .padding(.horizontal, FmSpace.x6)
            if let next = next {
                Button(action: { onNext(next) }) {
                    HStack(spacing: FmSpace.x3) {
                        Image(systemName: "sparkles")
                            .font(.system(size: 18, weight: .semibold))
                            .foregroundStyle(fm.accent)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Keep going: \(next.title)")
                                .fmText(.titleSmall)
                            Text(nextMeta(next))
                                .fmText(.bodyMedium, color: fm.textSecondary, tabular: true)
                        }
                        Spacer(minLength: 0)
                        Image(systemName: "chevron.right")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(fm.textDisabled)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .frame(minHeight: 64)
                    .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
                    .contentShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
                }
                .buttonStyle(.plain)
                .padding(.horizontal, FmSpace.x5)
                .padding(.top, FmSpace.x5)
                .accessibilityIdentifier("tour-next-track")
            }
        }
        .padding(.bottom, FmSpace.x4)
        .onAppear {
            withAnimation(reduceMotion ? nil : .bouncy(duration: 0.5)) { drawn = true }
        }
        .onDisappear { drawn = false }
    }

    private func nextMeta(_ next: TourTrack) -> String {
        let added: Int = ShellTour.stops(for: next).count - ShellTour.stops(for: track).count
        return "\(added) more stops · about \(TourCatalog.minutes(for: added)) min"
    }
}
