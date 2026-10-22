import FMAccount
import FMApi
import FMBase
import Foundation

public struct ChatDirectRow: Equatable, Sendable, Identifiable {
    public let conversation: String
    public let name: String
    public let online: Bool
    public let unread: Int
    public let lastAt: Date?

    public var id: String { conversation }
}

extension ChatHub {
    public var ownNames: [String] {
        var names: [String] = []
        if let me = relay.roster().first(where: { $0.isSelf }) {
            names.append(me.name)
            if !me.owner.isEmpty { names.append(me.owner) }
        }
        return names
    }

    public var directoryChannels: [ChatDirectoryChannel] {
        var out: [ChatDirectoryChannel] = []
        for channel in channels where channel != meshConversation {
            out.append(ChatDirectoryChannel(conversation: channel, name: channel, detail: "Channel", isPrivate: false))
        }
        for room in office?.rooms ?? [] {
            let detail = room.members.isEmpty
                ? (room.isPrivate ? "Private channel" : "Channel")
                : "\(room.members.count) \(room.members.count == 1 ? "member" : "members")"
            out.append(ChatDirectoryChannel(conversation: room.conversation, name: "#\(room.name)", detail: detail, isPrivate: room.isPrivate))
        }
        out.append(ChatDirectoryChannel(conversation: meshConversation, name: meshConversation, detail: "Nearby devices"))
        return out
    }

    public func directorySource(searched: [String] = []) -> ChatDirectorySource {
        var source = ChatDirectorySource()
        source.own = ownNames
        source.recents = workspace.recentPeople
        source.roster = relay.rosterGroups()
        source.directs = direct?.records ?? []
        var members: [String] = []
        for room in office?.rooms ?? [] { members.append(contentsOf: room.members) }
        source.officeMembers = members
        var senders: [String] = []
        for conversation in relay.conversationsWithHistory {
            for message in relay.messages(conversation).suffix(100) where !message.isSelf && message.app == nil {
                senders.append(message.senderName)
            }
        }
        source.senders = senders
        source.searched = searched
        source.channels = directoryChannels
        return source
    }

    public func recipientSuggestions(_ query: String, selected: [ChatRecipient] = [], searched: [String] = [], limit: Int = 12)
        -> [ChatRecipient]
    {
        ChatDirectory.suggestions(directorySource(searched: searched), query: query, selected: selected, limit: limit)
    }

    public var canSearchAccounts: Bool { direct != nil && !isGuest() }

    public func searchAccounts(_ query: String) async -> [String] {
        guard let direct, !isGuest() else { return [] }
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        let bare = trimmed.hasPrefix("@") ? String(trimmed.dropFirst()) : trimmed
        if bare.count < 2 || trimmed.hasPrefix("#") { return [] }
        return await direct.searchUsers(bare)
    }

    public func conversation(for recipients: [ChatRecipient]) async -> String? {
        let people = recipients.filter { $0.kind == .person }
        let channels = recipients.filter { $0.kind == .channel }
        if people.isEmpty, channels.count == 1 { return channels[0].conversation }
        if recipients.isEmpty {
            post("Choose who to send it to.")
            return nil
        }
        if !channels.isEmpty {
            post("Send to one channel, or to people, not both.")
            return nil
        }
        if people.count == 1 { return await directConversation(with: people[0]) }
        return await groupConversation(with: people)
    }

    func directConversation(with person: ChatRecipient) async -> String? {
        if isGuest() {
            onGuestBlocked?("send direct messages")
            return nil
        }
        if DirectMessageFrames.isDirect(person.conversation) { return person.conversation }
        let account = ChatNodeOwners.normalized(person.account)
        if account.isEmpty {
            post("\(person.name.isEmpty ? "This person" : person.name) is a guest without an account, so there is no direct message to open.")
            return nil
        }
        guard let direct else {
            post("Sign in to send direct messages.")
            return nil
        }
        if let record = direct.records.first(where: { $0.otherUser == account }) { return record.conversation }
        do {
            return try await direct.start(username: account).conversation
        } catch {
            post("\(error)")
            return nil
        }
    }

