// The logic half of the chat screen (and `sendChatText`, its composer's send): which room
// is open, what it shows, who is in it, what the composer may do, and the guest rules. The
// SwiftUI screen (Features/Chat/ChatScreen.swift) lays it out.
//
// Also the shell's `ShellChatBridge` (FMApi): relay state and unread counts for the top bar.

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

/// Guests may post only in the public `#general` channel and the local mesh room.
public let guestChatChannelName = "general"

public typealias ChatExpiry = @MainActor (_ key: String, _ timestamp: Date) -> Bool

/// One transcript row, with what it derives from its neighbours.
public struct ChatRow: Equatable, Identifiable, Sendable {
    public let message: ChatMessage
    /// Same sender as the message above, within five minutes: no avatar, name or time.
    public let continuesGroup: Bool
    /// A #mesh message of this device's that no device has taken yet.
    public let queued: Bool
    /// Only this device's own messages in a relay room can be taken back.
    public let deletable: Bool
    public var text: String = ""
    public var edited = false
    public var editable = false
    public var reactions: [ReactionChip] = []
    public var thread: ChatThreadSummary?
    public var attachment: ChatAttachment?
    public var visitor = false
    public var receipt: ChatReceipt?
    public var mentionsYou = false

    public init(message: ChatMessage, continuesGroup: Bool, queued: Bool, deletable: Bool) {
        self.message = message
        self.continuesGroup = continuesGroup
        self.queued = queued
        self.deletable = deletable
        text = message.text
    }

    public var id: String { message.id }
}

/// Preformatted runs of a message body (``` fences), for a monospace face.
public struct ChatBodySegment: Equatable, Sendable {
    public let code: Bool
    public let text: String
}

@MainActor
@Observable
public final class ChatHub: ShellChatBridge {
    nonisolated static let groupWindow: TimeInterval = 5 * 60

    @ObservationIgnored public let relay: RelayService
    @ObservationIgnored public let mesh: LocalMeshService
    @ObservationIgnored let isGuest: @MainActor () -> Bool
    @ObservationIgnored let clock: Clock
    @ObservationIgnored private var conversationChosen = false

    /// Called with the "Sign in to ..." phrase when a guest tries to post where they cannot; the
    /// view shows the guest sheet.
    @ObservationIgnored public var onGuestBlocked: (@MainActor (String) -> Void)?
    public let extras: ChatExtras
    public var workspace: ChatWorkspace
    public var pendingSearch: String?
    public var pendingSaved = false
    public private(set) var revealRequests = 0
    public var office: OfficeChannelService? {
        didSet {
            office?.isExpired = expiry
            office?.isRemoved = { [weak self] id in self?.extras.isRemoved(id) ?? false }
            office?.onRemoved = { [weak self] conversation, id in self?.messageRemoved(conversation, id) }
            office?.onFrame = { [weak self] frame, conversation in
                guard let self, let office = self.office else { return }
                self.extras.applyRoomFrame(frame, conversation: conversation, selfId: office.currentSelfId)
            }
            office?.onMessage = { [weak self] message in
                guard let self else { return }
                self.filePing(message)
                if self.relay.currentConversation == message.conversation {
                    self.queueReadMark(message.conversation)
                } else if !self.isReadElsewhere(message) {
                    self.relay.noteUnread(message.conversation)
                }
            }
        }
    }
    public var direct: DirectMessageService? {
        didSet {
            direct?.isExpired = expiry
            direct?.isRemoved = { [weak self] id in self?.extras.isRemoved(id) ?? false }
            direct?.onRemoved = { [weak self] conversation, id in self?.messageRemoved(conversation, id) }
            direct?.onChange = { [weak self] in self?.persistSoon() }
            direct?.onTrustEvent = { [weak self] event in self?.trustEvent(event) }
            direct?.attachMesh(mesh)
            restoreDirectHistory()
            direct?.onFrame = { [weak self] frame, conversation in
                guard let self, let direct = self.direct else { return }
                if frame["type"]?.text == DirectMessageFrames.typingType {
                    let other = direct.record(for: conversation)?.otherUser ?? ""
                    let sender = (frame["sender"]?.text ?? "").lowercased()
                    if other.isEmpty || sender != other || self.workspace.isBlocked(other) { return }
                }
                self.extras.applyRoomFrame(frame, conversation: conversation, selfId: direct.currentSelfId)
            }
            direct?.onMessage = { [weak self] message in
                guard let self, let direct = self.direct else { return }
                self.filePing(message)
                if self.relay.currentConversation == message.conversation {
                    Task { await direct.markRead(message.conversation) }
                    self.queueReadMark(message.conversation)
                } else if !self.isReadElsewhere(message) {
                    direct.bumpUnread(message.conversation)
                }
            }
        }
    }
    @ObservationIgnored public var readMarks: ChatReadMarks?
    @ObservationIgnored public var onMessageRemoved: (@MainActor (String, String) -> Void)?
    @ObservationIgnored public var calls: DirectCallEngine?
    @ObservationIgnored public var live: DirectLiveEngine?
    @ObservationIgnored public var pings: LocalPingLog? {
        didSet { purgePrivatePings() }
    }
    public private(set) var retention: ChatRetention = .standard
    @ObservationIgnored private var pruneTask: Task<Void, Never>?
    @ObservationIgnored public var forkbot: ChatAccountCalling?
    public internal(set) var directoryMembers: [String] = []
    @ObservationIgnored var directoryLoadedAt: Date?
    @ObservationIgnored var directoryLoading = false
    @ObservationIgnored public var apps: AppsClient?
    public private(set) var appCommands = AppCommandCatalog()
    public private(set) var unreadStart: [String: String] = [:]
    public private(set) var appBusy: Set<String> = []
    @ObservationIgnored private var appCommandsLoaded = false
    @ObservationIgnored private var ephemeralSequence = 0
    @ObservationIgnored private var directTypingSent: [String: Date] = [:]
    private var ephemerals: [String: [ChatMessage]] = [:]
    @ObservationIgnored public var history: ChatHistoryStore?
    @ObservationIgnored public var historyScope: @MainActor () -> String = { "" }
    @ObservationIgnored private var loadedScope = ""
    @ObservationIgnored private var historyToken: ListenerToken?
    @ObservationIgnored private var directRestore: (messages: [ChatMessage], receipts: [String: ChatReceipt])?
    @ObservationIgnored var trustChangedAt: [String: Date] = [:]
    public private(set) var replyingTo: ChatReplyQuote?
    @ObservationIgnored private var replyConversation = ""
    /// The last transient notice ("Chat is offline. Reconnecting..."), shown inline above the composer.
    public private(set) var notice: ChatNotice?
    public var pendingIssue: String?

