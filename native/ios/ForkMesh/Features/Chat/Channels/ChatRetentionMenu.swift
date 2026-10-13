import FMApi
import FMBase
import FMChat
import SwiftUI

struct ChatRetentionMenu: View {
    let hub: ChatHub
    let conversation: String
    @State private var service: ChatRetentionService? = Services.get(ChatRetentionService.self)

    var body: some View {
        if let service = service, service.signedIn {
            let key: String = hub.retentionKey(conversation)
            let chosen: Int64? = service.retention.override(for: key)
            Menu {
                Button(action: { choose(nil) }) {
                    ChatRetentionChoiceLabel(text: service.retention.useDefaultLabel(), chosen: chosen == nil)
                }
                ForEach(ChatRetention.presets) { (preset: ChatRetentionPreset) in
                    Button(action: { choose(preset.ms) }) {
                        ChatRetentionChoiceLabel(text: preset.label, chosen: chosen == preset.ms)
                    }
                }
            } label: {
                Label(
                    "Messages disappear after \(ChatRetention.label(service.retention.effectiveMs(for: key)))",
                    systemImage: "timer")
            }
            .disabled(service.busy)
        }
    }

    private func choose(_ ms: Int64?) {
        guard let service = service else { return }
        let key: String = hub.retentionKey(conversation)
        let hub: ChatHub = self.hub
        Task { @MainActor in
            do {
                try await service.setChat(key, ms)
            } catch {
                hub.post("\(error)")
            }
        }
    }
}

struct ChatRetentionChoiceLabel: View {
    let text: String
    let chosen: Bool

    var body: some View {
        if chosen {
            Label(text, systemImage: "checkmark")
        } else {
            Text(text)
        }
    }
}
