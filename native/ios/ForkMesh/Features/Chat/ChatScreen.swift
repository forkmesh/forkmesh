// Encrypted chat against the live relay room and the local #mesh room: channel list, member roster, and the transcript with a composer. The logic is
// FMChat's ChatHub; this lays it out.

import FMAccount
import FMApi
import FMBase
import FMChat
import Observation
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct ChatScreen: View {
    @State private var hub: ChatHub? = ChatFeature.hub()

    var body: some View {
        if let hub = hub {
            ChatScreenBody(hub: hub)
                .fmQuietSounds()
        } else {
            FmEmptyState(icon: "chat_bubble_outline", title: "Chat is not available")
        }
    }
}

private struct ChatScreenBody: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.horizontalSizeClass) private var sizeClass
    let hub: ChatHub
    @State private var channelsOpen: Bool = false
    @State private var notice: FmNotice?
    @State private var pushed: Route?
    @State private var issueDraft: ChatIssueDraftBox?
    @State private var revealedTab: AppTab?
    @State private var composing: Bool = false
    @State private var searchRequest: ChatSearchRequest?

    var body: some View {
        content
            .directCalls(hub)
            .onAppear {
                hub.restoreLastConversation()
                syncShellTop(channelsOpen)
            }
            .onDisappear { syncShellTop(false) }
            .sheet(item: $issueDraft) { (box: ChatIssueDraftBox) in
                ChatIssueSheet(draft: box.draft, onFiled: { notice = FmNotice($0) })
            }
            .navigationDestination(item: $pushed) { (route: Route) in
                RouteDestinationView(route: route)
                    .shellPageClearance()
            }
            .onChange(of: hub.notice) { _, posted in
                if let posted = posted {
                    notice = FmNotice(posted.text)
                    hub.clearNotice()
                }
            }
            .onChange(of: hub.extras.inviteNotice) { _, posted in
                if let posted = posted {
                    notice = FmNotice(posted.text)
                    hub.extras.clearInviteNotice()
                }
            }
            .onChange(of: hub.pendingIssue) { _, argument in
                guard let argument = argument else { return }
                hub.pendingIssue = nil
                openIssue(argument)
            }
            .onChange(of: hub.pendingSearch) { _, query in
                guard let query = query else { return }
                hub.pendingSearch = nil
                searchRequest = ChatSearchRequest(query: query, scope: .messages)
            }
            .onChange(of: hub.pendingSaved) { _, wanted in
                guard wanted else { return }
                hub.pendingSaved = false
                searchRequest = ChatSearchRequest(scope: .saved)
            }
            .sheet(isPresented: $composing) {
                ChatNewMessageSheet(hub: hub)
            }
            .sheet(item: $searchRequest) { (request: ChatSearchRequest) in
                ChatSearchSheet(hub: hub, request: request, onOpen: { (conversation: String) in
                    closeChannels()
                    hub.open(conversation)
                })
            }
    }

    private func openIssue(_ argument: String) {
        let author: String = Services.get(AuthService.self)?.session?.nodeName ?? ""
        switch ChatIssueSheet.draft(fromCommand: argument, channel: hub.currentConversation, author: author) {
        case .success(let draft):
            issueDraft = ChatIssueDraftBox(draft: draft)
        case .failure(.unknownRepository(let name)):
            notice = FmNotice.error("Chat: repository \(name) is not available.")
        case .failure:
            notice = FmNotice.error("Usage: /issue [owner/repo] <title>")
        }
    }

    @ViewBuilder private var content: some View {
        if sizeClass == .regular {
            HStack(spacing: 0) {
                ChatChannelSidebar(
                    hub: hub, onSelected: nil, onRoute: { pushed = $0 }, onCompose: { composing = true },
                    onSearch: { searchRequest = ChatSearchRequest() })
                    .frame(width: 240)
                fm.canvas.frame(width: FmSpace.x2)
                transcript
            }
        } else {
            transcript
        }
    }

    @ViewBuilder private var channelDrawer: some View {
        if channelsOpen {
            Color.black.opacity(0.45)
                .ignoresSafeArea(.container, edges: .bottom)
                .onTapGesture { closeChannels() }
                .transition(.opacity)
                .zIndex(1)
            ChatChannelSidebar(
                hub: hub, onSelected: { closeChannels() },
                onRoute: { route in
                    closeChannels()
                    pushed = route
                },
                onCompose: { composing = true }, onSearch: { searchRequest = ChatSearchRequest() },
                onClose: { closeChannels() })
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background { drawerBackdrop }
            .shadow(color: .black.opacity(0.3), radius: 18, x: 6)
            .transition(.move(edge: .leading))
            .zIndex(2)
        }
    }

    @ViewBuilder private var drawerBackdrop: some View {
        if fm.wallpaper, let settings = Services.get(SettingsService.self) {
            FmWallpaperBackdrop(name: settings.backgroundImage, dim: settings.backgroundDim)
        } else {
            fm.bgBase.ignoresSafeArea(.container, edges: .bottom)
        }
    }

    private func openChannels() {
        ShellKeyboard.dismiss()
        withAnimation(.easeOut(duration: 0.25)) {
            channelsOpen = true
            syncShellTop(true)
        }
    }

    private func closeChannels() {
        withAnimation(.easeOut(duration: 0.25)) {
            channelsOpen = false
            syncShellTop(false)
        }
    }

    private func syncShellTop(_ open: Bool) {
        if open, revealedTab == nil {
            let tab: AppTab = AppNavigator.shared?.router.selectedTab ?? .chat
            revealedTab = tab
            ShellChrome.shared.reveal(on: tab)
        } else if !open, let tab = revealedTab {
            revealedTab = nil
            ShellChrome.shared.conceal(on: tab)
        }
    }

    private var transcript: some View {
        ChatTranscript(
            hub: hub,
            onOpenChannels: sizeClass == .regular ? nil : { channelsOpen ? closeChannels() : openChannels() },
            showsHeader: !channelsOpen,
            drawer: channelDrawer,
            notice: $notice,
            onCopied: { notice = FmNotice("Copied to clipboard") },
            onRoute: { pushed = $0 },
            onIssue: { issueDraft = ChatIssueDraftBox(draft: $0) },
            onCompose: { composing = true },
            onNotice: { notice = FmNotice($0) })
    }
}

