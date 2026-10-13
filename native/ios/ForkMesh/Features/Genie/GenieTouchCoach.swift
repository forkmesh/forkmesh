import FMAgentWatch
import FMAssistant
import Foundation
import UIKit

@MainActor
final class GenieTouchCoach {
    static let shared: GenieTouchCoach = GenieTouchCoach()
    nonisolated static let stillEvery: Duration = .milliseconds(250)

    private var play: GenieTouchPlay = GenieTouchPlay()
    private var voice: GenieMoveVoice = GenieMoveVoice()
    private var watch: Task<Void, Never>?

    var holding: Bool { play.holding }

    func moved(to point: CGPoint) {
        let now: Double = Date().timeIntervalSinceReferenceDate
        let x: Double = Double(point.x)
        let y: Double = Double(point.y)
        guard play.holding else {
            voice.begin(x: x, y: y, at: now)
            perform(play.begin(x: x, y: y, at: now, height: Double(GenieCrawler.shared.full.height)))
            watchStill()
            return
        }
        if let chime = voice.move(x: x, y: y, at: now) { GenieSounds.shared.play(chime) }
        perform(play.move(x: x, y: y, at: now))
    }

    func released(at point: CGPoint, velocity: CGSize, parked: Bool) {
        watch?.cancel()
        watch = nil
        perform(play.end(
            x: Double(point.x), y: Double(point.y), vx: Double(velocity.width), vy: Double(velocity.height),
            at: Date().timeIntervalSinceReferenceDate, parked: parked))
    }

    private func watchStill() {
        watch?.cancel()
        watch = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: GenieTouchCoach.stillEvery)
                guard !Task.isCancelled, let self = self, self.play.holding else { return }
                self.perform(self.play.still(at: Date().timeIntervalSinceReferenceDate))
            }
        }
    }

    private func perform(_ gesture: GenieTouchGesture?) {
        guard let gesture = gesture, let trick = play.trick(for: gesture) else { return }
        let hub: GenieHub = GenieHub.shared
        guard !hub.menuOpen, !hub.bubbleOpen, !hub.sheetShown else { return }
        GenieHandCoach.shared.perform(GenieImprovisation(trick: trick, line: "", byModel: false))
    }
}