    nonisolated public static func coveInviteText(inviterName: String, coveName: String, shareUrl: String) -> String {
        let who = inviterName.trimmingCharacters(in: .whitespacesAndNewlines)
        let cove = coveName.trimmingCharacters(in: .whitespacesAndNewlines)
        return "\(who.isEmpty ? "Someone" : "@" + who) invited you to the cove \u{201C}\(cove.isEmpty ? "untitled" : cove)\u{201D}: \(shareUrl)"
    }

    @discardableResult
    public func sendDirectNote(to account: String, text: String) async -> Bool {
        let name = ChatNodeOwners.normalized(account)
        guard let direct, !name.isEmpty, !isGuest(), !text.dartTrimmed.isEmpty else { return false }
        let conversation: String
        if let record = direct.records.first(where: { $0.otherUser == name }) {
            conversation = record.conversation
        } else {
            guard let started = try? await direct.start(username: name) else { return false }
            conversation = started.conversation
        }
        return await direct.send(conversation, text: text)
    }

    public func canMessage(_ member: ChatChannelMember) -> Bool {
        !member.isSelf && !member.account.isEmpty && !ChatFrameText.isIntegration(member.peerId) && !isGuest()
    }

    @discardableResult
    public func message(_ member: ChatChannelMember) async -> Bool {
        guard canMessage(member) else { return false }
        return await openPerson(
            ChatRecipient(kind: .person, name: member.name, account: member.account, peerId: member.peerId, online: member.online))
    }

    func accountRecipient(_ conversation: String) -> ChatRecipient? {
        guard conversation.hasPrefix("@") else { return nil }
        let name = String(conversation.dropFirst()).trimmingCharacters(in: .whitespaces)
        if name.isEmpty { return nil }
        let others = relay.roster().filter { !$0.isSelf }
        if let member = others.first(where: { $0.id == name }) {
            if member.owner.isEmpty { return ChatRecipient(kind: .person, name: member.name, peerId: member.id, online: member.online) }
            return ChatRecipient(
                kind: .person, name: member.name, account: member.owner.lowercased(), peerId: member.id, online: member.online)
        }
        let folded = name.lowercased()
        let matches = others.filter {
            $0.owner.lowercased() == folded || $0.name.lowercased() == folded || $0.nodeName.lowercased() == folded
        }
        let peer = matches.first(where: \.online) ?? matches.first
        let account = peer.map { $0.owner.isEmpty ? folded : $0.owner.lowercased() } ?? folded
        return ChatRecipient(
            kind: .person, name: peer?.name ?? name, account: account, peerId: peer?.id ?? "", online: peer?.online ?? false)
    }

    func groupConversation(with people: [ChatRecipient]) async -> String? {
        guard let office, !isGuest() else {
            if isGuest() { onGuestBlocked?("message several people at once") } else {
                post("Sign in to message several people at once.")
            }
            return nil
        }
        let names = people.map { $0.handle.lowercased() }
        let wanted = Set(names + ownNames.map { $0.lowercased() })
        if let existing = office.rooms.first(where: { room in
            guard room.isPrivate, !room.readOnly, !room.serverVisible, !room.members.isEmpty else { return false }
            let members = Set(room.members.map { $0.lowercased() })
            return Set(names).isSubset(of: members) && members.isSubset(of: wanted)
        }) {
            return existing.conversation
        }
        do {
            if let room = try await office.create(name: DirectMessageFrames.groupChannelName(names), isPrivate: true, members: names) {
                return room.conversation
            }
            post("The group could not be created.")
        } catch {
            post("The group could not be created: \(error)")
        }
        return nil
    }

    @discardableResult
    public func sendNewMessage(to recipients: [ChatRecipient], text: String) async -> Bool {
        guard let target = await conversation(for: recipients) else { return false }
        workspace.noteRecipients(recipients)
        select(target)
        if text.dartTrimmed.isEmpty { return true }
        let sent = await deliver(text, to: target)
        if !sent { workspace.setDraft(text, for: target) }
        return sent
    }

    public static func recipient(for group: MemberGroup) -> ChatRecipient {
        ChatRecipient(
            kind: .person, name: group.name, account: group.account, peerId: group.id, online: group.online,
            away: group.online && group.userStatus.away)
    }

