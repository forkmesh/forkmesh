import FMBase
import Foundation

public enum ReticulumMessageState: Int, Sendable, Equatable, Comparable {
    case queued
    case stamping
    case findingPath
    case linking
    case sending
    case sent
    case delivered
    case failed
    case received

    public static func < (lhs: Self, rhs: Self) -> Bool { lhs.rawValue < rhs.rawValue }

    public var label: String {
        switch self {
        case .queued: return "Waiting to send"
        case .stamping: return "Computing proof of work"
        case .findingPath: return "Finding a path"
        case .linking: return "Opening a link"
        case .sending: return "Sending"
        case .sent: return "Sent"
        case .delivered: return "Delivered"
        case .failed: return "Not sent"
        case .received: return "Received"
        }
    }
}

public struct ReticulumChatMessage: Equatable, Sendable, Identifiable {
    static let maxStoredAttachmentBytes = 4 * 1024 * 1024

    public var id = ""
    public var peer: [UInt8] = []
    public var outgoing = false
    public var title = ""
    public var content = ""
    public var timestamp: Double = 0
    public var state: ReticulumMessageState = .queued
    public var method = ""
    public var note = ""
    public var verified = false
    public var read = true
    public var progress = 0
    public var attempts = 0
    public var attachments: [ReticulumAttachment] = []

    public init() {}

    public var json: JSONObject {
        var object: JSONObject = [
            "id": .string(id), "peer": .string(ReticulumWire.hex(peer)), "outgoing": .bool(outgoing), "content": .string(content),
            "timestamp": .double(timestamp), "state": .int(Int64(state.rawValue)), "verified": .bool(verified), "read": .bool(read),
            "attempts": .int(Int64(attempts)),
        ]
        if !title.isEmpty { object["title"] = .string(title) }
        if !method.isEmpty { object["method"] = .string(method) }
        if !note.isEmpty { object["note"] = .string(note) }
        if !attachments.isEmpty {
            var total = 0
            object["attachments"] = .array(attachments.map { attachment in
                var file: JSONObject = ["name": .string(attachment.name), "size": .int(Int64(attachment.data.count))]
                total += attachment.data.count
                if total <= Self.maxStoredAttachmentBytes { file["data"] = .string(Data(attachment.data).base64EncodedString()) }
                return .object(file)
            })
        }
        return object
    }

    public init?(json object: JSONObject) {
        id = object.str("id")
        peer = CryptoHex.decode(object.str("peer")) ?? []
        guard id.count == 64, peer.count == Reticulum.hashLength else { return nil }
        outgoing = object.bool("outgoing")
        title = object.str("title")
        content = object.str("content")
        timestamp = object.double("timestamp")
        state = ReticulumMessageState(rawValue: min(max(object.int("state"), 0), ReticulumMessageState.received.rawValue)) ?? .queued
        method = object.str("method")
        note = object.str("note")
        verified = object.bool("verified")
        read = object.optBool("read") ?? true
        attempts = object.int("attempts")
        attachments = object.maps("attachments").map { file in
            ReticulumAttachment(name: file.str("name"), data: Data(base64Encoded: file.str("data")).map { [UInt8]($0) } ?? [])
        }
    }
}

public struct ReticulumConversation: Equatable, Sendable, Identifiable {
    public var peer: [UInt8] = []
    public var name = ""
    public var last = ReticulumChatMessage()
    public var unread = 0
    public var count = 0
    public var id: [UInt8] { peer }
}

public final class ReticulumMessenger {
    public static let maxMessages = 5000
    public static let maxContentBytes = 64 * 1024
    public static let maxIncomingResource = ReticulumLxmf.resourceMaxSize
    static let pathWaitMs: Int64 = 30_000
    static let linkWaitMs: Int64 = 30_000
    static let resourceStallMs: Int64 = 15_000
    static let linkIdleMs: Int64 = 120_000
    static let requestWindow = 64
    static let maxLinks = 64
    static let maxIncomingResources = 8

    final class Link {
        var id: [UInt8] = []
        var initiator = false
        var active = false
        var peer: [UInt8] = []
        var interfaceId = ""
        var ownEncryptionPrivate: [UInt8] = []
        var ownEncryptionPublic: [UInt8] = []
        var ownSigningPrivate: [UInt8] = []
        var peerSigningPublic: [UInt8] = []
        var key: [UInt8] = []
        var mtu = Reticulum.mtu
        var requestedAt: Int64 = 0
        var lastInbound: Int64 = 0
        var lastOutbound: Int64 = 0
        var waiting: [String] = []
    }

    struct Receipt {
        let messageId: String
        let packetHash: [UInt8]
        let signingPublic: [UInt8]
        let sentAt: Int64
    }

    final class IncomingResource {
        var linkId: [UInt8] = []
        var advertisement = ReticulumResourceAdvertisement()
        var hashmap: [[UInt8]] = []
        var parts: [[UInt8]] = []
        var received = 0
        var requested = 0
        var waitingForHashmap = false
        var lastActivity: Int64 = 0
        var retries = 0
    }

    final class OutgoingResource {
        var linkId: [UInt8] = []
        var messageId = ""
        var hash: [UInt8] = []
        var randomHash: [UInt8] = []
        var expectedProof: [UInt8] = []
        var parts: [[UInt8]] = []
        var mapHashes: [[UInt8]] = []
        var dataSize: Int64 = 0
        var transferSize: Int64 = 0
        var sent = Set<Int>()
        var lastActivity: Int64 = 0
        var advertisements = 0
        var requested = false
    }

