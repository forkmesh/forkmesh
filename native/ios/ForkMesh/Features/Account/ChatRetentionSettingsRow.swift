import FMApi
import SwiftUI

struct ChatRetentionSettingsRow: View {
    @Environment(\.fmPalette) var fm
    let service: ChatRetentionService
    let onRefused: (String) -> Void

    var body: some View {
        let current: Int64 = service.retention.defaultMs
        HStack(spacing: FmSpace.x3) {
            FmIcon("timer_outlined", size: 24, color: fm.textSecondary)
            VStack(alignment: .leading, spacing: 2) {
                Text("Messages disappear after").fmText(.bodyLarge)
                Text("Your default for every chat. Change one chat from its menu in the channel list.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            Menu {
                ForEach(ChatRetention.presets) { (preset: ChatRetentionPreset) in
                    Button(action: { choose(preset.ms) }) {
                        ChatRetentionChoiceLabel(text: preset.label, chosen: current == preset.ms)
                    }
                }
            } label: {
                Text(ChatRetention.label(current))
                    .fmText(.bodyLarge, color: fm.accent)
                    .frame(minHeight: 44)
            }
            .disabled(service.busy)
            .accessibilityLabel("Messages disappear after \(ChatRetention.label(current))")
        }
        .padding(.vertical, FmSpace.x2)
        .accessibilityIdentifier("settings-chat-retention")
    }

    private func choose(_ ms: Int64) {
        let service: ChatRetentionService = self.service
        let onRefused: (String) -> Void = self.onRefused
        Task { @MainActor in
            do {
                try await service.setDefault(ms)
            } catch {
                onRefused("\(error)")
            }
        }
    }
}
