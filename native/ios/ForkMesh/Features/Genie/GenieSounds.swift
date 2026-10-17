import AVFoundation
import FMAgentWatch
import FMBase
import Foundation

@MainActor
final class GenieSounds {
    static let shared: GenieSounds = GenieSounds()

    private var players: [GenieChime: AVAudioPlayer] = [:]
    private var loading: Bool = false
    private var lastPlayed: [GenieChime: Date] = [:]

    func play(_ chime: GenieChime) {
        guard FmSoundCues.isEnabled, !FmCuePlayer.shared.isQuiet, !MessagesVoiceRecorder.isRecording else { return }
        let now: Date = Date()
        if let last = lastPlayed[chime], now.timeIntervalSince(last) < 0.25 { return }
        lastPlayed[chime] = now
        guard let player = players[chime] else {
            load()
            return
        }
        prepareSession()
        player.currentTime = 0
        player.play()
    }

    func load() {
        guard players.isEmpty, !loading else { return }
        loading = true
        Task.detached(priority: .utility) {
            var clips: [GenieChime: Data] = [:]
            for chime in GenieChime.allCases {
                clips[chime] = wavData(chime.render())
            }
            let rendered: [GenieChime: Data] = clips
            await MainActor.run { GenieSounds.shared.install(rendered) }
        }
    }

    private func install(_ clips: [GenieChime: Data]) {
        loading = false
        for (chime, clip) in clips {
            guard let player = try? AVAudioPlayer(data: clip) else { continue }
            player.volume = 0.8
            player.prepareToPlay()
            players[chime] = player
        }
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
}