    let node: ReticulumNode
    private let scheduler: ReticulumScheduler
    public private(set) var displayName = ""
    public var onChange: (() -> Void)?
    public var onIncoming: ((ReticulumChatMessage) -> Void)?
    public var onSave: ((String) -> Void)?
    private(set) var messages: [ReticulumChatMessage] = []
    private var names: [[UInt8]: String] = [:]
    private var seenIds = Set<String>()
    private var stamps: [String: [UInt8]] = [:]
    private var waitingSince: [String: Int64] = [:]
    private var sentAt: [String: Int64] = [:]
    private(set) var links: [[UInt8]: Link] = [:]
    private var receipts: [[UInt8]: Receipt] = [:]
    private var incomingResources: [[UInt8]: IncomingResource] = [:]
    private(set) var outgoingResources: [[UInt8]: OutgoingResource] = [:]
    private(set) var queuedResources: [[UInt8]: [OutgoingResource]] = [:]
    private var stampJobs: [String: ReticulumCancel] = [:]
    private var tickTimer: ReticulumTimer?
    private var saveTimer: ReticulumTimer?
    private var changeTimer: ReticulumTimer?
    private var lastOwnAnnounce: Int64 = 0

    public init(node: ReticulumNode, scheduler: ReticulumScheduler) {
        self.node = node
        self.scheduler = scheduler
        node.addLocalDestination(ReticulumLxmf.deliveryAspect) { [weak self] in
            ReticulumLxmf.announceData(displayName: self?.displayName ?? "", stampCost: 0)
        }
    }

    deinit {
        for cancel in stampJobs.values { cancel.cancel() }
        tickTimer?.cancel()
        saveTimer?.cancel()
        changeTimer?.cancel()
    }

    private var nowMs: Int64 { Int64((scheduler.now * 1000).rounded()) }

    public var address: [UInt8] { node.localDestinationHash(ReticulumLxmf.deliveryAspect) }

    private var signingPrivate: [UInt8] { node.identity.signingPrivateKey }

    public func setDisplayName(_ name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        displayName = ReticulumLxmf.leftUTF16(trimmed, 64)
    }

    public func load(_ text: String) {
        let root = JSON.parseObject(text)
        messages = []
        seenIds = []
        for object in root.maps("messages") {
            guard var message = ReticulumChatMessage(json: object), !seenIds.contains(message.id) else { continue }
            if message.outgoing && message.state < .sent {
                message.state = .failed
                message.note = "Not sent before ForkMesh closed. Retry to send it."
            }
            seenIds.insert(message.id)
            messages.append(message)
        }
        let stored = root.obj("names")
        for key in stored.keys {
            if let hash = CryptoHex.decode(key), hash.count == Reticulum.hashLength {
                names[hash] = ReticulumLxmf.leftUTF16(stored.str(key), 64)
            }
        }
        notifyChanged()
    }

    public func saveJson() -> String {
        let start = max(0, messages.count - Self.maxMessages)
        var stored = JSONObject()
        for key in names.keys.sorted(by: { ReticulumWire.hex($0) < ReticulumWire.hex($1) }) {
            stored[ReticulumWire.hex(key)] = .string(names[key] ?? "")
        }
        let root: JSONObject = ["version": .int(1), "messages": .array(messages[start...].map { .object($0.json) }), "names": .object(stored)]
        return JSON.encode(root)
    }

    private func index(_ id: String) -> Int? { messages.firstIndex { $0.id == id } }

    public func message(_ id: String) -> ReticulumChatMessage? { index(id).map { messages[$0] } }

    @discardableResult
    public func send(to peer: [UInt8], content: String, title: String = "") -> String {
        guard peer.count == Reticulum.hashLength, !content.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              content.utf8.count <= Self.maxContentBytes, node.identity.hasPrivateKey, peer != address else { return "" }
        var message = ReticulumChatMessage()
        message.peer = peer
        message.outgoing = true
        message.title = ReticulumLxmf.leftUTF16(title, 256)
        message.content = content
        message.timestamp = Double(nowMs) / 1000
        message.id = ReticulumWire.hex(ReticulumLxmf.messageId(destination: peer, source: address,
                                                               payload: ReticulumLxmf.payload(timestamp: message.timestamp, title: message.title, content: content)))
        guard !seenIds.contains(message.id) else { return "" }
        seenIds.insert(message.id)
        messages.append(message)
        let name = peerName(peer)
        if !name.hasPrefix("<") { names[peer] = name }
        scheduleSave()
        let id = message.id
        if nowMs - lastOwnAnnounce > 30 * 60 * 1000 && node.onlineCount > 0 && node.announceDestination(address) {
            lastOwnAnnounce = nowMs
            _ = scheduler.after(1.5) { [weak self] in self?.process(id) }
        } else {
            process(id)
        }
        notifyChanged()
        return id
    }

    public func retry(_ id: String) {
        guard let position = index(id), messages[position].outgoing, messages[position].state != .delivered else { return }
        messages[position].attempts = 0
        messages[position].note = ""
        setState(id, .queued)
        process(id)
    }

    public func forget(_ id: String) {
        stampJobs.removeValue(forKey: id)?.cancel()
        messages.removeAll { $0.id == id }
        scheduleSave()
        notifyChanged()
    }

    public func clearConversation(_ peer: [UInt8]) {
        messages.removeAll { $0.peer == peer }
        scheduleSave()
        notifyChanged()
    }

    public func markRead(_ peer: [UInt8]) {
        var changed = false
        for position in messages.indices where messages[position].peer == peer && !messages[position].read {
            messages[position].read = true
            changed = true
        }
        if changed {
            scheduleSave()
            notifyChanged()
        }
    }

    public func messages(with peer: [UInt8]) -> [ReticulumChatMessage] {
        messages.filter { $0.peer == peer }.enumerated().sorted { ($0.element.timestamp, $0.offset) < ($1.element.timestamp, $1.offset) }.map(\.element)
    }

