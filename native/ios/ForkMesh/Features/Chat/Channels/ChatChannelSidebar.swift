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
    @State private var desktop: LocalControlService? = Services.get(LocalControlService.self)
    @Environment(\.shellDock) private var shell
    @State private var statusSheet: Bool = false
    @State private var newChannel: Bool = false
    @State private var newGroup: Bool = false
    @State private var groupName: String = ""
    @State private var renaming: String?
    @State private var moving: String?
    @State private var inviting: String?
    @State private var managing: OfficeRoom?
    @State private var leaving: String?
    @State private var deleting: String?
    @State private var blocking: String?
    @State private var person: ChatPersonKey?
    @State private var showingOnline: Bool = false

    private var canSetStatus: Bool { Services.get(UserStatusService.self)?.signedIn ?? false }

    private func select(_ conversation: String) {
        hub.select(conversation)
        onSelected?()
    }

    private var rows: [ChannelGroups.Row] {
        let state = groups?.state ?? ChannelGroups.State()
        let listed: [String] = hub.listedChannels.filter { !hub.workspace.isStarred($0) }
        return ChannelGroups.layout(state, channels: listed) { channel in
            OfficeFrames.isOffice(channel) && !hub.isEveryoneChannel(channel) ? "Office" : ""
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if onSearch != nil || onCompose != nil || onClose != nil {
                    jumpBar.padding(.bottom, FmSpace.x3)
                }
                recentSection.padding(.bottom, FmSpace.x4)
                if !hub.workspace.starred.isEmpty {
                    sectionTitle("Starred", count: hub.workspace.starred.count)
                    card {
                        ForEach(hub.workspace.starred, id: \.self) { (conversation: String) in
                            conversationTile(conversation)
                        }
                    }
                    .padding(.bottom, FmSpace.x4)
                }
                sectionTitle("Channels", count: rows.filter { !$0.header }.count) { addMenu }
                card {
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
                }
                directMessages
                if !hub.memberGroups.isEmpty {
                    sectionTitle("People", count: hub.memberGroups.count)
                        .padding(.top, FmSpace.x4)
                    card {
                        ForEach(hub.memberGroups) { (group: MemberGroup) in
                            ChatMemberRow(
                                group: group, selected: memberSelected(group),
                                onTap: memberTap(group), onProfile: { person = ChatPersonKey(group: group) }, retentionHub: hub)
                        }
                    }
                }
                connectedApps
            }
            .padding(FmSpace.x3)
        }
        .safeAreaPadding(.bottom, shell?.clearance ?? 0)
        .background(fm.wallpaper ? Color.clear : fm.bgBase)
        .fmRefreshable {
            await hub.office?.refresh()
            await hub.direct?.refresh()
            await hub.refreshAppCommands(force: true)
        }
        .task {
            await hub.direct?.refresh()
            await hub.refreshAppCommands()
        }
        .sheet(isPresented: $statusSheet) { UserStatusSheet() }
        .sheet(item: $person) { (key: ChatPersonKey) in
            ChatPersonSheet(
                hub: hub, person: key, onMessage: { (recipient: ChatRecipient) in open(recipient) },
                onProfile: { (name: String) in onRoute(.profile(name: name)) })
        }
        .sheet(isPresented: $showingOnline) {
            ChatOnlineSheet(
                hub: hub,
                onMessage: { (recipient: ChatRecipient) in
                    showingOnline = false
                    open(recipient)
                },
                onProfile: { (name: String) in
                    showingOnline = false
                    onRoute(.profile(name: name))
                })
        }
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
                    if hub.currentConversation == channel { hub.select(hub.generalConversation) }
                }
                leaving = nil
            }
        } message: {
            Text("It disappears from this phone. Messages others send there stop arriving here.")
        }
        .confirmationDialog(
            "Delete this conversation?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                if let conversation = deleting { hub.deleteConversation(conversation) }
                deleting = nil
            }
        } message: {
            Text("It is removed from this phone only, and comes back if they message you again.")
        }
        .confirmationDialog(
            "Block \(blocking ?? "")?", isPresented: Binding(get: { blocking != nil }, set: { if !$0 { blocking = nil } }),
            titleVisibility: .visible
        ) {
            Button("Block", role: .destructive) {
                if let name = blocking { hub.setBlocked(name, true) }
                blocking = nil
            }
        } message: {
            Text("Their messages, typing and alerts are hidden on this phone. You can unblock them from their profile.")
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
                            .fmText(.bodyLarge, color: fm.textTertiary)
                            .lineLimit(1)
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, FmSpace.x3)
                    .frame(minHeight: 44)
                    .contentShape(Rectangle())
                    .shellGlass(RoundedRectangle(cornerRadius: 22, style: .continuous))
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

    private var cardFill: Color { fm.wallpaper ? fm.bgRaised.opacity(0.62) : fm.bgRaised }

    private func card<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 0) { content() }
            .padding(FmSpace.x1)
            .background(FmRoundedShape(radius: FmRadius.lg).fill(cardFill))
            .overlay(FmRoundedShape(radius: FmRadius.lg).stroke(fm.border.opacity(fm.wallpaper ? 0.35 : 0.6), lineWidth: 0.5))
    }

    private func sectionTitle(_ title: String, count: Int) -> some View {
        sectionTitle(title, count: count) { EmptyView() }
    }

    private func sectionTitle<Trailing: View>(_ title: String, count: Int, @ViewBuilder trailing: () -> Trailing) -> some View {
        HStack(spacing: FmSpace.x2) {
            Text(title.uppercased())
                .fmText(.labelMedium, color: fm.textSecondary, tracking: 0.6)
                .lineLimit(1)
            if count > 0 {
                Text("\(count)")
                    .fmText(.labelSmall, color: fm.textTertiary, tabular: true)
                    .padding(.horizontal, 7)
                    .frame(minHeight: 18)
                    .background(Capsule().fill(fm.textPrimary.opacity(0.08)))
            }
            Spacer(minLength: 0)
            trailing()
        }
        .padding(.horizontal, FmSpace.x2)
        .frame(minHeight: 44)
    }

    @ViewBuilder private var recentSection: some View {
        let recent: [String] = hub.recentConversations(limit: ChatChannelSidebar.recentLimit)
        sectionTitle("Recent", count: 0) { onlineButton }
        if !recent.isEmpty {
            card {
                ForEach(recent, id: \.self) { (conversation: String) in
                    if ChatHub.isPersonConversation(conversation) || conversation == meshConversation {
                        conversationTile(conversation)
                    } else {
                        channelTile(conversation)
                    }
                }
            }
        }
    }

    static let recentLimit: Int = 5

    private var onlineButton: some View {
        let online: Int = hub.memberGroups.filter(\.online).count
        return Button(action: { showingOnline = true }) {
            HStack(spacing: 5) {
                Circle()
                    .fill(online > 0 ? fm.success : fm.textTertiary)
                    .frame(width: 7, height: 7)
                Text("\(online) online")
                    .fmText(.labelSmall, color: fm.textSecondary, tabular: true)
                    .lineLimit(1)
            }
            .padding(.horizontal, FmSpace.x2)
            .frame(minHeight: 26)
            .background(Capsule().fill(fm.textPrimary.opacity(0.06)))
            .frame(minHeight: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(online) online")
        .accessibilityHint("Shows who is online")
    }

    private var desktopChip: (label: String, tone: LocalDesktopTone)? {
        guard let desktop else { return nil }
        return LocalDesktopText.chip(desktop)
    }

    @ViewBuilder private var connectedApps: some View {
        let apps: [ChatConnectedApp] = hub.connectedApps
        let chip: (label: String, tone: LocalDesktopTone)? = desktopChip
        sectionTitle("Connected apps", count: apps.count + (discord != nil ? 1 : 0) + (chip != nil ? 1 : 0)) {
            Button(action: { onRoute(.settings(section: "apps")) }) {
                Text("Manage")
                    .fmText(.labelMedium, color: fm.accent)
                    .frame(minWidth: 44, minHeight: 44)
            }
            .buttonStyle(.plain)
        }
        .padding(.top, FmSpace.x4)
        card {
            ForEach(apps) { (app: ChatConnectedApp) in
                ChatAppTile(
                    title: app.name, detail: app.detail, monogram: String(app.name.prefix(1)).uppercased(),
                    tint: fm.accent, onTap: { onRoute(.settings(section: "apps")) })
            }
            if let discord {
                ChatAppTile(
                    title: "Discord", detail: discord.status.connected ? "Connected through your desktop" : "Through your desktop",
                    symbol: "bubble.left.and.bubble.right.fill", tint: Color(red: 0.345, green: 0.396, blue: 0.949),
                    status: discord.status.connected, onTap: { onRoute(.discord(guild: "", channel: nil)) })
            }
            if let chip {
                ChatAppTile(
                    title: "Desktop on this network", detail: chip.label, symbol: "desktopcomputer", tint: fm.success,
                    status: chip.tone == .success, onTap: { onRoute(.desktops) })
            }
            if apps.isEmpty && discord == nil && chip == nil {
                ChatAppTile(
                    title: "No apps connected", detail: "Install apps from the Market on the web or the desktop",
                    symbol: "puzzlepiece.extension", tint: fm.textTertiary, onTap: { onRoute(.settings(section: "apps")) })
            }
        }
    }

    @ViewBuilder private var directMessages: some View {
        let rows: [ChatDirectRow] = hub.directRows.filter { !hub.workspace.isStarred($0.conversation) }
        if !rows.isEmpty || onCompose != nil {
            sectionTitle("Direct messages", count: rows.count) {
                if let onCompose {
                    Button(action: onCompose) {
                        FmIcon("add", size: 20, color: fm.textSecondary, label: "New message")
                            .frame(width: 44, height: 44)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.top, FmSpace.x4)
            if !rows.isEmpty {
                card {
                    ForEach(rows) { (row: ChatDirectRow) in
                        conversationTile(row.conversation, direct: row)
                    }
                }
            }
        }
    }

    @ViewBuilder private func conversationTile(_ conversation: String, direct: ChatDirectRow? = nil) -> some View {
        let office: OfficeRoom? = hub.office?.room(for: conversation)
        let person: Bool = conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation)
        let locked: Bool = office?.isPrivate ?? hub.extras.isPrivate(conversation)
        let icon: String = conversation == meshConversation ? "point.3.connected.trianglepath.dotted" : (person ? "at" : (locked ? "lock.fill" : "number"))
        let title: String = direct.map { $0.name } ?? hub.conversationLabel(conversation)
        let preview: ChatListPreview? = hub.listPreview(conversation)
        let row: ChatDirectRow? = direct ?? (person ? hub.directRows.first(where: { $0.conversation == conversation }) : nil)
        ChatChannelTile(
            title: title, icon: icon, selected: hub.currentConversation == conversation, unread: hub.isUnread(conversation),
            onTap: { select(conversation) }, muted: hub.workspace.isMuted(conversation),
            draft: hub.workspace.hasDraft(conversation) && hub.currentConversation != conversation,
            count: hub.workspace.isMuted(conversation) ? 0 : (row?.unread ?? 0), online: row?.online,
            preview: preview?.text ?? hub.listFallback(conversation), time: preview.map { hub.listTimeLabel($0) } ?? "",
            quiet: preview == nil, avatar: person ? (row?.name ?? String(title.drop(while: { $0 == "@" }))) : nil)
            .contextMenu {
                conversationMenu(conversation)
                if person || conversation == meshConversation {
                    ChatRetentionMenu(hub: hub, conversation: conversation)
                }
                if person {
                    let handle: String = hub.conversationHandle(conversation)
                    if !handle.isEmpty {
                        Button(role: .destructive, action: { blocking = handle }) {
                            Label("Block \(handle)", systemImage: "hand.raised")
                        }
                    }
                    Button(role: .destructive, action: { deleting = conversation }) {
                        Label("Delete Conversation", systemImage: "trash")
                    }
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
            HStack(spacing: FmSpace.x3) {
                Image(systemName: "chevron.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
                    .rotationEffect(.degrees(row.collapsed ? 0 : 90))
                    .frame(width: 34, height: 20)
                Text(row.group)
                    .fmText(.labelLarge, color: fm.textSecondary)
                    .lineLimit(1)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal, FmSpace.x2)
            .padding(.top, FmSpace.x2)
            .frame(minHeight: 40)
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
            title: office?.name.isEmpty == false ? "#\(office!.name)" : hub.conversationLabel(channel),
            icon: locked ? "lock.fill" : (channel == ChatEveryone.worldConversation ? "globe" : "number"),
            selected: hub.currentConversation == channel, unread: hub.isUnread(channel), onTap: { select(channel) },
            muted: hub.workspace.isMuted(channel),
            draft: hub.workspace.hasDraft(channel) && hub.currentConversation != channel,
            preview: preview?.text ?? hub.listFallback(channel), time: preview.map { hub.listTimeLabel($0) } ?? "",
            quiet: preview == nil)
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
                    if hub.canLeaveChannel(channel) {
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
        return { open(ChatHub.recipient(for: group)) }
    }

    private func memberSelected(_ group: MemberGroup) -> Bool {
        let current: String = hub.currentConversation
        if current == "@\(group.id)" { return true }
        guard DirectMessageFrames.isDirect(current), !group.account.isEmpty else { return false }
        return hub.direct?.record(for: current)?.otherUser == group.account
    }

    private func open(_ recipient: ChatRecipient) {
        Task {
            if await hub.openPerson(recipient) { onSelected?() }
        }
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
    var quiet: Bool = false
    var avatar: String? = nil

    static let leadingSize: CGFloat = 36

    var body: some View {
        Button(action: onTap) {
            HStack(alignment: .center, spacing: FmSpace.x3) {
                leading
                VStack(alignment: .leading, spacing: 2) {
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                        Text(title)
                            .fmText(.bodyLarge, color: selected || unread ? fm.textPrimary : fm.textSecondary,
                                    weight: selected || unread ? FmType.emphasis : FmType.regular)
                            .lineLimit(1)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        if !time.isEmpty {
                            Text(time)
                                .fmText(.bodySmall, color: unread ? fm.danger : fm.textTertiary, tabular: true)
                                .lineLimit(1)
                                .fixedSize()
                        }
                    }
                    if let preview, !preview.isEmpty {
                        Text(preview)
                            .fmText(.bodyMedium, color: quiet ? fm.textTertiary.opacity(0.75) : fm.textTertiary)
                            .lineLimit(1)
                            .truncationMode(.tail)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                trailing
            }
            .opacity(muted && !selected ? 0.6 : 1)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x2)
            .frame(minHeight: 56)
            .background(selected ? fm.accentSubtle : Color.clear)
            .clipShape(FmRoundedShape(radius: FmRadius.md))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    @ViewBuilder private var trailing: some View {
        if draft || muted || count > 0 || unread {
            HStack(spacing: FmSpace.x1) {
                if draft {
                    Image(systemName: "pencil")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(fm.textTertiary)
                        .accessibilityLabel("Draft")
                }
                if muted {
                    Image(systemName: "bell.slash")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(fm.textTertiary)
                        .accessibilityLabel("Muted")
                }
                if count > 0 {
                    Text("\(count)")
                        .fmText(.labelSmall, color: Color.white, tabular: true)
                        .padding(.horizontal, 6)
                        .frame(minWidth: 20, minHeight: 20)
                        .background(fm.danger)
                        .clipShape(Capsule())
                } else if unread {
                    Circle().fill(fm.danger).frame(width: 9, height: 9)
                }
            }
        }
    }

    @ViewBuilder private var leading: some View {
        let size: CGFloat = ChatChannelTile.leadingSize
        if let avatar {
            ZStack(alignment: .bottomTrailing) {
                MessagesPersonAvatar(name: avatar, size: size)
                if let online {
                    PresenceDot(online: online, size: 10)
                        .overlay { Circle().strokeBorder(selected ? fm.accentSubtle : fm.bgRaised, lineWidth: 1.5) }
                        .offset(x: 2, y: 2)
                }
            }
            .frame(width: size, height: size)
        } else {
            Image(systemName: icon)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(selected ? fm.accent : (unread ? fm.textPrimary : fm.textSecondary))
                .frame(width: size, height: size, alignment: .center)
                .background(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(selected ? fm.accent.opacity(0.18) : fm.textPrimary.opacity(0.07)))
                .overlay(alignment: .bottomTrailing) {
                    if let online {
                        PresenceDot(online: online, size: 9).offset(x: 2, y: 2)
                    }
                }
        }
    }
}

struct ChatAppTile: View {
    @Environment(\.fmPalette) private var fm
    let title: String
    let detail: String
    var monogram: String = ""
    var symbol: String = ""
    let tint: Color
    var status: Bool? = nil
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(alignment: .center, spacing: FmSpace.x3) {
                badge
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .fmText(.bodyLarge, color: fm.textPrimary)
                        .lineLimit(1)
                    if !detail.isEmpty {
                        Text(detail)
                            .fmText(.bodyMedium, color: fm.textTertiary)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                Image(systemName: "chevron.right")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
            }
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x2)
            .frame(minHeight: 56)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private var badge: some View {
        let size: CGFloat = ChatChannelTile.leadingSize
        return ZStack {
            RoundedRectangle(cornerRadius: 10, style: .continuous).fill(tint.opacity(0.9))
            if symbol.isEmpty {
                Text(monogram)
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(Color.white)
            } else {
                Image(systemName: symbol)
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(Color.white)
            }
        }
        .frame(width: size, height: size)
        .overlay(alignment: .bottomTrailing) {
            if let status {
                PresenceDot(online: status, size: 10)
                    .overlay { Circle().strokeBorder(fm.bgRaised, lineWidth: 1.5) }
                    .offset(x: 3, y: 3)
            }
        }
        .accessibilityHidden(true)
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
        HStack(alignment: .center, spacing: FmSpace.x3) {
            Button(action: onProfile) {
                ZStack(alignment: .bottomTrailing) {
                    MessagesPersonAvatar(name: group.name, size: ChatChannelTile.leadingSize)
                    PresenceDot(online: group.online, away: away, size: 10)
                        .overlay { Circle().strokeBorder(fm.bgRaised, lineWidth: 1.5) }
                        .offset(x: 2, y: 2)
                }
                .frame(width: ChatChannelTile.leadingSize, height: ChatChannelTile.leadingSize)
                .frame(minWidth: 44, minHeight: 44)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(visitor)
            .accessibilityLabel("\(group.name)'s profile")
            Button(action: { onTap?() }) {
                details
                    .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(onTap == nil)
            .accessibilityHint(group.isSelf ? "" : "Opens your direct messages with \(group.name)")
        }
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, FmSpace.x2)
        .frame(minHeight: 56)
        .background(selected ? fm.accentSubtle : Color.clear)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .contentShape(Rectangle())
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
                    .fmText(.bodyLarge, color: selected ? fm.textPrimary : fm.textSecondary,
                            weight: selected ? FmType.emphasis : FmType.regular)
                    .lineLimit(1)
                UserStatusEmoji(status: group.userStatus, size: FmType.body)
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
