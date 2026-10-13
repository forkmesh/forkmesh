// The watch's switches (docs/native/PARITY.md, 4.3): Listen (every agent's
// sounds, on AirPods or a Bluetooth speaker, wrist down included), the live
// view's sounds and haptics, the announcer, and the per-agent mutes.

import FMWatchWire
import SwiftUI

struct WatchSettingsView: View {
    @Environment(WatchStore.self) private var store

    var body: some View {
        @Bindable var feedback = store.feedback
        List {
            Section {
                Toggle("Listen", isOn: Binding(
                    get: { store.feedback.listening },
                    set: { store.setListening($0) }))
            } footer: {
                Text("Plays every agent's work as music on AirPods or a Bluetooth speaker, even with your wrist down.")
            }
            Section("Live view") {
                Toggle("Sounds", isOn: $feedback.soundsOn)
                Toggle("Haptics", isOn: $feedback.hapticsOn)
                Toggle("Announcer", isOn: $feedback.announceOn)
            }
            let muted = store.frames.filter { store.feedback.isMuted($0.key) }
            if !muted.isEmpty {
                Section("Muted agents") {
                    ForEach(muted) { (frame: AgentFrame) in
                        Button {
                            store.feedback.setMuted(frame.key, false)
                        } label: {
                            Label(frame.title, systemImage: "speaker.slash")
                                .lineLimit(1)
                        }
                    }
                }
            }
        }
        .navigationTitle("Settings")
    }
}