    public init(
        relay: RelayService, mesh: LocalMeshService, isGuest: @escaping @MainActor () -> Bool = { false },
        clock: Clock = SystemClock()
    ) {
        self.relay = relay
        self.mesh = mesh
        self.isGuest = isGuest
        self.clock = clock
        let extras = ChatExtras(clock: clock)
        self.extras = extras
        self.workspace = ChatWorkspace()
        extras.sink = relay
        extras.authorOf = { [weak self, weak relay] conversation, id in
            if let self { return self.messages(in: conversation).first { $0.id == id }?.senderId }
            return relay?.messages(conversation).first { $0.id == id }?.senderId
        }
        extras.onRemoved = { [weak self] conversation, id in self?.forgetRemoved(conversation, id) }
        extras.onChannelJoined = { [weak relay] channel in relay?.addChannel(channel) }
        extras.onChannelRemoved = { [weak relay] channel in relay?.removeChannel(channel) }
        relay.frameObserver = { [weak extras] frame in extras?.apply(frame) }
        relay.onMessage = { [weak self] message in
            guard let self else { return }
            self.filePing(message)
            if self.relay.currentConversation == message.conversation { self.queueReadMark(message.conversation) }
        }
        relay.isReadElsewhere = { [weak self] message in self?.isReadElsewhere(message) ?? false }
        relay.isExpired = expiry
        mesh.isExpired = expiry
        extras.isExpired = expiry
    }

    private var expiry: ChatExpiry {
        { [weak self] key, timestamp in self?.isExpired(key, timestamp) ?? false }
    }

    public func isExpired(_ key: String, _ timestamp: Date) -> Bool {
        retention.isExpired(timestamp, key: key, now: clock.now)
    }

    public func retentionKey(_ conversation: String) -> String {
        office?.retentionKey(conversation) ?? conversation
    }

    public func applyRetention(_ next: ChatRetention) {
        if next == retention { return }
        retention = next
        pruneExpired()
    }

    @discardableResult
    public func pruneExpired() -> Bool {
        let relayPruned = relay.pruneExpired()
        let officePruned = office?.pruneExpired() ?? false
        let directPruned = direct?.pruneExpired() ?? false
        let meshPruned = mesh.pruneExpired()
        let threadsPruned = extras.pruneExpired()
        let savedPruned = workspace.pruneSaved { item in
            self.isExpired(self.retentionKey(item.conversation), item.sentAt)
        }
        return relayPruned || officePruned || directPruned || meshPruned || threadsPruned || savedPruned
    }

    public func startRetentionPruning(
        every interval: TimeInterval = 300,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        pruneTask?.cancel()
        pruneExpired()
        pruneTask = Task { [weak self] in
            while !Task.isCancelled {
                do { try await sleep(interval) } catch { return }
                guard let self, !Task.isCancelled else { return }
                self.pruneExpired()
            }
        }
    }

    public func stopRetentionPruning() {
        pruneTask?.cancel()
        pruneTask = nil
    }

    public func attachHistory(_ store: ChatHistoryStore, scope: @escaping @MainActor () -> String) {
        history = store
        historyScope = scope
        restoreHistoryIfNeeded()
        extras.onChange = { [weak self] in self?.persistSoon() }
        if let token = historyToken { relay.removeListener(token) }
        historyToken = relay.addListener { [weak self] in
            Task { @MainActor in self?.persistSoon() }
        }
    }

    private func restoreHistoryIfNeeded() {
        guard let history else { return }
        let scope = historyScope()
        if scope.isEmpty || scope == loadedScope { return }
        loadedScope = scope
        guard let snapshot = history.load(scope: scope) else { return }
        extras.restoreChannels(privateChannels: snapshot.privateChannels, hiddenChannels: snapshot.hiddenChannels)
        purgePrivatePings()
        relay.restoreHistory(
            snapshot.messages.filter {
                !snapshot.hiddenChannels.contains($0.conversation) && !DirectMessageFrames.isDirect($0.conversation)
            })
        directRestore = (snapshot.messages.filter { DirectMessageFrames.isDirect($0.conversation) }, snapshot.receipts)
        restoreDirectHistory()
        extras.restore(
            replies: snapshot.replies, roots: snapshot.replyRoots, reactions: snapshot.reactions, order: snapshot.reactionOrder,
            edits: snapshot.edits)
        extras.restoreHiddenMessages(snapshot.hiddenMessages)
        extras.restoreRemovedMessages(snapshot.removedMessages)
        extras.restoreLooks(snapshot.looks, events: snapshot.lookEvents)
    }

    private func restoreDirectHistory() {
        guard let direct, let saved = directRestore else { return }
        directRestore = nil
        direct.restore(saved.messages, receipts: saved.receipts)
    }

    private func persistSoon() {
        guard let history else { return }
        restoreHistoryIfNeeded()
        let scope = loadedScope
        if scope.isEmpty { return }
        history.scheduleSave(scope: scope) { [weak self] in self?.historyParts() ?? ChatHistoryParts() }
    }

    public func historySnapshot() -> ChatHistorySnapshot { historyParts().snapshot }

    public func historyParts() -> ChatHistoryParts {
        var parts = ChatHistoryParts()
        parts.conversations = relay.historySnapshot.merging(direct?.historySnapshot ?? [:]) { kept, _ in kept }
        parts.receipts = direct?.receiptSnapshot ?? [:]
        parts.replies = extras.snapshotThreads()
        parts.replyRoots = extras.snapshotReplyRoots()
        parts.reactions = extras.snapshotReactions()
        parts.reactionOrder = extras.snapshotReactionOrder()
        parts.edits = extras.snapshotEdits()
        parts.privateChannels = extras.privateChannels
        parts.hiddenChannels = extras.hiddenChannels
        parts.hiddenMessages = extras.snapshotHiddenMessages()
        parts.removedMessages = extras.snapshotRemovedMessages()
        parts.looks = extras.snapshotLooks()
        parts.lookEvents = extras.snapshotLookEvents()
        parts.retention = retention
        parts.now = clock.now
        return parts
    }

    // MARK: ShellChatBridge

    public var relayState: ShellRelayState {
        switch relay.state {
        case .offline: return .offline
        case .connecting: return .connecting
        case .connected: return .connected
        }
    }

