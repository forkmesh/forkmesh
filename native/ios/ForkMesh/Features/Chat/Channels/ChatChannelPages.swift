import FMApi
import FMBase
import FMChat
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct ChatChannelFilesScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let hub: ChatHub
    let conversation: String
    @State private var filter: Int = 0
    @State private var opened: ChatChannelFile?
    @State private var service: ChatLinkPreviewService? = Services.get(ChatLinkPreviewService.self)

    private var items: [ChatChannelFile] {
        let all: [ChatChannelFile] = hub.channelFiles(conversation)
        switch filter {
        case 1: return all.filter { $0.kind == .file }
        case 2: return all.filter { $0.kind == .link }
        default: return all
        }
    }

    var body: some View {
        List {
            Section {
                Picker("Show", selection: $filter) {
                    Text("All").tag(0)
                    Text("Files").tag(1)
                    Text("Links").tag(2)
                }
                .pickerStyle(.segmented)
                .listRowBackground(Color.clear)
            }
            if items.isEmpty {
                Text(filter == 2 ? "No links shared here yet." : (filter == 1 ? "No files shared here yet." : "Nothing shared here yet."))
                    .foregroundStyle(fm.textSecondary)
            }
            ForEach(items) { (item: ChatChannelFile) in
                if item.kind == .link {
                    linkRow(item)
                } else {
                    Button(action: { opened = item }) { fileRow(item) }
                        .buttonStyle(.plain)
                }
            }
        }
        .navigationTitle("Files & Links")
        .navigationBarTitleDisplayMode(.inline)
        .scrollContentBackground(.hidden)
        .sheet(item: $opened) { (item: ChatChannelFile) in
            NavigationStack {
                ScrollView {
                    ChatAttachmentView(fileName: item.name, attachment: item.attachment)
                        .padding()
                }
                .navigationTitle(item.name)
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) { Button("Done") { opened = nil } }
                    if let attachment = item.attachment, let file = ChatAttachmentView.temporaryFile(attachment) {
                        ToolbarItem(placement: .primaryAction) {
                            ShareLink(item: file) { Image(systemName: "square.and.arrow.up") }
                        }
                    }
                }
            }
        }
    }

    private func meta(_ item: ChatChannelFile) -> String {
        "\(item.senderName) \u{00B7} \(ChatHub.dayDividerLabel(item.sentAt, now: Date())) \(ChatHub.timeLabel(item.sentAt))"
    }

    private func fileRow(_ item: ChatChannelFile) -> some View {
        HStack(spacing: FmSpace.x3) {
            Image(systemName: symbol(item.attachment?.kind))
                .font(.system(size: 20))
                .foregroundStyle(fm.accent)
                .frame(width: 40, height: 40)
                .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(fm.accentSubtle))
            VStack(alignment: .leading, spacing: 2) {
                Text(item.name).font(.system(size: 16, weight: .medium)).foregroundStyle(fm.textPrimary).lineLimit(1)
                Text(item.attachment == nil ? "\(meta(item)) \u{00B7} not on this phone" : meta(item))
                    .font(.system(size: 13)).foregroundStyle(fm.textSecondary).lineLimit(1)
            }
            Spacer(minLength: 0)
        }
        .contentShape(Rectangle())
    }

    private func symbol(_ kind: ChatAttachmentKind?) -> String {
        switch kind {
        case .image: return "photo"
        case .audio: return "waveform"
        case .video: return "film"
        default: return "doc"
        }
    }

    @ViewBuilder private func linkRow(_ item: ChatChannelFile) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Button(action: { if let url = URL(string: item.url) { openURL(url) } }) {
                HStack(spacing: FmSpace.x3) {
                    ChatLinkSiteBadge(site: URL(string: item.url).flatMap(ChatLinkSites.site(for:)))
                    VStack(alignment: .leading, spacing: 2) {
                        Text(item.url).font(.system(size: 15)).foregroundStyle(fm.accent).lineLimit(1).truncationMode(.middle)
                        Text(meta(item)).font(.system(size: 13)).foregroundStyle(fm.textSecondary).lineLimit(1)
                    }
                }
            }
            .buttonStyle(.plain)
            if let service, service.enabled, let url = ChatLinkPreviewParsing.previewable(item.url) {
                ChatLinkPreviewCard(url: url, service: service)
            }
        }
        .contextMenu {
            Button(action: {
                #if canImport(UIKit)
                UIPasteboard.general.string = item.url
                #endif
            }) { Label("Copy link", systemImage: "doc.on.doc") }
        }
    }
}