    public func conversations() -> [ReticulumConversation] {
        var byPeer: [[UInt8]: ReticulumConversation] = [:]
        for message in messages {
            var conversation = byPeer[message.peer] ?? ReticulumConversation()
            conversation.peer = message.peer
            conversation.count += 1
            if !message.read { conversation.unread += 1 }
            if conversation.count == 1 || message.timestamp >= conversation.last.timestamp { conversation.last = message }
            byPeer[message.peer] = conversation
        }
        return byPeer.values.map { conversation in
            var named = conversation
            named.name = peerName(conversation.peer)
            return named
        }.sorted { $0.last.timestamp > $1.last.timestamp }
    }

    public var unreadCount: Int { messages.filter { !$0.read }.count }

    public func peerName(_ peer: [UInt8]) -> String {
        if let record = node.destinations[peer], !record.displayName.isEmpty { return record.displayName }
        if let stored = names[peer], !stored.isEmpty { return stored }
        let hex = ReticulumWire.hex(peer)
        return "<\(hex.prefix(6))…\(hex.suffix(4))>"
    }

    public var activeLinks: Int { links.values.filter(\.active).count }

    public func peerStampCost(_ peer: [UInt8]) -> Int {
        guard let record = node.destinations[peer] else { return 0 }
        return ReticulumLxmf.stampCost(appData: record.appData)
    }

    public func destinationAnnounced(_ hash: [UInt8]) {
        if let record = node.destinations[hash], record.kind == "peer", !record.displayName.isEmpty, messages.contains(where: { $0.peer == hash }) {
            names[hash] = record.displayName
            scheduleSave()
        }
        for message in messages where message.peer == hash && message.state == .findingPath {
            let id = message.id
            _ = scheduler.after(0) { [weak self] in self?.process(id) }
        }
    }

    private func packed(_ message: ReticulumChatMessage) -> [UInt8] {
        ReticulumLxmf.pack(source: node.identity, sourceHash: address, destinationHash: message.peer, timestamp: message.timestamp,
                           title: message.title, content: message.content, stamp: stamps[message.id] ?? [])?.packed ?? []
    }

    private func setState(_ id: String, _ state: ReticulumMessageState, note: String? = nil) {
        guard let position = index(id) else { return }
        messages[position].state = state
        if let note { messages[position].note = note }
        scheduleSave()
        notifyChanged()
        updateTicking()
    }

    private func fail(_ id: String, _ note: String) {
        guard let position = index(id), messages[position].state != .delivered else { return }
        setState(id, .failed, note: note)
        let peer = messages[position].peer
        node.addEvent("message", "A message to \(peerName(peer)) was not sent: \(note)", peer)
    }

    private func process(_ id: String) {
        guard let position = index(id), messages[position].outgoing else { return }
        let message = messages[position]
        switch message.state {
        case .queued, .findingPath, .sent: break
        default: return
        }
        if node.onlineCount == 0 {
            setState(id, .queued, note: "Waiting for an interface to come online")
            return
        }
        guard let peer = node.identityFor(message.peer) else {
            if message.state != .findingPath {
                node.requestPath(message.peer)
                waitingSince[id] = nowMs
                setState(id, .findingPath, note: "")
            }
            return
        }
        let cost = peerStampCost(message.peer)
        if cost > 0 && stamps[id] == nil {
            if cost > ReticulumLxmf.maxStampCost {
                fail(id, "The recipient asks for more proof of work (\(cost) bits) than ForkMesh computes")
                return
            }
            let cancel = ReticulumCancel()
            stampJobs[id] = cancel
            let messageId = CryptoHex.decode(id) ?? []
            let scheduler = self.scheduler
            setState(id, .stamping, note: "The recipient asks for \(cost) bits of proof of work")
            scheduler.background { [weak self] in
                let result = ReticulumLxmf.generateStamp(messageId: messageId, cost: cost, cancel: cancel)
                _ = scheduler.after(0) { [weak self] in self?.stampFinished(id, cancel: cancel, stamp: result?.stamp ?? []) }
            }
            return
        }
        let raw = packed(message)
        let contentSize = raw.count - (2 * Reticulum.hashLength + Reticulum.signatureLength) - 16
        if contentSize <= ReticulumLxmf.opportunisticMaxContent && message.attempts < 2 {
            deliverOpportunistic(id, packed: raw, peer: peer)
            return
        }
        guard let link = linkTo(message.peer) ?? openLink(message.peer) else {
            fail(id, "Could not open a link")
            return
        }
        if link.active {
            deliverOverLink(id, link)
        } else {
            if !link.waiting.contains(id) { link.waiting.append(id) }
            waitingSince[id] = nowMs
            setState(id, .linking, note: "")
        }
    }

    private func stampFinished(_ id: String, cancel: ReticulumCancel, stamp: [UInt8]) {
        if stampJobs[id] === cancel { stampJobs.removeValue(forKey: id) }
        if cancel.cancelled { return }
        guard !stamp.isEmpty else {
            fail(id, "Could not compute the proof of work")
            return
        }
        stamps[id] = stamp
        guard index(id) != nil else { return }
        setState(id, .queued, note: "")
        process(id)
    }

    private func deliverOpportunistic(_ id: String, packed: [UInt8], peer: ReticulumIdentity) {
        guard let position = index(id) else { return }
        let target = messages[position].peer
        var packet = ReticulumPacket()
        packet.packetType = .data
        packet.destinationType = .single
        packet.destinationHash = target
        packet.data = peer.encrypt(Array(packed.dropFirst(Reticulum.hashLength)), ratchet: node.destinations[target]?.ratchetKey ?? [])
        guard !packet.data.isEmpty else {
            fail(id, "Could not encrypt the message")
            return
        }
        let raw = node.sendPacket(packet)
        let hash = ReticulumWire.packetHash(raw)
        receipts[Array(hash.prefix(Reticulum.hashLength))] = Receipt(messageId: id, packetHash: hash, signingPublic: peer.signingPublicKey, sentAt: nowMs)
        messages[position].attempts += 1
        messages[position].method = "opportunistic"
        sentAt[id] = nowMs
        setState(id, .sent, note: "Waiting for delivery confirmation")
    }

