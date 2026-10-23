import AVFoundation
import FMAccount
import FMBase
import FMChat
import Foundation
import Observation

@MainActor
final class DirectCallAudio {
    static let shared = DirectCallAudio()

    private weak var engine: DirectCallEngine?
    private var interruptions: NSObjectProtocol?
    private var lifecycle: UUID?

    func attach(_ engine: DirectCallEngine) {
        self.engine = engine
        observe(engine)
        refresh()
        if interruptions == nil {
            interruptions = NotificationCenter.default.addObserver(
                forName: AVAudioSession.interruptionNotification, object: nil, queue: .main
            ) { [weak self] (note: Notification) in
                let raw = (note.userInfo?[AVAudioSessionInterruptionTypeKey] as? NSNumber)?.uintValue ?? 0
                let ended = AVAudioSession.InterruptionType(rawValue: raw) == .ended
                MainActor.assumeIsolated {
                    if ended { self?.resume() }
                }
            }
        }
        if lifecycle == nil {
            lifecycle = AppLifecycle.shared.observe { [weak self] (state: AppLifecycleState) in
                if state == .resumed { self?.resume() }
            }
        }
    }

    private func observe(_ engine: DirectCallEngine) {
        withObservationTracking {
            _ = engine.phase
            #if canImport(WebRTC)
            _ = NativeGroupCall.shared.active
            #endif
        } onChange: { [weak self, weak engine] in
            Task { @MainActor [weak self, weak engine] in
                guard let self, let engine, self.engine === engine else { return }
                self.refresh()
                self.observe(engine)
            }
        }
    }

    private func refresh() {
        var busy = engine?.active ?? false
        #if canImport(WebRTC)
        busy = busy || NativeGroupCall.shared.active
        #endif
        OtherAppCall.shared.setOwnCall(busy)
    }

    private func resume() {
        guard let engine, engine.active else { return }
        if engine.peerFactory is WebCallPeerFactory {
            try? AVAudioSession.sharedInstance().setActive(true)
        }
        engine.resumeMedia()
    }
}