struct ChatChannelBookmarksScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let hub: ChatHub
    let conversation: String
    @State private var adding: Bool = false
    @State private var title: String = ""
    @State private var link: String = ""

    var body: some View {
        let bookmarks: [ChatBookmark] = hub.workspace.bookmarks(for: conversation)
        let saved: [ChatSavedItem] = hub.savedItems(in: conversation)
        List {
            Section {
                ForEach(bookmarks) { (bookmark: ChatBookmark) in
                    Button(action: { if let url = URL(string: bookmark.url) { openURL(url) } }) {
                        HStack(spacing: FmSpace.x3) {
                            ChatLinkSiteBadge(site: URL(string: bookmark.url).flatMap(ChatLinkSites.site(for:)))
                            VStack(alignment: .leading, spacing: 2) {
                                Text(bookmark.title).font(.system(size: 16, weight: .medium)).foregroundStyle(fm.textPrimary)
                                Text(bookmark.url).font(.system(size: 13)).foregroundStyle(fm.textSecondary).lineLimit(1)
                            }
                        }
                    }
                    .buttonStyle(.plain)
                    .swipeActions {
                        Button(role: .destructive) { hub.workspace.removeBookmark(conversation, id: bookmark.id) } label: {
                            Label("Remove", systemImage: "trash")
                        }
                    }
                }
                Button(action: {
                    title = ""
                    link = UIPasteboard.general.hasURLs ? (UIPasteboard.general.url?.absoluteString ?? "") : ""
                    adding = true
                }) {
                    Label("Add a bookmark", systemImage: "plus")
                }
            } header: {
                Text("Bookmarks")
            } footer: {
                Text("Bookmarks and the canvas are kept on this phone.")
            }
            Section("Saved messages from this channel") {
                if saved.isEmpty {
                    Text("Long-press a message and choose Save for later to keep it here.")
                        .foregroundStyle(fm.textSecondary)
                }
                ForEach(saved) { (item: ChatSavedItem) in
                    VStack(alignment: .leading, spacing: 3) {
                        Text(item.senderName).font(.system(size: 15, weight: .semibold))
                        Text(item.text.isEmpty ? item.fileName : item.text)
                            .font(.system(size: 15)).foregroundStyle(fm.textSecondary).lineLimit(3)
                    }
                    .swipeActions {
                        Button(role: .destructive) { hub.workspace.unsave(item.id) } label: { Label("Remove", systemImage: "bookmark.slash") }
                    }
                }
            }
        }
        .navigationTitle("Bookmarks")
        .navigationBarTitleDisplayMode(.inline)
        .scrollContentBackground(.hidden)
        .alert("Add a bookmark", isPresented: $adding) {
            TextField("Name", text: $title)
            TextField("https://", text: $link)
                .keyboardType(.URL)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Cancel", role: .cancel) {}
            Button("Add") {
                let address: String = link.contains("://") ? link : "https://" + link
                hub.addBookmark(title, url: address, to: conversation)
            }
        } message: {
            Text("Kept on this phone with \(hub.conversationLabel(conversation)).")
        }
    }
}

