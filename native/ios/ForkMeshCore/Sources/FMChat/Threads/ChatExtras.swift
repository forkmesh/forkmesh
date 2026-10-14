import FMApi
import FMBase
import Foundation
import Observation

public enum ChatAttachmentKind: String, Equatable, Sendable {
    case image, audio, video, file

    public static func of(fileName: String, mime: String) -> ChatAttachmentKind {
        let type = mime.lowercased()
        if type.hasPrefix("image/") { return .image }
        if type.hasPrefix("audio/") { return .audio }
        if type.hasPrefix("video/") { return .video }
        let ext = (fileName as NSString).pathExtension.lowercased()
        if ["png", "jpg", "jpeg", "gif", "heic", "webp", "bmp"].contains(ext) { return .image }
        if ["mp3", "m4a", "aac", "wav", "ogg", "opus", "flac", "caf"].contains(ext) { return .audio }
        if ["mp4", "mov", "m4v", "webm"].contains(ext) { return .video }
        return .file
    }
}

public struct ChatAttachment: Equatable, Sendable {
    public let messageId: String
    public let fileName: String
    public let mime: String
    public let data: Data

    public init(messageId: String, fileName: String, mime: String, data: Data) {
        self.messageId = messageId
        self.fileName = fileName
        self.mime = mime
        self.data = data
    }

    public var kind: ChatAttachmentKind { ChatAttachmentKind.of(fileName: fileName, mime: mime) }
}

public struct ChatThreadSummary: Equatable, Sendable {
    public let rootId: String
    public let replyCount: Int
    public let latestReplier: String
    public let latestAt: Date?
    public let faces: [String]

    public var label: String { replyCount == 1 ? "1 reply" : "\(replyCount) replies" }
}

public struct ReactionChip: Equatable, Sendable, Identifiable {
    public let value: String
    public let count: Int
    public let names: [String]
    public let mine: Bool

    public var id: String { value }
    public var emoji: String { ReactionCatalog.emoji(for: value) }
    public var label: String { ReactionCatalog.label(for: value) }
}

public struct ChatTypingPeer: Equatable, Sendable {
    public let name: String
    public let until: Date
}

@MainActor
@Observable
public final class ChatExtras {
    nonisolated public static let maxReactionsPerMessage = 32
    nonisolated public static let maxReactorsPerReaction = 500
    nonisolated public static let maxRepliesPerThread = 500

    @ObservationIgnored let clock: Clock
    @ObservationIgnored public weak var sink: ChatFrameSink?
    @ObservationIgnored public var authorOf: (@MainActor (String, String) -> String?)?
    @ObservationIgnored public var onChannelJoined: (@MainActor (String) -> Void)?
    @ObservationIgnored public var onChannelRemoved: (@MainActor (String) -> Void)?
    @ObservationIgnored public var onChange: (@MainActor () -> Void)?
    @ObservationIgnored public var onRemoved: (@MainActor (String, String) -> Void)?
    @ObservationIgnored private var pendingEdits: [String: JSONObject] = [:]
    @ObservationIgnored private var pendingEditOrder: [String] = []
    @ObservationIgnored private var removedOrder: [String] = []
    @ObservationIgnored public var isExpired: ChatExpiry?
    @ObservationIgnored private var lastTypingSent: [String: Date] = [:]

    public private(set) var revision = 0
    public private(set) var privateChannels: Set<String> = []
    public private(set) var hiddenChannels: Set<String> = []
    public private(set) var inviteNotice: ChatNotice?

    private var replies: [String: [ChatMessage]] = [:]
    private var replyRoot: [String: String] = [:]
    private var rootChannel: [String: String] = [:]
    private var reactionState: [String: [String: [String: String]]] = [:]
    private var reactionOrder: [String: [String]] = [:]
    private var editedText: [String: String] = [:]
    private var typingState: [String: [String: ChatTypingPeer]] = [:]
    private var files: [String: ChatAttachment] = [:]
    @ObservationIgnored private var fileOrder: [String] = []
    @ObservationIgnored private var fileBytes = 0
    private var hiddenMessageSet: Set<String> = []
    private var removedSet: Set<String> = []
    private var looks: [String: ChatLook] = [:]
    private var lookEventLists: [String: [ChatLookEvent]] = [:]
    @ObservationIgnored private var hiddenMessageOrder: [String] = []

    public init(clock: Clock = SystemClock()) {
        self.clock = clock
    }

    private var selfId: String { sink?.selfId ?? "" }

    private func changed() {
        revision &+= 1
        onChange?()
    }

    public func clearInviteNotice() { inviteNotice = nil }

    public func replies(to rootId: String) -> [ChatMessage] { replies[rootId] ?? [] }