    public var onlineCount: Int { relay.onlineCount }
    public var totalUnread: Int { relay.totalUnread + (direct?.unreadTotal ?? 0) }
    public var badgeUnread: Int { relay.badgeUnread + (direct?.unreadTotal ?? 0) }
    public var currentConversation: String { relay.currentConversation }
    public var unreadConversations: [String] {
        relay.unreadConversations
            + (direct?.records.filter { $0.unreadCount > 0 }.map(\.conversation) ?? [])
    }

    public func connect() { Task { await relay.connect() } }

    nonisolated static let pingFreshness: TimeInterval = 10 * 60
    nonisolated static let welcomeChannel = "#welcome"

    func filePing(_ message: ChatMessage) {
        guard let pings, !message.isSelf, !message.id.isEmpty, !isFromBlocked(message),
              clock.now.timeIntervalSince(message.timestamp) < ChatHub.pingFreshness else { return }
        let conversation = message.conversation
        if isPrivateConversation(conversation) {
            pings.removeChats { $0 == conversation }
            return
        }
        let name = message.senderName.dartTrimmed
        let text = message.text.dartTrimmed
        let title: String
        var body = message.fileName.isEmpty ? text : "\u{1F4CE} \(message.fileName)" + (text.isEmpty ? "" : "\n\(text)")
        if ChatEveryone.isWelcome(message) {
            title = "New member joined"
        } else if conversation == ChatHub.welcomeChannel || conversation == ChatEveryone.welcomeConversation {
            title = "New user joined"
            body = name.isEmpty ? "Someone new said hello in #welcome" : "\(name) said hello in #welcome"
        } else {
            title = "\(name.isEmpty ? "Someone" : name) in \(office?.room(for: conversation)?.name ?? conversation)"
        }
        pings.recordChat(
            messageId: message.id, sender: name, title: title, message: body, conversation: conversation,
            tsMs: message.timestamp.epochMillis, read: relay.currentConversation == conversation)
    }

    public func markAllRead() {
        for conversation in relay.unreadConversations { queueReadMark(conversation) }
        relay.markAllRead()
    }
    public func markBadgeSeen() { relay.markBadgeSeen() }
    public func switchConversation(_ conversation: String) {
        let target = (try? ChatReplyTarget.resolve(
            conversation, channels: relay.channels, roster: relay.roster(), offices: officeChannels)) ?? conversation
        select(target)
        revealRequests += 1
    }

    /// `sendChatText`: the mesh room over the local transports, anything else through the relay.
    /// True once the message is on its way, so the caller can clear its field.
    public func sendChatText(_ text: String) async -> Bool {
        if text.dartTrimmed.isEmpty { return false }
        let conversation = relay.currentConversation
        var body = text
        if let slash = ChatComposerTools.parseSlash(text) {
            switch slash {
            case .issue(let argument):
                pendingIssue = argument
                return true
            case .topic(let topic):
                return await setTopic(topic)
            case .message(let name, let message):
                return await messagePerson(name, text: message)
            case .mute:
                let muted = !workspace.isMuted(conversation)
                workspace.setMuted(conversation, muted)
                notice = ChatNotice(muted ? "\(conversationLabel(conversation)) is muted." : "\(conversationLabel(conversation)) is unmuted.")
                return true
            case .search(let query):
                pendingSearch = query
                return true
            case .saved:
                pendingSaved = true
                return true
            case .shrug(let line), .me(let line):
                body = line
            }
        }
        if let blocked = guestComposeBlocked(conversation) {
            onGuestBlocked?(blocked)
            return false
        }
        if let handled = await runAppCommand(body, conversation: conversation) {
            if !OfficeFrames.isOffice(conversation) {
                extras.sendTyping(conversation: conversation, active: false)
                sendDirectTyping(conversation, active: false)
            }
            return handled
        }
        let sent = await deliver(body, to: conversation)
        if sent { workspace.setDraft("", for: conversation) }
        return sent
    }

    @discardableResult
    public func deliver(_ raw: String, to conversation: String) async -> Bool {
        let text = raw.contains("`") ? raw : ChatEmojiShortcodes.expand(raw)
        if text.dartTrimmed.isEmpty { return false }
        if let blocked = guestComposeBlocked(conversation) {
            onGuestBlocked?(blocked)
            return false
        }
        // The mesh room never goes through the relay, so it sends whether or not this device has
        // an account, a session, or any internet.
        if OfficeFrames.isOffice(conversation) {
            if isReadOnly(conversation) {
                notice = ChatNotice("\(conversation) is read-only.")
                return false
            }
            guard let office, let id = await office.post(conversation, text: text) else {
                notice = ChatNotice("\(conversation) is not writable right now.")
                return false
            }
            forwardAppMentions(conversation: conversation, messageId: id, text: text)
            await maybeAskForkbot(conversation: conversation, text: text)
            return true
        }
        if DirectMessageFrames.isDirect(conversation) {
            directTypingSent.removeValue(forKey: conversation)
            let quote = replyConversation == conversation ? replyingTo : nil
            guard let direct, await direct.send(conversation, text: text, replyTo: quote) else {
                notice = ChatNotice("The direct message could not be sent. Reconnecting...")
                return false
            }
            if quote != nil && replyingTo == quote { cancelReply() }
            return true
        }
        if conversation == meshConversation {
            let id = await mesh.sendMessage(text)
            if id.isEmpty {
                notice = ChatNotice("No one is on this network or Bluetooth yet.")
                return false
            }
            return true
        }
        extras.sendTyping(conversation: conversation, active: false)
        let sent = extras.isPrivate(conversation)
            ? extras.sendPrivateChat(conversation, text: text) : await relay.sendMessage(text, to: conversation)
        if sent && conversation.hasPrefix("#") { await maybeAskForkbot(conversation: conversation, text: text) }
        if !sent {
            notice = ChatNotice("Chat is offline. Reconnecting...")
            Task { await relay.connect() }
            return false
        }
        return true
    }

    public func clearNotice() { notice = nil }

    public func refreshAppCommands(force: Bool = false) async {
        guard let apps, !isGuest() else { return }
        if appCommandsLoaded && !force { return }
        appCommandsLoaded = true
        do {
            appCommands = try await apps.commands()
        } catch {
            appCommandsLoaded = false
        }
    }