struct ChatChannelCanvasScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let hub: ChatHub
    let conversation: String
    let onPost: (String) -> Void
    @State private var title: String = ""
    @State private var text: String = ""
    @State private var loaded: Bool = false
    @State private var confirmDelete: Bool = false
    @FocusState private var editing: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            TextField("Canvas title", text: $title)
                .font(.system(size: 24, weight: .bold))
                .padding(.horizontal, FmSpace.x4)
                .padding(.top, FmSpace.x3)
            TextEditor(text: $text)
                .font(.system(size: 17))
                .scrollContentBackground(.hidden)
                .padding(.horizontal, FmSpace.x3)
                .focused($editing)
                .overlay(alignment: .topLeading) {
                    if text.isEmpty {
                        Text("Write notes, agendas, links or checklists for \(hub.conversationLabel(conversation))\u{2026}")
                            .font(.system(size: 17))
                            .foregroundStyle(fm.textTertiary)
                            .padding(.horizontal, FmSpace.x4 + 4)
                            .padding(.top, 8)
                            .allowsHitTesting(false)
                    }
                }
            Text("\(text.count) / \(ChatWorkspace.maxCanvasChars) \u{00B7} kept on this phone")
                .font(.system(size: 12))
                .foregroundStyle(fm.textTertiary)
                .padding(.horizontal, FmSpace.x4)
                .padding(.bottom, FmSpace.x2)
        }
        .navigationTitle("Canvas")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Menu {
                    Button(action: {
                        save()
                        let heading: String = title.trimmingCharacters(in: .whitespaces).isEmpty ? "Canvas" : title
                        onPost("*\(heading)*\n\(text)")
                    }) { Label("Post to channel", systemImage: "paperplane") }
                    .disabled(text.dartTrimmed.isEmpty)
                    ShareLink(item: "\(title)\n\n\(text)") { Label("Share", systemImage: "square.and.arrow.up") }
                    if hub.workspace.canvas(for: conversation) != nil {
                        Button(role: .destructive, action: { confirmDelete = true }) { Label("Delete canvas", systemImage: "trash") }
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
            }
        }
        .confirmationDialog("Delete this canvas?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete canvas", role: .destructive) {
                hub.workspace.removeCanvas(conversation)
                title = ""
                text = ""
                dismiss()
            }
        }
        .onAppear {
            if loaded { return }
            loaded = true
            let canvas: ChatCanvas? = hub.workspace.canvas(for: conversation)
            title = canvas?.title ?? "Canvas"
            text = canvas?.text ?? ""
            if canvas == nil { editing = true }
        }
        .onChange(of: text) { _, _ in save() }
        .onChange(of: title) { _, _ in save() }
    }

    private func save() {
        guard loaded else { return }
        if text.dartTrimmed.isEmpty && hub.workspace.canvas(for: conversation) == nil { return }
        hub.saveCanvas(conversation, title: title, text: text)
    }
}