    public var allReplies: [ChatMessage] { replies.values.flatMap { $0 } }

    public func threadSummary(_ rootId: String) -> ChatThreadSummary? {
        let list = (replies[rootId] ?? []).filter { !hiddenMessageSet.contains($0.id) }
        guard let last = list.last else { return nil }
        var faces: [String] = []
        for message in list.reversed() where !faces.contains(message.senderName) {
            faces.append(message.senderName)
            if faces.count == 3 { break }
        }
        return ChatThreadSummary(
            rootId: rootId, replyCount: list.count, latestReplier: last.senderName, latestAt: last.timestamp,
            faces: faces)
    }

    public func isThreadReply(_ id: String) -> Bool { replyRoot[id] != nil }

    public func isHidden(_ id: String) -> Bool { hiddenMessageSet.contains(id) }

    public func hideMessage(_ id: String) {
        if !rememberHidden(id) { return }
        trimHidden()
        changed()
    }

    public func snapshotHiddenMessages() -> [String] { hiddenMessageOrder }

    public func isRemoved(_ id: String) -> Bool { removedSet.contains(id) }

    public func snapshotRemovedMessages() -> [String] { removedOrder }

    public func recordRemoved(_ id: String) {
        if id.isEmpty { return }
        forget(id)
        if removedSet.insert(id).inserted {
            removedOrder.append(id)
            let excess = removedOrder.count - ChatLimits.hiddenMessages
            if excess > 0 {
                for old in removedOrder.prefix(excess) { removedSet.remove(old) }
                removedOrder.removeFirst(excess)
            }
        }
        changed()
    }

    public func restoreRemovedMessages(_ ids: [String]) {
        var added = false
        for id in ids where !id.isEmpty && removedSet.insert(id).inserted {
            removedOrder.append(id)
            added = true
        }
        if added { changed() }
    }

    public func restoreHiddenMessages(_ ids: [String]) {
        var added = false
        for id in ids where rememberHidden(id) { added = true }
        if !added { return }
        trimHidden()
        changed()
    }

    private func rememberHidden(_ id: String) -> Bool {
        if id.isEmpty || !hiddenMessageSet.insert(id).inserted { return false }
        hiddenMessageOrder.append(id)
        return true
    }

    private func trimHidden() {
        let excess = hiddenMessageOrder.count - ChatLimits.hiddenMessages
        if excess <= 0 { return }
        for id in hiddenMessageOrder.prefix(excess) { hiddenMessageSet.remove(id) }
        hiddenMessageOrder.removeFirst(excess)
    }

    public func rootOfReply(_ id: String) -> String? { replyRoot[id] }

    public func text(for message: ChatMessage) -> String { editedText[message.id] ?? message.text }

    public func isEdited(_ id: String) -> Bool { editedText[id] != nil }

    public func reactions(for messageId: String, selfId roomSelfId: String? = nil) -> [ReactionChip] {
        guard let state = reactionState[messageId] else { return [] }
        let me = roomSelfId ?? selfId
        return (reactionOrder[messageId] ?? []).compactMap { value in
            guard let reactors = state[value], !reactors.isEmpty else { return nil }
            let names = reactors.keys.sorted().map { reactors[$0] ?? "" }.filter { !$0.isEmpty }
            return ReactionChip(value: value, count: reactors.count, names: names, mine: reactors[me] != nil)
        }
    }

    public func attachment(for messageId: String) -> ChatAttachment? { files[messageId] }

    public func attachments(in messageIds: [String]) -> [ChatAttachment] { messageIds.compactMap { files[$0] } }

    public func look(for conversation: String) -> ChatLook { looks[conversation] ?? ChatLook() }

    public func lookEvents(in conversation: String) -> [ChatLookEvent] { lookEventLists[conversation] ?? [] }

    public func typingNames(in conversation: String) -> [String] {
        let now = clock.now
        let peers = typingState[conversation] ?? [:]
        var names: [String] = []
        for key in peers.keys.sorted() {
            guard let peer = peers[key], peer.until > now else { continue }
            if !names.contains(peer.name) { names.append(peer.name) }
        }
        return names
    }

    public nonisolated static func typingLabel(_ names: [String]) -> String {
        switch names.count {
        case 0: return ""
        case 1: return "\(names[0]) is typing..."
        case 2: return "\(names[0]) and \(names[1]) are typing..."
        default: return "\(names.count) people are typing..."
        }
    }

    public func isPrivate(_ channel: String) -> Bool { privateChannels.contains(channel) }

