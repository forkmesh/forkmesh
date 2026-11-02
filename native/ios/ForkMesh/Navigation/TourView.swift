import FMApi
import FMPackages
import SwiftUI
import TipKit

struct TourLaunch: Identifiable, Hashable {
    let track: TourTrack
    let at: Int

    var id: String { "\(track.rawValue):\(at)" }
}

struct TourView: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var done: Set<TourTrack> = ShellTour.done
    @State private var place: TourPlace? = ShellTour.place
    @State private var playing: TourLaunch?
    let onFinish: (() -> Void)?
    var onJump: ((TourJump) -> Void)? = nil

    var body: some View {
        TourChooser(
            done: done, place: resumable, counts: counts, finished: nil,
            onPick: { (picked: TourTrack) in
                FmHaptics.tap()
                playing = TourLaunch(track: picked, at: 0)
            },
            onResume: { (resume: TourPlace, at: Int) in
                FmHaptics.tap()
                playing = TourLaunch(track: resume.track, at: at)
            },
            onSkip: onFinish.map { (finish: @escaping () -> Void) in { ShellTour.markSeen(); finish() } })
            .fmPreGlassBackground(fm.canvas)
            .navigationTitle("Tour")
            .navigationBarTitleDisplayMode(.inline)
            .fullScreenCover(item: $playing, onDismiss: refresh) { (launch: TourLaunch) in
                TourPlayer(track: launch.track, at: launch.at, onClose: { playing = nil }, onJump: onJump)
            }
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

    private func refresh() {
        done = ShellTour.done
        place = ShellTour.place
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
            VStack(alignment: .leading, spacing: 0) {
                Text("Take the tour")
                    .fmText(.displaySmall)
                    .accessibilityAddTraits(.isHeader)
                    .padding(.top, FmSpace.x5)
                Text("Pick how deep to go. Each stop shows the real screen.")
                    .fmText(.bodyLarge, color: fm.textSecondary, height: FmType.leadingSnug)
                    .padding(.top, FmSpace.x2)
                if let finished = finished {
                    group { finishedRow(finished) }
                        .padding(.top, FmSpace.x5)
                } else if let place = place {
                    group { resumeRow(place) }
                        .padding(.top, FmSpace.x5)
                }
                group {
                    ForEach(Array(TourTrack.allCases.enumerated()), id: \.element) {
                        (entry: (offset: Int, element: TourTrack)) in
                        if entry.offset > 0 {
                            FmRowDivider(indent: FmSpace.x4)
                        }
                        TourTrackRow(
                            track: entry.element, count: counts[entry.element] ?? 0,
                            done: done.contains(entry.element), suggested: suggested == entry.element,
                            onPick: { onPick(entry.element) })
                    }
                }
                .padding(.top, FmSpace.x5)
                if let onSkip = onSkip {
                    Button(finished == nil ? "Not now" : "Done for now", action: onSkip)
                        .fmText(.labelLarge, color: fm.textSecondary)
                        .frame(maxWidth: .infinity, minHeight: 44)
                        .padding(.top, FmSpace.x4)
                        .accessibilityIdentifier("tour-skip")
                }
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.bottom, FmSpace.x5)
        }
        .accessibilityIdentifier("tour-chooser")
    }

    private var suggested: TourTrack? {
        TourTrack.allCases.first { !done.contains($0) }
    }

    private func group<Rows: View>(@ViewBuilder _ rows: () -> Rows) -> some View {
        VStack(spacing: 0) {
            rows()
        }
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg * 2))
    }

    private func finishedRow(_ track: TourTrack) -> some View {
        HStack(spacing: FmSpace.x3) {
            VStack(alignment: .leading, spacing: 2) {
                Text("\(track.title) done")
                    .fmText(.titleSmall)
                Text("Keep going, or close the tour and explore.")
                    .fmText(.bodyMedium, color: fm.textSecondary)
            }
            Spacer(minLength: 0)
            Image(systemName: "checkmark")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(fm.success)
                .symbolEffect(.bounce, value: track)
        }
        .padding(.horizontal, FmSpace.x4)
        .frame(minHeight: 60)
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("tour-finished")
    }

    private func resumeRow(_ place: (place: TourPlace, index: Int, total: Int)) -> some View {
        Button(action: { onResume(place.place, place.index) }) {
            HStack(spacing: FmSpace.x3) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Pick up where you left off")
                        .fmText(.titleSmall, color: fm.accent)
                    Text("\(place.place.track.title) · stop \(place.index + 1) of \(place.total)")
                        .fmText(.bodyMedium, color: fm.textSecondary)
                }
                Spacer(minLength: 0)
                Image(systemName: "chevron.right")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(fm.textDisabled)
            }
            .padding(.horizontal, FmSpace.x4)
            .frame(minHeight: 60)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06)))
        .accessibilityIdentifier("tour-resume")
    }
}