    public func resetAppCommands(clearReplies: Bool = false) {
        appCommands = AppCommandCatalog()
        appCommandsLoaded = false
        if clearReplies { ephemerals = [:] }
    }

    public func appsChannelId(_ conversation: String) -> String {
        guard let room = office?.room(for: conversation), !room.isPrivate else { return "" }
        return room.id
    }

    public func appSuggestions(_ draft: String) -> [AppCommand] {
        guard apps != nil, !isGuest(), !inMesh else { return [] }
        return AppsChat.suggestions(draft, commands: appCommands.commands)
    }

    public func slashCommands(_ draft: String) -> [ChatSlashCommand] {
        let builtIn = ChatComposerTools.slashCommands(draft, canSetTopic: canSetTopic)
        let apps = appSuggestions(draft).map {
            ChatSlashCommand(command: "/\($0.command)", usage: $0.usage, summary: $0.summary)
        }
        return builtIn + apps.filter { app in !builtIn.contains { $0.command == app.command } }
    }

    func runAppCommand(_ text: String, conversation: String) async -> Bool? {
        guard let apps, !isGuest(), conversation != meshConversation, let slash = AppsChat.parseSlash(text) else { return nil }
        await refreshAppCommands()
        if slash.command == AppsChat.listCommand && appCommands.command(slash.command) == nil {
            addEphemeral(
                conversation: conversation, sender: "ForkMesh Apps", text: AppsChat.commandsText(appCommands.commands),
                app: AppMessage(blocks: [], appMessageId: "", ephemeral: true))
            return true
        }
        guard appCommands.command(slash.command) != nil else { return nil }
        let reply = await apps.run(
            text: text.trimmingCharacters(in: .whitespacesAndNewlines), channelId: appsChannelId(conversation))
        show(reply, conversation: conversation)
        return true
    }

    public func interact(_ message: ChatMessage, actionId: String, value: String) async {
        guard let apps, let app = message.app, !app.appMessageId.isEmpty, !actionId.isEmpty else { return }
        let key = "\(app.appMessageId)|\(actionId)"
        if appBusy.contains(key) { return }
        appBusy.insert(key)
        defer { appBusy.remove(key) }
        let reply = await apps.interact(
            appMessageId: app.appMessageId, channelId: app.channelId, actionId: actionId, value: value)
        show(reply, conversation: message.conversation)
    }

    public func isAppBusy(_ message: ChatMessage, actionId: String) -> Bool {
        guard let id = message.app?.appMessageId, !id.isEmpty else { return false }
        return appBusy.contains("\(id)|\(actionId)")
    }

    private func show(_ reply: AppReply, conversation: String) {
        if reply.isEphemeral {
            let content = AppMessage(
                blocks: reply.blocks, appMessageId: reply.appMessageId, appName: reply.appName, ephemeral: true)
            addEphemeral(conversation: conversation, sender: reply.sender, text: reply.text, app: content)
        } else if !reply.ok {
            notice = ChatNotice(reply.errorText)
        }
    }

    private func addEphemeral(conversation: String, sender: String, text: String, app: AppMessage) {
        if text.isEmpty && app.blocks.isEmpty { return }
        ephemeralSequence += 1
        let message = ChatMessage(
            id: "app-ephemeral-\(ephemeralSequence)", conversation: conversation, senderId: "app:", senderName: sender,
            text: text, timestamp: Date(epochMillis: clock.nowMillis), app: app)
        var list = ephemerals[conversation] ?? []
        list.append(message)
        if list.count > 50 { list.removeFirst(list.count - 50) }
        ephemerals[conversation] = list
    }

    private func forwardAppMentions(conversation: String, messageId: String, text: String) {
        guard let apps, !isGuest() else { return }
        let channelId = appsChannelId(conversation)
        if channelId.isEmpty || AppsChat.mentionedSlugs(text).isEmpty { return }
        Task {
            await refreshAppCommands()
            guard AppsChat.shouldForwardMentions(text, mentionable: appCommands.mentionable) else { return }
            _ = await apps.forwardMentions(channelId: channelId, messageId: messageId, text: text)
        }
    }

    public func post(_ text: String) { notice = ChatNotice(text) }

    public var canSetTopic: Bool { office?.room(for: relay.currentConversation)?.canManage == true }

    public func setTopic(_ topic: String) async -> Bool {
        guard let office, let room = office.room(for: relay.currentConversation) else {
            notice = ChatNotice("/topic is available in managed Office channels.")
            return false
        }
        if !room.canManage {
            notice = ChatNotice("Only the channel creator or an administrator can change this channel topic.")
            return false
        }
        do {
            try await office.update(room, topic: topic)
            return true
        } catch {
            notice = ChatNotice("\(error)")
            return false
        }
    }

    // MARK: The open room

    public var inMesh: Bool { relay.currentConversation == meshConversation }

    public var inOffice: Bool { OfficeFrames.isOffice(relay.currentConversation) }

    public var inDirect: Bool { DirectMessageFrames.isDirect(relay.currentConversation) }

    public var messages: [ChatMessage] {
        if inMesh { return mesh.messages }
        if inOffice { return office?.messages(relay.currentConversation) ?? [] }
        if inDirect { return direct?.messages(relay.currentConversation) ?? [] }
        return relay.messages(relay.currentConversation)
    }

    public var transcript: [ChatMessage] {
        let list = messages
        guard let extra = ephemerals[relay.currentConversation], !extra.isEmpty else { return list }
        var merged = list
        for message in extra {
            var at = merged.count
            while at > 0 && merged[at - 1].timestamp > message.timestamp { at -= 1 }
            merged.insert(message, at: at)
        }
        return merged
    }

