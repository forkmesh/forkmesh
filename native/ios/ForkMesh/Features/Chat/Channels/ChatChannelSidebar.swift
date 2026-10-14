import FMAccount
import FMApi
import FMBase
import FMChat
import FMDiscord
import SwiftUI

struct ChatChannelSidebar: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let onSelected: (() -> Void)?
    let onRoute: (Route) -> Void
    var onCompose: (() -> Void)? = nil
    var onSearch: (() -> Void)? = nil
    var onClose: (() -> Void)? = nil
    @State private var groups: ChannelGroupsStore? = Services.get(ChannelGroupsStore.self)
    @State private var discord: DiscordBridge? = Services.get(DiscordBridge.self)
    @State private var statusSheet: Bool = false
    @State private var newChannel: Bool = false
    @State private var newGroup: Bool = false
    @State private var groupName: String = ""
    @State private var renaming: String?
    @State private var moving: String?
    @State private var inviting: String?
    @State private var managing: OfficeRoom?
    @State private var leaving: String?

    private var canSetStatus: Bool { Services.get(UserStatusService.self)?.signedIn ?? false }

    private func select(_ conversation: String) {
        hub.select(conversation)
        onSelected?()
    }

    private var rows: [ChannelGroups.Row] {
        let state = groups?.state ?? ChannelGroups.State()
        let listed: [String] = (hub.channels + hub.officeChannels).filter { !hub.workspace.isStarred($0) }
        return ChannelGroups.layout(state, channels: listed) { channel in
            OfficeFrames.isOffice(channel) ? "Office" : ""
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if onSearch != nil || onCompose != nil || onClose != nil {
                    jumpBar.padding(.bottom, FmSpace.x3)
                }
                if !hub.workspace.starred.isEmpty {
                    FmSectionHeader(title: "Starred")
                        .padding(.bottom, FmSpace.x2)
                    ForEach(hub.workspace.starred, id: \.self) { (conversation: String) in
                        conversationTile(conversation)
                    }
                    Color.clear.frame(height: FmSpace.x3)
                }
                HStack {
                    FmSectionHeader(title: "Channels")
                    Spacer()
                    addMenu
                }
                .padding(.bottom, FmSpace.x2)
                ForEach(ChannelGroups.visibleRows(rows)) { (row: ChannelGroups.Row) in
                    if row.header {
                        groupHeader(row)
                    } else {
                        channelTile(row.channel)
                    }
                }
                if !hub.workspace.isStarred(meshConversation) {
                    conversationTile(meshConversation)
                }
                if discord != nil {
                    Button(action: { onRoute(.discord(guild: "", channel: nil)) }) {
                        HStack(spacing: FmSpace.x3) {
                            FmIcon("desktop_windows", size: 17, color: fm.textTertiary)
                            Text("Discord (through your desktop)")
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .lineLimit(1)
                                .frame(maxWidth: .infinity, alignment: .leading)
                            FmIcon("chevron_right", size: 16, color: fm.textTertiary)
                        }
                        .padding(.horizontal, FmSpace.x3)
                        .padding(.vertical, FmSpace.x2)
                        .frame(minHeight: 44)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
                directMessages
                FmSectionHeader(title: "People")
                    .padding(.top, FmSpace.x4)
                    .padding(.bottom, FmSpace.x2)
                ForEach(hub.memberGroups) { (group: MemberGroup) in
                    ChatMemberRow(
                        group: group, selected: hub.currentConversation == "@\(group.id)",
                        onTap: memberTap(group), onProfile: { onRoute(.profile(name: group.name)) }, retentionHub: hub)
                }
            }
            .padding(FmSpace.x3)
        }
        .background(fm.bgRaised)
        .fmRefreshable {
            await hub.office?.refresh()
            await hub.direct?.refresh()
        }
        .task { await hub.direct?.refresh() }
        .sheet(isPresented: $statusSheet) { UserStatusSheet() }
        .sheet(isPresented: $newChannel) { NewChatChannelSheet(hub: hub, onCreated: { select($0) }) }
        .sheet(item: Binding(get: { inviting.map(ChatSheetKey.init) }, set: { inviting = $0?.value })) { key in
            InviteToChannelSheet(hub: hub, channel: key.value)
        }
        .sheet(item: Binding(get: { moving.map(ChatSheetKey.init) }, set: { moving = $0?.value })) { key in
            MoveChannelSheet(channel: key.value, rows: rows, onMove: { group in move(key.value, to: group) })
        }
        .sheet(item: $managing) { (room: OfficeRoom) in
            if let office = hub.office {
                NavigationStack { OfficeChannelSettingsScreen(office: office, room: room) }
            }
        }
        .alert("New group", isPresented: $newGroup) {
            TextField("Name", text: $groupName)
            Button("Cancel", role: .cancel) { groupName = "" }
            Button("Create") {
                let name: String = groupName
                groups?.update { ChannelGroups.createGroup(&$0, name) }
                groupName = ""
            }
        }
        .alert("Rename group", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $groupName)
            Button("Cancel", role: .cancel) { renaming = nil }
            Button("Rename") {
                if let from = renaming {
                    let members: [String] = ChannelGroups.renderedMembers(rows, from)
                    let to: String = groupName
                    groups?.update { ChannelGroups.renameGroup(&$0, from: from, to: to, members: members) }
                }
                renaming = nil
                groupName = ""
            }
        }
        .confirmationDialog(
            "Leave \(leaving ?? "")?", isPresented: Binding(get: { leaving != nil }, set: { if !$0 { leaving = nil } }),
            titleVisibility: .visible
        ) {
            Button("Leave channel", role: .destructive) {
                if let channel = leaving {
                    hub.workspace.forget(conversation: channel)
                    hub.extras.removeChannel(channel)
                    if hub.currentConversation == channel { hub.select("#general") }
                }
                leaving = nil
            }
        } message: {
            Text("It disappears from this phone. Messages others send there stop arriving here.")
        }
    }

    private var jumpBar: some View {
        HStack(spacing: FmSpace.x2) {
            if let onClose {
                Button(action: onClose) {
                    Image(systemName: "xmark")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                        .frame(width: 44, height: 44)
                        .contentShape(Circle())
                        .shellGlass(Circle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Close conversations")
            }
            if let onSearch {
                Button(action: onSearch) {
                    HStack(spacing: FmSpace.x2) {
                        Image(systemName: "magnifyingglass")
                            .font(.system(size: 15, weight: .medium))
                            .foregroundStyle(fm.textTertiary)
                        Text("Jump to or search\u{2026}")
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .lineLimit(1)
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, FmSpace.x3)
                    .frame(minHeight: 40)
                    .contentShape(Rectangle())
                    .shellGlass(RoundedRectangle(cornerRadius: 20, style: .continuous))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Jump to or search")
            }
            if let onCompose {
                Button(action: onCompose) {
                    Image(systemName: "square.and.pencil")
                        .font(.system(size: 18, weight: .medium))
                        .foregroundStyle(fm.textPrimary)
                        .frame(width: 44, height: 44)
                        .contentShape(Circle())
                        .shellGlass(Circle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("New message")
            }
        }
    }

    @ViewBuilder private var directMessages: some View {
        let rows: [ChatDirectRow] = hub.directRows.filter { !hub.workspace.isStarred($0.conversation) }
        if !rows.isEmpty || onCompose != nil {
            HStack {
                FmSectionHeader(title: "Direct messages")
                Spacer()
                if let onCompose {
                    Button(action: onCompose) {
                        FmIcon("add", size: 20, color: fm.textSecondary, label: "New message")
                            .frame(width: 44, height: 44)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.top, FmSpace.x4)
            .padding(.bottom, FmSpace.x2)
            ForEach(rows) { (row: ChatDirectRow) in
                conversationTile(row.conversation, direct: row)
            }
        }
    }

    @ViewBuilder private func conversationTile(_ conversation: String, direct: ChatDirectRow? = nil) -> some View {
        let office: OfficeRoom? = hub.office?.room(for: conversation)
        let person: Bool = conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation)
        let locked: Bool = office?.isPrivate ?? hub.extras.isPrivate(conversation)
        let icon: String = conversation == meshConversation ? "lan_outlined" : (person ? "alternate_email" : (locked ? "lock_outline" : "tag"))
        let title: String = direct.map { $0.name } ?? hub.conversationLabel(conversation)
        let preview: ChatListPreview? = hub.listPreview(conversation)
        let row: ChatDirectRow? = direct ?? (person ? hub.directRows.first(where: { $0.conversation == conversation }) : nil)
        ChatChannelTile(
            title: title, icon: icon, selected: hub.currentConversation == conversation, unread: hub.isUnread(conversation),
            onTap: { select(conversation) }, muted: hub.workspace.isMuted(conversation),
            draft: hub.workspace.hasDraft(conversation) && hub.currentConversation != conversation,
            count: hub.workspace.isMuted(conversation) ? 0 : (row?.unread ?? 0), online: row?.online,
            preview: preview?.text ?? "", time: preview.map { hub.listTimeLabel($0) } ?? "",
            avatar: person ? (row?.name ?? String(title.drop(while: { $0 == "@" }))) : nil)
            .contextMenu {
                conversationMenu(conversation)
                if person || conversation == meshConversation {
                    ChatRetentionMenu(hub: hub, conversation: conversation)
                }
            }
    }

    @ViewBuilder private func conversationMenu(_ conversation: String) -> some View {
        let starred: Bool = hub.workspace.isStarred(conversation)
        let muted: Bool = hub.workspace.isMuted(conversation)
        Button(action: { hub.toggleStarred(conversation) }) {
            Label(starred ? "Unstar" : "Star", systemImage: starred ? "star.slash" : "star")
        }
        Button(action: { hub.toggleMuted(conversation) }) {
            Label(muted ? "Unmute" : "Mute", systemImage: muted ? "bell" : "bell.slash")
        }
    }

    private var addMenu: some View {
        Menu {
            if let onCompose {
                Button(action: onCompose) { Label("New message", systemImage: "square.and.pencil") }
            }
            Button(action: { newChannel = true }) { Label("New channel", systemImage: "number") }
            Button(action: { newGroup = true }) { Label("New group", systemImage: "folder.badge.plus") }
        } label: {
            FmIcon("add", size: 20, color: fm.textSecondary, label: "Add")
                .frame(width: 44, height: 44)
        }
    }

    private func groupHeader(_ row: ChannelGroups.Row) -> some View {
        Button(action: {
            let collapsed: Bool = !row.collapsed
            groups?.update { ChannelGroups.setCollapsed(&$0, row.group, collapsed: collapsed, automatic: row.automatic) }
        }) {
            HStack(spacing: FmSpace.x2) {
                FmIcon(row.collapsed ? "chevron_right" : "expand_more", size: 16, color: fm.textTertiary)
                Text(row.group)
                    .fmText(.labelMedium, color: fm.textTertiary)
                    .lineLimit(1)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal, FmSpace.x2)
            .frame(minHeight: 36)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .contextMenu {
            if !row.automatic {
                Button(action: {
                    groupName = row.group
                    renaming = row.group
                }) { Label("Rename group", systemImage: "pencil") }
                Button(role: .destructive, action: {
                    let members: [String] = ChannelGroups.renderedMembers(rows, row.group)
                    groups?.update { ChannelGroups.removeGroup(&$0, row.group, members: members) }
                }) { Label("Delete group", systemImage: "trash") }
            }
        }
    }

    @ViewBuilder private func channelTile(_ channel: String) -> some View {
        let office: OfficeRoom? = hub.office?.room(for: channel)
        let locked: Bool = office?.isPrivate ?? hub.extras.isPrivate(channel)
        let preview: ChatListPreview? = hub.listPreview(channel)
        ChatChannelTile(
            title: office?.name.isEmpty == false ? "#\(office!.name)" : channel, icon: locked ? "lock_outline" : "tag",
            selected: hub.currentConversation == channel, unread: hub.isUnread(channel), onTap: { select(channel) },
            muted: hub.workspace.isMuted(channel),
            draft: hub.workspace.hasDraft(channel) && hub.currentConversation != channel,
            preview: preview?.text ?? "", time: preview.map { hub.listTimeLabel($0) } ?? "")
            .contextMenu {
                conversationMenu(channel)
                Button(action: { moving = channel }) { Label("Move to group", systemImage: "folder") }
                ChatRetentionMenu(hub: hub, conversation: channel)
                if let office {
                    if office.canManage {
                        Button(action: { managing = office }) { Label("Channel settings", systemImage: "gearshape") }
                    }
                } else {
                    if hub.extras.isPrivate(channel) {
                        Button(action: { inviting = channel }) { Label("Invite people", systemImage: "person.badge.plus") }
                    }
                    if !ChatEveryone.contains(channel) {
                        Button(role: .destructive, action: { leaving = channel }) {
                            Label("Leave channel", systemImage: "rectangle.portrait.and.arrow.right")
                        }
                    }
                }
            }
    }

    private func move(_ channel: String, to group: String) {
        let members: [String] = ChannelGroups.renderedMembers(rows, group)
        let automatic: Bool = ChannelGroups.renderedGroupIsAutomatic(rows, group)
        groups?.update { ChannelGroups.assign(&$0, channel: channel, to: group, index: -1, members: members, automaticGroup: automatic) }
    }

    private func memberTap(_ group: MemberGroup) -> (() -> Void)? {
        if group.isSelf {
            return canSetStatus ? { statusSheet = true } : nil
        }
        return { select("@\(group.id)") }
    }
}

struct ChatSheetKey: Identifiable {
    let value: String
    var id: String { value }
}

struct ChatChannelTile: View {
    @Environment(\.fmPalette) private var fm
    let title: String
    let icon: String
    let selected: Bool
    let unread: Bool
    let onTap: () -> Void
    var muted: Bool = false
    var draft: Bool = false
    var count: Int = 0
    var online: Bool? = nil
    var preview: String? = nil
    var time: String = ""
    var avatar: String? = nil

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: FmSpace.x3) {
                leading
                VStack(alignment: .leading, spacing: 1) {
                    HStack(spacing: FmSpace.x2) {
                        Text(title)
                            .fmText(.bodySmall, color: selected ? fm.textPrimary : (unread ? fm.textPrimary : fm.textSecondary),
                                    weight: selected || unread ? FmType.emphasis : FmType.regular)
                            .lineLimit(1)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        if !time.isEmpty {
                            Text(time)
                                .fmText(.bodySmall, color: fm.textTertiary, tabular: true)
                                .lineLimit(1)
                                .fixedSize()
                        }
                    }
                    if let preview {
                        Text(preview.isEmpty ? " " : preview)
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .lineLimit(1)
                            .truncationMode(.tail)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .accessibilityHidden(preview.isEmpty)
                    }
                }
                if draft {
                    Image(systemName: "pencil")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(fm.textTertiary)
                        .accessibilityLabel("Draft")
                }
                if muted {
                    Image(systemName: "bell.slash")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(fm.textTertiary)
                        .accessibilityLabel("Muted")
                }
                if count > 0 {
                    Text("\(count)")
                        .fmText(.labelSmall, color: Color.white, tabular: true)
                        .padding(.horizontal, 6)
                        .frame(minWidth: 20, minHeight: 18)
                        .background(fm.danger)
                        .clipShape(Capsule())
                } else if unread {
                    Circle().fill(fm.danger).frame(width: 8, height: 8)
                }
            }
            .opacity(muted && !selected ? 0.6 : 1)
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .frame(minHeight: 44)
            .background(selected ? fm.accentSubtle : Color.clear)
            .clipShape(FmRoundedShape(radius: FmRadius.md))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .padding(.bottom, FmSpace.x1)
    }

    @ViewBuilder private var leading: some View {
        if let avatar {
            ZStack(alignment: .bottomTrailing) {
                MessagesPersonAvatar(name: avatar, size: 28)
                if let online {
                    PresenceDot(online: online, size: 9)
                        .overlay { Circle().strokeBorder(selected ? fm.accentSubtle : fm.bgRaised, lineWidth: 1.5) }
                        .offset(x: 2, y: 2)
                }
            }
            .frame(width: 28, height: 28)
        } else {
            ZStack(alignment: .bottomTrailing) {
                FmIcon(icon, size: 17, color: selected ? fm.accent : fm.textTertiary)
                if let online {
                    PresenceDot(online: online, size: 7).offset(x: 3, y: 3)
                }
            }
            .frame(width: 28)
        }
    }
}

struct ChatMemberRow: View {
    @Environment(\.fmPalette) private var fm
    let group: MemberGroup
    let selected: Bool
    let onTap: (() -> Void)?
    let onProfile: () -> Void
    var retentionHub: ChatHub? = nil

    private var away: Bool { group.online && group.userStatus.away }
    private var visitor: Bool { ChatVisitorPresence.isTransientVisitor(name: group.name) }

    var body: some View {
        Button(action: { onTap?() }) {
            HStack(alignment: .top, spacing: FmSpace.x3) {
                PresenceDot(online: group.online, away: away)
                    .padding(.top, 4)
                details
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .frame(minHeight: 44)
            .background(selected ? fm.accentSubtle : Color.clear)
            .clipShape(FmRoundedShape(radius: FmRadius.md))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(onTap == nil)
        .padding(.bottom, FmSpace.x1)
        .contextMenu {
            if !group.isSelf && !visitor {
                Button(action: onProfile) { Label("View profile", systemImage: "person.crop.circle") }
            }
            if !group.isSelf, let hub = retentionHub {
                ChatRetentionMenu(hub: hub, conversation: "@\(group.id)")
            }
        }
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 1) {
            HStack(spacing: 4) {
                Text(group.isSelf ? "\(group.name) (you)" : group.name)
                    .fmText(.bodySmall, color: selected ? fm.textPrimary : fm.textSecondary,
                            weight: selected ? FmType.emphasis : FmType.regular)
                    .lineLimit(1)
                UserStatusEmoji(status: group.userStatus, size: FmType.small)
            }
            if visitor {
                Text("Visitor").fmText(.labelSmall, color: fm.textTertiary)
            } else if away {
                Text("Away").fmText(.labelSmall, color: fm.warning)
            }
            if !group.disambiguator.isEmpty {
                Text("id \(group.disambiguator)")
                    .fmText(.labelSmall, color: fm.textTertiary)
                    .lineLimit(1)
            }
            HStack(spacing: FmSpace.x1) {
                ForEach(group.members) { (member: Member) in
                    FmIcon(ChatHub.platformIcon(member.platform), size: 13, color: fm.textTertiary,
                           label: member.platform.isEmpty ? "node" : member.platform)
                }
            }
            .padding(.top, FmSpace.x1)
        }
    }
}

struct NewChatChannelSheet: View {
    @Environment(\.dismiss) private var dismiss
    let hub: ChatHub
    let onCreated: (String) -> Void
    @State private var name: String = ""
    @State private var kind: Int = 0
    @State private var members: String = ""
    @State private var working: Bool = false
    @State private var error: String?

    private var managedAvailable: Bool { hub.office != nil }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("channel-name", text: $name)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }
                Section {
                    Picker("Who can see it", selection: $kind) {
                        Text("Public — every signed-in member").tag(0)
                        if managedAvailable {
                            Text("Private — only people you invite").tag(2)
                        }
                    }
                    .pickerStyle(.inline)
                    .labelsHidden()
                } footer: {
                    Text(footer)
                }
                if kind == 2 {
                    Section("Invite people") {
                        TextField("usernames, separated by commas", text: $members)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                    }
                }
                if let error {
                    Section { Text(error).foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
                }
            }
            .navigationTitle("New channel")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") { Task { await create() } }
                        .disabled(working || name.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }

    private var footer: String {
        switch kind {
        case 2:
            let invited = members.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
            if invited.isEmpty {
                return "Only you can see this room until you invite someone. Nobody else, including administrators, is added."
            }
            return "Only you and " + invited.map { "@" + $0 }.joined(separator: ", ") + " can see, join or read this room. Nobody else, including administrators, is added."
        default:
            return managedAvailable
                ? "Anyone in this project's chat can read it. ForkBot and integrations can take part."
                : "Anyone in this project's chat can read it. Sign in to create a private room that only invited people can see."
        }
    }

    private func create() async {
        working = true
        defer { working = false }
        switch kind {
        case 2:
            guard let office = hub.office else { return }
            let list: [String] = members.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
            do {
                if let room = try await office.create(name: name, isPrivate: true, members: list) {
                    onCreated(room.conversation)
                }
                dismiss()
            } catch {
                self.error = "\(error)"
            }
        default:
            guard let created = hub.extras.addPublicChannel(name) else {
                error = "Use letters, digits, dashes or underscores."
                return
            }
            onCreated(created)
            dismiss()
        }
    }
}

struct InviteToChannelSheet: View {
    @Environment(\.dismiss) private var dismiss
    let hub: ChatHub
    let channel: String
    @State private var invited: Set<String> = []

    private var people: [Member] {
        hub.relay.roster().filter { !$0.isSelf && $0.online && !ChatVisitorPresence.isTransientVisitor(name: $0.name) }
    }

    var body: some View {
        NavigationStack {
            List {
                if people.isEmpty {
                    Text("Nobody else is online right now. Invitations go to devices that are connected.")
                        .foregroundStyle(.secondary)
                }
                ForEach(people) { (member: Member) in
                    Button(action: {
                        if hub.extras.invite(peerId: member.id, to: channel) { invited.insert(member.id) }
                    }) {
                        HStack {
                            VStack(alignment: .leading) {
                                Text(member.owner.isEmpty ? member.name : member.owner)
                                if !member.nodeName.isEmpty {
                                    Text(member.nodeName).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                            Spacer()
                            if invited.contains(member.id) {
                                Image(systemName: "checkmark").foregroundStyle(.green)
                            }
                        }
                    }
                    .disabled(invited.contains(member.id))
                }
            }
            .navigationTitle("Invite to \(channel)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
    }
}

struct MoveChannelSheet: View {
    @Environment(\.dismiss) private var dismiss
    let channel: String
    let rows: [ChannelGroups.Row]
    let onMove: (String) -> Void

    var body: some View {
        NavigationStack {
            List {
                Button("No group") {
                    onMove("")
                    dismiss()
                }
                ForEach(ChannelGroups.renderedGroupNames(rows), id: \.self) { (group: String) in
                    Button(group) {
                        onMove(group)
                        dismiss()
                    }
                }
            }
            .navigationTitle("Move \(channel)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
        }
    }
}