    public func apply(_ frame: JSONObject) {
        let type = frame["type"]?.text ?? ""
        let senderId = frame["senderId"]?.text ?? ""
        let conversation = ChatFrameText.conversation(of: frame)
        if conversation == "#team" { return }
        switch type {
        case "chat", "dm":
            rememberFile(frame)
            if frame["private"]?.isTrue == true, conversation.hasPrefix("#"), !hiddenChannels.contains(conversation) {
                if privateChannels.insert(conversation).inserted { changed() }
            }
        case "thread-reply":
            applyReply(frame, senderId: senderId)
        case "reaction":
            applyReaction(frame, senderId: senderId)
        case "edit":
            applyEdit(frame, senderId: senderId, conversation: conversation)
        case "delete":
            applyDelete(frame, senderId: senderId)
        case "typing":
            applyTyping(frame, senderId: senderId, conversation: conversation)
        case "invite":
            applyInvite(frame)
        case "channel":
            let name = ChatExtras.normalizedChannel(frame["name"]?.text ?? "")
            if !name.isEmpty, name != "#team", !hiddenChannels.contains(name) { onChannelJoined?(name) }
        case "history":
            applyHistory(frame)
        case ChatLookFrames.type:
            applyLook(frame, senderId: senderId, conversation: relayConversation(frame, senderId: senderId))
        default:
            break
        }
    }

    public func applyRoomFrame(_ frame: JSONObject, conversation: String, selfId roomSelfId: String) {
        let type = frame["type"]?.text ?? ""
        let senderId = frame["senderId"]?.text ?? ""
        let id = frame["id"]?.text ?? ""
        switch type {
        case "chat":
            rememberFile(frame)
            retryPendingEdit(id, conversation: conversation)
        case ChatLookFrames.type:
            applyLook(frame, senderId: senderId, conversation: conversation, selfId: roomSelfId)
        case "thread-reply":
            if removedSet.contains(id) { return }
            applyReply(frame, senderId: senderId, channel: conversation, selfId: roomSelfId)
            retryPendingEdit(id, conversation: conversation)
        case "reaction":
            applyReaction(frame, senderId: senderId)
        case "edit":
            let target = frame["target"]?.text ?? ""
            if !target.isEmpty, author(of: target, conversation: conversation) == nil {
                holdEdit(frame, target: target)
            } else {
                applyEdit(frame, senderId: senderId, conversation: conversation)
            }
        case "delete":
            applyRoomDelete(frame, senderId: senderId, conversation: conversation)
        default:
            break
        }
    }

    public nonisolated static func reactionFields(added: Bool, messageId: String, value: String, selfId: String) -> JSONObject {
        ["target": .string(messageId), "emoji": .string(value), "reactorId": .string(selfId), "added": .bool(added)]
    }

    public func reactionAdds(messageId: String, value: String, selfId: String) -> Bool {
        reactionState[messageId]?[value]?[selfId] == nil
    }

    public func roomAuthor(of id: String, conversation: String) -> String? { author(of: id, conversation: conversation) }

    private func holdEdit(_ frame: JSONObject, target: String) {
        if pendingEdits.updateValue(frame, forKey: target) == nil { pendingEditOrder.append(target) }
        while pendingEditOrder.count > DirectMessageFrames.maxPendingDeletes {
            pendingEdits.removeValue(forKey: pendingEditOrder.removeFirst())
        }
    }

    private func retryPendingEdit(_ id: String, conversation: String) {
        guard !id.isEmpty, let frame = pendingEdits.removeValue(forKey: id) else { return }
        pendingEditOrder.removeAll { $0 == id }
        applyEdit(frame, senderId: frame["senderId"]?.text ?? "", conversation: conversation)
    }

    private func applyRoomDelete(_ frame: JSONObject, senderId: String, conversation: String) {
        let target = frame["target"]?.text ?? ""
        guard !target.isEmpty, !senderId.isEmpty, let root = replyRoot[target] else { return }
        guard var list = replies[root], let reply = list.first(where: { $0.id == target }), reply.senderId == senderId else {
            return
        }
        list.removeAll { $0.id == target }
        replies[root] = list.isEmpty ? nil : list
        replyRoot.removeValue(forKey: target)
        recordRemoved(target)
        onRemoved?(conversation, target)
    }

    private func relayConversation(_ frame: JSONObject, senderId: String) -> String {
        let conversation = ChatFrameText.conversation(of: frame)
        guard conversation.hasPrefix("@") else { return conversation }
        if !senderId.isEmpty && senderId == selfId { return conversation }
        guard !senderId.isEmpty, (frame["to"]?.text ?? "") == selfId, !selfId.isEmpty else { return "" }
        return "@\(senderId)"
    }