    /// The transcript with grouping, queue and delete flags worked out.
    public var rows: [ChatRow] {
        _ = extras.revision
        let blockedIds = blockedSenderIds()
        let list = transcript.filter {
            !extras.isHidden($0.id) && !extras.isRemoved($0.id) && !isFromBlocked($0, blockedIds: blockedIds)
        }
        var out: [ChatRow] = []
        out.reserveCapacity(list.count)
        let mutableRoom = !inMesh && !isReadOnly(relay.currentConversation)
        let own = ownNames
        for (index, message) in list.enumerated() {
            let previous = index == 0 ? nil : list[index - 1]
            let mutable = mutableRoom && message.app?.ephemeral != true
            let queued = inMesh && message.isSelf && mesh.isQueued(message.id)
            var row = ChatRow(
                message: message,
                continuesGroup: ChatHub.continuesGroup(message, previous),
                queued: queued,
                // The mesh room has no server to tell other devices a message is gone, so
                // deleting there would only remove this device's own copy.
                deletable: mutable && message.isSelf)
            row.text = mutable ? extras.text(for: message) : message.text
            row.edited = mutable && extras.isEdited(message.id)
            row.editable = mutable && message.isSelf && !message.text.isEmpty
            if mutable {
                row.reactions = extras.reactions(for: message.id, selfId: roomSelfId(message.conversation))
                row.thread = ChatHub.hasThreads(message.conversation) ? extras.threadSummary(message.id) : nil
            }
            if !message.fileName.isEmpty { row.attachment = extras.attachment(for: message.id) }
            row.visitor = !message.isSelf && ChatVisitorPresence.isTransientVisitor(name: message.senderName)
            row.receipt = receipt(for: message, queued: queued)
            row.mentionsYou = !message.isSelf && ChatSmartMentions.mentionsYou(row.text, own: own)
            out.append(row)
        }
        return out
    }

    public nonisolated static func hasThreads(_ conversation: String) -> Bool {
        conversation.hasPrefix("#") || DirectMessageFrames.isDirect(conversation)
    }

    public nonisolated static func isRoomConversation(_ conversation: String) -> Bool {
        DirectMessageFrames.isDirect(conversation) || OfficeFrames.isOffice(conversation)
    }

    public func receipt(for message: ChatMessage, queued: Bool = false) -> ChatReceipt? {
        guard message.isSelf else { return nil }
        if queued { return .sending }
        let conversation = message.conversation
        if DirectMessageFrames.isDirect(conversation) { return direct?.receipt(message.id) ?? .sent }
        if OfficeFrames.isOffice(conversation) { return office?.receipt(message.id) ?? .sent }
        return .sent
    }

    func roomSelfId(_ conversation: String) -> String {
        if DirectMessageFrames.isDirect(conversation) { return direct?.currentSelfId ?? "" }
        if OfficeFrames.isOffice(conversation) { return office?.currentSelfId ?? "" }
        return relay.selfId
    }

    func postRoomFrame(_ conversation: String, type: String, fields: JSONObject) async -> Bool {
        if DirectMessageFrames.isDirect(conversation) {
            return await direct?.sendFrame(conversation, type: type, fields: fields) != nil
        }
        guard OfficeFrames.isOffice(conversation), let office, let room = office.room(for: conversation) else { return false }
        var routed = fields
        if type != "thread-reply" { routed["conversation"] = .string("#\(room.name)") }
        return await office.postFrame(conversation, type: type, fields: routed) != nil
    }

    func messageRemoved(_ conversation: String, _ id: String) {
        extras.recordRemoved(id)
        forgetRemoved(conversation, id)
    }

    func forgetRemoved(_ conversation: String, _ id: String) {
        pings?.remove("chat:\(id)")
        onMessageRemoved?(conversation, id)
    }

    public func threadRows(_ rootId: String) -> [ChatRow] {
        _ = extras.revision
        let list = extras.replies(to: rootId).filter { !extras.isHidden($0.id) && !extras.isRemoved($0.id) }
        var out: [ChatRow] = []
        for (index, message) in list.enumerated() {
            var row = ChatRow(
                message: message, continuesGroup: ChatHub.continuesGroup(message, index == 0 ? nil : list[index - 1]),
                queued: false, deletable: message.isSelf)
            row.text = extras.text(for: message)
            row.edited = extras.isEdited(message.id)
            row.editable = message.isSelf && !message.text.isEmpty
            row.reactions = extras.reactions(for: message.id, selfId: roomSelfId(message.conversation))
            row.attachment = extras.attachment(for: message.id)
            row.receipt = receipt(for: message)
            out.append(row)
        }
        return out
    }

    public func rootMessage(_ rootId: String, in channel: String) -> ChatMessage? {
        messages(in: channel).first { $0.id == rootId }
    }

