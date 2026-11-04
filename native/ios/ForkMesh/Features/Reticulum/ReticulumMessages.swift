import CoreTransferable
import FMBase
import FMPackages
import SwiftUI
import UniformTypeIdentifiers

struct ReticulumAttachmentItem: Transferable, Sendable {
    let name: String
    let data: Data

    static var transferRepresentation: some TransferRepresentation {
        DataRepresentation(exportedContentType: .data) { item in item.data }
            .suggestedFileName { item in item.name.isEmpty ? "attachment" : item.name }
    }
}

struct ReticulumMessagesTab: View {
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub
    let onNew: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            HStack(spacing: FmSpace.x2) {
                FmFilledButton("New message", icon: "edit", onPressed: onNew)
                    .accessibilityIdentifier("reticulum-new-message")
                Spacer(minLength: 0)
            }
            let conversations = hub.conversations
            if conversations.isEmpty {
                Text("No conversations yet. Start one with New message, or wait for someone to write to you.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            } else {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(conversations) { conversation in
                        ReticulumConversationRow(hub: hub, conversation: conversation)
                        FmRowDivider(indent: FmSpace.x3)
                    }
                }
                .background(fm.bgRaised)
                .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
            }
            Text("Messages use LXMF, so Sideband, MeshChat and Nomad Network users can write to you. Announce so they can find you.")
                .fmText(.bodySmall, color: fm.textTertiary)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: FmSpace.x2) {
                Text("Your address").fmText(.labelMedium, color: fm.textTertiary)
                Text(hub.messagingAddressText).fmMono(color: fm.textSecondary).textSelection(.enabled).lineLimit(1).minimumScaleFactor(0.7)
            }
        }
        .padding(.top, FmSpace.x2)
    }
}

struct ReticulumConversationRow: View {
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub
    let conversation: ReticulumConversation

    var body: some View {
        Button {
            FmHaptics.tap()
            hub.openConversation(conversation.peer)
        } label: {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(conversation.name)
                        .fmText(.titleSmall, weight: conversation.unread > 0 ? .bold : nil)
                        .lineLimit(1)
                    Text(hub.conversationPreview(conversation))
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .lineLimit(1)
                }
                Spacer(minLength: 0)
                if conversation.unread > 0 { FmCountPill(count: conversation.unread, active: true) }
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("reticulum-conversation")
    }
}

struct ReticulumConversationScreen: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var hub: ReticulumHub
    @State private var confirmDelete = false

    var body: some View {
        let messages = hub.chat
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text(hub.chatSubtitle)
                        .fmText(.labelSmall, color: fm.textTertiary)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    if messages.isEmpty {
                        Text("No messages yet. Write the first one below.")
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .padding(.top, FmSpace.x4)
                    }
                    ForEach(messages) { message in
                        ReticulumBubble(hub: hub, message: message).id(message.id)
                    }
                    Color.clear.frame(height: 1).id("reticulum-chat-end")
                }
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
            }
            .onAppear { proxy.scrollTo("reticulum-chat-end", anchor: .bottom) }
            .onChange(of: messages.count) { _, _ in
                withAnimation { proxy.scrollTo("reticulum-chat-end", anchor: .bottom) }
                hub.chatShown()
            }
        }
        .background(fm.canvas)
        .safeAreaInset(edge: .bottom) { composer }
        .navigationTitle(hub.chatTitle)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Menu {
                    Button("Find path", systemImage: "point.topleft.down.to.point.bottomright.curvepath") { hub.requestPath(hub.currentPeer) }
                        .disabled(!hub.canAnnounce)
                    Button("Copy address", systemImage: "doc.on.doc") { ReticulumCopy.copy(ReticulumWire.hex(hub.currentPeer), "Address", hub: hub) }
                    Button("Delete conversation", systemImage: "trash", role: .destructive) { confirmDelete = true }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityIdentifier("reticulum-chat-menu")
            }
        }
        .onAppear {
            hub.chatVisible = true
            hub.chatShown()
        }
        .onDisappear { hub.chatVisible = false }
        .fmDialog(isPresented: $confirmDelete) {
            FmConfirmDialog(
                title: "Delete conversation",
                message: "Delete every message with \(hub.chatTitle) from this \(hub.deviceNoun)?",
                confirmLabel: "Delete",
                destructive: true,
                onCancel: { confirmDelete = false },
                onConfirm: {
                    confirmDelete = false
                    hub.deleteConversation()
                })
        }
    }

    private var composer: some View {
        FmGlassSurface(cornerRadius: FmRadius.lg, floating: true) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                HStack(alignment: .bottom, spacing: FmSpace.x2) {
                    FmTextField(text: $hub.composerText, hint: "Write a message", multiline: true)
                        .accessibilityIdentifier("reticulum-composer")
                    FmFilledButton("Send", onPressed: hub.composerText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : {
                        FmHaptics.tap()
                        hub.sendMessage()
                    })
                    .accessibilityIdentifier("reticulum-send")
                }
                Text(hub.composerHint).fmText(.labelSmall, color: fm.textTertiary).lineLimit(1)
            }
            .padding(FmSpace.x2)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.bottom, FmSpace.x2)
    }
}