    private func applyLook(_ frame: JSONObject, senderId: String, conversation: String, selfId lookSelfId: String? = nil) {
        if conversation.isEmpty || conversation == "#team" || hiddenChannels.contains(conversation) || senderId.isEmpty { return }
        let id = frame["id"]?.text ?? ""
        guard !id.isEmpty, let change = ChatLookFrames.change(from: frame) else { return }
        let millis = frame["ts"]?.doubleValue.map { Int64($0) } ?? clock.nowMillis
        let timestamp = Date(epochMillis: millis)
        if isExpired?(conversation, timestamp) == true { return }
        var events = lookEventLists[conversation] ?? []
        if events.contains(where: { $0.id == id }) { return }
        let me = lookSelfId ?? selfId
        let event = ChatLookEvent(
            id: id, conversation: conversation, field: change.field,
            actorName: ChatFrameText.bounded(frame["sender"], ChatLimits.maxDisplayNameChars),
            isSelf: !me.isEmpty && senderId == me, timestamp: timestamp, cleared: change.cleared, value: change.name)
        var at = events.count
        while at > 0 && events[at - 1].timestamp > timestamp { at -= 1 }
        events.insert(event, at: at)
        if events.count > ChatLookFrames.maxEventsPerConversation {
            events.removeFirst(events.count - ChatLookFrames.maxEventsPerConversation)
        }
        lookEventLists[conversation] = events
        if let next = ChatLookFrames.applying(change, to: look(for: conversation), stamp: ChatLookStamp(ts: millis, id: id)) {
            looks[conversation] = next
        }
        changed()
    }

    private func applyReply(_ frame: JSONObject, senderId: String, channel room: String? = nil, selfId roomSelfId: String? = nil) {
        let rootId = String((frame["rootId"]?.text ?? "").trimmingCharacters(in: .whitespaces).prefix(ChatLimits.maxRootIdChars))
        let channel = room ?? (frame["channel"]?.text ?? "")
        let id = frame["id"]?.text ?? ""
        let me = roomSelfId ?? selfId
        if rootId.isEmpty || id.isEmpty || channel.isEmpty || hiddenChannels.contains(channel) { return }
        if room == nil && !channel.hasPrefix("#") { return }
        if replyRoot[id] != nil { return }
        let millis = frame["ts"]?.doubleValue.map { Int64($0) } ?? clock.nowMillis
        let message = ChatMessage(
            id: id, conversation: channel, senderId: senderId,
            senderName: ChatFrameText.bounded(frame["sender"], ChatLimits.maxDisplayNameChars),
            text: ChatFrameText.bounded(frame["text"], ChatLimits.maxTextChars), timestamp: Date(epochMillis: millis),
            isSelf: !senderId.isEmpty && senderId == me,
            fileName: ChatFrameText.bounded(frame["fileName"], ChatLimits.maxFileNameChars))
        if isExpired?(channel, message.timestamp) == true { return }
        var list = replies[rootId] ?? []
        var at = list.count
        while at > 0 && RelayService.compareMessages(list[at - 1], message) > 0 { at -= 1 }
        list.insert(message, at: at)
        if list.count > ChatExtras.maxRepliesPerThread {
            let dropped = list.removeFirst()
            replyRoot.removeValue(forKey: dropped.id)
        }
        replies[rootId] = list
        replyRoot[id] = rootId
        rootChannel[rootId] = channel
        if frame["private"]?.isTrue == true { privateChannels.insert(channel) }
        rememberFile(frame)
        changed()
    }