struct ChatIssueDraftBox: Identifiable {
    let id: UUID = UUID()
    let draft: ChatIssueDraft
}

// MARK: - Transcript

private struct ChatTranscript<Drawer: View>: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let onOpenChannels: (() -> Void)?
    let showsHeader: Bool
    let drawer: Drawer
    @Binding var notice: FmNotice?
    let onCopied: () -> Void
    let onRoute: (Route) -> Void
    let onIssue: (ChatIssueDraft) -> Void
    let onCompose: () -> Void
    let onNotice: (String) -> Void
    @State private var draft: String = ""
    @State private var draftRoom: String = ""
    @State private var sharing: ChatShareRequest?
    @State private var composerFocused: Bool = false
    @State private var lastSeenId: String = ""
    @State private var sealInfo: Bool = false
    @State private var editing: ChatMessage?
    @State private var editText: String = ""
    @State private var inviting: Bool = false
    @State private var managing: OfficeRoom?
    @State private var meshDebugShown: Bool = false
    @State private var channelInfo: Bool = false
    @State private var broadcast: ChatBroadcastWarning?
    @Environment(\.chatShellActions) private var shell

    var body: some View {
        Group {
            if hub.inConversation {
                MessagesConversationView(
                    hub: hub, onBack: onOpenChannels, showsHeader: showsHeader, drawer: drawer,
                    actions: { (row: ChatRow) in actions(for: row) }, onRoute: onRoute, draft: $draft,
                    focused: $composerFocused, onSend: { send() }, notice: $notice)
            } else {
                roomLayout
            }
        }
        .onAppear {
            MeshAlerts.onScreen = true
            hub.roomShown()
            loadDraft(hub.currentConversation)
        }
        .onDisappear {
            MeshAlerts.onScreen = false
            hub.workspace.setDraft(draft, for: draftRoom)
        }
        .onChange(of: hub.currentConversation) { _, now in
            hub.workspace.setDraft(draft, for: draftRoom)
            loadDraft(now)
        }
        .sheet(item: $sharing) { (request: ChatShareRequest) in
            ChatShareMessageSheet(hub: hub, request: request)
        }
        .task { await hub.refreshAppCommands() }
        .onChange(of: hub.mesh.unread) { _, _ in if MeshAlerts.foreground { hub.roomShown() } }
        .alert(hub.isPrivateRoom ? "End-to-end encryption" : "Public channel", isPresented: $sealInfo) {
            Button("OK", role: .cancel) {}
            Button("Copy") { UIPasteboard.general.string = hub.sealSummary }
            Button("Send to agent") { FmMessageHandoff.send(hub.sealSummary) }
        } message: {
            Text(hub.sealSummary)
        }
        .sheet(isPresented: $inviting) {
            InviteToChannelSheet(hub: hub, channel: hub.currentConversation)
        }
        .sheet(item: $managing) { (room: OfficeRoom) in
            if let office = hub.office {
                NavigationStack { OfficeChannelSettingsScreen(office: office, room: room) }
            }
        }
        .sheet(isPresented: $channelInfo) {
            ChatChannelSheet(
                hub: hub, conversation: hub.currentConversation,
                onInsert: { (text: String) in
                    draft = (draft.isEmpty || draft.hasSuffix(" ") ? draft : draft + " ") + text
                    composerFocused = true
                },
                onRoute: onRoute)
        }
        .alert(
            broadcast?.title ?? "", isPresented: Binding(get: { broadcast != nil }, set: { if !$0 { broadcast = nil } }),
            presenting: broadcast
        ) { (_: ChatBroadcastWarning) in
            Button("Send") {
                broadcast = nil
                send(confirmed: true)
            }
            Button("Cancel", role: .cancel) { broadcast = nil }
        } message: { (warning: ChatBroadcastWarning) in
            Text(warning.message)
        }
        .alert("Edit message", isPresented: Binding(get: { editing != nil }, set: { if !$0 { editing = nil } })) {
            TextField("Message", text: $editText)
            Button("Cancel", role: .cancel) { editing = nil }
            Button("Save") {
                if let message = editing { hub.edit(message, text: editText) }
                editing = nil
            }
        }
    }

    private var roomLayout: some View {
        VStack(spacing: 0) {
            if showsHeader {
                header
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            ZStack(alignment: .leading) {
                roomColumn
                    .shellDock { composer }
                drawer
            }
        }
        .background { MessagesBackdrop(look: hub.conversationLook, canvas: fm.canvas).ignoresSafeArea() }
        .environment(\.chatMentionNames, hub.ownNames)
    }

    private var roomColumn: some View {
        VStack(spacing: 0) {
            if hub.inMesh && meshDebugShown {
                MeshDebugBar(mesh: hub.mesh)
            }
            if let topic = hub.office?.room(for: hub.currentConversation)?.topic, !topic.isEmpty {
                Text(topic)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(2)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x1)
            }
            if !hub.inMesh && hub.totalUnread > 0 {
                UnreadBanner(count: hub.totalUnread, onTap: { hub.markAllRead() })
            }
            messages
            TimelineView(.periodic(from: Date(), by: 1)) { (_: TimelineViewDefaultContext) in
                let line: String = hub.typingLine
                if !line.isEmpty {
                    Text(line)
                        .fmText(.labelSmall, color: fm.textTertiary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.vertical, 2)
                        .accessibilityAddTraits(.updatesFrequently)
                }
            }
            noticeLine
        }
    }

    @ViewBuilder private var noticeLine: some View {
        if notice != nil {
            FmNoticeLine($notice)
                .padding(.horizontal, FmSpace.x4)
        }
    }

    private var header: some View {
        HStack(spacing: FmSpace.x2) {
            if let onOpenChannels = onOpenChannels {
                Button(action: onOpenChannels) {
                    ZStack(alignment: .topTrailing) {
                        Image(systemName: "chevron.left")
                            .font(.system(size: 19, weight: .semibold))
                            .foregroundStyle(fm.textPrimary)
                            .frame(width: 48, height: 48)
                        if hub.otherRoomsUnread {
                            Circle().fill(fm.danger).frame(width: 9, height: 9).padding(8)
                        }
                    }
                    .chatGlass(Circle(), border: fm.border)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Channels")
            }
            roomPill
            if hub.inMesh {
                Button(action: { meshDebugShown.toggle() }) {
                    Image(systemName: "waveform.path.ecg")
                        .font(.system(size: 18, weight: .semibold))
                        .foregroundStyle(meshDebugShown ? fm.accent : fm.textPrimary)
                        .frame(width: 48, height: 48)
                        .chatGlass(Circle(), border: meshDebugShown ? fm.accent : fm.border)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(meshDebugShown ? "Hide mesh diagnostics" : "Show mesh diagnostics")
                .accessibilityIdentifier("chat-mesh-debug-toggle")
            }
            Button(action: onCompose) {
                Image(systemName: "square.and.pencil")
                    .font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(fm.textPrimary)
                    .frame(width: 48, height: 48)
                    .chatGlass(Circle(), border: fm.border)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("New message")
            if let onNewAgent = shell.onNewAgent {
                ShellNewAgentButton(onNewAgent: onNewAgent, onPalette: shell.onPalette)
            }
            if shell.onHub != nil {
                shellCapsule
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
    }

    private var roomPill: some View {
        Button(action: {
            if hub.isChannel(hub.currentConversation) {
                ShellKeyboard.dismiss()
                channelInfo = true
            } else if !hub.inMesh {
                sealInfo = true
            }
        }) {
            HStack(spacing: FmSpace.x2) {
                FmIcon(hub.titleIcon, size: 20, color: fm.textPrimary)
                VStack(alignment: .leading, spacing: 0) {
                    Text(hub.roomName)
                        .font(.system(size: 16, weight: .bold))
                        .foregroundStyle(fm.textPrimary)
                        .lineLimit(1)
                    Text(hub.headerDetail)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .lineLimit(1)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, FmSpace.x4)
            .frame(maxWidth: .infinity, minHeight: 48)
            .contentShape(Capsule())
            .chatGlass(Capsule(), border: fm.border)
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityHint(
            hub.isChannel(hub.currentConversation)
                ? "Shows the channel's members, files, bookmarks and settings"
                : (hub.inMesh ? "" : "Explains how this conversation is encrypted"))
        .accessibilityIdentifier("chat-room-header")
    }

    private var shellCapsule: some View {
        HStack(spacing: FmSpace.x1) {
            if let onHub = shell.onHub {
                Button(action: onHub) {
                    FmAvatar(name: shell.name, avatarPng: shell.avatarPng, size: 30, fontSize: FmType.meta)
                        .frame(width: 40, height: 44)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("You: projects, tools and settings")
            }
        }
        .padding(.horizontal, FmSpace.x2)
        .frame(minHeight: 48)
        .chatGlass(Capsule(), border: fm.border)
    }

    private var describeAction: (() -> Void)? {
        let topic: String = hub.office?.room(for: hub.currentConversation)?.topic ?? ""
        guard hub.canSetTopic, topic.isEmpty else { return nil }
        return {
            draft = "/topic "
            composerFocused = true
        }
    }

    private var addPeopleAction: (() -> Void)? {
        let conversation: String = hub.currentConversation
        if let room = hub.office?.room(for: conversation) {
            guard room.canManage, room.isPrivate else { return nil }
            return { managing = room }
        }
        guard hub.extras.isPrivate(conversation) else { return nil }
        return { inviting = true }
    }

    @ViewBuilder private var messages: some View {
        let rows: [ChatRow] = hub.rows
        let newStart: String = hub.unreadStart[hub.currentConversation] ?? ""
        if rows.isEmpty && !hub.inMesh {
            ChatRoomIntro(hub: hub, onDescribe: describeAction, onAddPeople: addPeopleAction)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomLeading)
                .contentShape(Rectangle())
                .onTapGesture { composerFocused = false }
        } else if rows.isEmpty {
            FmEmptyState(
                icon: hub.inMesh ? "lan_outlined" : "chat_bubble_outline", title: hub.emptyTitle,
                message: hub.emptyMessage)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .contentShape(Rectangle())
                .onTapGesture { composerFocused = false }
        } else {
            ScrollViewReader { (proxy: ScrollViewProxy) in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(rows.enumerated()), id: \.element.id) { (entry: (offset: Int, element: ChatRow)) in
                            let row: ChatRow = entry.element
                            if ChatHub.startsNewDay(row.message, after: entry.offset > 0 ? rows[entry.offset - 1].message : nil) {
                                ChatDayDivider(date: row.message.timestamp)
                            }
                            if row.id == newStart {
                                ChatNewDivider()
                            }
                            ChatMessageRow(
                                row: row, liveStatus: hub.liveStatus(for: row.message),
                                actions: actions(for: row))
                                .id(row.id)
                        }
                    }
                    .padding(.horizontal, FmSpace.x2)
                    .padding(.vertical, FmSpace.x3)
                }
                .defaultScrollAnchor(.bottom)
                .scrollDismissesKeyboard(.interactively)
                .scrollContentBackground(.hidden)
                .onTapGesture { composerFocused = false }
                .onAppear { scrollToEnd(proxy, rows, animated: false) }
                .onChange(of: rows.last?.id ?? "") { _, _ in scrollToEnd(proxy, rows, animated: true) }
                .chatKeepsEndInView(focused: composerFocused) {
                    if let last: ChatRow = rows.last {
                        withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(last.id, anchor: .bottom) }
                    }
                }
                .onTabReselect(.chat) {
                    if let last: ChatRow = rows.last {
                        withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(last.id, anchor: .bottom) }
                    }
                }
            }
        }
    }

    private func actions(for row: ChatRow) -> ChatRowActions {
        let message: ChatMessage = row.message
        let relayRoom: Bool = !hub.inMesh && !hub.isReadOnly(message.conversation)
        let channel: String = message.conversation
        var react: ((String) -> Void)? = nil
        var thread: (() -> Void)? = nil
        var edit: (() -> Void)? = nil
        var delete: (() -> Void)? = nil
        var profile: (() -> Void)? = nil
        let ephemeral: Bool = message.app?.ephemeral == true
        if relayRoom && !ephemeral {
            react = { (value: String) in hub.toggleReaction(message, value: value) }
            if ChatHub.hasThreads(channel) {
                thread = { onRoute(.thread(channel: channel, id: message.id)) }
            }
        }
        if row.editable {
            edit = {
                editText = row.text
                editing = message
            }
        }
        if row.deletable {
            delete = { Task { await hub.delete(message) } }
        }
        if !message.isSelf && !row.visitor && message.senderId != "forkbot" && !message.senderId.hasPrefix("app:") {
            profile = { onRoute(.profile(name: message.senderName)) }
        }
        let issue: () -> Void = {
            let draft: ChatIssueDraft = ChatIssueDraft.fromMessage(
                message, text: row.text, repo: RepoRef(owner: "forkmesh", name: "forkmesh"), channel: channel, now: Date())
            onIssue(draft)
        }
        let prompt: () -> Void = {
            let target: ComposeTarget = ComposeTarget(channel: channel, text: row.text)
            if let compose = NotificationActions.onCompose {
                compose(target)
            } else {
                onRoute(.compose(target))
            }
        }
        var appAction: ((String, String) -> Void)? = nil
        if message.app != nil {
            appAction = { (actionId: String, value: String) in
                Task { await hub.interact(message, actionId: actionId, value: value) }
            }
        }
        let text: String = row.text
        let ephemeralNote: Bool = message.app?.ephemeral == true
        return ChatRowActions(
            onCopied: onCopied, onReact: react, onThread: thread, onEdit: edit, onDelete: delete, onIssue: issue,
            onPrompt: prompt, onProfile: profile, onHide: { hub.hide(message) }, onAppAction: appAction,
            appBusy: { (actionId: String) in hub.isAppBusy(message, actionId: actionId) },
            saved: hub.workspace.isSaved(message.id),
            onSave: ephemeralNote ? nil : { hub.toggleSaved(message, text: text) },
            onShare: ephemeralNote ? nil : { sharing = ChatShareRequest(message: message, text: text) },
            onRemind: ephemeralNote ? nil : { (option: ChatReminderOption) in
                let place: String = hub.conversationLabel(message.conversation)
                Task { onNotice(await ChatReminders.schedule(option, message: message, text: text, place: place)) }
            })
    }

    private func loadDraft(_ conversation: String) {
        draftRoom = conversation
        draft = hub.workspace.draft(for: conversation)
    }

    private func scrollToEnd(_ proxy: ScrollViewProxy, _ rows: [ChatRow], animated: Bool) {
        guard let last = rows.last else { return }
        if last.id == lastSeenId { return }
        lastSeenId = last.id
        if animated {
            withAnimation(.easeOut(duration: 0.2)) { proxy.scrollTo(last.id, anchor: .bottom) }
        } else {
            proxy.scrollTo(last.id, anchor: .bottom)
        }
    }

    @ViewBuilder private var composer: some View {
        Group {
            if let blocked = hub.composeBlocked {
                GuestComposeNotice(action: blocked, onLogIn: {
                    if let signIn = Services.get(SignInCoordinator.self) {
                        Task { _ = await signIn.requireAccount(action: blocked) }
                    }
                })
            } else if hub.isReadOnly(hub.relay.currentConversation) {
                Text(verbatim: "Only ForkMesh posts in this channel.")
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("Only ForkMesh posts in this channel.", alignment: .center)
                    .padding(.horizontal, FmSpace.x4)
                    .frame(maxWidth: .infinity, minHeight: 48, alignment: .leading)
                    .chatGlass(RoundedRectangle(cornerRadius: 24, style: .continuous), border: fm.border)
            } else {
                ChatComposerBar(
                    hub: hub,
                    placeholder: hub.isChannel(hub.currentConversation) && !hub.isPrivateRoom
                        ? "Message #\(hub.roomName)" : "Message \(hub.roomName)",
                    allowAttachments: !hub.inMesh && !hub.inOffice,
                    draft: $draft, focused: $composerFocused, onSend: { send() }, glass: true)
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
    }

    private func send(confirmed: Bool = false) {
        let text: String = draft
        if !confirmed, let warning = hub.broadcastWarning(text, in: hub.currentConversation) {
            broadcast = warning
            return
        }
        Task {
            if await hub.sendChatText(text) {
                draft = ""
            }
        }
    }
}

struct ChatShellActions {
    var onHub: (() -> Void)?
    var onNewAgent: (() -> Void)?
    var onPalette: () -> Void = {}
    var name: String = ""
    var avatarPng: String = ""
}

private struct ChatShellActionsKey: EnvironmentKey {
    static let defaultValue: ChatShellActions = ChatShellActions()
}

extension EnvironmentValues {
    var chatShellActions: ChatShellActions {
        get { self[ChatShellActionsKey.self] }
        set { self[ChatShellActionsKey.self] = newValue }
    }
}

private struct ChatRoomIntro: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let onDescribe: (() -> Void)?
    let onAddPeople: (() -> Void)?

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            HStack(spacing: FmSpace.x1) {
                FmIcon(hub.titleIcon, size: 28, color: fm.textPrimary)
                Text(hub.roomName)
                    .font(.system(size: 30, weight: .bold))
                    .foregroundStyle(fm.textPrimary)
                    .lineLimit(2)
            }
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isHeader)
            Text(hub.introLine)
                .fmText(.bodyLarge, color: fm.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            if onDescribe != nil || onAddPeople != nil {
                HStack(spacing: FmSpace.x2) {
                    if let onDescribe = onDescribe {
                        card(symbol: "pencil", title: "Add description", action: onDescribe)
                    }
                    if let onAddPeople = onAddPeople {
                        card(symbol: "person.badge.plus", title: "Add people", action: onAddPeople)
                    }
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.bottom, FmSpace.x4)
    }

    private func card(symbol: String, title: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: FmSpace.x2) {
                Image(systemName: symbol)
                    .font(.system(size: 20, weight: .regular))
                    .foregroundStyle(fm.textPrimary)
                Text(title)
                    .fmText(.titleSmall, color: fm.textPrimary)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, FmSpace.x4)
            .background(
                RoundedRectangle(cornerRadius: FmRadius.lg * 1.5, style: .continuous)
                    .strokeBorder(fm.border, lineWidth: 1))
            .contentShape(RoundedRectangle(cornerRadius: FmRadius.lg * 1.5, style: .continuous))
        }
        .buttonStyle(.plain)
    }
}