    private func deliverOverLink(_ id: String, _ link: Link) {
        guard let position = index(id) else { return }
        let raw = packed(messages[position])
        let contentSize = raw.count - (2 * Reticulum.hashLength + Reticulum.signatureLength) - 16
        messages[position].attempts += 1
        if contentSize <= ReticulumLxmf.linkPacketMaxContent {
            let sent = linkPacket(link, type: .data, context: ReticulumContext.none, plaintext: raw)
            let hash = ReticulumWire.packetHash(sent)
            receipts[Array(hash.prefix(Reticulum.hashLength))] = Receipt(messageId: id, packetHash: hash, signingPublic: link.peerSigningPublic, sentAt: nowMs)
            messages[position].method = "link"
            sentAt[id] = nowMs
            setState(id, .sent, note: "Waiting for delivery confirmation")
            return
        }
        if raw.count > ReticulumLxmf.resourceMaxSize {
            fail(id, "The message is too large")
            return
        }
        let resource = OutgoingResource()
        resource.linkId = link.id
        resource.messageId = id
        resource.dataSize = Int64(raw.count)
        let blob = ReticulumCipher.tokenEncrypt(key: link.key, plaintext: ReticulumWire.randomBytes(ReticulumLxmf.resourceRandomLength) + raw)
        resource.transferSize = Int64(blob.count)
        let sdu = link.mtu - Reticulum.headerMaxSize - 1
        var offset = 0
        while offset < blob.count {
            resource.parts.append(Array(blob[offset..<min(blob.count, offset + sdu)]))
            offset += sdu
        }
        for _ in 0..<8 {
            resource.randomHash = ReticulumWire.randomBytes(ReticulumLxmf.resourceRandomLength)
            resource.mapHashes = resource.parts.map { ReticulumLxmf.mapHash($0, randomHash: resource.randomHash) }
            if Set(resource.mapHashes).count == resource.mapHashes.count { break }
        }
        resource.hash = ReticulumWire.fullHash(raw + resource.randomHash)
        resource.expectedProof = ReticulumWire.fullHash(raw + resource.hash)
        resource.lastActivity = nowMs
        messages[position].method = "resource"
        messages[position].progress = 0
        if outgoingResources.values.contains(where: { $0.linkId == link.id }) {
            queuedResources[link.id, default: []].append(resource)
            setState(id, .sending, note: "Waiting for the previous transfer")
            return
        }
        start(resource)
    }

    private func start(_ resource: OutgoingResource) {
        resource.lastActivity = nowMs
        outgoingResources[resource.hash] = resource
        advertise(resource)
        setState(resource.messageId, .sending, note: "Offering \(resource.dataSize) bytes")
    }

    private func resourceEnded(_ linkId: [UInt8]) {
        guard links[linkId] != nil, !outgoingResources.values.contains(where: { $0.linkId == linkId }) else { return }
        while var queue = queuedResources[linkId], !queue.isEmpty {
            let next = queue.removeFirst()
            queuedResources[linkId] = queue.isEmpty ? nil : queue
            if index(next.messageId) != nil {
                start(next)
                return
            }
        }
        queuedResources.removeValue(forKey: linkId)
    }

    private func advertise(_ resource: OutgoingResource) {
        guard let link = links[resource.linkId] else { return }
        var advertisement = ReticulumResourceAdvertisement()
        advertisement.transferSize = resource.transferSize
        advertisement.dataSize = resource.dataSize
        advertisement.parts = resource.parts.count
        advertisement.hash = resource.hash
        advertisement.randomHash = resource.randomHash
        advertisement.originalHash = resource.hash
        advertisement.flags = 0x01
        advertisement.hashmap = resource.mapHashes.flatMap { $0 }
        linkPacket(link, type: .data, context: ReticulumContext.resourceAdvertisement, plaintext: ReticulumLxmf.packAdvertisement(advertisement, segment: 0))
        resource.advertisements += 1
        resource.lastActivity = nowMs
    }

    private func linkTo(_ peer: [UInt8]) -> Link? { links.values.first { $0.initiator && $0.peer == peer } }

    private func openLink(_ peer: [UInt8]) -> Link? {
        guard let identity = node.identityFor(peer), links.count < Self.maxLinks else { return nil }
        let ephemeral = ReticulumIdentity.generate()
        let link = Link()
        link.initiator = true
        link.peer = peer
        link.ownEncryptionPrivate = ephemeral.encryptionPrivateKey
        link.ownEncryptionPublic = ephemeral.encryptionPublicKey
        link.ownSigningPrivate = ephemeral.signingPrivateKey
        link.peerSigningPublic = identity.signingPublicKey
        link.requestedAt = nowMs
        link.interfaceId = node.destinations[peer]?.interfaceId ?? ""
        var packet = ReticulumPacket()
        packet.packetType = .linkRequest
        packet.destinationType = .single
        packet.destinationHash = peer
        packet.data = link.ownEncryptionPublic + ephemeral.signingPublicKey
        let raw = node.sendPacket(packet)
        link.id = Array(ReticulumWire.packetHash(raw).prefix(Reticulum.hashLength))
        links[link.id] = link
        node.addEvent("link", "Opening a link to \(peerName(peer))", peer)
        updateTicking()
        return link
    }

    @discardableResult
    private func linkPacket(_ link: Link, type: ReticulumPacketType, context: UInt8, plaintext: [UInt8], encrypt: Bool = true) -> [UInt8] {
        var packet = ReticulumPacket()
        packet.packetType = type
        packet.destinationType = .link
        packet.destinationHash = link.id
        packet.context = context
        packet.data = encrypt ? ReticulumCipher.tokenEncrypt(key: link.key, plaintext: plaintext) : plaintext
        let raw = ReticulumWire.packPacket(packet)
        node.sendRaw(raw, onlyInterface: link.interfaceId)
        link.lastOutbound = nowMs
        return raw
    }