    private func applyReaction(_ frame: JSONObject, senderId: String) {
        let target = frame["target"]?.text ?? ""
        let raw = frame["emoji"]?.text ?? ""
        let reactorId = frame["reactorId"]?.text ?? senderId
        if target.isEmpty || raw.isEmpty || raw.count > 16 || reactorId.isEmpty { return }
        if !senderId.isEmpty && reactorId != senderId { return }
        let value = ReactionCatalog.value(forEmoji: raw)
        let name = String((frame["reactorName"]?.text ?? frame["sender"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars))
        var state = reactionState[target] ?? [:]
        var order = reactionOrder[target] ?? []
        var reactors = state[value] ?? [:]
        if frame["added"]?.isTrue == true {
            if reactors[reactorId] != nil { return }
            if state[value] == nil && state.count >= ChatExtras.maxReactionsPerMessage { return }
            if reactors.count >= ChatExtras.maxReactorsPerReaction { return }
            reactors[reactorId] = name
            if !order.contains(value) { order.append(value) }
        } else {
            if reactors.removeValue(forKey: reactorId) == nil { return }
        }
        state[value] = reactors.isEmpty ? nil : reactors
        order = order.filter { state[$0] != nil }
        reactionState[target] = state.isEmpty ? nil : state
        reactionOrder[target] = order.isEmpty ? nil : order
        changed()
    }

    private func author(of target: String, conversation: String) -> String? {
        if let root = replyRoot[target] { return replies[root]?.first { $0.id == target }?.senderId }
        return authorOf?(conversation, target)
    }

    private func applyEdit(_ frame: JSONObject, senderId: String, conversation: String) {
        let target = frame["target"]?.text ?? ""
        let text = ChatFrameText.bounded(frame["text"], ChatLimits.maxTextChars).trimmingCharacters(in: .whitespacesAndNewlines)
        if target.isEmpty || text.isEmpty || senderId.isEmpty { return }
        guard let author = author(of: target, conversation: conversation), author == senderId else { return }
        editedText[target] = text
        changed()
    }

    private func applyDelete(_ frame: JSONObject, senderId: String) {
        let target = frame["target"]?.text ?? ""
        if target.isEmpty { return }
        if let root = replyRoot[target] {
            guard var list = replies[root], let reply = list.first(where: { $0.id == target }), reply.senderId == senderId else {
                return
            }
            list.removeAll { $0.id == target }
            replies[root] = list.isEmpty ? nil : list
            replyRoot.removeValue(forKey: target)
        } else if let author = authorOf?(ChatFrameText.conversation(of: frame), target), author != senderId {
            return
        }
        forget(target)
        changed()
        onRemoved?(ChatFrameText.conversation(of: frame), target)
    }

    private func forget(_ id: String) {
        editedText.removeValue(forKey: id)
        reactionState.removeValue(forKey: id)
        reactionOrder.removeValue(forKey: id)
        if let file = files.removeValue(forKey: id) {
            fileBytes -= file.data.count
            fileOrder.removeAll { $0 == id }
        }
    }

    private func applyTyping(_ frame: JSONObject, senderId: String, conversation: String) {
        if senderId.isEmpty || senderId == selfId || conversation.isEmpty { return }
        let target = conversation.hasPrefix("@") ? "@\(senderId)" : conversation
        var peers = typingState[target] ?? [:]
        if frame["active"]?.isTrue == true {
            let name = String((frame["sender"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars))
            peers[senderId] = ChatTypingPeer(
                name: name.isEmpty ? "Someone" : name, until: clock.now.addingTimeInterval(ChatLimits.typingTimeout))
        } else if peers.removeValue(forKey: senderId) == nil {
            return
        }
        typingState[target] = peers.isEmpty ? nil : peers
    }

    private func applyInvite(_ frame: JSONObject) {
        if (frame["to"]?.text ?? "") != selfId || selfId.isEmpty { return }
        let channel = ChatExtras.normalizedChannel(String((frame["channel"]?.text ?? "").prefix(120)))
        if channel.isEmpty || channel == "#team" || hiddenChannels.contains(channel) { return }
        privateChannels.insert(channel)
        onChannelJoined?(channel)
        let sender = frame["sender"]?.text ?? "Someone"
        inviteNotice = ChatNotice("\(sender) invited you to private room \(channel).")
        changed()
    }

    private func applyHistory(_ frame: JSONObject) {
        let to = frame["to"]?.text ?? ""
        if !to.isEmpty && to != selfId { return }
        for entry in frame["entries"]?.objects.prefix(ChatLimits.historyEntriesPerChannel) ?? [] {
            let type = entry["type"]?.text ?? ""
            if type != "chat" && type != "thread-reply" { continue }
            let channel = entry["channel"]?.text ?? ""
            if !channel.hasPrefix("#") || channel == "#team" || hiddenChannels.contains(channel) { continue }
            if entry["private"]?.isTrue == true && !privateChannels.contains(channel) { continue }
            sink?.applyFrame(entry)
        }
    }

    private func rememberFile(_ frame: JSONObject) {
        let id = frame["id"]?.text ?? ""
        let name = ChatFrameText.bounded(frame["fileName"], ChatLimits.maxFileNameChars)
        guard !id.isEmpty, !name.isEmpty, files[id] == nil else { return }
        let encoded = frame["file"]?.text ?? ""
        if encoded.isEmpty || encoded.count > (ChatLimits.maxFileBytes / 3 + 1) * 4 { return }
        guard let data = Data(base64Encoded: encoded), !data.isEmpty, data.count <= ChatLimits.maxFileBytes else { return }
        store(ChatAttachment(messageId: id, fileName: name, mime: ChatFrameText.bounded(frame["fileMime"], ChatLimits.maxMimeChars), data: data))
    }

    public func store(_ attachment: ChatAttachment) {
        if files[attachment.messageId] != nil { return }
        files[attachment.messageId] = attachment
        fileOrder.append(attachment.messageId)
        fileBytes += attachment.data.count
        while fileBytes > ChatLimits.attachmentBudgetBytes, fileOrder.count > 1 {
            let oldest = fileOrder.removeFirst()
            if let dropped = files.removeValue(forKey: oldest) { fileBytes -= dropped.data.count }
        }
        changed()
    }

    public nonisolated static func normalizedChannel(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty || trimmed == "#" { return "" }
        return trimmed.hasPrefix("#") ? trimmed : "#\(trimmed)"
    }

    public nonisolated static func validChannelName(_ raw: String) -> Bool {
        let name = normalizedChannel(raw)
        if name.count < 2 || name.count > 41 || name == "#team" { return false }
        return name.dropFirst().allSatisfy { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" }
    }

    @discardableResult
    public func createPrivateChannel(_ raw: String) -> String? {
        guard ChatExtras.validChannelName(raw) else { return nil }
        let name = ChatExtras.normalizedChannel(raw).lowercased()
        hiddenChannels.remove(name)
        privateChannels.insert(name)
        onChannelJoined?(name)
        changed()
        return name
    }

    @discardableResult
    public func addPublicChannel(_ raw: String) -> String? {
        guard ChatExtras.validChannelName(raw), let sink else { return nil }
        let name = ChatExtras.normalizedChannel(raw).lowercased()
        hiddenChannels.remove(name)
        onChannelJoined?(name)
        var frame = sink.makeFrame("channel")
        frame["name"] = .string(name)
        sink.applyFrame(frame)
        sink.sendFrame(frame)
        changed()
        return name
    }

    public func removeChannel(_ channel: String) {
        if ChatEveryone.contains(channel) { return }
        hiddenChannels.insert(channel)
        privateChannels.remove(channel)
        looks.removeValue(forKey: channel)
        lookEventLists.removeValue(forKey: channel)
        for (root, owner) in rootChannel where owner == channel {
            for reply in replies[root] ?? [] { replyRoot.removeValue(forKey: reply.id) }
            replies.removeValue(forKey: root)
            rootChannel.removeValue(forKey: root)
        }
        onChannelRemoved?(channel)
        changed()
    }

    public func restoreChannels(privateChannels: [String], hiddenChannels: [String]) {
        self.hiddenChannels.formUnion(hiddenChannels.filter { !ChatEveryone.contains($0) })
        for channel in privateChannels where !self.hiddenChannels.contains(channel) {
            self.privateChannels.insert(channel)
            onChannelJoined?(channel)
        }
        changed()
    }

    private func routed(_ frame: inout JSONObject, conversation: String, persist: Bool) {
        frame["conversation"] = .string(conversation)
        if conversation.hasPrefix("#") {
            frame["channel"] = .string(conversation)
            if privateChannels.contains(conversation) { frame["private"] = true }
            if persist { frame["persist"] = true }
        } else if conversation.hasPrefix("@") {
            frame["to"] = .string(String(conversation.dropFirst()))
        }
    }

    private func takeBack(_ id: String, conversation: String) {
        guard let sink else { return }
        var undo = sink.makeFrame("delete")
        undo["conversation"] = .string(conversation)
        undo["target"] = .string(id)
        sink.applyFrame(undo)
    }

    @discardableResult
    public func sendPrivateChat(_ channel: String, text: String) -> Bool {
        let body = String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxTextChars))
        guard let sink, !body.isEmpty, channel.hasPrefix("#") else { return false }
        var frame = sink.makeFrame("chat")
        frame["text"] = .string(body)
        routed(&frame, conversation: channel, persist: true)
        frame["private"] = true
        sink.applyFrame(frame)
        if sink.sendFrame(frame) { return true }
        takeBack(frame["id"]?.text ?? "", conversation: channel)
        return false
    }

    @discardableResult
    public func sendText(conversation: String, text: String) async -> Bool {
        if privateChannels.contains(conversation) { return sendPrivateChat(conversation, text: text) }
        guard let sink else { return false }
        return await sink.sendMessage(String(text.prefix(ChatLimits.maxTextChars)), to: conversation)
    }

    @discardableResult
    public func sendBotReply(channel: String, rootId: String, text: String) -> Bool {
        let body = String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxTextChars))
        guard let sink, !body.isEmpty, channel.hasPrefix("#"), !privateChannels.contains(channel) else { return false }
        let root = String(rootId.prefix(ChatLimits.maxRootIdChars))
        var frame: JSONObject = [
            "type": .string(root.isEmpty ? "chat" : "thread-reply"),
            "id": .string(UUID().uuidString.lowercased()),
            "senderId": "forkbot",
            "sender": "forkbot",
            "accountKind": "user",
            "ts": .int(clock.nowMillis),
            "channel": .string(channel),
            "text": .string(body),
        ]
        if !root.isEmpty { frame["rootId"] = .string(root) }
        sink.applyFrame(frame)
        return sink.sendFrame(frame)
    }

    @discardableResult
    public func sendThreadReply(channel: String, rootId: String, text: String) -> Bool {
        let body = String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxTextChars))
        let root = String(rootId.trimmingCharacters(in: .whitespaces).prefix(ChatLimits.maxRootIdChars))
        guard let sink, !body.isEmpty, !root.isEmpty, channel.hasPrefix("#") else { return false }
        var frame = sink.makeFrame("thread-reply")
        frame["rootId"] = .string(root)
        frame["text"] = .string(body)
        routed(&frame, conversation: channel, persist: true)
        sink.applyFrame(frame)
        if sink.sendFrame(frame) { return true }
        takeBack(frame["id"]?.text ?? "", conversation: channel)
        return false
    }

    @discardableResult
    public func toggleReaction(conversation: String, messageId: String, value: String) -> Bool {
        guard let sink, !messageId.isEmpty, !value.isEmpty else { return false }
        let me = sink.selfId
        let added = reactionState[messageId]?[value]?[me] == nil
        var frame = sink.makeFrame("reaction")
        frame["target"] = .string(messageId)
        frame["emoji"] = .string(value)
        frame["reactorId"] = .string(me)
        frame["reactorName"] = .string(frame["sender"]?.text ?? "")
        frame["added"] = .bool(added)
        routed(&frame, conversation: conversation, persist: true)
        sink.applyFrame(frame)
        if sink.sendFrame(frame) { return true }
        var undo = sink.makeFrame("reaction")
        for (key, value) in frame where key != "id" && key != "ts" { undo[key] = value }
        undo["added"] = .bool(!added)
        sink.applyFrame(undo)
        return false
    }

    @discardableResult
    public func edit(conversation: String, messageId: String, text: String) -> Bool {
        let body = String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxTextChars))
        guard let sink, !body.isEmpty, !messageId.isEmpty else { return false }
        guard let author = author(of: messageId, conversation: conversation), author == sink.selfId else { return false }
        var frame = sink.makeFrame("edit")
        frame["target"] = .string(messageId)
        frame["text"] = .string(body)
        routed(&frame, conversation: conversation, persist: true)
        let previous = editedText[messageId]
        sink.applyFrame(frame)
        if sink.sendFrame(frame) { return true }
        editedText[messageId] = previous
        changed()
        return false
    }

    @discardableResult
    public func deleteReply(_ reply: ChatMessage) -> Bool {
        guard let sink, reply.senderId == sink.selfId, replyRoot[reply.id] != nil else { return false }
        var frame = sink.makeFrame("delete")
        frame["target"] = .string(reply.id)
        routed(&frame, conversation: reply.conversation, persist: true)
        sink.applyFrame(frame)
        return sink.sendFrame(frame)
    }

    @discardableResult
    public func sendTyping(conversation: String, active: Bool) -> Bool {
        guard let sink, conversation.hasPrefix("#") || conversation.hasPrefix("@") else { return false }
        let now = clock.now
        if active {
            if let last = lastTypingSent[conversation], now.timeIntervalSince(last) < ChatLimits.typingResend { return false }
            lastTypingSent[conversation] = now
        } else {
            if lastTypingSent.removeValue(forKey: conversation) == nil { return false }
        }
        var frame = sink.makeFrame("typing")
        frame["active"] = .bool(active)
        routed(&frame, conversation: conversation, persist: false)
        frame.removeValue(forKey: "channel")
        return sink.sendFrame(frame)
    }

    @discardableResult
    public func invite(peerId: String, to channel: String) -> Bool {
        guard let sink, !peerId.isEmpty, peerId != sink.selfId, privateChannels.contains(channel), channel != "#team" else {
            return false
        }
        var frame = sink.makeFrame("invite")
        frame["to"] = .string(peerId)
        frame["channel"] = .string(channel)
        return sink.sendFrame(frame)
    }

    @discardableResult
    public func sendFile(conversation: String, fileName: String, mime: String, data: Data) -> Bool {
        let name = String(fileName.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxFileNameChars))
        guard let sink, !name.isEmpty, !data.isEmpty, data.count <= ChatFileLimits.maxSendBytes else { return false }
        var frame = sink.makeFrame(conversation.hasPrefix("@") ? "dm" : "chat")
        frame["fileName"] = .string(name)
        frame["fileMime"] = .string(String(mime.prefix(ChatLimits.maxMimeChars)))
        frame["file"] = .string(data.base64EncodedString())
        routed(&frame, conversation: conversation, persist: true)
        if frame["channel"] == nil { frame.removeValue(forKey: "persist") }
        let id = frame["id"]?.text ?? ""
        store(ChatAttachment(messageId: id, fileName: name, mime: mime, data: data))
        sink.applyFrame(frame)
        if sink.sendFrame(frame) { return true }
        takeBack(id, conversation: conversation)
        forget(id)
        return false
    }

    @discardableResult
    public func sendLook(conversation: String, change: ChatLookChange) -> Bool {
        guard let sink, conversation.hasPrefix("#") || conversation.hasPrefix("@") else { return false }
        var frame = sink.makeFrame(ChatLookFrames.type)
        for (key, value) in ChatLookFrames.fields(change) { frame[key] = value }
        routed(&frame, conversation: conversation, persist: true)
        if !sink.sendFrame(frame) { return false }
        sink.applyFrame(frame)
        return true
    }

    @discardableResult
    public func pruneExpired() -> Bool {
        guard let isExpired else { return false }
        var pruned = false
        for (conversation, events) in lookEventLists {
            let kept = events.filter { !isExpired(conversation, $0.timestamp) }
            if kept.count == events.count { continue }
            pruned = true
            lookEventLists[conversation] = kept.isEmpty ? nil : kept
        }
        for (root, list) in replies {
            let kept = list.filter { !isExpired($0.conversation, $0.timestamp) }
            if kept.count == list.count { continue }
            pruned = true
            let keptIds = Set(kept.map(\.id))
            for reply in list where !keptIds.contains(reply.id) {
                replyRoot.removeValue(forKey: reply.id)
                forget(reply.id)
            }
            if kept.isEmpty {
                replies.removeValue(forKey: root)
                rootChannel.removeValue(forKey: root)
            } else {
                replies[root] = kept
            }
        }
        if pruned { changed() }
        return pruned
    }

    public func snapshotThreads() -> [ChatMessage] { replies.values.flatMap { $0 } }

    public func snapshotReplyRoots() -> [String: String] { replyRoot }

    public func snapshotReactions() -> [String: [String: [String: String]]] { reactionState }

    public func snapshotReactionOrder() -> [String: [String]] { reactionOrder }

    public func snapshotEdits() -> [String: String] { editedText }

    public func snapshotLooks() -> [String: ChatLook] { looks.filter { !$0.value.isEmpty } }

    public func snapshotLookEvents() -> [ChatLookEvent] { lookEventLists.values.flatMap { $0 } }

    public func restoreLooks(_ restored: [String: ChatLook], events: [ChatLookEvent]) {
        var touched = false
        for (conversation, look) in restored where looks[conversation] == nil && !hiddenChannels.contains(conversation) {
            looks[conversation] = look
            touched = true
        }
        for event in events where !hiddenChannels.contains(event.conversation) {
            if isExpired?(event.conversation, event.timestamp) == true { continue }
            var list = lookEventLists[event.conversation] ?? []
            if list.contains(where: { $0.id == event.id }) { continue }
            var at = list.count
            while at > 0 && list[at - 1].timestamp > event.timestamp { at -= 1 }
            list.insert(event, at: at)
            if list.count > ChatLookFrames.maxEventsPerConversation {
                list.removeFirst(list.count - ChatLookFrames.maxEventsPerConversation)
            }
            lookEventLists[event.conversation] = list
            touched = true
        }
        if touched { changed() }
    }

    public func restore(
        replies restoredReplies: [ChatMessage], roots: [String: String], reactions: [String: [String: [String: String]]],
        order: [String: [String]], edits: [String: String]
    ) {
        for reply in restoredReplies {
            guard let root = roots[reply.id], replyRoot[reply.id] == nil, !hiddenChannels.contains(reply.conversation),
                isExpired?(reply.conversation, reply.timestamp) != true
            else {
                continue
            }
            var list = replies[root] ?? []
            var at = list.count
            while at > 0 && RelayService.compareMessages(list[at - 1], reply) > 0 { at -= 1 }
            list.insert(reply, at: at)
            replies[root] = list
            replyRoot[reply.id] = root
            rootChannel[root] = reply.conversation
        }
        for (id, state) in reactions where reactionState[id] == nil {
            reactionState[id] = state
            reactionOrder[id] = (order[id] ?? Array(state.keys.sorted())).filter { state[$0] != nil }
        }
        for (id, text) in edits where editedText[id] == nil { editedText[id] = text }
        changed()
    }
}

public enum ChatFileLimits {
    public static let maxSendBytes = 2 * 1024 * 1024
}
