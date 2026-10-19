import AVFoundation
import CallKit
import Foundation

@MainActor
final class OtherAppCall: NSObject, CXCallObserverDelegate {
    static let shared: OtherAppCall = OtherAppCall()

    static var inProgress: Bool { live(shared.observer.calls) }

    static var duckingOptions: AVAudioSession.CategoryOptions {
        inProgress ? [.mixWithOthers] : [.duckOthers]
    }

    private let observer: CXCallObserver = CXCallObserver()
    private var active: Bool = false
    private var listeners: [@MainActor (Bool) -> Void] = []

    override private init() {
        super.init()
        observer.setDelegate(self, queue: .main)
        active = Self.live(observer.calls)
    }

    func onChange(_ listener: @escaping @MainActor (Bool) -> Void) {
        listeners.append(listener)
        listener(active)
    }

    nonisolated func callObserver(_ callObserver: CXCallObserver, callChanged call: CXCall) {
        MainActor.assumeIsolated { self.refresh() }
    }

    private func refresh() {
        let now: Bool = Self.live(observer.calls)
        guard now != active else { return }
        active = now
        if now {
            PromptDictation.yieldOverhearing()
            DesktopAlarm.shared.hush()
            GlassesWatcher.shared.hush()
        }
        for listener in listeners { listener(now) }
    }

    private static func live(_ calls: [CXCall]) -> Bool {
        calls.contains { (call: CXCall) -> Bool in !call.hasEnded }
    }
}
