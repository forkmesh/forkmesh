import FMApi
import FMPackages
import SwiftUI
import TipKit

struct TourView: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var track: TourTrack?
    @State private var stops: [TourStop] = []
    @State private var index: Int = 0
    @State private var done: Set<TourTrack> = ShellTour.done
    @State private var place: TourPlace? = ShellTour.place
    @State private var finished: TourTrack?
    let onFinish: (() -> Void)?
    var onJump: ((TourJump) -> Void)? = nil

    var body: some View {
        ZStack {
            if let track = track {
                player(track)
                    .transition(.move(edge: .trailing).combined(with: .opacity))
            } else {
                chooser
                    .transition(.move(edge: .leading).combined(with: .opacity))
            }
        }
        .fmPreGlassBackground(fm.canvas)
        .navigationTitle("Tour")
        .navigationBarTitleDisplayMode(.inline)
    }

    private var chooser: some View {
        TourChooser(
            done: done, place: resumable, counts: counts, finished: finished,
            onPick: { (picked: TourTrack) in start(picked, at: 0) },
            onResume: { (resume: TourPlace, at: Int) in start(resume.track, at: at) },
            onSkip: onFinish == nil ? nil : { close(finished: false) })
    }

    private var counts: [TourTrack: Int] {
        var out: [TourTrack: Int] = [:]
        for option in TourTrack.allCases {
            out[option] = ShellTour.stops(for: option).count
        }
        return out
    }

    private var resumable: (place: TourPlace, index: Int, total: Int)? {
        guard let place = place else { return nil }
        let list: [TourStop] = ShellTour.stops(for: place.track)
        guard let at = TourCatalog.index(of: place, in: list), at > 0 else { return nil }
        return (place, at, list.count)
    }

    private func player(_ track: TourTrack) -> some View {
        VStack(spacing: 0) {
            TourPlayerHeader(
                track: track, index: index, count: stops.count,
                onTracks: { pick(nil) },
                onClose: { close(finished: false) },
                onSelect: { (at: Int) in go(to: at) })
            TabView(selection: $index) {
                ForEach(Array(stops.enumerated()), id: \.element.id) { (entry: (offset: Int, element: TourStop)) in
                    TourStopPage(stop: entry.element, active: entry.offset == index)
                        .tag(entry.offset)
                }
            }
            .tabViewStyle(.page(indexDisplayMode: .never))
            .onChange(of: index) { (_: Int, _: Int) in
                FmHaptics.selection()
                remember()
            }
            footer
        }
    }

    private var current: TourStop? { stops.indices.contains(index) ? stops[index] : nil }

    private var isLast: Bool { index >= stops.count - 1 }

    private var footer: some View {
        HStack(spacing: FmSpace.x3) {
            if index > 0 {
                FmIconButton(label: "Back", onPressed: { go(to: index - 1) }) {
                    Image(systemName: "chevron.left")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                }
                .accessibilityIdentifier("tour-back")
            }
            if let stop = current, let jump = stop.jump, canJump(jump) {
                FmOutlinedButton(stop.jumpLabel, onPressed: { take(jump) })
                    .accessibilityIdentifier("tour-show-me")
            }
            Spacer(minLength: 0)
            FmFilledButton(isLast ? "Finish" : "Next", onPressed: {
                if isLast {
                    finish()
                } else {
                    go(to: index + 1)
                }
            })
            .accessibilityIdentifier("tour-next")
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
    }

    private func pick(_ next: TourTrack?) {
        withAnimation(reduceMotion ? nil : .snappy) {
            track = next
        }
    }

    private func start(_ picked: TourTrack, at: Int) {
        FmHaptics.tap()
        finished = nil
        stops = ShellTour.stops(for: picked)
        index = min(max(0, at), max(0, stops.count - 1))
        pick(picked)
        remember()
    }

    private func go(to next: Int) {
        guard stops.indices.contains(next) else { return }
        withAnimation(reduceMotion ? nil : .snappy) {
            index = next
        }
    }

    private func remember() {
        guard let track = track, let stop = current else { return }
        let next: TourPlace = TourPlace(track: track, stop: stop.id)
        ShellTour.remember(next)
        place = next
    }

    private func finish() {
        FmHaptics.success()
        if let track = track {
            ShellTour.markDone(track)
            done = ShellTour.done
        }
        ShellTour.remember(nil)
        place = nil
        guard let track = track, TourTrack.allCases.contains(where: { !done.contains($0) && $0.rank > track.rank }) else {
            close(finished: true)
            return
        }
        finished = track
        pick(nil)
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
        guard let router = AppNavigator.shared?.router else { return }
        switch jump {
        case let .tab(tab): router.select(tab)
        case let .route(route): router.open(route)
        case .home, .hub, .composer, .palette, .projects: return
        }
    }

    private func close(finished: Bool) {
        ShellTour.markSeen()
        if let onFinish = onFinish {
            onFinish()
        } else if track != nil {
            pick(nil)
        } else {
            dismiss()
        }
    }
}