    @discardableResult
    public func replyInThread(channel: String, rootId: String, text: String) async -> Bool {
        if let blocked = guestComposeBlocked(channel) {
            onGuestBlocked?(blocked)
            return false
        }
        if ChatHub.isRoomConversation(channel) {
            let body = String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxTextChars))
            let root = String(rootId.trimmingCharacters(in: .whitespaces).prefix(ChatLimits.maxRootIdChars))
            if body.isEmpty || root.isEmpty { return false }
            let sent = await postRoomFrame(channel, type: "thread-reply", fields: ["rootId": .string(root), "text": .string(body)])
            if !sent { notice = ChatNotice("The reply could not be sent.") }
            return sent
        }
        if relay.state != .connected {
            notice = ChatNotice("Chat is offline. Reconnecting...")
            Task { await relay.connect() }
            return false
        }
        let sent = extras.sendThreadReply(channel: channel, rootId: rootId, text: text)
        if sent { await maybeAskForkbot(conversation: channel, text: text, threadRootId: rootId) }
        return sent
    }

    public func toggleReaction(_ message: ChatMessage, value: String) {
        let conversation = message.conversation
        if ChatHub.isRoomConversation(conversation) {
            let me = roomSelfId(conversation)
            guard !me.isEmpty, !message.id.isEmpty, !value.isEmpty else { return }
            let fields = ChatExtras.reactionFields(
                added: extras.reactionAdds(messageId: message.id, value: value, selfId: me), messageId: message.id,
                value: value, selfId: me)
            Task {
                if !(await self.postRoomFrame(conversation, type: "reaction", fields: fields)) {
                    self.notice = ChatNotice("The reaction could not be sent.")
                }
            }
            return
        }
        if !extras.toggleReaction(conversation: message.conversation, messageId: message.id, value: value) {
            notice = ChatNotice("Chat is offline. Reconnecting...")
        }
    }

    @discardableResult
    public func edit(_ message: ChatMessage, text: String) -> Bool {
        let conversation = message.conversation
        if ChatHub.isRoomConversation(conversation) {
            let body = String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxTextChars))
            let me = roomSelfId(conversation)
            guard !body.isEmpty, !me.isEmpty, message.senderId == me else {
                notice = ChatNotice("The message could not be changed.")
                return false
            }
            let fields: JSONObject = ["target": .string(message.id), "text": .string(body), "editedAt": .int(clock.nowMillis)]
            Task {
                if !(await self.postRoomFrame(conversation, type: "edit", fields: fields)) {
                    self.notice = ChatNotice("The message could not be changed.")
                }
            }
            return true
        }
        let sent = extras.edit(conversation: message.conversation, messageId: message.id, text: text)
        if !sent { notice = ChatNotice("The message could not be changed.") }
        return sent
    }

    public func deleteReply(_ message: ChatMessage) {
        if ChatHub.isRoomConversation(message.conversation) {
            Task { await self.delete(message) }
            return
        }
        if !extras.deleteReply(message) { notice = ChatNotice("The reply could not be deleted.") }
    }

    public func composerChanged(_ text: String) {
        let conversation = relay.currentConversation
        if conversation == meshConversation || inOffice { return }
        let active = !text.dartTrimmed.isEmpty
        if DirectMessageFrames.isDirect(conversation) {
            sendDirectTyping(conversation, active: active)
            return
        }
        extras.sendTyping(conversation: conversation, active: active)
    }

    func sendDirectTyping(_ conversation: String, active: Bool) {
        guard let direct, DirectMessageFrames.isDirect(conversation) else { return }
        let now = clock.now
        if active {
            if let last = directTypingSent[conversation], now.timeIntervalSince(last) < ChatLimits.typingResend { return }
            directTypingSent[conversation] = now
        } else if directTypingSent.removeValue(forKey: conversation) == nil {
            return
        }
        Task { await direct.sendTyping(conversation, active: active) }
    }

    public func setFocus(_ conversation: String, _ on: Bool) {
        guard let direct, DirectMessageFrames.isDirect(conversation) else { return }
        direct.setFocus(conversation, on)
    }

    public func directRoute(_ conversation: String) -> DirectRoute {
        guard let direct, DirectMessageFrames.isDirect(conversation) else { return .none }
        return direct.route(conversation)
    }

    public var typingLine: String {
        _ = extras.revision
        return ChatExtras.typingLabel(typingNames(in: relay.currentConversation))
    }

    public func typingNames(in conversation: String) -> [String] {
        extras.typingNames(in: conversation).filter { !workspace.isBlocked($0) }
    }

    public var mentionNames: [String] {
        var names: [String] = []
        for group in relay.rosterGroups() where !group.isSelf { names.append(group.name) }
        for message in relay.messages(relay.currentConversation).suffix(200) where !message.isSelf {
            names.append(message.senderName)
        }
        if let office, let room = office.room(for: relay.currentConversation) { names.append(contentsOf: room.members) }
        return names
    }

    public var isPrivateRoom: Bool { isPrivateConversation(relay.currentConversation) }

    public func isPrivateConversation(_ conversation: String) -> Bool {
        if conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation) { return true }
        if let room = office?.room(for: conversation) { return room.isPrivate }
        return extras.isPrivate(conversation)
    }

    func purgePrivatePings() {
        pings?.removeChats { self.isPrivateConversation($0) }
    }

    public func isServerVisible(_ conversation: String) -> Bool { office?.room(for: conversation)?.serverVisible == true }

    public func isReadOnly(_ conversation: String) -> Bool { office?.room(for: conversation)?.readOnly == true }

    public var sealSummary: String {
        if inMesh { return "" }
        if isServerVisible(relay.currentConversation) {
            return "Server notices: ForkMesh writes these notes itself, so the server can read them. "
                + "They are not end-to-end encrypted. Only you can see this channel, and nobody can post in it."
        }
        if !isPrivateRoom {
            return "This channel is public and controller-readable. It still uses authenticated AES-256-GCM transport, "
                + "while allowing ForkBot, MCP, and incoming webhooks to read and post messages.\n\n"
                + "Private channels and direct messages add automatic device-to-device encryption beyond the controller key."
        }
        return "Encryption is automatic. This device created a private X25519 key locally and sends only its public key. "
            + "Each message uses a fresh key wrapped for known room devices, so the controller's room key alone can forward "
            + "and retain it but cannot decrypt it.\n\nA new device can read messages addressed after its first visit; it "
            + "cannot retroactively open older history. ForkBot runs on the relay and is unavailable in end-to-end encrypted rooms."
    }

    public func canAskForkbot(_ conversation: String) -> Bool {
        if !conversation.hasPrefix("#") || conversation == meshConversation { return false }
        if let room = office?.room(for: conversation) { return !room.isPrivate }
        return !extras.isPrivate(conversation)
    }

    public func maybeAskForkbot(conversation: String, text: String, threadRootId: String = "") async {
        guard ChatComposerTools.mentionsForkbot(text), let forkbot else { return }
        if !canAskForkbot(conversation) {
            notice = ChatNotice("ForkBot is available only in public, controller-readable channels.")
            return
        }
        let history = OfficeFrames.isOffice(conversation) ? (office?.messages(conversation) ?? []) : relay.messages(conversation)
        let replies = threadRootId.isEmpty ? [] : extras.replies(to: threadRootId)
        let context = ChatComposerTools.forkbotContext(history, threadRootId: threadRootId, replies: replies)
        let sender = relay.roster().first { $0.isSelf }?.name ?? ""
        do {
            let answer = try await forkbot.accountApiCall(
                "POST", "/api/forkbot/chat", ChatComposerTools.forkbotPayload(message: text, sender: sender, context: context))
            let bot = (answer["botMessage"]?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            if bot.isEmpty { return }
            if OfficeFrames.isOffice(conversation) {
                _ = await office?.send(conversation, text: bot, asBot: true)
            } else {
                extras.sendBotReply(channel: conversation, rootId: threadRootId, text: bot)
            }
        } catch {
            notice = ChatNotice("ForkBot did not answer.")
        }
    }

    public func sendAttachment(fileName: String, mime: String, data: Data) -> Bool {
        let conversation = relay.currentConversation
        if inMesh || inOffice || DirectMessageFrames.isDirect(conversation) { return false }
        if data.count > ChatFileLimits.maxSendBytes {
            notice = ChatNotice("Files up to 2 MB can be sent from this phone.")
            return false
        }
        let sent = extras.sendFile(conversation: conversation, fileName: fileName, mime: mime, data: data)
        if !sent { notice = ChatNotice("The file could not be sent.") }
        return sent
    }

    nonisolated static func continuesGroup(
        _ message: ChatMessage, _ previous: ChatMessage?, timeZone: TimeZone = .current
    ) -> Bool {
        guard let previous else { return false }
        if previous.senderName != message.senderName || previous.isSelf != message.isSelf { return false }
        if startsNewDay(message, after: previous, timeZone: timeZone) { return false }
        return abs(message.timestamp.timeIntervalSince(previous.timestamp)) < groupWindow
    }

    public nonisolated static func startsNewDay(
        _ message: ChatMessage, after previous: ChatMessage?, timeZone: TimeZone = .current
    ) -> Bool {
        guard let previous else { return true }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        return !calendar.isDate(message.timestamp, inSameDayAs: previous.timestamp)
    }

    public nonisolated static func dayDividerLabel(_ date: Date, now: Date, timeZone: TimeZone = .current) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let days =
            calendar.dateComponents([.day], from: calendar.startOfDay(for: date), to: calendar.startOfDay(for: now)).day ?? 0
        if days == 0 { return "Today" }
        if days == 1 { return "Yesterday" }
        let parts = calendar.dateComponents([.year, .month, .day, .weekday], from: date)
        let weekdays = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
        let months = [
            "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
            "November", "December",
        ]
        let label =
            "\(weekdays[((parts.weekday ?? 1) - 1 + 7) % 7]), \(months[((parts.month ?? 1) - 1 + 12) % 12]) \(parts.day ?? 1)"
        if parts.year == calendar.component(.year, from: now) { return label }
        return "\(label), \(parts.year ?? 0)"
    }

    /// The header title: the channel, or "@name" for a direct message.
    public var title: String {
        let c = relay.currentConversation
        if OfficeFrames.isOffice(c) { return c }
        if DirectMessageFrames.isDirect(c) {
            return "@\(direct?.record(for: c)?.otherUser ?? "direct message")"
        }
        if c.hasPrefix("@") {
            let id = String(c.dropFirst())
            let name = relay.roster().first { $0.id == id }?.name ?? "direct message"
            return "@\(name)"
        }
        return c
    }

    /// The header icon: "lan_outlined", "alternate_email" or "tag".
    public var titleIcon: String {
        if inMesh { return "lan_outlined" }
        if isPrivateRoom && !title.hasPrefix("@") { return "lock_outline" }
        return title.hasPrefix("@") ? "alternate_email" : "tag"
    }

    /// The header badge: "encrypted", or where the mesh room travels.
    public var badge: String {
        if !inMesh {
            if isServerVisible(relay.currentConversation) { return "server-visible" }
            return isPrivateRoom ? "end-to-end" : "public"
        }
        var carried: [String] = []
        if mesh.networkActive { carried.append("this network") }
        if mesh.bluetoothActive { carried.append("bluetooth") }
        if carried.isEmpty { return "local only" }
        let devices = mesh.peerCount
        let linked = devices == 0 ? "" : " · \(devices) \(devices == 1 ? "device" : "devices")"
        return "local · \(carried.joined(separator: " + "))\(linked)"
    }

    public var roomName: String {
        let c = relay.currentConversation
        if OfficeFrames.isOffice(c) { return String(c.dropFirst(OfficeFrames.prefix.count)) }
        let t = title
        return t.hasPrefix("#") || t.hasPrefix("@") ? String(t.dropFirst()) : t
    }

    public var serverCopiesLine: String {
        guard inDirect, let stored = direct?.serverCopies(relay.currentConversation) else { return "" }
        return DirectMessageFrames.serverCopiesLabel(stored)
    }

    public var headerDetail: String {
        if inMesh { return badge }
        if !serverCopiesLine.isEmpty { return "Direct message • \(serverCopiesLine)" }
        if title.hasPrefix("@") { return "Direct message • \(badge)" }
        return channelDetail(relay.currentConversation)
    }

    public var introLine: String {
        if title.hasPrefix("@") { return "This is the very beginning of your direct messages with \(roomName)." }
        return "This is the very beginning of the \(roomName) channel."
    }

    public var emptyTitle: String { "No messages yet" }

    public var emptyMessage: String {
        if !inMesh { return "Encrypted relay messages will appear here." }
        if mesh.localNetworkBlocked {
            return "This phone is not allowed onto the local network, so no nearby device can be reached. "
                + "Turn on Settings › Privacy & Security › Local Network › ForkMesh."
        }
        let searching =
            mesh.networkActive && mesh.peerCount == 0 ? " Looking for devices with ForkMesh open on this Wi-Fi." : ""
        return "Messages stay on this network and this Bluetooth link. Nothing here is sent to the internet.\(searching)"
    }

    /// Something waiting in a room this screen is not showing (the channels button's dot).
    public var otherRoomsUnread: Bool { relay.totalUnread > 0 || (!inMesh && mesh.hasUnread) }

    /// The mesh room is writable whenever it is open; relay rooms need the socket.
    public var composerConnected: Bool { inMesh || inOffice || inDirect || relay.state == .connected }

    /// The "Sign in to ..." phrase for the open conversation when a guest cannot post there.
    public var composeBlocked: String? { guestComposeBlocked(relay.currentConversation) }

    public func guestComposeBlocked(_ conversation: String) -> String? {
        if !isGuest() { return nil }
        if conversation == "#\(guestChatChannelName)" || conversation == meshConversation { return nil }
        if conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation) {
            return "send direct messages"
        }
        return "post in \(conversation)"
    }

    /// The status the sender's node is announcing live, or nil (then ask the account directory).
    /// Mesh peers are not relay peers, so there only your own status is known live.
    public func liveStatus(for message: ChatMessage) -> UserStatus? {
        if inMesh { return message.isSelf ? relay.liveStatusFor(relay.selfId) : nil }
        return relay.liveStatusFor(message.senderId)
    }

    /// In the mesh room the people shown are the ones reachable from here, not the relay's roster.
    public var memberGroups: [MemberGroup] {
        if inMesh {
            return RelayService.groups(mesh.members())
        }
        return relay.rosterGroups()
    }

    public var channels: [String] {
        _ = extras.revision
        return relay.channels.filter {
            !extras.hiddenChannels.contains($0) && !OfficeFrames.isOffice($0) && !ChatEveryone.retiredRelayLabels.contains($0)
        }
    }

    public var officeChannels: [String] { ChatEveryone.order(office?.conversations ?? [], office: office) }

    public func readMarkKey(_ conversation: String) -> String? {
        if DirectMessageFrames.isDirect(conversation) { return conversation }
        if OfficeFrames.isOffice(conversation) {
            return office?.room(for: conversation).map { "private:\($0.id)" }
        }
        return ChatReadMarks.isSyncable(conversation) ? conversation : nil
    }

    func isReadElsewhere(_ message: ChatMessage) -> Bool {
        guard let readMarks, let key = readMarkKey(message.conversation) else { return false }
        return readMarks.isRead(key, message.timestamp)
    }

    public func queueReadMark(_ conversation: String) {
        guard let readMarks, let key = readMarkKey(conversation),
            let newest = messages(in: conversation).last?.timestamp
        else { return }
        readMarks.queue(key, newest)
    }

    public func applyReadMarks() async {
        guard let readMarks, await readMarks.fetch() else { return }
        for conversation in unreadConversations {
            guard let key = readMarkKey(conversation),
                let newest = messages(in: conversation).last?.timestamp,
                readMarks.isRead(key, newest)
            else { continue }
            if DirectMessageFrames.isDirect(conversation) {
                direct?.clearUnread(conversation)
            } else {
                relay.markConversationRead(conversation)
            }
        }
    }

    public func restoreLastConversation() {
        if conversationChosen { return }
        conversationChosen = true
        let last = workspace.lastConversation
        if last.isEmpty || last == relay.currentConversation { return }
        open(last)
    }

    public func startReply(_ message: ChatMessage, text: String) {
        replyingTo = ChatHub.replyQuote(message, text: text)
        replyConversation = message.conversation
    }

    public func cancelReply() {
        replyingTo = nil
        replyConversation = ""
    }

    public func reply(in conversation: String) -> ChatReplyQuote? {
        replyConversation == conversation ? replyingTo : nil
    }

    public func replyAuthor(_ quote: ChatReplyQuote, in conversation: String) -> String {
        let me = roomSelfId(conversation)
        return !me.isEmpty && quote.senderId == me ? "You" : quote.senderName
    }

    public nonisolated static func replyQuote(_ message: ChatMessage, text: String) -> ChatReplyQuote {
        var body = ChatListPreviews.plain(text)
        if body.isEmpty && !message.fileName.isEmpty { body = ChatListPreviews.filePrefix + message.fileName }
        return ChatReplyQuote(
            id: message.id, senderId: message.senderId, senderName: message.senderName,
            text: String(body.prefix(ChatReplyQuote.maxTextChars)))
    }

    public func select(_ conversation: String) {
        conversationChosen = true
        if conversation != replyConversation { cancelReply() }
        if ChatHub.isPersonConversation(conversation) { workspace.restoreConversation(conversation) }
        if conversation != relay.currentConversation || unreadStart[conversation] == nil {
            let start = ChatHub.firstUnreadId(messages(in: conversation), unread: unreadCount(conversation))
            unreadStart = start.map { [conversation: $0] } ?? [:]
        }
        workspace.setLastConversation(conversation)
        extras.sendTyping(conversation: relay.currentConversation, active: false)
        sendDirectTyping(relay.currentConversation, active: false)
        relay.switchConversation(conversation)
        queueReadMark(conversation)
        pings?.markConversationRead(conversation)
        if conversation == meshConversation { Task { await mesh.roomOpened() } }
        if OfficeFrames.isOffice(conversation), let office {
            Task {
                if office.room(for: conversation) == nil { await office.refresh() }
                await office.loadHistory(conversation)
                _ = await office.open(conversation)
                self.queueReadMark(conversation)
            }
        }
        if DirectMessageFrames.isDirect(conversation), let direct {
            Task {
                if direct.record(for: conversation) == nil { await direct.refresh() }
                _ = await direct.open(conversation)
                await direct.markRead(conversation)
                self.queueReadMark(conversation)
            }
        }
    }

    /// Opening the room marks the mesh read.
    public func roomShown() {
        if inMesh { Task { await mesh.roomOpened() } }
        if inMesh && mesh.hasUnread { mesh.markRead() }
    }

    public func delete(_ message: ChatMessage) async {
        let conversation = message.conversation
        if DirectMessageFrames.isDirect(conversation) || OfficeFrames.isOffice(conversation) {
            guard message.isSelf, message.senderId == roomSelfId(conversation) else { return }
            let removed = DirectMessageFrames.isDirect(conversation)
                ? await direct?.deleteForEveryone(conversation, messageId: message.id) ?? false
                : await office?.deleteForEveryone(conversation, messageId: message.id) ?? false
            if !removed { notice = ChatNotice("The message could not be deleted. Check the connection and try again.") }
            return
        }
        _ = await relay.deleteMessage(message)
    }

    public func hide(_ message: ChatMessage) { extras.hideMessage(message.id) }

    /// Splits on ``` fences into (code, text) segments, dropping empties. An unterminated fence
    /// leaves a trailing odd segment, treated as code.
    public nonisolated static func segments(_ source: String) -> [ChatBodySegment] {
        let parts = source.components(separatedBy: "```")
        var out: [ChatBodySegment] = []
        for (i, part) in parts.enumerated() {
            let isCode = i % 2 == 1
            if part.dartTrimmed.isEmpty { continue }
            out.append(ChatBodySegment(code: isCode, text: part.dartTrimmed))
        }
        return out
    }

    /// "h:mm AM" in the local zone.
    public nonisolated static func timeLabel(_ date: Date, timeZone: TimeZone = .current) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let parts = calendar.dateComponents([.hour, .minute], from: date)
        let hour = parts.hour ?? 0
        let meridiem = hour < 12 ? "AM" : "PM"
        return String(format: "%d:%02d", hour % 12 == 0 ? 12 : hour % 12, parts.minute ?? 0) + " \(meridiem)"
    }

    /// The name row of a message: "name (you)" for your own.
    public nonisolated static func senderLabel(_ message: ChatMessage) -> String {
        message.isSelf ? "\(message.senderName) (you)" : message.senderName
    }

    /// `_platformIcon`: a Material icon name for a node's platform.
    public nonisolated static func platformIcon(_ platform: String) -> String {
        switch platform.lowercased() {
        case "android": return "android"
        case "ios": return "phone_iphone"
        case "macos": return "laptop_mac"
        case "windows": return "desktop_windows"
        case "linux": return "dvr_outlined"
        case "web": return "public"
        default: return "devices_other"
        }
    }

    /// "1 unread message" / "n unread messages".
    public nonisolated static func unreadLabel(_ count: Int) -> String {
        count == 1 ? "1 unread message" : "\(count) unread messages"
    }
}

/// A transient inline message; a new one replaces the old even with the same text.
public struct ChatNotice: Equatable, Identifiable, Sendable {
    public let id = UUID()
    public let text: String

    public init(_ text: String) { self.text = text }
}
