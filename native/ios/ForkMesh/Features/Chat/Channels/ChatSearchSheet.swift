import FMApi
import FMBase
import FMChat
import SwiftUI

enum ChatSearchScope: String, CaseIterable, Identifiable {
    case messages
    case mentions
    case saved

    var id: String { rawValue }

    var title: String {
        switch self {
        case .messages: return "Messages"
        case .mentions: return "Mentions"
        case .saved: return "Saved"
        }
    }
}

struct ChatSearchRequest: Identifiable {
    let id: UUID = UUID()
    var query: String = ""
    var scope: ChatSearchScope = .messages
}

struct ChatSearchSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let request: ChatSearchRequest
    let onOpen: (String) -> Void
    @State private var query: String = ""
    @State private var scope: ChatSearchScope = .messages
    @State private var started: Bool = false

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                Picker("Show", selection: $scope) {
                    ForEach(ChatSearchScope.allCases) { (item: ChatSearchScope) in
                        Text(item.title).tag(item)
                    }
                }
                .pickerStyle(.segmented)
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x2)
                content
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle(scope == .saved ? "Saved items" : "Search")
            .navigationBarTitleDisplayMode(.inline)
            .searchable(
                text: $query, placement: .navigationBarDrawer(displayMode: .always),
                prompt: scope == .messages ? "Jump to or search" : "Filter")
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } }
            }
            .onAppear {
                if started { return }
                started = true
                query = request.query
                scope = request.scope
            }
        }
    }

    @ViewBuilder private var content: some View {
        switch scope {
        case .messages: messages
        case .mentions: hitList(filtered(hub.mentionHits()), empty: "Messages that mention you show up here.")
        case .saved: saved
        }
    }

    private func filtered(_ hits: [ChatSearchHit]) -> [ChatSearchHit] {
        let trimmed: String = query.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty { return hits }
        let parsed: ChatSearchQuery = ChatSearch.parse(trimmed)
        return hits.filter { ChatSearch.matches($0.message, text: $0.message.text, place: $0.place, query: parsed) }
    }

    @ViewBuilder private var messages: some View {
        let trimmed: String = query.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty {
            tips
        } else {
            let jumps: [ChatRecipient] = hub.jumpTargets(trimmed)
            let hits: [ChatSearchHit] = hub.searchMessages(trimmed)
            List {
                if !jumps.isEmpty {
                    Section("Conversations") {
                        ForEach(jumps) { (target: ChatRecipient) in
                            Button(action: { Task { await jump(target) } }) {
                                HStack(spacing: FmSpace.x3) {
                                    if target.kind == .channel {
                                        FmIcon("tag", size: 18, color: fm.textSecondary).frame(width: 28)
                                    } else {
                                        FmAvatar(name: target.name, size: 28, fontSize: FmType.meta)
                                    }
                                    VStack(alignment: .leading, spacing: 1) {
                                        Text(target.label).fmText(.bodyMedium, color: fm.textPrimary).lineLimit(1)
                                        if !target.detail.isEmpty {
                                            Text(target.detail).fmText(.bodySmall, color: fm.textTertiary).lineLimit(1)
                                        }
                                    }
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
                Section(hits.isEmpty ? "No messages match" : "\(hits.count) \(hits.count == 1 ? "message" : "messages")") {
                    ForEach(hits) { (hit: ChatSearchHit) in
                        hitRow(hit, terms: ChatSearch.parse(trimmed).terms)
                    }
                }
            }
            .listStyle(.plain)
        }
    }

    private var tips: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            Text("Search the messages on this phone, or jump to a person or channel.")
                .fmText(.bodyMedium, color: fm.textSecondary)
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                tip("from:@pat", "messages from someone")
                tip("in:#general", "messages in one conversation")
                tip("has:file", "messages with a file")
                tip("has:link", "messages with a link")
            }
        }
        .padding(FmSpace.x4)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    private func tip(_ token: String, _ meaning: String) -> some View {
        Button(action: { query = token + " " }) {
            HStack(spacing: FmSpace.x2) {
                Text(token)
                    .fmMono(size: FmType.small, color: fm.accent)
                    .padding(.horizontal, FmSpace.x2)
                    .padding(.vertical, 2)
                    .background(fm.accentSubtle)
                    .clipShape(FmRoundedShape(radius: FmRadius.sm))
                Text(meaning).fmText(.bodySmall, color: fm.textSecondary)
            }
        }
        .buttonStyle(.plain)
    }

    @ViewBuilder private func hitList(_ hits: [ChatSearchHit], empty: String) -> some View {
        if hits.isEmpty {
            FmEmptyState(icon: "alternate_email", title: "Nothing here yet", message: empty)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            List {
                ForEach(hits) { (hit: ChatSearchHit) in
                    hitRow(hit, terms: ChatSearch.parse(query).terms)
                }
            }
            .listStyle(.plain)
        }
    }

    private func hitRow(_ hit: ChatSearchHit, terms: [String]) -> some View {
        Button(action: { open(hit.message.conversation) }) {
            ChatFoundMessageRow(
                sender: hit.message.senderName, place: hit.place, at: hit.message.timestamp,
                text: ChatSearch.snippet(hit.message.text.isEmpty ? hit.message.fileName : hit.message.text, terms: terms))
        }
        .buttonStyle(.plain)
        .swipeActions(edge: .trailing) {
            Button(action: { hub.toggleSaved(hit.message, text: hit.message.text) }) {
                Label(hub.workspace.isSaved(hit.message.id) ? "Unsave" : "Save", systemImage: "bookmark")
            }
            .tint(fm.accent)
        }
    }

    @ViewBuilder private var saved: some View {
        let trimmed: String = query.trimmingCharacters(in: .whitespaces).lowercased()
        let items: [ChatSavedItem] = hub.workspace.saved.filter { (item: ChatSavedItem) -> Bool in
            trimmed.isEmpty || item.text.lowercased().contains(trimmed) || item.senderName.lowercased().contains(trimmed)
                || item.place.lowercased().contains(trimmed)
        }
        if items.isEmpty {
            FmEmptyState(
                icon: "label_outline", title: "No saved items",
                message: "Press and hold a message and choose Save for later to keep it here.")
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            List {
                ForEach(items) { (item: ChatSavedItem) in
                    Button(action: { open(item.conversation) }) {
                        ChatFoundMessageRow(
                            sender: item.senderName, place: item.place, at: item.sentAt,
                            text: ChatSearch.snippet(item.text.isEmpty ? item.fileName : item.text, terms: []))
                    }
                    .buttonStyle(.plain)
                    .swipeActions(edge: .trailing) {
                        Button(role: .destructive, action: { hub.workspace.unsave(item.id) }) {
                            Label("Remove", systemImage: "bookmark.slash")
                        }
                    }
                }
            }
            .listStyle(.plain)
        }
    }

    private func jump(_ target: ChatRecipient) async {
        if !target.conversation.isEmpty {
            open(target.conversation)
            return
        }
        if let conversation = await hub.conversation(for: [target]) {
            open(conversation)
        }
    }

    private func open(_ conversation: String) {
        onOpen(conversation)
        dismiss()
    }
}

struct ChatFoundMessageRow: View {
    @Environment(\.fmPalette) private var fm
    let sender: String
    let place: String
    let at: Date
    let text: String

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            FmAvatar(name: sender, size: 32, fontSize: FmType.meta)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: FmSpace.x1) {
                    Text(sender).fmText(.titleSmall, color: FmColors.senderColor(sender)).lineLimit(1)
                    Text(place).fmText(.bodySmall, color: fm.textTertiary).lineLimit(1)
                    Spacer(minLength: 0)
                    Text(ChatConversationLayout.timeLabel(at, now: Date()))
                        .fmText(.labelSmall, color: fm.textTertiary, tabular: true)
                        .lineLimit(1)
                }
                ChatLinkText(text: text)
                    .fmText(.bodyMedium, color: fm.textPrimary)
                    .lineLimit(3)
            }
        }
        .padding(.vertical, FmSpace.x1)
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
    }
}
