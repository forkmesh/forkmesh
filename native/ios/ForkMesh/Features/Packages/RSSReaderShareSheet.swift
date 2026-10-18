import FMChat
import FMPackages
import SwiftUI

struct RSSChatShare: Identifiable {
    let id: UUID = UUID()
    let title: String
    let link: String
    let feed: String
}

struct RSSReaderShareSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let share: RSSChatShare
    let onSent: (String) -> Void
    @State private var selected: [ChatRecipient] = []
    @State private var query: String = ""
    @State private var comment: String = ""
    @State private var sending: Bool = false
    @State private var error: String?
    @FocusState private var toFocused: Bool

    private var host: String { RSSURL.host(share.link) }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                ChatRecipientPicker(hub: hub, selected: $selected, query: $query, focused: $toFocused)
                if let error {
                    Text(error)
                        .fmText(.bodySmall, color: fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                        .padding(.horizontal, FmSpace.x4)
                }
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    TextField("Add a message, if you like", text: $comment, axis: .vertical)
                        .lineLimit(1...4)
                        .padding(FmSpace.x3)
                        .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg * 2, style: .continuous))
                        .accessibilityLabel("Message")
                    HStack(alignment: .top, spacing: FmSpace.x2) {
                        Rectangle().fill(fm.border).frame(width: 3)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(share.feed.isEmpty ? host : share.feed)
                                .fmText(.labelMedium, color: fm.textSecondary)
                                .lineLimit(1)
                            Text(share.title.isEmpty ? "Untitled" : share.title)
                                .fmText(.bodySmall, color: fm.textPrimary)
                                .lineLimit(3)
                            Text(share.link)
                                .fmText(.bodySmall, color: fm.textTertiary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                    }
                    .fixedSize(horizontal: false, vertical: true)
                }
                .padding(FmSpace.x3)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Send to Chat")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") { send() }
                        .disabled(sending || selected.isEmpty)
                        .accessibilityIdentifier("rss-share-chat-send")
                }
            }
            .onAppear { toFocused = true }
            .onChange(of: selected) { _, _ in error = nil }
        }
    }

    private func send() {
        let recipients: [ChatRecipient] = selected
        let text: String = RSSReaderText.shareMessage(title: share.title, link: share.link, feed: share.feed, comment: comment)
        sending = true
        Task {
            guard let conversation = await hub.conversation(for: recipients) else {
                sending = false
                error = hub.notice?.text ?? "It could not be sent."
                return
            }
            hub.workspace.noteRecipients(recipients)
            let ok: Bool = await hub.deliver(text, to: conversation)
            sending = false
            if ok {
                onSent(hub.conversationLabel(conversation))
                dismiss()
            } else {
                error = hub.notice?.text ?? "It could not be sent."
            }
        }
    }
}