struct TourChooser: View {
    @Environment(\.fmPalette) private var fm

    let done: Set<TourTrack>
    let place: (place: TourPlace, index: Int, total: Int)?
    let counts: [TourTrack: Int]
    let finished: TourTrack?
    let onPick: (TourTrack) -> Void
    let onResume: (TourPlace, Int) -> Void
    let onSkip: (() -> Void)?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x4) {
                TourChooserHero()
                    .padding(.top, FmSpace.x2)
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text("Take the tour")
                        .fmText(.titleLarge)
                        .accessibilityAddTraits(.isHeader)
                    Text("Pick how deep to go. Each stop shows the real screen, and you can try it right there.")
                        .fmText(.bodyLarge, color: fm.textSecondary)
                }
                if let finished = finished {
                    finishedCard(finished)
                } else if let place = place {
                    resumeCard(place)
                }
                ForEach(TourTrack.allCases) { (option: TourTrack) in
                    TourTrackCard(
                        track: option, count: counts[option] ?? 0, done: done.contains(option),
                        suggested: suggested == option,
                        onPick: { onPick(option) })
                }
                if let onSkip = onSkip {
                    Button(finished == nil ? "Not now" : "Done for now", action: onSkip)
                        .fmText(.labelLarge, color: fm.textSecondary)
                        .frame(maxWidth: .infinity, minHeight: 44)
                        .accessibilityIdentifier("tour-skip")
                }
                Text("Tour stays on Home, so you can come back for the next track.")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .frame(maxWidth: .infinity)
                    .multilineTextAlignment(.center)
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.bottom, FmSpace.x5)
        }
        .accessibilityIdentifier("tour-chooser")
    }

    private var suggested: TourTrack? {
        TourTrack.allCases.first { !done.contains($0) }
    }

    private func finishedCard(_ track: TourTrack) -> some View {
        HStack(spacing: FmSpace.x3) {
            Image(systemName: "checkmark.seal.fill")
                .font(.system(size: 28, weight: .regular))
                .foregroundStyle(fm.success)
                .symbolEffect(.bounce, value: track)
            VStack(alignment: .leading, spacing: 2) {
                Text("\(track.title) done")
                    .fmText(.titleSmall)
                Text("Keep going with the next track, or close the tour and explore.")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(FmSpace.x3)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("tour-finished")
    }

    private func resumeCard(_ place: (place: TourPlace, index: Int, total: Int)) -> some View {
        Button(action: { onResume(place.place, place.index) }) {
            HStack(spacing: FmSpace.x3) {
                Image(systemName: "play.circle.fill")
                    .font(.system(size: 28, weight: .regular))
                    .foregroundStyle(fm.accent)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Pick up where you left off")
                        .fmText(.titleSmall)
                    Text("\(place.place.track.title), stop \(place.index + 1) of \(place.total)")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                Spacer(minLength: 0)
                Image(systemName: "chevron.right")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
            }
            .padding(FmSpace.x3)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(fm.accentSubtle, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
            .contentShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("tour-resume")
    }
}

struct TourChooserHero: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            ZStack {
                Circle().fill(fm.accent)
                ShellSparkleField(tilt: .zero, energy: 0.4, paused: reduceMotion, stars: 6)
                Image(systemName: "plus")
                    .font(.system(size: 22, weight: .bold))
                    .foregroundStyle(Color.white)
            }
            .frame(width: 52, height: 52)
            .clipShape(Circle())
            .shadow(color: fm.accent.opacity(0.45), radius: 12, y: 4)
            ForEach(["square.grid.2x2", "sparkles", "tray", "books.vertical"], id: \.self) { (symbol: String) in
                Image(systemName: symbol)
                    .font(.system(size: 20, weight: .medium))
                    .symbolRenderingMode(.hierarchical)
                    .foregroundStyle(fm.textSecondary)
                    .frame(width: 40, height: 40)
                    .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            }
        }
        .accessibilityHidden(true)
    }
}