    private func closeLink(_ linkId: [UInt8], notifyPeer: Bool) {
        guard let link = links[linkId] else { return }
        if notifyPeer && !link.key.isEmpty { linkPacket(link, type: .data, context: ReticulumContext.linkClose, plaintext: link.id) }
        let waiting = link.waiting
        links.removeValue(forKey: linkId)
        incomingResources = incomingResources.filter { $0.value.linkId != linkId }
        for (hash, resource) in outgoingResources where resource.linkId == linkId {
            outgoingResources.removeValue(forKey: hash)
            fail(resource.messageId, "The link closed before the message arrived")
        }
        for resource in queuedResources.removeValue(forKey: linkId) ?? [] {
            fail(resource.messageId, "The link closed before the message arrived")
        }
        for id in waiting { fail(id, "The link closed") }
        notifyChanged()
    }

    public func handlePacket(interfaceId: String, packet: ReticulumPacket, raw: [UInt8], hops: Int) {
        guard node.identity.hasPrivateKey else { return }
        if packet.packetType == .proof {
            handleProof(interfaceId: interfaceId, packet: packet)
            return
        }
        if packet.packetType == .linkRequest {
            if packet.destinationHash == address { handleLinkRequest(interfaceId: interfaceId, packet: packet, raw: raw) }
            return
        }
        guard packet.packetType == .data else { return }
        if packet.destinationType == .link {
            handleLinkData(packet: packet, raw: raw)
            return
        }
        if packet.destinationType == .single && packet.destinationHash == address && packet.context == ReticulumContext.none {
            guard let plaintext = node.identity.decrypt(packet.data) else { return }
            let hash = ReticulumWire.packetHash(raw)
            node.sendRaw(ReticulumCipher.buildProof(signingPrivateKey: signingPrivate, hash: hash, explicit: false,
                                                    destination: Array(hash.prefix(Reticulum.hashLength)), destinationType: .single),
                         onlyInterface: interfaceId)
            receive(address + plaintext, method: "opportunistic")
        }
    }

    private func delivered(_ id: String) {
        guard let position = index(id) else { return }
        setState(id, .delivered, note: "")
        let peer = messages[position].peer
        node.addEvent("message", "Message delivered to \(peerName(peer))", peer)
    }

    private func handleProof(interfaceId: String, packet: ReticulumPacket) {
        if packet.destinationType == .link && packet.context == ReticulumContext.linkRequestProof {
            guard let link = links[packet.destinationHash], link.initiator, !link.active,
                  packet.data.count == 96 || packet.data.count == 99 else { return }
            var signal: [UInt8] = []
            var mtu = Reticulum.mtu
            if packet.data.count == 99 {
                signal = Array(packet.data[96..<99])
                guard signal[0] >> 5 == 1 else { return }
                mtu = Int((UInt32(signal[0]) << 16 | UInt32(signal[1]) << 8 | UInt32(signal[2])) & 0x1FFFFF)
            }
            let peerPublic = Array(packet.data[64..<96])
            guard ReticulumCipher.verify(link.peerSigningPublic, signature: Array(packet.data[0..<64]),
                                         data: link.id + peerPublic + link.peerSigningPublic + signal) else { return }
            link.key = ReticulumWire.hkdf(length: 64, material: ReticulumCipher.x25519Exchange(link.ownEncryptionPrivate, peerPublic), salt: link.id)
            guard !link.key.isEmpty else { return }
            link.active = true
            link.mtu = min(max(mtu, Reticulum.mtu), Reticulum.streamHardwareMtu)
            link.interfaceId = interfaceId
            link.lastInbound = nowMs
            let rtt = Double(nowMs - link.requestedAt) / 1000
            linkPacket(link, type: .data, context: ReticulumContext.linkRtt, plaintext: ReticulumMsgPackWriter().real(rtt).bytes)
            node.addEvent("link", "Link to \(peerName(link.peer)) is up (\(Int64(rtt * 1000)) ms)", link.peer)
            let waiting = link.waiting
            link.waiting = []
            for id in waiting where links[link.id] != nil { deliverOverLink(id, link) }
            return
        }
        if packet.destinationType == .link && packet.context == ReticulumContext.resourceProof {
            guard packet.data.count == 2 * Reticulum.fullHashLength else { return }
            let key = Array(packet.data.prefix(Reticulum.fullHashLength))
            guard let resource = outgoingResources[key], Array(packet.data.suffix(Reticulum.fullHashLength)) == resource.expectedProof else { return }
            outgoingResources.removeValue(forKey: key)
            if let position = index(resource.messageId) { messages[position].progress = 100 }
            delivered(resource.messageId)
            resourceEnded(resource.linkId)
            return
        }
        var key = packet.destinationHash
        if packet.destinationType == .link {
            guard packet.data.count == Reticulum.fullHashLength + Reticulum.signatureLength else { return }
            key = Array(packet.data.prefix(Reticulum.hashLength))
        }
        guard let receipt = receipts[key],
              ReticulumCipher.validateProof(signingPublicKey: receipt.signingPublic, hash: receipt.packetHash, proof: packet.data) else { return }
        receipts.removeValue(forKey: key)
        delivered(receipt.messageId)
    }

