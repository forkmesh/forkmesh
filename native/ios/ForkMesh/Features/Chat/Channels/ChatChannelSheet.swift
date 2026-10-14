import FMApi
import FMBase
import FMChat
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

enum ChatChannelPage: Hashable {
    case files, bookmarks, canvas, members, apps, notifications, details, office
}

@MainActor
enum ChatChannelGrouping {
    static func rows(_ hub: ChatHub, _ groups: ChannelGroupsStore?) -> [ChannelGroups.Row] {
        let state: ChannelGroups.State = groups?.state ?? ChannelGroups.State()
        let listed: [String] = hub.listedChannels.filter { !hub.workspace.isStarred($0) }
        return ChannelGroups.layout(state, channels: listed) { (channel: String) in
            OfficeFrames.isOffice(channel) && !hub.isEveryoneChannel(channel) ? "Office" : ""
        }
    }

    static func move(_ channel: String, to group: String, rows: [ChannelGroups.Row], groups: ChannelGroupsStore?) {
        let members: [String] = ChannelGroups.renderedMembers(rows, group)
        let automatic: Bool = ChannelGroups.renderedGroupIsAutomatic(rows, group)
        groups?.update {
            ChannelGroups.assign(&$0, channel: channel, to: group, index: -1, members: members, automaticGroup: automatic)
        }
    }

    static func group(of channel: String, rows: [ChannelGroups.Row]) -> String {
        rows.first { !$0.header && $0.channel == channel }?.group ?? ""
    }
}