extension View {
    @ViewBuilder func chatGlass<S: Shape>(_ shape: S, border: Color) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            self.contentShape(shape).glassEffect(.regular.interactive(), in: shape)
        } else {
            self.contentShape(shape).background(shape.fill(.ultraThinMaterial)).overlay(shape.stroke(border.opacity(0.6), lineWidth: 0.5))
        }
        #else
        self.contentShape(shape).background(shape.fill(.ultraThinMaterial)).overlay(shape.stroke(border.opacity(0.6), lineWidth: 0.5))
        #endif
    }
}

struct ChatRowActions {
    let onCopied: () -> Void
    let onReact: ((String) -> Void)?
    let onThread: (() -> Void)?
    let onEdit: (() -> Void)?
    let onDelete: (() -> Void)?
    let onIssue: (() -> Void)?
    let onPrompt: (() -> Void)?
    let onProfile: (() -> Void)?
    var onHide: (() -> Void)? = nil
    var onAppAction: ((String, String) -> Void)? = nil
    var appBusy: (String) -> Bool = { _ in false }
    var saved: Bool = false
    var onSave: (() -> Void)? = nil
    var onShare: (() -> Void)? = nil
    var onRemind: ((ChatReminderOption) -> Void)? = nil
}

struct ChatDayDivider: View {
    @Environment(\.fmPalette) private var fm
    let date: Date