    private func handleLinkRequest(interfaceId: String, packet: ReticulumPacket, raw: [UInt8]) {
        guard packet.data.count == 64 || packet.data.count == 67, links.count < Self.maxLinks else { return }
        var mtu = Reticulum.mtu
        if packet.data.count == 67 {
            guard (packet.data[64] & 0xE0) >> 5 == 1 else { return }
            mtu = Int((UInt32(packet.data[64]) << 16 | UInt32(packet.data[65]) << 8 | UInt32(packet.data[66])) & 0x1FFFFF)
            mtu = min(max(mtu, Reticulum.mtu), Reticulum.streamHardwareMtu)
        }
        let start = packet.headerType == 1 ? Reticulum.hashLength + 2 : 2
        var hashable = [raw[0] & 0x0F] + (start < raw.count ? Array(raw[start...]) : [])
        if packet.data.count > 64 { hashable.removeLast(packet.data.count - 64) }
        let linkId = ReticulumWire.truncatedHash(hashable)
        guard links[linkId] == nil else { return }
        let ephemeral = ReticulumIdentity.generate()
        let link = Link()
        link.id = linkId
        link.initiator = false
        link.interfaceId = interfaceId
        link.ownEncryptionPrivate = ephemeral.encryptionPrivateKey
        link.ownEncryptionPublic = ephemeral.encryptionPublicKey
        link.ownSigningPrivate = signingPrivate
        link.peerSigningPublic = Array(packet.data[32..<64])
        link.key = ReticulumWire.hkdf(length: 64, material: ReticulumCipher.x25519Exchange(link.ownEncryptionPrivate, Array(packet.data[0..<32])), salt: linkId)
        link.mtu = mtu
        link.requestedAt = nowMs
        link.lastInbound = nowMs
        guard !link.key.isEmpty else { return }
        let value = UInt32(mtu & 0x1FFFFF) + (UInt32((1 << 5) & 0xE0) << 16)
        let signal = [UInt8((value >> 16) & 0xff), UInt8((value >> 8) & 0xff), UInt8(value & 0xff)]
        let signature = ReticulumCipher.sign(link.ownSigningPrivate, linkId + link.ownEncryptionPublic + node.identity.signingPublicKey + signal)
        var proof = ReticulumPacket()
        proof.packetType = .proof
        proof.destinationType = .link
        proof.destinationHash = linkId
        proof.context = ReticulumContext.linkRequestProof
        proof.data = signature + link.ownEncryptionPublic + signal
        node.sendRaw(ReticulumWire.packPacket(proof), onlyInterface: interfaceId)
        links[linkId] = link
        updateTicking()
    }

    private func handleLinkData(packet: ReticulumPacket, raw: [UInt8]) {
        guard let link = links[packet.destinationHash], !link.key.isEmpty else { return }
        link.lastInbound = nowMs
        switch packet.context {
        case ReticulumContext.keepalive:
            if !link.initiator && packet.data == [0xFF] {
                linkPacket(link, type: .data, context: ReticulumContext.keepalive, plaintext: [0xFE], encrypt: false)
            }
            return
        case ReticulumContext.resource:
            handleResourcePart(link, packet.data)
            return
        default:
            break
        }
        guard let plaintext = ReticulumCipher.tokenDecrypt(key: link.key, token: packet.data) else { return }
        switch packet.context {
        case ReticulumContext.linkRtt:
            if !link.initiator { link.active = true }
        case ReticulumContext.linkClose:
            if plaintext == link.id { closeLink(link.id, notifyPeer: false) }
        case ReticulumContext.none:
            let hash = ReticulumWire.packetHash(raw)
            var proof = ReticulumPacket()
            proof.packetType = .proof
            proof.destinationType = .link
            proof.destinationHash = link.id
            proof.data = hash + ReticulumCipher.sign(link.ownSigningPrivate, hash)
            node.sendRaw(ReticulumWire.packPacket(proof), onlyInterface: link.interfaceId)
            if !link.initiator { receive(plaintext, method: "link") }
        case ReticulumContext.resourceAdvertisement:
            handleAdvertisement(link, plaintext)
        case ReticulumContext.resourceRequest:
            handleResourceRequest(link, plaintext)
        case ReticulumContext.resourceHashmapUpdate:
            handleHashmapUpdate(plaintext)
        case ReticulumContext.resourceInitiatorCancel:
            incomingResources.removeValue(forKey: plaintext)
        case ReticulumContext.resourceReceiverCancel:
            if let resource = outgoingResources.removeValue(forKey: plaintext) {
                fail(resource.messageId, "The recipient refused the message")
                resourceEnded(resource.linkId)
            }
        default:
            break
        }
    }

    private func handleAdvertisement(_ link: Link, _ plaintext: [UInt8]) {
        guard let advertisement = ReticulumLxmf.unpackAdvertisement(plaintext), incomingResources[advertisement.hash] == nil else { return }
        if link.initiator || advertisement.compressed || advertisement.segments > 1 || advertisement.isRequest || advertisement.isResponse
            || advertisement.transferSize > Int64(Self.maxIncomingResource + 4096) || incomingResources.count >= Self.maxIncomingResources {
            linkPacket(link, type: .data, context: ReticulumContext.resourceReceiverCancel, plaintext: advertisement.hash)
            return
        }
        let resource = IncomingResource()
        resource.linkId = link.id
        resource.advertisement = advertisement
        resource.parts = Array(repeating: [], count: advertisement.parts)
        resource.hashmap = Array(repeating: [], count: advertisement.parts)
        let size = ReticulumLxmf.resourceMapHashLength
        var position = 0
        while position * size < advertisement.hashmap.count && position < advertisement.parts {
            resource.hashmap[position] = Array(advertisement.hashmap[(position * size)..<((position + 1) * size)])
            position += 1
        }
        resource.lastActivity = nowMs
        incomingResources[advertisement.hash] = resource
        requestNextParts(resource)
        updateTicking()
    }

