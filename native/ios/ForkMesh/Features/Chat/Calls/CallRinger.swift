import AVFoundation
import AudioToolbox
import FMAccount
import FMAgentWatch
import FMChat
import Foundation
import Observation
import UserNotifications

enum CallRingKind: Equatable {
    case ringback
    case incoming
}

@MainActor
final class CallRinger {
    static let shared = CallRinger()

    private static let buzzEvery: UInt64 = 2_000_000_000

    private weak var engine: DirectCallEngine?
    private var playing: CallRingKind?
    private var player: AVAudioPlayer?
    private var clips: [CallRingKind: Data] = [:]
    private var buzz: Task<Void, Never>?
    private var hushed = ""
    private var noticed = ""
    private var lifecycle: UUID?

    func attach(_ engine: DirectCallEngine) {
        stop()
        clearNotice()
        hushed = ""
        self.engine = engine
        if lifecycle == nil {
            lifecycle = AppLifecycle.shared.observe { [weak self] (_: AppLifecycleState) in
                self?.refresh()
            }
        }
        observe(engine)
        refresh()
    }

    func hush() {
        guard let invite = engine?.invite else { return }
        hushed = invite.callId
        refresh()
    }

    private func observe(_ engine: DirectCallEngine) {
        withObservationTracking {
            _ = engine.phase
            _ = engine.invite
        } onChange: { [weak self, weak engine] in
            Task { @MainActor [weak self, weak engine] in
                guard let self, let engine, self.engine === engine else { return }
                self.refresh()
                self.observe(engine)
            }
        }
    }

    private func refresh() {
        guard let engine else {
            stop()
            clearNotice()
            return
        }
        let pending: DirectCallInvite? = engine.active ? nil : engine.invite
        if pending?.callId != hushed { hushed = "" }
        var wanted: CallRingKind?
        if engine.phase == .ringing {
            wanted = .ringback
        } else if let pending, pending.callId != hushed {
            wanted = .incoming
        }
        if wanted != playing {
            stop()
            if let wanted { start(wanted) }
        }
        notify(pending)
    }

    private func start(_ kind: CallRingKind) {
        playing = kind
        prepareSession()
        if let made = makePlayer(kind) {
            made.currentTime = 0
            made.play()
            player = made
        }
        guard kind == .incoming else { return }
        buzz = Task { @MainActor in
            while !Task.isCancelled {
                AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
                try? await Task.sleep(nanoseconds: CallRinger.buzzEvery)
            }
        }
    }

    private func stop() {
        playing = nil
        player?.stop()
        player = nil
        buzz?.cancel()
        buzz = nil
    }

    private func makePlayer(_ kind: CallRingKind) -> AVAudioPlayer? {
        let clip: Data
        if let cached = clips[kind] {
            clip = cached
        } else {
            let pcm: [Int16] = kind == .ringback ? DirectCallTones.ringback() : DirectCallTones.ring()
            clip = wavData(pcm, rate: DirectCallTones.rate, channels: 1)
            clips[kind] = clip
        }
        guard let made = try? AVAudioPlayer(data: clip) else { return nil }
        made.numberOfLoops = -1
        made.volume = kind == .ringback ? 0.6 : 1
        made.prepareToPlay()
        return made
    }

    private func prepareSession() {
        if PromptDictation.isListening || PromptDictation.isOverhearing { return }
        let session: AVAudioSession = AVAudioSession.sharedInstance()
        if session.category == .playAndRecord { return }
        if session.category != .playback || !session.categoryOptions.contains(.mixWithOthers) {
            try? session.setCategory(.playback, options: [.mixWithOthers])
        }
        try? session.setActive(true)
    }

    private func notify(_ invite: DirectCallInvite?) {
        let id: String = invite.map { "direct-call-\($0.callId)" } ?? ""
        let foreground: Bool = AppLifecycle.shared.state == .resumed
        if !noticed.isEmpty && (noticed != id || foreground) { clearNotice() }
        guard let invite, !foreground, noticed != id else { return }
        noticed = id
        let content: UNMutableNotificationContent = UNMutableNotificationContent()
        content.title = invite.media == .video ? "Incoming video call" : "Incoming voice call"
        content.body = "@\(invite.partner) is calling you"
        content.sound = .default
        content.interruptionLevel = .timeSensitive
        content.threadIdentifier = "direct-call"
        content.userInfo = ["topic": "direct-messages", "conversation": invite.conversation]
        let request: UNNotificationRequest = UNNotificationRequest(identifier: id, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(request) { _ in }
    }

    private func clearNotice() {
        guard !noticed.isEmpty else { return }
        let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
        center.removePendingNotificationRequests(withIdentifiers: [noticed])
        center.removeDeliveredNotifications(withIdentifiers: [noticed])
        noticed = ""
    }
}