    var body: some View {
        let label: String = ChatHub.dayDividerLabel(date, now: Date())
        HStack(spacing: FmSpace.x2) {
            fm.border.frame(height: 1)
            Text(label)
                .fmText(.labelSmall, color: fm.textSecondary)
                .lineLimit(1)
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, 3)
                .background(Capsule().fill(fm.bgBase))
                .overlay(Capsule().stroke(fm.border, lineWidth: 1))
            fm.border.frame(height: 1)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.top, FmSpace.x4)
        .padding(.bottom, FmSpace.x1)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(label)
        .accessibilityAddTraits(.isHeader)
    }
}

struct ChatNewDivider: View {
    @Environment(\.fmPalette) private var fm

    var body: some View {
        VStack(alignment: .trailing, spacing: 3) {
            Text("NEW")
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(fm.danger)
                .padding(.trailing, FmSpace.x1)
            fm.danger.frame(height: 1)
        }
        .padding(.leading, FmSpace.x2)
        .padding(.top, FmSpace.x3)
        .padding(.bottom, FmSpace.x1)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("New messages")
    }
}

private struct UnreadBanner: View {
    @Environment(\.fmPalette) private var fm
    let count: Int
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: FmSpace.x2) {
                FmIcon("mark_chat_unread_outlined", size: 16, color: fm.accent)
                Text(ChatHub.unreadLabel(count))
                    .fmText(.labelMedium, color: fm.accent)
                    .frame(maxWidth: .infinity, alignment: .leading)
                FmIcon("keyboard_arrow_up_rounded", size: 22, color: fm.accent, label: "Mark as read")
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
            .contentShape(Capsule())
            .shellGlass(Capsule())
        }
        .buttonStyle(.plain)
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x1)
    }
}

