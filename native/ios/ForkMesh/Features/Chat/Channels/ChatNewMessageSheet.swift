import FMAccount
import FMApi
import FMBase
import FMChat
import SwiftUI

struct ChatRecipientPicker: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    @Binding var selected: [ChatRecipient]
    @Binding var query: String
    var focused: FocusState<Bool>.Binding
    var peopleOnly: Bool = false
    @State private var searched: [String] = []
    @State private var searching: Bool = false

    private var channelChosen: Bool { selected.contains { $0.kind == .channel } }

    private var suggestions: [ChatRecipient] {
        if channelChosen { return [] }
        let list: [ChatRecipient] = hub.recipientSuggestions(query, selected: selected, searched: searched, limit: 40)
        if peopleOnly || !selected.isEmpty { return list.filter { $0.kind == .person } }
        return list
    }

    var body: some View {
        VStack(spacing: 0) {
            toField
            list
        }
        .task(id: query) { await search() }
    }

    private var toField: some View {
        HStack(alignment: .center, spacing: FmSpace.x2) {
            Text("To:")
                .fmText(.bodyMedium, color: fm.textSecondary)
            ScrollViewReader { (proxy: ScrollViewProxy) in
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: FmSpace.x1) {
                        ForEach(selected) { (recipient: ChatRecipient) in
                            chip(recipient)
                        }
                        if !channelChosen {
                            TextField(selected.isEmpty ? "@somebody or #channel" : "Add someone", text: $query)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                                .focused(focused)
                                .submitLabel(.next)
                                .onSubmit(pickTop)
                                .frame(minWidth: 140)
                                .id("field")
                                .accessibilityLabel("To")
                        }
                    }
                    .padding(.vertical, FmSpace.x1)
                }
                .onChange(of: selected.count) { _, _ in
                    withAnimation(.easeOut(duration: 0.15)) { proxy.scrollTo("field", anchor: .trailing) }
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .frame(minHeight: 52)
        .shellGlass(RoundedRectangle(cornerRadius: 26, style: .continuous))
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x1)
    }

    private func chip(_ recipient: ChatRecipient) -> some View {
        Button(action: { remove(recipient) }) {
            HStack(spacing: 4) {
                if recipient.kind == .channel {
                    Image(systemName: "number").font(.system(size: 12, weight: .semibold))
                } else {
                    FmAvatar(name: recipient.name, size: 20, fontSize: FmType.micro)
                }
                Text(recipient.kind == .channel ? String(recipient.label.dropFirst()) : recipient.label)
                    .fmText(.labelMedium, color: fm.accent)
                    .lineLimit(1)
                Image(systemName: "xmark").font(.system(size: 10, weight: .bold)).foregroundStyle(fm.accent)
            }
            .padding(.horizontal, FmSpace.x2)
            .frame(minHeight: 30)
            .background(fm.accentSubtle)
            .clipShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Remove \(recipient.label)")
    }

    @ViewBuilder private var list: some View {
        let items: [ChatRecipient] = suggestions
        if channelChosen {
            Spacer(minLength: 0)
        } else if items.isEmpty {
            VStack(spacing: FmSpace.x2) {
                if searching {
                    FmCircularProgressIndicator(size: 20, strokeWidth: 2)
                } else {
                    Text(emptyLine)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                        .multilineTextAlignment(.center)
                }
            }
            .padding(FmSpace.x6)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    if query.isEmpty && selected.isEmpty {
                        FmSectionHeader(title: "Suggested")
                            .padding(.horizontal, FmSpace.x4)
                            .padding(.top, FmSpace.x3)
                    }
                    ForEach(items) { (recipient: ChatRecipient) in
                        row(recipient)
                    }
                }
            }
            .scrollDismissesKeyboard(.interactively)
        }
    }

    private var emptyLine: String {
        if query.trimmingCharacters(in: .whitespaces).isEmpty {
            return hub.canSearchAccounts
                ? "Type a name to find anyone on ForkMesh."
                : "Type a name. Sign in to message people who are not online."
        }
        return "Nobody matches \u{201C}\(query)\u{201D}."
    }

    private func row(_ recipient: ChatRecipient) -> some View {
        Button(action: { add(recipient) }) {
            HStack(spacing: FmSpace.x3) {
                if recipient.kind == .channel {
                    FmIcon(recipient.conversation == meshConversation ? "lan_outlined" : "tag", size: 20, color: fm.textSecondary)
                        .frame(width: 32, height: 32)
                        .background(fm.bgOverlay)
                        .clipShape(FmRoundedShape(radius: FmRadius.md))
                } else {
                    ZStack(alignment: .bottomTrailing) {
                        FmAvatar(name: recipient.name, size: 32, fontSize: FmType.meta)
                        if recipient.online {
                            PresenceDot(online: true, away: recipient.away)
                                .padding(2)
                                .background(fm.bgBase.clipShape(Circle()))
                                .offset(x: 3, y: 3)
                        }
                    }
                }
                VStack(alignment: .leading, spacing: 1) {
                    Text(recipient.label)
                        .fmText(.bodyMedium, color: fm.textPrimary)
                        .lineLimit(1)
                    if !recipient.detail.isEmpty {
                        Text(recipient.detail)
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .lineLimit(1)
                    }
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
            .frame(minHeight: 52)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityHint(recipient.kind == .channel ? "Channel" : "Person")
    }

    private func add(_ recipient: ChatRecipient) {
        if recipient.kind == .channel { selected.removeAll() }
        if !selected.contains(where: { $0.key == recipient.key }) { selected.append(recipient) }
        query = ""
        searched = []
    }

    private func remove(_ recipient: ChatRecipient) {
        selected.removeAll { $0.key == recipient.key }
        focused.wrappedValue = true
    }

    private func pickTop() {
        let trimmed: String = query.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty { return }
        if let top = suggestions.first {
            add(top)
        } else if hub.canSearchAccounts, !trimmed.hasPrefix("#") {
            add(ChatRecipient.person(trimmed))
        }
        focused.wrappedValue = true
    }

    private func search() async {
        let trimmed: String = query.trimmingCharacters(in: .whitespaces)
        if trimmed.count < 2 || !hub.canSearchAccounts {
            searched = []
            return
        }
        try? await Task.sleep(nanoseconds: 250_000_000)
        if Task.isCancelled { return }
        searching = true
        let found: [String] = await hub.searchAccounts(trimmed)
        if Task.isCancelled { return }
        searched = found
        searching = false
    }
}

struct ChatNewMessageSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    var initial: [ChatRecipient] = []
    @State private var selected: [ChatRecipient] = []
    @State private var query: String = ""
    @State private var message: String = ""
    @State private var sending: Bool = false
    @State private var error: String?
    @FocusState private var toFocused: Bool
    @FocusState private var messageFocused: Bool

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
                        .padding(.vertical, FmSpace.x2)
                }
                if !selected.isEmpty {
                    composer
                }
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("New message")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                if !selected.isEmpty && message.dartTrimmed.isEmpty {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Open") { send() }.disabled(sending)
                    }
                }
            }
            .onAppear {
                if selected.isEmpty { selected = initial }
                toFocused = initial.isEmpty
                if !initial.isEmpty { messageFocused = true }
            }
            .onChange(of: selected) { _, now in
                error = nil
                if now.contains(where: { $0.kind == .channel }) { messageFocused = true }
            }
        }
    }

    private var placeholder: String {
        if selected.count == 1 { return "Message \(selected[0].label)" }
        return "Message \(selected.count) people"
    }

    private var composer: some View {
        HStack(alignment: .bottom, spacing: FmSpace.x2) {
            TextField(placeholder, text: $message, axis: .vertical)
                .lineLimit(1...6)
                .focused($messageFocused)
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
                .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg * 2, style: .continuous))
                .accessibilityLabel("Message")
            Button(action: send) {
                Group {
                    if sending {
                        FmCircularProgressIndicator(size: 18, strokeWidth: 2)
                    } else {
                        FmIcon("send", size: 18, color: Color.white)
                    }
                }
                .frame(width: 48, height: 48)
                .background(message.dartTrimmed.isEmpty ? fm.textTertiary : fm.accent)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
            }
            .buttonStyle(.plain)
            .disabled(sending || message.dartTrimmed.isEmpty)
            .accessibilityLabel("Send message")
        }
        .padding(FmSpace.x3)
    }

    private func send() {
        let recipients: [ChatRecipient] = selected
        let text: String = message
        sending = true
        Task {
            let ok: Bool = await hub.sendNewMessage(to: recipients, text: text)
            sending = false
            if ok {
                dismiss()
            } else {
                error = hub.notice?.text ?? "It could not be sent."
            }
        }
    }
}

