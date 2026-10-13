import AVFoundation
import FMApi
import Observation
import SwiftUI
import UIKit

struct DesktopAlarmRing: Identifiable, Equatable {
    let id: String
    let alarm: String
    let title: String
    let body: String
}

@MainActor
@Observable
final class DesktopAlarm {
    static let shared: DesktopAlarm = DesktopAlarm()
    static let soundName: String = "desktop_power_alarm"
    static let ringSeconds: UInt64 = 5 * 60

    var ringing: DesktopAlarmRing?

    @ObservationIgnored private var player: AVAudioPlayer?
    @ObservationIgnored private var timeout: Task<Void, Never>?

    func ring(_ open: PushOpen) {
        guard open.isAlarm else { return }
        let id: String = open.alertId.isEmpty ? UUID().uuidString : open.alertId
        if ringing?.id == id { return }
        ringing = DesktopAlarmRing(id: id, alarm: open.alarm, title: open.title, body: open.body)
        startSound()
        UINotificationFeedbackGenerator().notificationOccurred(.warning)
        timeout?.cancel()
        timeout = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: DesktopAlarm.ringSeconds * 1_000_000_000)
            guard !Task.isCancelled else { return }
            self?.stop()
        }
    }

    func stop() {
        timeout?.cancel()
        timeout = nil
        player?.stop()
        player = nil
        ringing = nil
        if !PromptDictation.isListening {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        }
    }

    private func startSound() {
        guard player == nil,
            let url = Bundle.main.url(forResource: DesktopAlarm.soundName, withExtension: "wav"),
            let sound = try? AVAudioPlayer(contentsOf: url)
        else { return }
        if !PromptDictation.isListening {
            let session: AVAudioSession = AVAudioSession.sharedInstance()
            try? session.setCategory(.playback, options: [.duckOthers])
            try? session.setActive(true)
        }
        sound.numberOfLoops = -1
        sound.volume = 1
        sound.prepareToPlay()
        sound.play()
        player = sound
    }
}

struct DesktopAlarmView: View {
    let ring: DesktopAlarmRing
    let onStop: () -> Void

    var body: some View {
        ZStack {
            Color.red.opacity(0.92).ignoresSafeArea()
            VStack(spacing: 24) {
                Spacer()
                Image(systemName: "bolt.slash.fill")
                    .font(.system(size: 72, weight: .bold))
                    .symbolEffect(.pulse)
                    .accessibilityHidden(true)
                Text(ring.title.isEmpty ? "Your desktop lost power" : ring.title)
                    .font(.largeTitle.bold())
                    .multilineTextAlignment(.center)
                if !ring.body.isEmpty {
                    Text(ring.body)
                        .font(.title3)
                        .multilineTextAlignment(.center)
                }
                Spacer()
                Button(action: onStop) {
                    Text("Stop alarm")
                        .font(.title2.bold())
                        .frame(maxWidth: .infinity, minHeight: 64)
                }
                .buttonStyle(.borderedProminent)
                .tint(.white)
                .foregroundStyle(Color.red)
                .accessibilityIdentifier("desktopAlarmStop")
            }
            .foregroundStyle(.white)
            .padding(32)
        }
    }
}