    @discardableResult
    public func openPerson(_ person: ChatRecipient) async -> Bool {
        guard let target = await conversation(for: [person]) else { return false }
        workspace.noteRecipients([person])
        select(target)
        return true
    }

    func messagePerson(_ name: String, text: String) async -> Bool {
        let bare = name.trimmingCharacters(in: .whitespaces)
        if bare.isEmpty {
            post("Usage: /msg @person <message>")
            return false
        }
        let recipient = ChatDirectory.exact(directorySource(), name: bare) ?? ChatRecipient.person(bare)
        return await sendNewMessage(to: [recipient], text: text)
    }

    @discardableResult
    public func share(_ message: ChatMessage, text: String, comment: String, to recipients: [ChatRecipient]) async -> Bool {
        guard let target = await conversation(for: recipients) else { return false }
        workspace.noteRecipients(recipients)
        let body = ChatRichText.forwardText(
            comment: comment, sender: message.senderName, place: conversationLabel(message.conversation), text: text,
            fileName: message.fileName)
        let sent = await deliver(body, to: target)
        if sent { post("Shared to \(conversationLabel(target)).") }
        return sent
    }

    public func conversationLabel(_ conversation: String) -> String {
        if conversation == meshConversation { return meshConversation }
        if conversation == ChatEveryone.worldConversation { return ChatEveryone.worldLabel }
        if OfficeFrames.isOffice(conversation) { return "#" + conversation.dropFirst(OfficeFrames.prefix.count) }
        if DirectMessageFrames.isDirect(conversation) {
            return "@" + (direct?.record(for: conversation)?.otherUser ?? "direct message")
        }
        if conversation.hasPrefix("@") {
            let id = String(conversation.dropFirst())
            if let name = relay.roster().first(where: { $0.id == id })?.name { return "@\(name)" }
            if let name = relay.messages(conversation).first(where: { !$0.isSelf })?.senderName { return "@\(name)" }
            return "@direct message"
        }
        return conversation
    }

    public func messages(in conversation: String) -> [ChatMessage] {
        if conversation == meshConversation { return mesh.messages }
        if OfficeFrames.isOffice(conversation) { return office?.messages(conversation) ?? [] }
        if DirectMessageFrames.isDirect(conversation) { return direct?.messages(conversation) ?? [] }
        return relay.messages(conversation)
    }

    public var searchableConversations: [String] {
        var out: [String] = []
        for conversation in relay.conversationsWithHistory + (office?.conversations ?? []) + (direct?.conversations ?? [])
            + [meshConversation] where !out.contains(conversation)
        {
            out.append(conversation)
        }
        return out
    }

    public func searchMessages(_ raw: String, limit: Int = 200) -> [ChatSearchHit] {
        let query = ChatSearch.parse(raw)
        if query.isEmpty { return [] }
        var hits: [ChatSearchHit] = []
        for conversation in searchableConversations {
            let place = conversationLabel(conversation)
            for message in messages(in: conversation) where !extras.isHidden(message.id) {
                let text = extras.text(for: message)
                if ChatSearch.matches(message, text: text, place: place, query: query) {
                    hits.append(ChatSearchHit(message: message, place: place))
                }
            }
        }
        for reply in extras.allReplies where !extras.isHidden(reply.id) {
            let place = conversationLabel(reply.conversation)
            if ChatSearch.matches(reply, text: extras.text(for: reply), place: place, query: query) {
                hits.append(ChatSearchHit(message: reply, place: "Thread in \(place)"))
            }
        }
        hits.sort { $0.message.timestamp > $1.message.timestamp }
        return Array(hits.prefix(limit))
    }

    public func mentionHits(limit: Int = 100) -> [ChatSearchHit] {
        let names = ownNames
        if names.isEmpty { return [] }
        var hits: [ChatSearchHit] = []
        for conversation in searchableConversations {
            let place = conversationLabel(conversation)
            for message in messages(in: conversation) where !message.isSelf && !extras.isHidden(message.id) {
                if ChatSearch.mentions(extras.text(for: message), names: names) {
                    hits.append(ChatSearchHit(message: message, place: place))
                }
            }
        }
        hits.sort { $0.message.timestamp > $1.message.timestamp }
        return Array(hits.prefix(limit))
    }