struct ChatShareMessageSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let request: ChatShareRequest
    @State private var selected: [ChatRecipient] = []
    @State private var query: String = ""
    @State private var comment: String = ""
    @State private var sending: Bool = false
    @State private var error: String?
    @FocusState private var toFocused: Bool

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
                    HStack(alignment: .top, spacing: FmSpace.x2) {
                        Rectangle().fill(fm.border).frame(width: 3)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("\(request.message.senderName) in \(hub.conversationLabel(request.message.conversation))")
                                .fmText(.labelMedium, color: fm.textSecondary)
                            Text(request.text.isEmpty ? request.message.fileName : request.text)
                                .fmText(.bodySmall, color: fm.textPrimary)
                                .lineLimit(4)
                        }
                    }
                    .fixedSize(horizontal: false, vertical: true)
                }
                .padding(FmSpace.x3)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Share message")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") { send() }.disabled(sending || selected.isEmpty)
                }
            }
            .onAppear { toFocused = true }
        }
    }

    private func send() {
        let recipients: [ChatRecipient] = selected
        sending = true
        Task {
            let ok: Bool = await hub.share(request.message, text: request.text, comment: comment, to: recipients)
            sending = false
            if ok {
                dismiss()
            } else {
                error = hub.notice?.text ?? "It could not be shared."
            }
        }
    }
}