/// The mesh transports' counters in one tiny line under the #mesh header, refreshed every second.
/// Tap for the per-device breakdown; the copy button puts all of it on the clipboard.
private struct MeshDebugBar: View {
    @Environment(\.fmPalette) private var fm
    let mesh: LocalMeshService
    @State private var expanded: Bool = false

    var body: some View {
        TimelineView(.periodic(from: Date(), by: 1)) { (_: TimelineViewDefaultContext) in
            HStack(alignment: .top, spacing: 0) {
                Text(expanded ? mesh.diagnosticsDetail() : mesh.diagnosticsLine())
                    .fmMono(size: FmType.micro, color: fm.textTertiary)
                    .lineLimit(expanded ? 16 : 2)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .contentShape(Rectangle())
                    .onTapGesture { expanded.toggle() }
                Button(action: copy) {
                    FmIcon("copy", size: 14, color: fm.textTertiary)
                        .frame(width: 28, height: 28)
                        .shellGlass(Circle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Copy mesh diagnostics")
            }
            .padding(.leading, FmSpace.x3)
            .padding(.vertical, FmSpace.x1)
            .padding(.trailing, FmSpace.x1)
        }
    }

    private func copy() {
        #if canImport(UIKit)
        UIPasteboard.general.string = mesh.diagnosticsDetail()
        #endif
    }
}

// MARK: - One message

struct ChatMessageRow: View {
    @Environment(\.fmPalette) private var fm
    let row: ChatRow
    let liveStatus: UserStatus?
    let actions: ChatRowActions
    @State private var confirmDelete: Bool = false
    @State private var selecting: Bool = false

    private var message: ChatMessage { row.message }
    private var copyText: String { row.text.isEmpty ? message.fileName : row.text }

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            if row.continuesGroup {
                Color.clear.frame(width: ChatSenderAvatar.roomSize, height: 1)
            } else {
                ChatSenderAvatar(message: message, size: ChatSenderAvatar.roomSize, corner: 9)
                    .padding(.top, 2)
            }
            VStack(alignment: .leading, spacing: 0) {
                if actions.saved {
                    Label("Saved for later", systemImage: "bookmark.fill")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(fm.accent)
                        .padding(.bottom, 2)
                }
                if !row.continuesGroup {
                    nameLine.padding(.bottom, 2)
                }
                if !row.text.isEmpty {
                    ChatMessageBody(text: row.text)
                    ChatLinkPreviews(text: row.text)
                }
                if row.edited {
                    Text("(edited)").fmText(.labelSmall, color: fm.textTertiary)
                }
                if let app = message.app, !app.blocks.isEmpty {
                    ChatAppBlocksView(
                        app: app, busy: actions.appBusy,
                        onAction: { (actionId: String, value: String) in actions.onAppAction?(actionId, value) })
                }
                if !message.fileName.isEmpty {
                    ChatAttachmentView(fileName: message.fileName, attachment: row.attachment)
                        .padding(.top, FmSpace.x1)
                }
                if row.queued {
                    Text("Queued · goes to the next device that joins")
                        .fmText(.bodySmall, color: fm.warning)
                        .fmMessageTools("Queued · goes to the next device that joins")
                        .padding(.top, 2)
                }
                if let onReact = actions.onReact {
                    ChatReactionBar(chips: row.reactions, onToggle: onReact)
                }
                if let summary = row.thread, let onThread = actions.onThread {
                    ChatThreadSummaryButton(summary: summary, onOpen: onThread)
                        .padding(.top, FmSpace.x1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .overlay(alignment: .topTrailing) {
                if row.continuesGroup, let receipt = row.receipt {
                    ChatReceiptMark(receipt: receipt, muted: fm.textTertiary, background: fm.bgBase)
                        .padding(.top, 3)
                }
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.top, row.continuesGroup ? 1 : FmSpace.x3)
        .padding(.bottom, row.mentionsYou ? FmSpace.x1 : 1)
        .background(alignment: .leading) {
            if row.mentionsYou {
                ZStack(alignment: .leading) {
                    Color(fmARGB: fm.dark ? 0x1FF2C744 : 0x2EF2C744)
                    Color(fmARGB: 0xFFE8B730).frame(width: 3)
                }
                .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
                .accessibilityHidden(true)
            }
        }
        .contentShape(Rectangle())
        .contextMenu { menu }
        .sheet(isPresented: $selecting) { FmSelectableTextSheet(text: copyText) }
        .confirmationDialog("Delete message?", isPresented: $confirmDelete, titleVisibility: .visible) {
            if let onDelete = actions.onDelete {
                Button("Delete for everyone", role: .destructive, action: onDelete)
            }
            if let onHide = actions.onHide {
                Button("Delete for me", role: .destructive, action: onHide)
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(deleteNote)
        }
    }

    private var deleteNote: String {
        if actions.onDelete == nil { return "It is removed from this device only. Others still see it." }
        if actions.onHide == nil { return "This removes it for anyone whose device syncs this room." }
        return "Delete for everyone removes it for anyone whose device syncs this room. Delete for me removes it from this device only."
    }

    @ViewBuilder private var menu: some View {
        if let onReact = actions.onReact {
            ControlGroup {
                ForEach(ReactionCatalog.quick, id: \.self) { (value: String) in
                    Button(ReactionCatalog.emoji(for: value)) { onReact(value) }
                }
            }
            ChatReactionMenu(onPick: onReact)
        }
        if let onThread = actions.onThread {
            Button(action: onThread) { Label("Reply", systemImage: "arrowshape.turn.up.left") }
        }
        ChatMessageLaterMenu(actions: actions)
        if !copyText.isEmpty {
            Button(action: copy) { Label("Copy text", systemImage: "doc.on.doc") }
            Button(action: { selecting = true }) { Label("Select text", systemImage: "selection.pin.in.out") }
        }
        if let onEdit = actions.onEdit {
            Button(action: onEdit) { Label("Edit message", systemImage: "pencil") }
        }
        if !row.text.isEmpty, let onPrompt = actions.onPrompt {
            Button(action: onPrompt) { Label("Send to prompt", systemImage: "text.bubble") }
        }
        if !row.text.isEmpty, let onIssue = actions.onIssue {
            Button(action: onIssue) { Label("Create issue", systemImage: "exclamationmark.circle") }
        }
        if let onProfile = actions.onProfile {
            Button(action: onProfile) { Label("View \(message.senderName)", systemImage: "person.crop.circle") }
        }
        if actions.onDelete != nil || actions.onHide != nil {
            Button(role: .destructive, action: { confirmDelete = true }) {
                Label("Delete message", systemImage: "trash")
            }
        }
    }

    private func copy() {
        #if canImport(UIKit)
        UIPasteboard.general.string = copyText
        #endif
        actions.onCopied()
    }

    private var nameLine: some View {
        HStack(spacing: 0) {
            Text(ChatHub.senderLabel(message))
                .font(.system(size: 17, weight: .bold))
                .foregroundStyle(fm.textPrimary)
                .lineLimit(1)
            if row.visitor {
                Text(" visitor").fmText(.labelSmall, color: fm.textTertiary)
            }
            if message.app?.ephemeral == true {
                Text(" \(AppsChat.ephemeralNote)").fmText(.labelSmall, color: fm.textTertiary).lineLimit(1)
            }
            ChatSenderStatus(message: message, live: liveStatus)
            Text(ChatHub.timeLabel(message.timestamp))
                .font(.system(size: 13).monospacedDigit())
                .foregroundStyle(fm.textTertiary)
                .padding(.leading, FmSpace.x2)
            if let receipt = row.receipt {
                ChatReceiptMark(receipt: receipt, muted: fm.textTertiary, background: fm.bgBase)
                    .padding(.leading, FmSpace.x1)
            }
            if let label = OfficeFrames.recipientsLabel(message.recipients),
                let description = OfficeFrames.recipientsDescription(message.recipients)
            {
                Text(label)
                    .fmText(.bodySmall, color: fm.textTertiary, tabular: true)
                    .padding(.leading, FmSpace.x1)
                    .help(description)
                    .accessibilityLabel(description)
            }
        }
    }
}

/// Message text, with ``` fenced runs kept in a monospace face that scrolls instead of wrapping.
struct ChatMessageBody: View {
    @Environment(\.fmPalette) private var fm
    let text: String

    var body: some View {
        let parts: [ChatBodySegment] = ChatHub.segments(text)
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            ForEach(Array(parts.enumerated()), id: \.offset) { (entry: (offset: Int, element: ChatBodySegment)) in
                segment(entry.element)
            }
        }
    }

    @ViewBuilder private func segment(_ part: ChatBodySegment) -> some View {
        if part.code {
            ScrollView(.horizontal, showsIndicators: false) {
                Text(part.text)
                    .fmMono(height: FmType.leadingSnug)
                    .fixedSize(horizontal: true, vertical: false)
            }
            .padding(FmSpace.x2)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(fm.bgOverlay)
            .clipShape(FmRoundedShape(radius: FmRadius.md))
        } else {
            ForEach(Array(ChatRichText.blocks(part.text).enumerated()), id: \.offset) { (entry: (offset: Int, element: ChatTextBlock)) in
                if entry.element.quote {
                    HStack(alignment: .top, spacing: FmSpace.x2) {
                        RoundedRectangle(cornerRadius: 1.5).fill(fm.border).frame(width: 3)
                        ChatLinkText(text: entry.element.text)
                            .foregroundStyle(fm.textSecondary)
                    }
                    .fixedSize(horizontal: false, vertical: true)
                } else {
                    ChatLinkText(text: entry.element.text)
                }
            }
        }
    }
}

/// The sender's account avatar over a letter circle (the circle stays while it loads and for
/// accounts without one).
struct ChatSenderAvatar: View {
    static let roomSize: CGFloat = 38
    let message: ChatMessage
    var size: CGFloat = 28
    var corner: CGFloat? = nil
    @State private var pictures: MessagesPeoplePictures = MessagesPeoplePictures.shared
    @State private var avatars: ChatAvatarCache = ChatAvatarCache.shared

    var body: some View {
        let custom: String = message.isSelf ? "" : pictures.picture(for: message.senderName)
        FmAvatar(
            name: message.senderName, avatarPng: custom.isEmpty ? known : custom, size: size,
            fontSize: corner == nil ? FmType.meta : FmType.heading, corner: corner)
            .task(id: message.senderName) {
                if message.isSelf && !own.isEmpty { return }
                await avatars.refresh(message.senderName)
            }
    }

    private var own: String {
        Services.get(AuthService.self)?.session?.avatarPng ?? ""
    }

    private var known: String {
        if message.isSelf && !own.isEmpty { return own }
        return avatars.png(for: message.senderName)
    }
}

@MainActor
@Observable
final class ChatAvatarCache {
    static let shared: ChatAvatarCache = ChatAvatarCache(file: ChatAvatarCache.defaultFile)
    nonisolated static let maxPeople: Int = 200

    private var pngs: [String: String] = [:]
    @ObservationIgnored private var order: [String] = []
    @ObservationIgnored private var touched: Set<String> = []
    @ObservationIgnored private let file: URL?
    @ObservationIgnored private var warmed: Bool = false
    @ObservationIgnored private var saveQueued: Bool = false

    init(file: URL?) {
        self.file = file
    }

    nonisolated static var defaultFile: URL? {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first?
            .appendingPathComponent("chat-avatars.json")
    }

    nonisolated static func key(_ name: String) -> String {
        name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    func png(for name: String) -> String {
        pngs[ChatAvatarCache.key(name)] ?? ""
    }

    func refresh(_ name: String) async {
        let trimmed: String = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, let api = Services.get(ApiService.self) else { return }
        guard let png = await api.accountAvatarPngIfReached(trimmed) else { return }
        store(png, for: trimmed)
    }

    func store(_ png: String, for name: String) {
        let key: String = ChatAvatarCache.key(name)
        if key.isEmpty { return }
        touched.insert(key)
        if (pngs[key] ?? "") == png { return }
        order.removeAll { $0 == key }
        if png.isEmpty {
            pngs[key] = nil
        } else {
            pngs[key] = png
            order.append(key)
        }
        while order.count > ChatAvatarCache.maxPeople {
            pngs[order.removeFirst()] = nil
        }
        scheduleSave()
    }

    func warm() {
        if warmed { return }
        warmed = true
        guard let file else { return }
        Task {
            let saved: [[String]] = await Task.detached(priority: .userInitiated) { ChatAvatarCache.read(file) }.value
            var merged: [String: String] = pngs
            var keys: [String] = []
            for pair in saved where !touched.contains(pair[0]) && merged[pair[0]] == nil {
                merged[pair[0]] = pair[1]
                keys.append(pair[0])
            }
            if keys.isEmpty { return }
            order = keys + order
            pngs = merged
        }
    }

    private func scheduleSave() {
        if saveQueued { return }
        saveQueued = true
        Task {
            try? await Task.sleep(for: .seconds(1))
            saveQueued = false
            guard let file else { return }
            let snapshot: [[String]] = order.compactMap { key in pngs[key].map { [key, $0] } }
            await Task.detached(priority: .utility) { ChatAvatarCache.write(snapshot, to: file) }.value
        }
    }

    nonisolated static func read(_ file: URL) -> [[String]] {
        guard let data = try? Data(contentsOf: file),
            let pairs = try? JSONDecoder().decode([[String]].self, from: data)
        else { return [] }
        return pairs.filter { $0.count == 2 && !$0[0].isEmpty && !$0[1].isEmpty }
    }

    nonisolated static func write(_ pairs: [[String]], to file: URL) {
        guard let data = try? JSONEncoder().encode(pairs) else { return }
        do {
            try FileManager.default.createDirectory(
                at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: file, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        } catch {
            Log.warning("chat", "could not save chat pictures: \(error)")
        }
    }
}

/// The sender's status emoji: the live relay value when their node is here, else the account's
/// public profile.
struct ChatSenderStatus: View {
    let message: ChatMessage
    let live: UserStatus?
    @State private var fetched: UserStatus = UserStatus.empty

    var body: some View {
        UserStatusEmoji(status: live ?? fetched)
            .task(id: message.senderName) {
                guard live == nil, let api = Services.get(ApiService.self) else { return }
                let name: String = message.senderName.trimmingCharacters(in: .whitespaces)
                if name.isEmpty { return }
                fetched = await api.accountUserStatus(name)
            }
    }
}

private struct ChatKeepsEndInView: ViewModifier {
    let focused: Bool
    let scroll: () -> Void
    @State private var height: CGFloat = 0

    func body(content: Content) -> some View {
        content
            .onChange(of: focused) { _, on in
                if on { scroll() }
            }
            .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { (new: CGFloat) in
                let old = height
                height = new
                if focused && new < old { scroll() }
            }
    }
}

extension View {
    func chatKeepsEndInView(focused: Bool, scroll: @escaping () -> Void) -> some View {
        modifier(ChatKeepsEndInView(focused: focused, scroll: scroll))
    }
}
