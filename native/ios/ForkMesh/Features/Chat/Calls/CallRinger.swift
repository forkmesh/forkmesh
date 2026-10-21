import AVFoundation
import AudioToolbox
import FMAccount
import FMAgentWatch
import FMBase
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
    private var swept = ""
    private var ringSound: UNNotificationSound = .default
    private var lifecycle: UUID?

    func attach(_ engine: DirectCallEngine) {
        stop()
        clearNotice()
        hushed = ""
        self.engine = engine
        installRingSound()
        if lifecycle == nil {
            lifecycle = AppLifecycle.shared.observe { [weak self] (_: AppLifecycleState) in
                self?.refresh()
            }
        }
        observe(engine)
        refresh()
    }

    func recheck() {
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
        let systemRings: Bool = pending.map { DirectCallKit.shared.ringing($0.callId) } ?? false
        var wanted: CallRingKind?
        if engine.phase == .ringing {
            wanted = .ringback
        } else if let pending, pending.callId != hushed, !systemRings {
            wanted = .incoming
        }
        if wanted != playing {
            stop()
            if let wanted { start(wanted) }
        }
        notify(systemRings ? nil : pending)
        sweepPushes(pending)
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
        content.sound = ringSound
        content.interruptionLevel = .timeSensitive
        content.threadIdentifier = "direct-call"
        content.userInfo = ["topic": "direct-messages", "conversation": invite.conversation]
        let request: UNNotificationRequest = UNNotificationRequest(identifier: id, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(request) { _ in }
    }

    private func installRingSound() {
        let files: FileManager = FileManager.default
        guard let library: URL = files.urls(for: .libraryDirectory, in: .userDomainMask).first else { return }
        let folder: URL = library.appendingPathComponent("Sounds", isDirectory: true)
        let file: URL = folder.appendingPathComponent(DirectCallEngine.ringSound)
        if !files.fileExists(atPath: file.path) {
            let pcm: [Int16] = DirectCallTones.ringLoop(seconds: DirectCallEngine.ringSoundSeconds)
            do {
                try files.createDirectory(at: folder, withIntermediateDirectories: true)
                try wavData(pcm, rate: DirectCallTones.rate, channels: 1)
                    .write(to: file, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
            } catch {
                return
            }
        }
        ringSound = UNNotificationSound(named: UNNotificationSoundName(DirectCallEngine.ringSound))
    }

    private func sweepPushes(_ invite: DirectCallInvite?) {
        let foreground: Bool = AppLifecycle.shared.state == .resumed
        let ringing: String = invite?.conversation ?? ""
        let ended: String = swept == ringing ? "" : swept
        swept = ringing
        guard foreground || !ringing.isEmpty || !ended.isEmpty else { return }
        Task { @MainActor in
            let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
            var gone: [String] = []
            var waiting: Set<String> = []
            for delivered in await center.deliveredNotifications() {
                let info: [AnyHashable: Any] = delivered.request.content.userInfo
                guard (info["topic"] as? String) == DirectCallEngine.ringTopic else { continue }
                let conversation: String = (info["conversation"] as? String) ?? ""
                guard foreground || conversation == ringing || conversation == ended else { continue }
                gone.append(delivered.request.identifier)
                if foreground, ringing.isEmpty, -delivered.date.timeIntervalSinceNow < DirectCallEngine.ringTimeout {
                    waiting.insert(conversation)
                }
            }
            if !gone.isEmpty { center.removeDeliveredNotifications(withIdentifiers: gone) }
            guard let direct: DirectMessageService = Services.get(DirectMessageService.self) else { return }
            for conversation in waiting { await direct.openRinging(conversation) }
        }
    }

    private func clearNotice() {
        guard !noticed.isEmpty else { return }
        let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
        center.removePendingNotificationRequests(withIdentifiers: [noticed])
        center.removeDeliveredNotifications(withIdentifiers: [noticed])
        noticed = ""
    }
}