struct TourTrackRow: View {
    @Environment(\.fmPalette) private var fm

    let track: TourTrack
    let count: Int
    let done: Bool
    let suggested: Bool
    let onPick: () -> Void

    var body: some View {
        Button(action: onPick) {
            HStack(spacing: FmSpace.x3) {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                        Text(track.title)
                            .fmText(.titleSmall)
                        if suggested {
                            Text(track == .essentials ? "Start here" : "Up next")
                                .fmText(.bodySmall, color: fm.accent, weight: FmType.emphasis)
                        }
                    }
                    Text(meta)
                        .fmText(.bodyMedium, color: fm.textSecondary, tabular: true)
                }
                Spacer(minLength: 0)
                Image(systemName: done ? "checkmark" : "chevron.right")
                    .font(.system(size: done ? 15 : 13, weight: .semibold))
                    .foregroundStyle(done ? fm.success : fm.textDisabled)
                    .accessibilityHidden(true)
            }
            .padding(.horizontal, FmSpace.x4)
            .frame(minHeight: 60)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06)))
        .accessibilityElement(children: .ignore)
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel(Text("\(track.title). \(meta). \(track.summary)\(done ? " Done." : "")"))
        .accessibilityIdentifier("tour-track-\(track.rawValue)")
    }

    private var meta: String {
        let minutes: Int = TourCatalog.minutes(for: count)
        return "\(count) stops · about \(minutes) min"
    }
}

enum ShellTour {
    static let seenKey = "fm.tour.seen.v1"
    static let doneKey = "fm.tour.done.v1"
    static let placeKey = "fm.tour.place.v1"

    static var seen: Bool { UserDefaults.standard.bool(forKey: seenKey) }

    static func markSeen() {
        UserDefaults.standard.set(true, forKey: seenKey)
        allowTips()
    }

    static func allowTips() {
        VoiceTip.afterTour = true
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
        if StoreScreenshotLaunch.on {
            Tips.hideAllTipsForTesting()
        }
        try? Tips.configure([.displayFrequency(.daily)])
    }
}

struct HubTip: Tip {
    var rules: [Rule] { [#Rule(VoiceTip.$afterTour) { $0 }] }
    var title: Text { Text("Your hub") }
    var message: Text? { Text("Your status, Pocket mode, Settings and signing out live here.") }
    var image: Image? { Image(systemName: "person.crop.circle") }
}

struct VoiceTip: Tip {
    @Parameter static var afterTour: Bool = false
    var rules: [Rule] { [#Rule(VoiceTip.$afterTour) { $0 }] }
    var title: Text { Text("Hold to talk") }
    var message: Text? { Text("Hold the new agent button and say what the agent should do. It starts when you let go.") }
    var image: Image? { Image(systemName: "waveform") }
}

struct PaletteTip: Tip {
    var rules: [Rule] { [#Rule(VoiceTip.$afterTour) { $0 }] }
    var title: Text { Text("Hold for commands") }
    var message: Text? { Text("Hold the new agent button to open the command palette.") }
    var image: Image? { Image(systemName: "command") }
}