struct ChatChannelMembersScreen: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let conversation: String
    let onAdd: () -> Void
    let onProfile: (String) -> Void
    let onMessage: (ChatChannelMember) -> Void
    @State private var query: String = ""

    var body: some View {
        let members: [ChatChannelMember] = hub.channelMembers(conversation)
        let shown: [ChatChannelMember] = query.isEmpty
            ? members
            : members.filter { $0.name.localizedCaseInsensitiveContains(query) || $0.account.localizedCaseInsensitiveContains(query) }
        let online: Int = members.filter(\.online).count
        List {
            Section {
                Button(action: onAdd) { Label("Add people", systemImage: "person.badge.plus") }
            }
            Section {
                ForEach(shown) { (member: ChatChannelMember) in
                    HStack(spacing: FmSpace.x3) {
                        FmAvatar(name: member.name, avatarPng: ChatAvatarCache.shared.png(for: member.name), size: 36, fontSize: FmType.body, corner: 8)
                            .overlay(alignment: .bottomTrailing) {
                                PresenceDot(online: member.online, away: member.away, size: 10).offset(x: 3, y: 3)
                            }
                        VStack(alignment: .leading, spacing: 1) {
                            Text(member.isSelf ? "\(member.name) (you)" : member.name)
                                .font(.system(size: 16, weight: .medium))
                                .foregroundStyle(fm.textPrimary)
                            if !member.account.isEmpty && member.account.lowercased() != member.name.lowercased() {
                                Text("@\(member.account)").font(.system(size: 13)).foregroundStyle(fm.textSecondary)
                            } else {
                                Text(member.online ? (member.away ? "Away" : "Online") : "Offline")
                                    .font(.system(size: 13)).foregroundStyle(fm.textSecondary)
                            }
                        }
                        Spacer(minLength: 0)
                    }
                    .contentShape(Rectangle())
                    .contextMenu {
                        if !member.isSelf {
                            Button(action: { onMessage(member) }) { Label("Message", systemImage: "bubble.left") }
                            Button(action: { onProfile(member.account.isEmpty ? member.name : member.account) }) {
                                Label("View profile", systemImage: "person.crop.circle")
                            }
                        }
                    }
                    .onTapGesture { if !member.isSelf { onProfile(member.account.isEmpty ? member.name : member.account) } }
                }
            } header: {
                Text("\(ChatHub.membersLabel(members.count)) \u{00B7} \(online) online")
            } footer: {
                if hub.office?.room(for: conversation) == nil && !hub.extras.isPrivate(conversation) {
                    Text("Everyone online in this project's chat, and everyone who has posted here.")
                }
            }
        }
        .searchable(text: $query, prompt: "Search members")
        .navigationTitle("Members")
        .navigationBarTitleDisplayMode(.inline)
        .scrollContentBackground(.hidden)
        .task { await ChatChannelMembersScreen.warm(members: hub.channelMembers(conversation)) }
    }

    static func warm(members: [ChatChannelMember]) async {
        for member in members.prefix(40) where ChatAvatarCache.shared.png(for: member.name).isEmpty {
            await ChatAvatarCache.shared.refresh(member.name)
        }
    }
}

