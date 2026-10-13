import Foundation
import Observation

enum GenieReaction: Equatable {
    case wave
    case cheer
    case glance(Double)
    case surprise
    case nod
    case dizzy
    case eureka
    case giggle

    var duration: Double {
        switch self {
        case .wave: return 1.8
        case .cheer: return 1.6
        case .glance: return 1.5
        case .surprise: return 1.3
        case .nod: return 0.9
        case .dizzy: return 1.7
        case .eureka: return 1.4
        case .giggle: return 1.1
        }
    }
}

struct GenieCue: Equatable {
    let reaction: GenieReaction
    let at: Date

    func age(_ now: Date) -> Double? {
        let age: Double = now.timeIntervalSince(at)
        return age >= 0 && age < reaction.duration ? age : nil
    }
}

@MainActor
@Observable
final class GenieCues {
    static let shared: GenieCues = GenieCues()
    nonisolated static let idleWaveEvery: Double = 21
    nonisolated static let quietAfterCue: Double = 4

    private(set) var cue: GenieCue?

    func play(_ reaction: GenieReaction) {
        cue = GenieCue(reaction: reaction, at: Date())
    }

    func tabChanged(to index: Int, of count: Int) {
        let share: Double = count > 1 ? Double(index) / Double(count - 1) : 0
        play(.glance(-1 + share * 0.6))
    }

    func agentsChanged(from old: Int, to new: Int) {
        if new > old {
            play(.cheer)
        } else if new < old {
            play(.nod)
        }
    }

    func unreadChanged(from old: Int, to new: Int) {
        if new > old { play(.surprise) }
    }

    nonisolated static func current(_ cue: GenieCue?, at now: Date, ambient: Bool) -> (GenieReaction, Double)? {
        if let cue = cue, let age = cue.age(now) {
            return (cue.reaction, age)
        }
        guard ambient else { return nil }
        if let cue = cue, now.timeIntervalSince(cue.at) < cue.reaction.duration + quietAfterCue {
            return nil
        }
        let phase: Double = now.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: idleWaveEvery)
        return phase < GenieReaction.wave.duration ? (.wave, phase) : nil
    }
}
