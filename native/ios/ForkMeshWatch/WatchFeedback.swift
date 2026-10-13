// What the watch plays and taps for an agent's beats (docs/native/PARITY.md,
// 4.3): haptics through WKInterfaceDevice (Core Haptics is not on watchOS),
// FMAgentWatch's sonifier notes through AVAudioEngine, and the announcer's
// one-liners through AVSpeechSynthesizer, all on one playback session.
//
// Short phrases play on the watch speaker while the app is in front; Listen
// switches the session to long-form audio, which routes to AirPods or a
// Bluetooth speaker and, with the `audio` background mode, keeps playing with
// the wrist down.

import AVFoundation
import FMAgentWatch
import FMWatchWire
import Foundation
import Observation
import WatchKit

@MainActor
@Observable
final class WatchFeedback {
    /// The settings screen's switches, kept across launches.
    var hapticsOn: Bool { didSet { save("haptics", hapticsOn) } }
    var soundsOn: Bool { didSet { save("sounds", soundsOn) } }
    var announceOn: Bool { didSet { save("announce", announceOn) } }
    private(set) var listening: Bool
    private(set) var muted: Set<String>

    @ObservationIgnored private var pacer = HapticPacer()
    @ObservationIgnored private let sound = AgentWatchSound()
    @ObservationIgnored private let engine = AVAudioEngine()
    @ObservationIgnored private let player = AVAudioPlayerNode()
    @ObservationIgnored private let speech = AVSpeechSynthesizer()
    @ObservationIgnored private var engineReady = false
    @ObservationIgnored private var sessionMode: SessionMode = .off
    @ObservationIgnored private let defaults = UserDefaults.standard

    private enum SessionMode { case off, speaker, longForm }

    init() {
        let d = UserDefaults.standard
        hapticsOn = d.object(forKey: "fm.watch.haptics") as? Bool ?? true
        soundsOn = d.object(forKey: "fm.watch.sounds") as? Bool ?? true
        announceOn = d.object(forKey: "fm.watch.announce") as? Bool ?? false
        listening = false
        muted = Set(d.stringArray(forKey: "fm.watch.muted") ?? [])
    }

    private func save(_ name: String, _ value: Bool) {
        defaults.set(value, forKey: "fm.watch.\(name)")
    }

    func isMuted(_ key: String) -> Bool { muted.contains(key) }

    func setMuted(_ key: String, _ on: Bool) {
        if on { muted.insert(key) } else { muted.remove(key) }
        defaults.set(Array(muted), forKey: "fm.watch.muted")
    }

    func setListening(_ on: Bool) {
        listening = on
        if on {
            activateSession(.longForm)
        } else {
            player.stop()
            deactivateSession()
        }
    }

    // MARK: Beats

    /// New beats of one agent. The agent open on the watch plays and taps;
    /// with Listen on, every agent that is not muted plays.
    func beats(_ beats: [WireBeat], of frame: AgentFrame, watched: Bool) {
        if beats.isEmpty || isMuted(frame.key) { return }
        if watched && hapticsOn, let haptic = pacer.next(for: beats, now: Date().timeIntervalSinceReferenceDate) {
            play(haptic)
        }
        if (watched && soundsOn) || listening {
            let notes = sound.notes(for: Array(beats.suffix(8)), key: frame.key, title: frame.title)
            playNotes(notes)
        }
    }

    /// A run started, finished, failed or wants attention: a tap for
    /// everyone, the closing cadence and the announcer's line.
    func statusChanged(_ frame: AgentFrame, from old: WireStatus, watched: Bool) {
        if isMuted(frame.key) { return }
        let haptic: WatchHapticKind
        let line: String
        var cadence: Bool?
        switch frame.status {
        case .done, .merged:
            haptic = .success
            line = "\(frame.title): run done."
            cadence = true
        case .failed:
            haptic = .failure
            line = "\(frame.title): run failed."
            cadence = false
        case .waiting:
            haptic = .notification
            line = "\(frame.title) is waiting for you."
        case .running where old == .queued:
            haptic = .click
            line = "\(frame.title) started."
        default:
            return
        }
        if hapticsOn, let pick = pacer.take(haptic, now: Date().timeIntervalSinceReferenceDate) {
            play(pick)
        }
        if let success = cadence, (watched && soundsOn) || listening {
            playNotes(sound.cadence(key: frame.key, title: frame.title, success: success))
        }
        if announceOn && (watched || listening) {
            announce("ForkMesh: \(line)")
        }
    }

    /// A control's answer: a failure buzzes, a success clicks.
    func acknowledge(_ reply: WatchReply) {
        guard hapticsOn else { return }
        play(reply.ok ? .click : .failure)
    }

    private func play(_ haptic: WatchHapticKind) {
        let type: WKHapticType
        switch haptic {
        case .click: type = .click
        case .directionUp: type = .directionUp
        case .failure: type = .failure
        case .success: type = .success
        case .notification: type = .notification
        }
        WKInterfaceDevice.current().play(type)
    }

    // MARK: Sound

    private func playNotes(_ notes: [Note]) {
        if notes.isEmpty { return }
        activateSession(listening ? .longForm : .speaker)
        guard prepareEngine() else { return }
        let from = sound.now
        Task { @MainActor [weak self] in
            guard let self, let buffer = await AgentWatchSound.buffer(notes, from: from) else { return }
            // Queued after whatever is still sounding, so phrases never overlap.
            self.player.scheduleBuffer(buffer, completionHandler: nil)
            if !self.player.isPlaying { self.player.play() }
        }
    }

    private func prepareEngine() -> Bool {
        if !engineReady {
            engine.attach(player)
            engine.connect(player, to: engine.mainMixerNode, format: AgentWatchSound.format)
            engineReady = true
        }
        if !engine.isRunning {
            do {
                try engine.start()
            } catch {
                return false
            }
        }
        return true
    }

    private func announce(_ line: String) {
        activateSession(listening ? .longForm : .speaker)
        let utterance = AVSpeechUtterance(string: line)
        utterance.rate = AVSpeechUtteranceDefaultSpeechRate
        speech.speak(utterance)
    }

    // MARK: Audio session

    private func activateSession(_ mode: SessionMode) {
        if sessionMode == mode { return }
        let session = AVAudioSession.sharedInstance()
        do {
            switch mode {
            case .off:
                return
            case .speaker:
                try session.setCategory(.playback, mode: .default, policy: .default, options: [])
                try session.setActive(true)
            case .longForm:
                try session.setCategory(.playback, mode: .default, policy: .longFormAudio, options: [])
                // Long-form audio on watchOS asks for a Bluetooth route first
                // (the system shows its picker when none is connected).
                session.activate(options: []) { (_: Bool, _: Error?) in }
            }
            sessionMode = mode
        } catch {
            sessionMode = .off
        }
    }

    private func deactivateSession() {
        if sessionMode == .off { return }
        if engine.isRunning { engine.stop() }
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        sessionMode = .off
    }
}