struct ChatChannelAppsScreen: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let onInsert: (String) -> Void

    var body: some View {
        let commands: [AppCommand] = hub.appCommands.commands
        let apps: [String] = Array(Set(commands.map { $0.appName.isEmpty ? "Apps" : $0.appName })).sorted()
        List {
            Section("Agents") {
                Button(action: { onInsert("@forkbot ") }) {
                    HStack(spacing: FmSpace.x3) {
                        Image(systemName: "sparkles")
                            .font(.system(size: 18))
                            .foregroundStyle(Color.white)
                            .frame(width: 36, height: 36)
                            .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(fm.accent))
                        VStack(alignment: .leading, spacing: 1) {
                            Text("ForkBot").font(.system(size: 16, weight: .semibold)).foregroundStyle(fm.textPrimary)
                            Text(hub.isPrivateRoom ? "Not available in end-to-end encrypted rooms" : "Mention @forkbot to ask a question")
                                .font(.system(size: 13)).foregroundStyle(fm.textSecondary)
                        }
                    }
                }
                .buttonStyle(.plain)
                .disabled(hub.isPrivateRoom)
            }
            if commands.isEmpty {
                Section("Apps") {
                    Text("No apps are installed for this project. Install them from the Market on the web or the desktop.")
                        .foregroundStyle(fm.textSecondary)
                }
            }
            ForEach(apps, id: \.self) { (app: String) in
                Section(app) {
                    ForEach(commands.filter { ($0.appName.isEmpty ? "Apps" : $0.appName) == app }) { (command: AppCommand) in
                        Button(action: { onInsert(command.command + " ") }) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(command.usageHint.isEmpty ? command.command : "\(command.command) \(command.usageHint)")
                                    .font(.system(size: 15, weight: .medium, design: .monospaced))
                                    .foregroundStyle(fm.textPrimary)
                                if !command.description.isEmpty {
                                    Text(command.description).font(.system(size: 13)).foregroundStyle(fm.textSecondary)
                                }
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
        .navigationTitle("Agents & Apps")
        .navigationBarTitleDisplayMode(.inline)
        .scrollContentBackground(.hidden)
        .refreshable { await hub.refreshAppCommands(force: true) }
        .task { await hub.refreshAppCommands() }
    }
}

struct ChatChannelNotificationsScreen: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let conversation: String

    var body: some View {
        let current: ChatNotifyLevel = hub.notifyLevel(conversation)
        List {
            Section {
                ForEach(ChatNotifyLevel.allCases) { (level: ChatNotifyLevel) in
                    Button(action: { hub.setNotifyLevel(level, for: conversation) }) {
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(level.label).font(.system(size: 16, weight: .medium)).foregroundStyle(fm.textPrimary)
                                Text(level.detail).font(.system(size: 13)).foregroundStyle(fm.textSecondary)
                            }
                            Spacer()
                            if level == current { Image(systemName: "checkmark").foregroundStyle(fm.accent) }
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            } footer: {
                Text("This changes the unread dot and badge on this phone. @here, @channel and @everyone count as mentions.")
            }
        }
        .navigationTitle("Notifications")
        .navigationBarTitleDisplayMode(.inline)
        .scrollContentBackground(.hidden)
    }
}

struct ChatChannelDetailsScreen: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let conversation: String
    let onLeave: (() -> Void)?
    let onOffice: (() -> Void)?
    @State private var topic: String = ""
    @State private var savingTopic: Bool = false
    @State private var previews: ChatLinkPreviewService? = Services.get(ChatLinkPreviewService.self)

    private var room: OfficeRoom? { hub.office?.room(for: conversation) }

    var body: some View {
        List {
            Section("Channel") {
                LabeledContent("Name", value: hub.conversationLabel(conversation))
                LabeledContent("Type", value: room?.isPrivate ?? hub.extras.isPrivate(conversation) ? "Private" : "Public")
                LabeledContent("Members", value: "\(hub.memberCount(conversation))")
            }
            if let room {
                Section {
                    if hub.canSetTopic && hub.currentConversation == conversation {
                        TextField("Add a topic", text: $topic, axis: .vertical)
                        Button(savingTopic ? "Saving\u{2026}" : "Save topic") {
                            savingTopic = true
                            Task {
                                _ = await hub.setTopic(topic)
                                savingTopic = false
                            }
                        }
                        .disabled(savingTopic || topic == room.topic)
                    } else {
                        Text(room.topic.isEmpty ? "No topic" : room.topic).foregroundStyle(fm.textSecondary)
                    }
                } header: {
                    Text("Topic")
                }
            }
            Section("Security") {
                Text(hub.currentConversation == conversation ? hub.sealSummary : "")
                    .font(.system(size: 14))
                    .foregroundStyle(fm.textSecondary)
                Button(action: {
                    #if canImport(UIKit)
                    UIPasteboard.general.string = hub.sealSummary
                    #endif
                    hub.post("Copied.")
                }) { Label("Copy", systemImage: "doc.on.doc") }
            }
            Section {
                ChatRetentionMenu(hub: hub, conversation: conversation)
                if let previews {
                    Toggle(isOn: Binding(get: { previews.enabled }, set: { previews.setEnabled($0) })) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Link previews")
                            Text("Fetches a title and picture for links in every chat, straight from the site.")
                                .font(.system(size: 13)).foregroundStyle(fm.textSecondary)
                        }
                    }
                }
            } header: {
                Text("On this phone")
            }
            if let onOffice {
                Section { Button(action: onOffice) { Label("Channel settings", systemImage: "gearshape") } }
            }
            if let onLeave {
                Section {
                    Button(role: .destructive, action: onLeave) {
                        Label("Leave channel", systemImage: "rectangle.portrait.and.arrow.right")
                    }
                }
            }
        }
        .navigationTitle("Settings & Details")
        .navigationBarTitleDisplayMode(.inline)
        .scrollContentBackground(.hidden)
        .onAppear { topic = room?.topic ?? "" }
    }
}