struct ReticulumBubble: View {
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub
    let message: ReticulumChatMessage

    var body: some View {
        HStack {
            if message.outgoing { Spacer(minLength: 48) }
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                if !message.title.isEmpty {
                    Text(verbatim: message.title).fmText(.labelLarge).fixedSize(horizontal: false, vertical: true)
                }
                Text(verbatim: String(message.content.prefix(20000)))
                    .fmText(.bodyMedium)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                ForEach(Array(message.attachments.enumerated()), id: \.offset) { _, attachment in
                    ShareLink(item: ReticulumAttachmentItem(name: attachment.name, data: Data(attachment.data)),
                              preview: SharePreview(attachment.name.isEmpty ? "Attachment" : attachment.name)) {
                        Label("Save \(attachment.name) (\(ReticulumFormat.bytes(Double(attachment.data.count))))", systemImage: "square.and.arrow.down")
                            .fmText(.labelMedium, color: fm.accent)
                    }
                    .disabled(attachment.data.isEmpty)
                }
                Text(verbatim: ReticulumHub.bubbleMeta(message)).fmText(.labelSmall, color: fm.textTertiary)
                if !message.note.isEmpty && message.state != .delivered {
                    Text(verbatim: message.note)
                        .fmText(.labelSmall, color: message.state == .failed ? fm.danger : fm.textTertiary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if message.outgoing && (message.state == .failed || message.state == .sent) {
                    HStack(spacing: FmSpace.x2) {
                        FmTextButton(message.state == .failed ? "Retry" : "Send again", minHeight: 32, onPressed: { hub.retryMessage(message.id) })
                        FmTextButton("Delete", color: fm.danger, minHeight: 32, onPressed: { hub.deleteMessage(message.id) })
                    }
                }
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .background(message.outgoing ? fm.accent.opacity(fm.dark ? 0.22 : 0.12) : fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
            .contextMenu {
                Button("Copy text", systemImage: "doc.on.doc") { ReticulumCopy.copy(message.content, "Message", hub: hub) }
                if message.outgoing { Button("Delete", systemImage: "trash", role: .destructive) { hub.deleteMessage(message.id) } }
            }
            if !message.outgoing { Spacer(minLength: 48) }
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier(message.outgoing ? "reticulum-bubble-out" : "reticulum-bubble-in")
    }
}

struct ReticulumNewMessageSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ReticulumHub
    @State private var filter = ""
    @State private var address = ""

    var body: some View {
        let now = hub.now
        let peers = hub.messagePeers.filter { record in
            filter.isEmpty || record.displayName.localizedCaseInsensitiveContains(filter)
        }
        NavigationStack {
            List {
                Section {
                    TextField("LXMF address, 32 hex characters", text: $address)
                        .font(.callout.monospaced())
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .accessibilityIdentifier("reticulum-new-address")
                } footer: {
                    Text("Pick someone who has announced on the network, or paste an LXMF address.")
                }
                Section("Heard on the network") {
                    if peers.isEmpty {
                        Text("No LXMF destinations heard yet.").foregroundStyle(.secondary)
                    }
                    ForEach(peers) { record in
                        Button {
                            open(record.hash)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(verbatim: record.displayName.isEmpty ? ReticulumWire.short(record.hash) : record.displayName)
                                    .foregroundStyle(fm.textPrimary)
                                Text("\(ReticulumFormat.hops(record.hops)) · heard \(ReticulumFormat.age(record.lastHeard, now: now))")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
            .searchable(text: $filter, prompt: "Filter by name")
            .navigationTitle("New message")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Open") { open(ReticulumWire.parseHash(address)) }
                        .disabled(ReticulumWire.parseHash(address).isEmpty)
                        .accessibilityIdentifier("reticulum-new-open")
                }
            }
        }
    }

    private func open(_ hash: [UInt8]) {
        guard !hash.isEmpty else { return }
        dismiss()
        hub.openConversation(hash)
    }
}