struct TourTrackCard: View {
    @Environment(\.fmPalette) private var fm

    let track: TourTrack
    let count: Int
    let done: Bool
    let suggested: Bool
    let onPick: () -> Void

    private var symbols: [String] {
        TourCatalog.stops.filter { $0.track == track }.map(\.symbol)
    }

    var body: some View {
        Button(action: onPick) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                HStack(alignment: .top, spacing: FmSpace.x3) {
                    Image(systemName: track.symbol)
                        .font(.system(size: 20, weight: .semibold))
                        .foregroundStyle(fm.onPrimary)
                        .frame(width: 40, height: 40)
                        .background(fm.accent, in: Circle())
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: FmSpace.x2) {
                            Text(track.title)
                                .fmText(.titleMedium)
                            if suggested {
                                Text(track == .essentials ? "Start here" : "Up next")
                                    .fmText(.labelSmall, color: fm.accent)
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(fm.accentSubtle, in: Capsule())
                            }
                        }
                        Text(meta)
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    Spacer(minLength: 0)
                    Image(systemName: done ? "checkmark.seal.fill" : "chevron.right")
                        .font(.system(size: done ? 20 : 13, weight: .semibold))
                        .foregroundStyle(done ? fm.success : fm.textTertiary)
                        .accessibilityHidden(true)
                }
                Text(track.summary)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .multilineTextAlignment(.leading)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 6) {
                    if track != .essentials {
                        Image(systemName: "plus")
                            .font(.system(size: 10, weight: .bold))
                            .foregroundStyle(fm.textTertiary)
                    }
                    ForEach(symbols, id: \.self) { (symbol: String) in
                        Image(systemName: symbol)
                            .font(.system(size: 12, weight: .medium))
                            .foregroundStyle(fm.accent)
                            .frame(width: 24, height: 24)
                            .background(fm.accentSubtle, in: RoundedRectangle(cornerRadius: 7, style: .continuous))
                    }
                }
                .accessibilityHidden(true)
            }
            .padding(FmSpace.x4)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 22, style: .continuous)
                    .strokeBorder(suggested ? fm.accent.opacity(0.6) : fm.border, lineWidth: suggested ? 1.5 : 0.5)
            }
            .contentShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel(Text("\(track.title). \(meta). \(track.summary)\(done ? " Done." : "")"))
        .accessibilityIdentifier("tour-track-\(track.rawValue)")
    }

    private var meta: String {
        let minutes: Int = TourCatalog.minutes(for: count)
        return "\(count) stops, about \(minutes) min"
    }
}

struct TourPlayerHeader: View {
    @Environment(\.fmPalette) private var fm

    let track: TourTrack
    let index: Int
    let count: Int
    let onTracks: () -> Void
    let onClose: () -> Void
    let onSelect: (Int) -> Void