    private func requestNextParts(_ resource: IncomingResource) {
        guard let link = links[resource.linkId], !resource.waitingForHashmap else { return }
        var hashes: [UInt8] = []
        var exhausted = false
        var requested = 0
        for position in resource.parts.indices where requested < Self.requestWindow {
            if !resource.parts[position].isEmpty { continue }
            if resource.hashmap[position].isEmpty {
                exhausted = true
                break
            }
            hashes += resource.hashmap[position]
            requested += 1
        }
        if requested == 0 && !exhausted { return }
        var request: [UInt8] = [exhausted ? 0xFF : 0x00]
        if exhausted {
            var height = 0
            while height < resource.hashmap.count && !resource.hashmap[height].isEmpty { height += 1 }
            if height == 0 { return }
            request += resource.hashmap[height - 1]
            resource.waitingForHashmap = true
        }
        request += resource.advertisement.hash + hashes
        resource.requested = requested
        resource.lastActivity = nowMs
        linkPacket(link, type: .data, context: ReticulumContext.resourceRequest, plaintext: request)
    }

    private func handleResourcePart(_ link: Link, _ data: [UInt8]) {
        for (hash, resource) in incomingResources where resource.linkId == link.id {
            let mapHash = ReticulumLxmf.mapHash(data, randomHash: resource.advertisement.randomHash)
            for position in resource.parts.indices where resource.parts[position].isEmpty && resource.hashmap[position] == mapHash {
                resource.parts[position] = data
                resource.received += 1
                resource.requested -= 1
                resource.lastActivity = nowMs
                resource.retries = 0
                if resource.received == resource.parts.count {
                    assemble(hash)
                } else if resource.requested <= 0 {
                    requestNextParts(resource)
                }
                return
            }
        }
    }

    private func handleHashmapUpdate(_ plaintext: [UInt8]) {
        guard plaintext.count > Reticulum.fullHashLength, let resource = incomingResources[Array(plaintext.prefix(Reticulum.fullHashLength))] else { return }
        let update = ReticulumWire.decodeMsgPack(Array(plaintext.dropFirst(Reticulum.fullHashLength)))?.list ?? []
        guard update.count >= 2 else { return }
        let segment = update[0].intValue
        let hashmap = update[1].byteValue
        let size = ReticulumLxmf.resourceMapHashLength
        var position = 0
        while position * size < hashmap.count {
            let target = segment * ReticulumLxmf.resourceHashmapMaxLength + position
            if target >= 0 && target < resource.hashmap.count {
                resource.hashmap[target] = Array(hashmap[(position * size)..<min(hashmap.count, (position + 1) * size)])
            }
            position += 1
        }
        resource.waitingForHashmap = false
        resource.lastActivity = nowMs
        requestNextParts(resource)
    }

    private func assemble(_ hash: [UInt8]) {
        guard let resource = incomingResources.removeValue(forKey: hash), let link = links[resource.linkId] else { return }
        guard let plain = ReticulumCipher.tokenDecrypt(key: link.key, token: resource.parts.flatMap { $0 }),
              plain.count >= ReticulumLxmf.resourceRandomLength else { return }
        let data = Array(plain.dropFirst(ReticulumLxmf.resourceRandomLength))
        guard ReticulumWire.fullHash(data + resource.advertisement.randomHash) == hash else { return }
        linkPacket(link, type: .proof, context: ReticulumContext.resourceProof, plaintext: hash + ReticulumWire.fullHash(data + hash), encrypt: false)
        var payload = data
        if resource.advertisement.hasMetadata && payload.count >= 3 {
            let size = Int(payload[0]) << 16 | Int(payload[1]) << 8 | Int(payload[2])
            payload = 3 + size <= payload.count ? Array(payload[(3 + size)...]) : []
        }
        receive(payload, method: "resource")
    }

    private func handleResourceRequest(_ link: Link, _ plaintext: [UInt8]) {
        guard let first = plaintext.first else { return }
        let exhausted = first == 0xFF
        let pad = exhausted ? 1 + ReticulumLxmf.resourceMapHashLength : 1
        guard plaintext.count >= pad + Reticulum.fullHashLength,
              let resource = outgoingResources[Array(plaintext[pad..<(pad + Reticulum.fullHashLength)])], resource.linkId == link.id else { return }
        resource.requested = true
        resource.lastActivity = nowMs
        let requested = Array(plaintext[(pad + Reticulum.fullHashLength)...])
        let size = ReticulumLxmf.resourceMapHashLength
        var offset = 0
        while offset + size <= requested.count {
            if let position = resource.mapHashes.firstIndex(of: Array(requested[offset..<(offset + size)])) {
                linkPacket(link, type: .data, context: ReticulumContext.resource, plaintext: resource.parts[position], encrypt: false)
                resource.sent.insert(position)
            }
            offset += size
        }
        if exhausted, let position = resource.mapHashes.firstIndex(of: Array(plaintext[1..<(1 + size)])) {
            let partIndex = position + 1
            if partIndex % ReticulumLxmf.resourceHashmapMaxLength == 0 {
                let segment = partIndex / ReticulumLxmf.resourceHashmapMaxLength
                let low = min(resource.mapHashes.count, segment * ReticulumLxmf.resourceHashmapMaxLength)
                let high = min(resource.mapHashes.count, low + ReticulumLxmf.resourceHashmapMaxLength)
                let hashmap = resource.mapHashes[low..<high].flatMap { $0 }
                if !hashmap.isEmpty {
                    linkPacket(link, type: .data, context: ReticulumContext.resourceHashmapUpdate,
                               plaintext: resource.hash + ReticulumMsgPackWriter().array(2).integer(Int64(segment)).bin(hashmap).bytes)
                }
            }
        }
        if let position = index(resource.messageId) {
            messages[position].progress = 100 * resource.sent.count / max(1, resource.parts.count)
            setState(resource.messageId, .sending, note: "\(messages[position].progress)% sent")
        }
    }