    public func jumpTargets(_ query: String, limit: Int = 6) -> [ChatRecipient] {
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty { return [] }
        var out: [ChatRecipient] = []
        for directRow in directRows {
            let label = directRow.name
            if ChatDirectory.score(label, query: trimmed.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "@"))) != nil {
                out.append(ChatRecipient(kind: .person, name: label, conversation: directRow.conversation, detail: "Direct message"))
            }
        }
        for recipient in recipientSuggestions(trimmed, limit: limit) where !out.contains(where: { $0.key == recipient.key }) {
            out.append(recipient)
        }
        return Array(out.prefix(limit))
    }

    public var directRows: [ChatDirectRow] {
        var out: [ChatDirectRow] = []
        let roster = relay.roster()
        let groups = memberGroups.filter { !$0.isSelf }
        for record in direct?.records ?? [] where !workspace.isBlocked(record.otherUser) {
            let online = peerPresence(record.conversation, groups: groups, roster: roster) != .offline
            let last = direct?.messages(record.conversation).last?.timestamp
                ?? (record.updatedAt > 0 ? Date(epochMillis: record.updatedAt) : nil)
            if workspace.isRemoved(record.conversation, lastActivity: last) { continue }
            out.append(
                ChatDirectRow(conversation: record.conversation, name: record.otherUser, online: online, unread: record.unreadCount, lastAt: last))
        }
        out.sort { ($0.lastAt ?? .distantPast) > ($1.lastAt ?? .distantPast) }
        return out
    }

    public func recentConversations(limit: Int = 5) -> [String] {
        recentConversations(limit: limit, directRows: directRows)
    }

    public func recentConversations(limit: Int, directRows: [ChatDirectRow]) -> [String] {
        var stamped: [(conversation: String, at: Date)] = []
        var listed = Set<String>()
        for row in directRows where listed.insert(row.conversation).inserted {
            if let at = row.lastAt { stamped.append((row.conversation, at)) }
        }
        for conversation in listedChannels + [meshConversation] where listed.insert(conversation).inserted {
            if let at = listPreview(conversation)?.at { stamped.append((conversation, at)) }
        }
        return stamped.sorted { $0.at > $1.at }.prefix(max(0, limit)).map(\.conversation)
    }

    public static func isPersonConversation(_ conversation: String) -> Bool {
        conversation.hasPrefix("@") || DirectMessageFrames.isDirect(conversation)
    }

    public func deleteConversation(_ conversation: String) {
        for message in messages(in: conversation) { extras.hideMessage(message.id) }
        guard ChatHub.isPersonConversation(conversation) else { return }
        workspace.setStarred(conversation, false)
        workspace.setDraft("", for: conversation)
        workspace.removeConversation(conversation, at: clock.now)
        if DirectMessageFrames.isDirect(conversation) {
            direct?.clearUnread(conversation)
            if let direct { Task { await direct.markRead(conversation) } }
        } else {
            relay.markConversationRead(conversation)
        }
        if relay.currentConversation == conversation { select(generalConversation) }
    }

    public func conversationHandle(_ conversation: String) -> String {
        if DirectMessageFrames.isDirect(conversation) { return direct?.record(for: conversation)?.otherUser ?? "" }
        guard conversation.hasPrefix("@") else { return "" }
        let id = String(conversation.dropFirst())
        if let member = relay.roster().first(where: { $0.id == id }) {
            return member.owner.isEmpty ? member.name : member.owner
        }
        return relay.messages(conversation).first(where: { !$0.isSelf })?.senderName ?? ""
    }

    public func isBlocked(_ handle: String) -> Bool { workspace.isBlocked(handle) }

    public func setBlocked(_ handle: String, _ on: Bool) {
        let key = ChatWorkspace.blockKey(handle)
        if key.isEmpty || workspace.isBlocked(key) == on { return }
        workspace.setBlocked(key, on)
        pushBlocks()
        if on {
            for row in directRowsIncludingBlocked where ChatWorkspace.blockKey(conversationHandle(row)) == key {
                if relay.currentConversation == row { select(generalConversation) }
                if DirectMessageFrames.isDirect(row) { direct?.clearUnread(row) } else { relay.markConversationRead(row) }
            }
            post("\(key) is blocked. You won't see their messages on this device.")
        } else {
            post("\(key) is unblocked.")
        }
    }

    var directRowsIncludingBlocked: [String] { direct?.conversations ?? [] }

    nonisolated public static let blocksPath = "/api/chat/blocks"

    func pushBlocks() {
        guard let accountApi, !isGuest() else { return }
        let accounts = Array(workspace.blocked.suffix(ChatWorkspace.maxBlocked))
        let previous = blocksTask
        blocksTask = Task { @MainActor in
            await previous?.value
            _ = try? await accountApi.accountApiCall("PUT", ChatHub.blocksPath, ["accounts": JSONValue(accounts)])
        }
    }

    public func syncBlocks() async {
        guard let accountApi, !isGuest() else { return }
        guard let answer = try? await accountApi.accountApiCall("GET", ChatHub.blocksPath, nil) else { return }
        let server = (answer["accounts"]?.strings ?? []).map(ChatWorkspace.blockKey).filter { !$0.isEmpty }
        workspace.replaceBlocked(server + workspace.blocked.filter { !server.contains($0) })
        if Set(workspace.blocked) != Set(server) { pushBlocks() }
        await blocksTask?.value
    }

    func blockedSenderIds() -> Set<String> {
        if workspace.blocked.isEmpty { return [] }
        var ids: Set<String> = []
        for member in relay.roster() where !member.isSelf {
            if workspace.isBlocked(member.owner) || (member.owner.isEmpty && workspace.isBlocked(member.name)) {
                ids.insert(member.id)
            }
        }
        return ids
    }

    public func isFromBlocked(_ message: ChatMessage, blockedIds: Set<String>? = nil) -> Bool {
        if message.isSelf || workspace.blocked.isEmpty { return false }
        if DirectMessageFrames.isDirect(message.conversation) {
            return workspace.isBlocked(direct?.record(for: message.conversation)?.otherUser ?? "")
        }
        if workspace.isBlocked(message.senderName) { return true }
        return (blockedIds ?? blockedSenderIds()).contains(message.senderId)
    }

    public func isUnread(_ conversation: String) -> Bool {
        if workspace.isMuted(conversation) { return false }
        if conversation == meshConversation { return mesh.hasUnread }
        let unread: Bool
        if DirectMessageFrames.isDirect(conversation) {
            unread = (direct?.record(for: conversation)?.unreadCount ?? 0) > 0
        } else {
            unread = relay.hasUnread(conversation)
        }
        if unread && workspace.notifyLevel(conversation) == .mentions { return hasUnreadMention(conversation) }
        return unread
    }

    public func toggleSaved(_ message: ChatMessage, text: String) {
        if workspace.isSaved(message.id) {
            workspace.unsave(message.id)
            post("Removed from saved items.")
            return
        }
        workspace.save(
            ChatSavedItem(
                id: message.id, conversation: message.conversation, place: conversationLabel(message.conversation),
                senderName: message.senderName, text: text, fileName: message.fileName, sentAt: message.timestamp,
                savedAt: clock.now))
        post("Saved for later.")
    }

    public func toggleMuted(_ conversation: String) {
        let muted = !workspace.isMuted(conversation)
        workspace.setMuted(conversation, muted)
        post(muted ? "\(conversationLabel(conversation)) is muted." : "\(conversationLabel(conversation)) is unmuted.")
    }

    public func toggleStarred(_ conversation: String) {
        workspace.setStarred(conversation, !workspace.isStarred(conversation))
    }

    public func open(_ conversation: String) {
        if conversation.hasPrefix("#") && !conversation.hasPrefix(OfficeFrames.prefix) && conversation != meshConversation
            && !relay.channels.contains(conversation)
        {
            relay.addChannel(conversation)
        }
        select(conversation)
    }
}
