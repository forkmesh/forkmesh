import Foundation
import FMAccount
import FMAgentWatch
import FMBase
import SwiftUI
import UIKit

/// Touch feedback, so the choice of intensity is made once, here, rather than
/// guessed at each call site. selectionClick is the selection generator;
/// lightImpact, mediumImpact and heavyImpact the impact generator.
///
/// Haptics are suppressed when the platform asks for reduced motion, since the
/// same accessibility settings that disable animation also cover vibration for
/// people with vestibular or sensory sensitivities.
///
/// The generators belong to the main actor. Each call hops to the main queue,
/// so these can be called from any closure without the caller having to prove
/// its isolation; the delay is one run-loop turn.
enum FmHaptics {
    static var enabled: Bool {
        guard let prefs = Services.get(KeyValueStore.self) else { return true }
        return SettingsService.hapticsEnabled(prefs)
    }

    /// Moving between tabs, segments, and filters: the lightest tick there is.
    static func selection(_ cue: FmCue = .select) {
        FmSoundCues.play(cue)
        DispatchQueue.main.async {
            if UIAccessibility.isReduceMotionEnabled || !FmHaptics.enabled {
                return
            }
            UISelectionFeedbackGenerator().selectionChanged()
        }
    }

    /// Tapping a row, toggling a switch, opening a sheet.
    static func tap(_ cue: FmCue = .click) {
        FmSoundCues.play(cue)
        impact(.light)
    }

    /// Committing something: publishing, committing, confirming a destructive
    /// action, or completing a long-running task.
    static func commit(_ cue: FmCue = .success) {
        FmSoundCues.play(cue)
        impact(.medium)
    }

    /// A refused action or a failed request.
    static func failure(_ cue: FmCue = .error) {
        FmSoundCues.play(cue)
        impact(.heavy)
    }

    static func send(_ cue: FmCue = .send) {
        FmSoundCues.play(cue)
        impact(.light)
    }

    static func success() {
        FmSoundCues.play(.success)
        notify(.success)
    }

    static func warning() {
        FmSoundCues.play(.warn)
        notify(.warning)
    }

    static func error() {
        FmSoundCues.play(.error)
        notify(.error)
    }

    static func palette() {
        FmSoundCues.play(.open)
        impact(.medium)
    }

    @MainActor private static let ratchetTooth = UIImpactFeedbackGenerator(style: .light)
    @MainActor private static let ratchetCatch = UIImpactFeedbackGenerator(style: .rigid)

    @MainActor static func ratchet(_ stroke: RatchetStroke) {
        if UIAccessibility.isReduceMotionEnabled || !enabled {
            return
        }
        let crisp: Bool = stroke.burst.pawl == .crisp
        ratchetTooth.impactOccurred(intensity: crisp ? 0.85 : 0.5)
        if stroke.caught {
            ratchetCatch.impactOccurred(intensity: 1)
        }
        ratchetTooth.prepare()
    }

    private static func notify(_ type: UINotificationFeedbackGenerator.FeedbackType) {
        DispatchQueue.main.async {
            if UIAccessibility.isReduceMotionEnabled || !FmHaptics.enabled {
                return
            }
            UINotificationFeedbackGenerator().notificationOccurred(type)
        }
    }

    static func impact(_ style: UIImpactFeedbackGenerator.FeedbackStyle) {
        DispatchQueue.main.async {
            if UIAccessibility.isReduceMotionEnabled || !FmHaptics.enabled {
                return
            }
            UIImpactFeedbackGenerator(style: style).impactOccurred()
        }
    }
}

enum FmFeedbackEvent: Equatable {
    case send
    case success
    case failure
    case destructiveConfirmation
    case selection
    case palette

    var feedback: SensoryFeedback {
        switch self {
        case .send: return .impact(weight: .light)
        case .success: return .success
        case .failure: return .error
        case .destructiveConfirmation: return .warning
        case .selection: return .selection
        case .palette: return .impact(weight: .medium)
        }
    }

    var cue: FmCue {
        switch self {
        case .send: return .send
        case .success: return .success
        case .failure: return .error
        case .destructiveConfirmation: return .warn
        case .selection: return .select
        case .palette: return .open
        }
    }
}

extension View {
    func fmFeedback<Trigger: Equatable>(_ event: FmFeedbackEvent, trigger: Trigger) -> some View {
        sensoryFeedback(event.feedback, trigger: trigger)
            .onChange(of: trigger) { (_: Trigger, _: Trigger) in
                FmSoundCues.play(event.cue)
            }
    }
}