    var body: some View {
        VStack(spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                Button(action: onTracks) {
                    HStack(spacing: 4) {
                        Image(systemName: "chevron.left")
                            .font(.system(size: 13, weight: .semibold))
                        Text(track.title)
                            .fmText(.labelLarge, color: fm.accent)
                    }
                    .foregroundStyle(fm.accent)
                    .frame(minHeight: 44)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Choose another track"))
                .accessibilityIdentifier("tour-tracks")
                Spacer(minLength: 0)
                Text("\(min(index + 1, count)) of \(count)")
                    .fmText(.labelMedium, color: fm.textSecondary, tabular: true)
                FmIconButton(label: "Close the tour", onPressed: onClose) {
                    Image(systemName: "xmark")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(fm.textSecondary)
                }
                .accessibilityIdentifier("tour-close")
            }
            HStack(spacing: 4) {
                ForEach(0..<count, id: \.self) { (at: Int) in
                    Capsule()
                        .fill(at <= index ? fm.accent : fm.border)
                        .frame(height: at == index ? 6 : 4)
                        .frame(maxWidth: .infinity, minHeight: 16)
                        .contentShape(Rectangle())
                        .onTapGesture { onSelect(at) }
                }
            }
            .animation(.snappy, value: index)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text("Stop \(min(index + 1, count)) of \(count)"))
            .accessibilityAdjustableAction { (direction: AccessibilityAdjustmentDirection) in
                switch direction {
                case .increment: onSelect(index + 1)
                case .decrement: onSelect(index - 1)
                @unknown default: return
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x1)
    }
}

struct TourStopPage: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let stop: TourStop
    let active: Bool

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x4) {
                TourStage(scene: stop.scene, title: stop.title, active: active)
                hint
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    HStack(spacing: FmSpace.x2) {
                        Image(systemName: stop.symbol)
                            .font(.system(size: 17, weight: .semibold))
                            .foregroundStyle(fm.accent)
                            .accessibilityHidden(true)
                        Text(stop.title)
                            .fmText(.titleLarge)
                            .accessibilityAddTraits(.isHeader)
                    }
                    Text(stop.message)
                        .fmText(.bodyLarge, color: fm.textSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    ForEach(stop.tips, id: \.self) { (tip: String) in
                        HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                            Image(systemName: "checkmark.circle.fill")
                                .font(.system(size: 13, weight: .semibold))
                                .foregroundStyle(fm.accent)
                                .accessibilityHidden(true)
                            Text(tip)
                                .fmText(.bodyMedium, color: fm.textSecondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
                if stop.fromMarket {
                    HStack(spacing: FmSpace.x2) {
                        Image(systemName: "bag")
                            .foregroundStyle(fm.accent)
                        Text("Not installed yet. Add it from the Market to use it.")
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    .padding(FmSpace.x3)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                    .accessibilityElement(children: .combine)
                }
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x3)
        }
        .scrollBounceBehavior(.basedOnSize)
    }

    private var hint: some View {
        HStack(spacing: FmSpace.x2) {
            Image(systemName: "hand.tap.fill")
                .symbolEffect(.pulse, options: .repeating, isActive: active && !reduceMotion)
            Text(stop.tryIt)
                .fmText(.labelMedium, color: fm.accent)
        }
        .foregroundStyle(fm.accent)
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, 6)
        .background(fm.accentSubtle, in: Capsule())
        .frame(maxWidth: .infinity)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text("Try it: \(stop.tryIt)"))
    }
}

enum ShellTour {
    static let seenKey = "fm.tour.seen.v1"
    static let doneKey = "fm.tour.done.v1"
    static let placeKey = "fm.tour.place.v1"

    static var seen: Bool { UserDefaults.standard.bool(forKey: seenKey) }

    static func markSeen() {
        UserDefaults.standard.set(true, forKey: seenKey)
    }

    static var done: Set<TourTrack> {
        let raw: String = UserDefaults.standard.string(forKey: doneKey) ?? ""
        return Set(raw.split(separator: ",").compactMap { TourTrack(rawValue: String($0)) })
    }

    static func markDone(_ track: TourTrack) {
        var next: Set<TourTrack> = done
        for option in TourTrack.allCases where track.includes(option) {
            next.insert(option)
        }
        let raw: String = TourTrack.allCases.filter(next.contains).map(\.rawValue).joined(separator: ",")
        UserDefaults.standard.set(raw, forKey: doneKey)
    }

    static var left: Int { TourTrack.allCases.count - done.count }

    static var place: TourPlace? {
        UserDefaults.standard.string(forKey: placeKey).flatMap { TourPlace(encoded: $0) }
    }

    static func remember(_ place: TourPlace?) {
        if let place = place {
            UserDefaults.standard.set(place.encoded, forKey: placeKey)
        } else {
            UserDefaults.standard.removeObject(forKey: placeKey)
        }
    }

    @MainActor static func stops(for track: TourTrack) -> [TourStop] {
        TourCatalog.stops(for: track, installed: { (id: String) -> Bool in PackageGateSync.isInstalled(id) })
    }

    static func configureTips() {
        try? Tips.configure([.displayFrequency(.daily)])
    }
}

struct HubTip: Tip {
    var title: Text { Text("Your hub") }
    var message: Text? { Text("Your status, Pocket mode, Settings and signing out live here.") }
    var image: Image? { Image(systemName: "person.crop.circle") }
}

struct VoiceTip: Tip {
    var title: Text { Text("Hold to talk") }
    var message: Text? { Text("Hold the new agent button and say what the agent should do. It starts when you let go.") }
    var image: Image? { Image(systemName: "waveform") }
}

struct PaletteTip: Tip {
    var title: Text { Text("Hold for commands") }
    var message: Text? { Text("Hold the new agent button to open the command palette.") }
    var image: Image? { Image(systemName: "command") }
}