struct ChatChannelSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let conversation: String
    let onInsert: (String) -> Void
    let onRoute: (Route) -> Void
    @State private var path: [ChatChannelPage] = []
    @State private var inviting: Bool = false
    @State private var moving: Bool = false
    @State private var leaving: Bool = false
    @State private var explainAdd: Bool = false

    private var office: OfficeRoom? { hub.office?.room(for: conversation) }
    private var locked: Bool { office?.isPrivate ?? hub.extras.isPrivate(conversation) }
    private var name: String {
        let label: String = hub.conversationLabel(conversation)
        return label.hasPrefix("#") ? String(label.dropFirst()) : label
    }
    private var canLeave: Bool { hub.canLeaveChannel(conversation) }

    var body: some View {
        NavigationStack(path: $path) {
            ScrollView {
                VStack(spacing: 0) {
                    header
                    actions
                        .padding(.top, FmSpace.x4)
                        .padding(.bottom, FmSpace.x4)
                    tabRows
                    Divider().overlay(fm.border).padding(.vertical, FmSpace.x2)
                    moreRows
                }
                .padding(.horizontal, FmSpace.x3)
                .padding(.bottom, FmSpace.x5)
            }
            .scrollContentBackground(.hidden)
            .toolbar(.hidden, for: .navigationBar)
            .navigationDestination(for: ChatChannelPage.self) { (page: ChatChannelPage) in
                destination(page)
            }
        }
        .presentationDetents([.large])
        .presentationDragIndicator(.visible)
        .presentationBackground(.ultraThinMaterial)
        .presentationCornerRadius(30)
        .sheet(isPresented: $inviting) { InviteToChannelSheet(hub: hub, channel: conversation) }
        .sheet(isPresented: $moving) { ChatMoveChannelSheet(hub: hub, channel: conversation) }
        .confirmationDialog("Leave \(hub.conversationLabel(conversation))?", isPresented: $leaving, titleVisibility: .visible) {
            Button("Leave channel", role: .destructive, action: leave)
        } message: {
            Text("It disappears from this phone. Messages others send there stop arriving here.")
        }
        .alert("Everyone can already read \(hub.conversationLabel(conversation))", isPresented: $explainAdd) {
            Button("Mention someone") { close { onInsert("@") } }
            Button("Notify @here") { close { onInsert("@here ") } }
            Button("OK", role: .cancel) {}
        } message: {
            Text("It is a public channel: anyone in this project's chat sees it. Mention people with @name to bring them in.")
        }
    }

    private func close(then: @escaping () -> Void) {
        dismiss()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.45, execute: then)
    }

    private func leave() {
        hub.workspace.forget(conversation: conversation)
        hub.extras.removeChannel(conversation)
        if hub.currentConversation == conversation { hub.select(hub.generalConversation) }
        dismiss()
    }

    private var header: some View {
        HStack(alignment: .center, spacing: FmSpace.x3) {
            Button(action: { dismiss() }) {
                Image(systemName: "xmark")
                    .font(.system(size: 22, weight: .regular))
                    .foregroundStyle(fm.textPrimary)
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close")
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 5) {
                    Image(systemName: locked ? "lock.fill" : "number")
                        .font(.system(size: 16, weight: .bold))
                        .foregroundStyle(fm.textPrimary)
                    Text(name)
                        .font(.system(size: 20, weight: .bold))
                        .foregroundStyle(fm.textPrimary)
                        .lineLimit(1)
                }
                Text(hub.channelDetail(conversation))
                    .font(.system(size: 16))
                    .foregroundStyle(fm.textSecondary)
                    .lineLimit(1)
            }
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isHeader)
            Spacer(minLength: 0)
            Menu { overflowMenu } label: {
                Image(systemName: "ellipsis")
                    .font(.system(size: 20, weight: .bold))
                    .foregroundStyle(fm.textPrimary)
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .accessibilityLabel("More channel actions")
        }
        .padding(.top, FmSpace.x5)
    }

    @ViewBuilder private var overflowMenu: some View {
        let starred: Bool = hub.workspace.isStarred(conversation)
        Button(action: { hub.toggleStarred(conversation) }) {
            Label(starred ? "Unstar channel" : "Star channel", systemImage: starred ? "star.slash" : "star")
        }
        Picker(selection: Binding(get: { hub.notifyLevel(conversation) }, set: { hub.setNotifyLevel($0, for: conversation) })) {
            ForEach(ChatNotifyLevel.allCases) { (level: ChatNotifyLevel) in
                Text(level.label).tag(level)
            }
        } label: {
            Label("Notifications", systemImage: "bell")
        }
        .pickerStyle(.menu)
        if hub.isUnread(conversation) || hub.unreadCount(conversation) > 0 {
            Button(action: { hub.relay.markConversationRead(conversation) }) {
                Label("Mark as read", systemImage: "checkmark.message")
            }
        }
        Button(action: {
            #if canImport(UIKit)
            UIPasteboard.general.string = hub.conversationLabel(conversation)
            #endif
            hub.post("Copied \(hub.conversationLabel(conversation)).")
        }) {
            Label("Copy channel name", systemImage: "doc.on.doc")
        }
        ChatRetentionMenu(hub: hub, conversation: conversation)
        if canLeave {
            Button(role: .destructive, action: { leaving = true }) {
                Label("Leave channel", systemImage: "rectangle.portrait.and.arrow.right")
            }
        }
    }

    private var actions: some View {
        HStack(spacing: FmSpace.x3) {
            actionButton("person.badge.plus", "Add", action: add)
            actionButton("star.fill", "Move", action: { moving = true })
            actionButton("magnifyingglass", "Search") {
                let place: String = hub.conversationLabel(conversation)
                close { hub.pendingSearch = "in:\(place) " }
            }
        }
    }

    private func add() {
        if let office {
            if office.canManage, office.isPrivate {
                path.append(.office)
            } else {
                explainAdd = true
            }
            return
        }
        if hub.extras.isPrivate(conversation) {
            inviting = true
        } else {
            explainAdd = true
        }
    }

    private func actionButton(_ symbol: String, _ title: String, action: @escaping () -> Void) -> some View {
        let shape: RoundedRectangle = RoundedRectangle(cornerRadius: 18, style: .continuous)
        return Button(action: action) {
            HStack(spacing: FmSpace.x2) {
                Image(systemName: symbol)
                    .font(.system(size: 18, weight: .regular))
                Text(title)
                    .font(.system(size: 18, weight: .medium))
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .foregroundStyle(fm.textPrimary)
            .frame(maxWidth: .infinity, minHeight: 56)
            .background(shape.fill(fm.bgRaised.opacity(fm.dark ? 0.18 : 0.5)))
            .overlay(shape.stroke(fm.border, lineWidth: 1))
            .contentShape(shape)
        }
        .buttonStyle(.plain)
    }

    private var tabRows: some View {
        VStack(spacing: 2) {
            row("bubble.left.and.bubble.right.fill", "Messages", selected: true) { dismiss() }
            row("doc.on.doc", "Files & Links") { path.append(.files) }
            let bookmarks: Int = hub.bookmarkCount(conversation)
            row("folder", "Bookmarks", trailing: bookmarks > 0 ? "\(bookmarks)" : nil) { path.append(.bookmarks) }
            if let canvas = hub.workspace.canvas(for: conversation) {
                row("doc.richtext", canvas.title) { path.append(.canvas) }
            } else {
                row("rectangle.badge.plus", "Add canvas") { path.append(.canvas) }
            }
        }
    }

    private var moreRows: some View {
        VStack(spacing: 2) {
            row("person.text.rectangle", "Members", trailing: "\(hub.memberCount(conversation))", chevron: true) {
                path.append(.members)
            }
            row("circle.grid.2x2", "Agents & Apps", chevron: true) { path.append(.apps) }
            row("bell", "Notifications", trailing: hub.notifyLevel(conversation).label, chevron: true) {
                path.append(.notifications)
            }
            row("gearshape", "Settings & Details", chevron: true) { path.append(.details) }
        }
    }

    private func row(
        _ symbol: String, _ title: String, trailing: String? = nil, chevron: Bool = false, selected: Bool = false,
        action: @escaping () -> Void
    ) -> some View {
        let shape: RoundedRectangle = RoundedRectangle(cornerRadius: 20, style: .continuous)
        return Button(action: action) {
            HStack(spacing: FmSpace.x4) {
                Image(systemName: symbol)
                    .font(.system(size: 20, weight: .regular))
                    .foregroundStyle(fm.textPrimary)
                    .frame(width: 30)
                Text(title)
                    .font(.system(size: 19, weight: selected ? .semibold : .regular))
                    .foregroundStyle(fm.textPrimary)
                    .lineLimit(1)
                Spacer(minLength: FmSpace.x2)
                if let trailing {
                    Text(trailing)
                        .font(.system(size: 18))
                        .foregroundStyle(fm.textSecondary)
                        .lineLimit(1)
                }
                if chevron {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(fm.textTertiary)
                }
            }
            .padding(.horizontal, FmSpace.x4)
            .frame(minHeight: 58)
            .background(shape.fill(selected ? fm.bgBase.opacity(fm.dark ? 0.85 : 0.7) : Color.clear))
            .contentShape(shape)
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
    }

    @ViewBuilder private func destination(_ page: ChatChannelPage) -> some View {
        switch page {
        case .files:
            ChatChannelFilesScreen(hub: hub, conversation: conversation)
        case .bookmarks:
            ChatChannelBookmarksScreen(hub: hub, conversation: conversation)
        case .canvas:
            ChatChannelCanvasScreen(hub: hub, conversation: conversation, onPost: { (text: String) in
                close { onInsert(text) }
            })
        case .members:
            ChatChannelMembersScreen(
                hub: hub, conversation: conversation, onAdd: add,
                onProfile: { (person: String) in close { onRoute(.profile(name: person)) } },
                onMessage: { (person: ChatChannelMember) in
                    close {
                        if !person.peerId.isEmpty && person.online && !person.peerId.hasPrefix("app:") {
                            hub.select("@\(person.peerId)")
                        } else {
                            onInsert("@\(person.account.isEmpty ? person.name : person.account) ")
                        }
                    }
                })
        case .apps:
            ChatChannelAppsScreen(hub: hub, onInsert: { (text: String) in close { onInsert(text) } })
        case .notifications:
            ChatChannelNotificationsScreen(hub: hub, conversation: conversation)
        case .details:
            ChatChannelDetailsScreen(
                hub: hub, conversation: conversation, onLeave: canLeave ? { leaving = true } : nil,
                onOffice: office?.canManage == true ? { path.append(.office) } : nil)
        case .office:
            if let office, let service = hub.office {
                OfficeChannelSettingsScreen(office: service, room: office)
            }
        }
    }
}

struct ChatMoveChannelSheet: View {
    @Environment(\.dismiss) private var dismiss
    let hub: ChatHub
    let channel: String
    @State private var groups: ChannelGroupsStore? = Services.get(ChannelGroupsStore.self)
    @State private var naming: Bool = false
    @State private var newName: String = ""

    var body: some View {
        let rows: [ChannelGroups.Row] = ChatChannelGrouping.rows(hub, groups)
        let current: String = ChatChannelGrouping.group(of: channel, rows: rows)
        NavigationStack {
            List {
                Section {
                    Button(action: { hub.toggleStarred(channel) }) {
                        HStack {
                            Label("Starred", systemImage: "star.fill")
                            Spacer()
                            if hub.workspace.isStarred(channel) { Image(systemName: "checkmark") }
                        }
                    }
                } footer: {
                    Text("Starred channels sit at the top of your channel list.")
                }
                Section("Move to group") {
                    groupButton("No group", group: "", current: current, rows: rows)
                    ForEach(ChannelGroups.renderedGroupNames(rows), id: \.self) { (group: String) in
                        groupButton(group, group: group, current: current, rows: rows)
                    }
                    Button(action: { naming = true }) { Label("New group\u{2026}", systemImage: "folder.badge.plus") }
                }
            }
            .navigationTitle("Move \(hub.conversationLabel(channel))")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .alert("New group", isPresented: $naming) {
                TextField("Name", text: $newName)
                Button("Cancel", role: .cancel) { newName = "" }
                Button("Create") {
                    let name: String = ChannelGroups.normalizeName(newName)
                    newName = ""
                    if name.isEmpty { return }
                    groups?.update { _ = ChannelGroups.createGroup(&$0, name) }
                    if hub.workspace.isStarred(channel) { hub.toggleStarred(channel) }
                    ChatChannelGrouping.move(channel, to: name, rows: ChatChannelGrouping.rows(hub, groups), groups: groups)
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    private func groupButton(_ title: String, group: String, current: String, rows: [ChannelGroups.Row]) -> some View {
        Button(action: {
            if hub.workspace.isStarred(channel) { hub.toggleStarred(channel) }
            ChatChannelGrouping.move(channel, to: group, rows: ChatChannelGrouping.rows(hub, groups), groups: groups)
            dismiss()
        }) {
            HStack {
                Text(title)
                Spacer()
                if !hub.workspace.isStarred(channel) && current == group { Image(systemName: "checkmark") }
            }
        }
    }
}