    private func receive(_ packed: [UInt8], method: String) {
        guard let lxmf = try? ReticulumLxmf.unpack(packed), lxmf.destinationHash == address else { return }
        let id = ReticulumWire.hex(lxmf.hash)
        guard !seenIds.contains(id) else { return }
        var message = ReticulumChatMessage()
        message.id = id
        message.peer = lxmf.sourceHash
        message.outgoing = false
        message.title = lxmf.title
        message.content = lxmf.content
        message.timestamp = lxmf.timestamp > 0 ? lxmf.timestamp : Double(nowMs) / 1000
        message.state = .received
        message.method = method
        message.read = false
        message.attachments = lxmf.attachments
        if let source = node.identityFor(lxmf.sourceHash) {
            message.verified = ReticulumLxmf.verify(lxmf, sourcePublicKey: source.publicKey)
            if !message.verified { message.note = "The signature does not match the sender's announced key" }
        } else {
            message.note = "Sender not yet heard on the network, so the signature is unchecked"
        }
        seenIds.insert(id)
        messages.append(message)
        if messages.count > Self.maxMessages + 500 { messages.removeFirst(messages.count - Self.maxMessages) }
        let name = peerName(message.peer)
        if !name.hasPrefix("<") { names[message.peer] = name }
        node.addEvent("message", "Message from \(name)", message.peer)
        scheduleSave()
        notifyChanged()
        onIncoming?(message)
    }

    private func notifyChanged() {
        guard changeTimer == nil else { return }
        changeTimer = scheduler.after(0.1) { [weak self] in
            guard let self else { return }
            self.changeTimer = nil
            self.onChange?()
        }
    }

    private func scheduleSave() {
        guard saveTimer == nil else { return }
        saveTimer = scheduler.after(1.5) { [weak self] in
            guard let self else { return }
            self.saveTimer = nil
            self.onSave?(self.saveJson())
        }
    }

    public func flush() {
        guard let timer = saveTimer else { return }
        timer.cancel()
        saveTimer = nil
        onSave?(saveJson())
    }

    private func updateTicking() {
        let pending = !links.isEmpty || !incomingResources.isEmpty || !outgoingResources.isEmpty || !queuedResources.isEmpty
            || messages.contains { $0.outgoing && ($0.state < .sent || ($0.state == .sent && $0.attempts < 3)) }
        if pending && tickTimer == nil {
            tickTimer = scheduler.after(1) { [weak self] in
                guard let self else { return }
                self.tickTimer = nil
                self.tick()
            }
        } else if !pending, let timer = tickTimer {
            timer.cancel()
            tickTimer = nil
        }
    }

    public func tick() {
        let now = nowMs
        var toProcess: [String] = []
        var toFail: [String] = []
        for message in messages where message.outgoing {
            if message.state == .queued && node.onlineCount > 0 {
                toProcess.append(message.id)
            } else if message.state == .findingPath {
                if node.identityFor(message.peer) != nil {
                    toProcess.append(message.id)
                } else if now - (waitingSince[message.id] ?? now) > Self.pathWaitMs {
                    toFail.append(message.id)
                }
            } else if message.state == .sent && message.method == "opportunistic" && now - (sentAt[message.id] ?? now) > 15_000 && message.attempts < 3 {
                toProcess.append(message.id)
            } else if message.state == .linking && now - (waitingSince[message.id] ?? now) > Self.linkWaitMs {
                toFail.append(message.id)
            }
        }
        for id in toFail {
            guard let position = index(id) else { continue }
            if messages[position].state == .findingPath {
                fail(id, "No path to this contact. They may be offline; retry later.")
            } else {
                fail(id, "Could not open a link to this contact")
            }
        }
        for id in toProcess { process(id) }
        var stale: [[UInt8]] = []
        for link in links.values {
            if !link.active && link.initiator && now - link.requestedAt > Self.linkWaitMs {
                stale.append(link.id)
                continue
            }
            let busy = !link.waiting.isEmpty || queuedResources[link.id] != nil || outgoingResources.values.contains { $0.linkId == link.id }
                || incomingResources.values.contains { $0.linkId == link.id }
            let idle = now - max(link.lastInbound, link.lastOutbound)
            if !busy && link.initiator && link.active && idle > Self.linkIdleMs {
                stale.append(link.id)
            } else if !link.initiator && !link.active && now - link.requestedAt > 60_000 {
                stale.append(link.id)
            } else if !link.initiator && now - link.lastInbound > 15 * 60 * 1000 {
                stale.append(link.id)
            }
        }
        for id in stale { closeLink(id, notifyPeer: true) }
        var dropped: [[UInt8]] = []
        for (hash, resource) in incomingResources where now - resource.lastActivity >= Self.resourceStallMs {
            resource.retries += 1
            if resource.retries > 5 {
                dropped.append(hash)
                continue
            }
            resource.waitingForHashmap = false
            requestNextParts(resource)
        }
        for hash in dropped { incomingResources.removeValue(forKey: hash) }
        var failed: [[UInt8]] = []
        for (hash, resource) in outgoingResources {
            if !resource.requested && now - resource.lastActivity > 10_000 {
                if resource.advertisements >= 4 { failed.append(hash) } else { advertise(resource) }
            } else if resource.requested && now - resource.lastActivity > 60_000 {
                failed.append(hash)
            }
        }
        for hash in failed {
            if let resource = outgoingResources.removeValue(forKey: hash) {
                fail(resource.messageId, "The transfer stalled")
                resourceEnded(resource.linkId)
            }
        }
        receipts = receipts.filter { now - $0.value.sentAt <= 10 * 60 * 1000 }
        for position in messages.indices where messages[position].outgoing && messages[position].state == .sent
            && messages[position].attempts >= 3 && messages[position].note == "Waiting for delivery confirmation" {
            messages[position].note = "No delivery confirmation yet"
            notifyChanged()
        }
        updateTicking()
    }
}
