package com.forkmesh.nativeapp.reticulum

import com.forkmesh.nativeapp.json.Json
import java.util.Base64
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

enum class MessageState(val label: String) {
    Queued("Waiting to send"),
    Stamping("Computing proof of work"),
    FindingPath("Finding a path"),
    Linking("Opening a link"),
    Sending("Sending"),
    Sent("Sent"),
    Delivered("Delivered"),
    Failed("Not sent"),
    Received("Received"),
}

data class ChatMessage(
    var id: String = "",
    var peer: ByteArray = ByteArray(0),
    var outgoing: Boolean = false,
    var title: String = "",
    var content: String = "",
    var timestamp: Double = 0.0,
    var state: MessageState = MessageState.Queued,
    var method: String = "",
    var note: String = "",
    var verified: Boolean = false,
    var read: Boolean = true,
    var progress: Int = 0,
    var attempts: Int = 0,
    var attachments: List<LxmfAttachment> = emptyList(),
) {
    val peerHex: String get() = Rns.hex(peer)

    fun toJson(): Map<String, Any?> {
        val out = linkedMapOf<String, Any?>(
            "id" to id, "peer" to Rns.hex(peer), "outgoing" to outgoing, "content" to content, "timestamp" to timestamp,
            "state" to state.ordinal.toLong(), "verified" to verified, "read" to read, "attempts" to attempts.toLong(),
        )
        if (title.isNotEmpty()) out["title"] = title
        if (method.isNotEmpty()) out["method"] = method
        if (note.isNotEmpty()) out["note"] = note
        if (attachments.isNotEmpty()) {
            var total = 0L
            out["attachments"] = attachments.map { attachment ->
                val file = linkedMapOf<String, Any?>("name" to attachment.name, "size" to attachment.data.size.toLong())
                total += attachment.data.size
                if (total <= MAX_STORED_ATTACHMENT_BYTES) file["data"] = Base64.getEncoder().encodeToString(attachment.data)
                file
            }
        }
        return out
    }

    companion object {
        const val MAX_STORED_ATTACHMENT_BYTES = 4L * 1024 * 1024

        fun fromJson(json: Map<*, *>): ChatMessage? {
            val id = json["id"] as? String ?: return null
            val peer = Rns.fromHex(json["peer"] as? String ?: "") ?: return null
            if (id.length != 64 || peer.size != Rns.HASH_LENGTH) return null
            fun number(key: String): Long = (json[key] as? Number)?.toLong() ?: 0L
            val state = number("state").toInt().coerceIn(0, MessageState.Received.ordinal)
            val files = (json["attachments"] as? List<*>).orEmpty().mapNotNull { value ->
                val file = value as? Map<*, *> ?: return@mapNotNull null
                LxmfAttachment(file["name"] as? String ?: "", runCatching { Base64.getDecoder().decode(file["data"] as? String ?: "") }.getOrDefault(ByteArray(0)))
            }
            return ChatMessage(
                id = id,
                peer = peer,
                outgoing = json["outgoing"] == true,
                title = json["title"] as? String ?: "",
                content = json["content"] as? String ?: "",
                timestamp = (json["timestamp"] as? Number)?.toDouble() ?: 0.0,
                state = MessageState.entries[state],
                method = json["method"] as? String ?: "",
                note = json["note"] as? String ?: "",
                verified = json["verified"] == true,
                read = json["read"] as? Boolean ?: true,
                attempts = number("attempts").toInt(),
                attachments = files,
            )
        }
    }
}

data class Conversation(val peer: ByteArray, val name: String, val last: ChatMessage, val unread: Int, val count: Int) {
    val peerHex: String get() = Rns.hex(peer)
}

class ReticulumMessenger(private val node: ReticulumNode) {
    private class Link(
        val id: ByteArray,
        val initiator: Boolean,
        val peer: ByteArray,
        var interfaceId: String,
        val ownEncryptionPrivate: ByteArray,
        val ownEncryptionPublic: ByteArray,
        val ownSigningPrivate: ByteArray,
        val peerSigningPublic: ByteArray,
        var key: ByteArray = ByteArray(0),
        var active: Boolean = false,
        var mtu: Int = Rns.MTU,
        var requestedAt: Long = 0,
        var lastInbound: Long = 0,
        var lastOutbound: Long = 0,
        val waiting: MutableList<String> = ArrayList(),
    ) {
        val hex: String = Rns.hex(id)
    }

    private class Receipt(val messageId: String, val packetHash: ByteArray, val signingPublic: ByteArray, val sentAt: Long)

    private class IncomingResource(
        val linkId: String,
        val advertisement: ResourceAdvertisement,
        val hashmap: Array<ByteArray>,
        val parts: Array<ByteArray>,
        var received: Int = 0,
        var requested: Int = 0,
        var waitingForHashmap: Boolean = false,
        var lastActivity: Long = 0,
        var retries: Int = 0,
    )

    private class OutgoingResource(
        val linkId: String,
        val messageId: String,
        val hash: ByteArray,
        val randomHash: ByteArray,
        val expectedProof: ByteArray,
        val parts: List<ByteArray>,
        val mapHashes: List<ByteArray>,
        val dataSize: Long,
        val transferSize: Long,
        val sent: MutableSet<Int> = HashSet(),
        var lastActivity: Long = 0,
        var advertisements: Int = 0,
        var requested: Boolean = false,
        var queued: Boolean = false,
    )

    private val lock get() = node.lock
    private val timers get() = node.timers
    private fun now(): Long = node.clock()

    private var displayName = ""
    private val messages = ArrayList<ChatMessage>()
    private val names = HashMap<String, String>()
    private val seenIds = HashSet<String>()
    private val stamps = HashMap<String, ByteArray>()
    private val waitingSince = HashMap<String, Long>()
    private val sentAt = HashMap<String, Long>()
    private val links = LinkedHashMap<String, Link>()
    private val receipts = HashMap<String, Receipt>()
    private val incomingResources = LinkedHashMap<String, IncomingResource>()
    private val outgoingResources = LinkedHashMap<String, OutgoingResource>()
    private val stampJobs = HashMap<String, AtomicBoolean>()
    private var tickTimer: ReticulumTimer? = null
    private var savePending = false
    private var changePending = false
    private var lastOwnAnnounce = 0L
    @Volatile private var closed = false

    @Volatile var changedHandler: (() -> Unit)? = null
    @Volatile var incomingHandler: ((ChatMessage) -> Unit)? = null
    @Volatile var saveHandler: ((String) -> Unit)? = null

    init {
        node.addLocalDestination(Lxmf.DELIVERY_ASPECT) { Lxmf.announceData(displayName, 0) }
    }

    fun address(): ByteArray = node.localDestinationHash(Lxmf.DELIVERY_ASPECT)

    private fun signingPrivate(): ByteArray = node.identity()?.privateKey?.copyOfRange(32, 64) ?: ByteArray(0)

    fun setDisplayName(name: String) = synchronized(lock) { displayName = name.trim().take(64) }

    fun displayName(): String = synchronized(lock) { displayName }

    fun close() {
        synchronized(lock) {
            closed = true
            stampJobs.values.forEach { it.set(true) }
            tickTimer?.cancel()
            tickTimer = null
        }
        if (savePending) saveHandler?.invoke(saveJson())
    }

    fun load(json: String) = synchronized(lock) {
        val root = runCatching { Json.parseObject(json) }.getOrNull() ?: emptyMap<String, Any?>()
        messages.clear()
        seenIds.clear()
        for (value in (root["messages"] as? List<*>).orEmpty()) {
            val message = (value as? Map<*, *>)?.let(ChatMessage::fromJson) ?: continue
            if (message.id in seenIds) continue
            if (message.outgoing && message.state.ordinal < MessageState.Sent.ordinal) {
                message.state = MessageState.Failed
                message.note = "Not sent before ForkMesh closed. Retry to send it."
            }
            seenIds.add(message.id)
            messages.add(message)
        }
        for ((key, value) in (root["names"] as? Map<*, *>).orEmpty()) {
            val hash = Rns.fromHex(key as? String ?: "") ?: continue
            if (hash.size == Rns.HASH_LENGTH) names[Rns.hex(hash)] = (value as? String ?: "").take(64)
        }
        notifyChanged()
    }

    fun saveJson(): String = synchronized(lock) {
        val start = maxOf(0, messages.size - MAX_MESSAGES)
        Json.stringify(
            linkedMapOf(
                "version" to 1L,
                "messages" to messages.subList(start, messages.size).map { it.toJson() },
                "names" to LinkedHashMap(names),
            ),
        )
    }

    private fun find(id: String): ChatMessage? = messages.firstOrNull { it.id == id }

    fun message(id: String): ChatMessage? = synchronized(lock) { find(id)?.copy() }

    fun send(peer: ByteArray, content: String, title: String = ""): String = synchronized(lock) {
        val own = node.identity()
        if (peer.size != Rns.HASH_LENGTH || content.isBlank() || content.toByteArray(Charsets.UTF_8).size > MAX_CONTENT_BYTES ||
            own == null || !own.hasPrivateKey || peer.contentEquals(address())
        ) return@synchronized ""
        val timestamp = now() / 1000.0
        val cleanTitle = title.take(256)
        val id = Rns.hex(Lxmf.messageId(peer, address(), Lxmf.payload(timestamp, cleanTitle, content)))
        if (id in seenIds) return@synchronized ""
        seenIds.add(id)
        messages.add(ChatMessage(id = id, peer = peer, outgoing = true, title = cleanTitle, content = content, timestamp = timestamp))
        val name = peerName(peer)
        if (!name.startsWith("<")) names[Rns.hex(peer)] = name
        scheduleSave()
        if (now() - lastOwnAnnounce > OWN_ANNOUNCE_GAP_MS && node.onlineCount() > 0 && node.announceDestination(address())) {
            lastOwnAnnounce = now()
            later(1500) { process(id) }
        } else {
            process(id)
        }
        notifyChanged()
        id
    }

    fun retry(id: String) = synchronized(lock) {
        val message = find(id) ?: return@synchronized
        if (!message.outgoing || message.state == MessageState.Delivered) return@synchronized
        message.attempts = 0
        message.note = ""
        setState(message, MessageState.Queued)
        process(id)
    }

    fun forget(id: String) = synchronized(lock) {
        stampJobs.remove(id)?.set(true)
        messages.removeAll { it.id == id }
        scheduleSave()
        notifyChanged()
    }

    fun clearConversation(peer: ByteArray) = synchronized(lock) {
        messages.removeAll { it.peer.contentEquals(peer) }
        scheduleSave()
        notifyChanged()
    }

    fun markRead(peer: ByteArray) = synchronized(lock) {
        var changed = false
        for (message in messages) {
            if (message.peer.contentEquals(peer) && !message.read) {
                message.read = true
                changed = true
            }
        }
        if (changed) {
            scheduleSave()
            notifyChanged()
        }
    }

    fun messages(peer: ByteArray): List<ChatMessage> = synchronized(lock) {
        messages.filter { it.peer.contentEquals(peer) }.map { it.copy() }.sortedBy { it.timestamp }
    }

    fun conversations(): List<Conversation> = synchronized(lock) {
        val byPeer = LinkedHashMap<String, MutableList<ChatMessage>>()
        for (message in messages) byPeer.getOrPut(message.peerHex) { ArrayList() }.add(message)
        byPeer.values.map { list ->
            var last = list.first()
            for (message in list) if (message.timestamp >= last.timestamp) last = message
            Conversation(last.peer, peerName(last.peer), last.copy(), list.count { !it.read }, list.size)
        }.sortedByDescending { it.last.timestamp }
    }

    fun unreadCount(): Int = synchronized(lock) { messages.count { !it.read } }

    fun peerName(peer: ByteArray): String = synchronized(lock) {
        val found = node.destination(peer)
        if (found != null && found.displayName.isNotEmpty()) return@synchronized found.displayName
        names[Rns.hex(peer)].orEmpty().ifEmpty { Rns.shortHash(peer) }
    }

    fun activeLinks(): Int = synchronized(lock) { links.values.count { it.active } }

    fun peerStampCost(peer: ByteArray): Int = node.destination(peer)?.let { Lxmf.stampCostFromAppData(it.appData) } ?: 0

    fun destinationAnnounced(hash: ByteArray) = synchronized(lock) {
        val found = node.destination(hash)
        val key = Rns.hex(hash)
        if (found != null && found.kind == "peer" && found.displayName.isNotEmpty() && messages.any { it.peer.contentEquals(hash) }) {
            names[key] = found.displayName
            scheduleSave()
        }
        for (message in messages) {
            if (message.peer.contentEquals(hash) && message.state == MessageState.FindingPath) {
                val id = message.id
                later(0) { process(id) }
            }
        }
    }

    fun tickForTest() = synchronized(lock) { tick() }

    private fun later(delayMs: Long, task: () -> Unit) {
        timers.schedule(delayMs) { synchronized(lock) { if (!closed) task() } }
    }

    private fun packedFor(message: ChatMessage): LxmfMessage? {
        val own = node.identity() ?: return null
        return Lxmf.pack(own, address(), message.peer, message.timestamp, message.title, message.content, stamps[message.id] ?: ByteArray(0))
    }

    private fun setState(message: ChatMessage, state: MessageState, note: String? = null) {
        message.state = state
        if (note != null) message.note = note
        scheduleSave()
        notifyChanged()
        updateTicking()
    }

    private fun fail(id: String, note: String) {
        val message = find(id) ?: return
        if (message.state == MessageState.Delivered) return
        setState(message, MessageState.Failed, note)
        node.addEvent("message", "A message to ${peerName(message.peer)} was not sent: $note", message.peer)
    }

    private fun process(id: String) {
        val message = find(id) ?: return
        if (!message.outgoing) return
        if (message.state != MessageState.Queued && message.state != MessageState.FindingPath && message.state != MessageState.Sent) return
        if (node.onlineCount() == 0) {
            setState(message, MessageState.Queued, "Waiting for an interface to come online")
            return
        }
        val peer = node.identityFor(message.peer)
        if (peer == null) {
            if (message.state != MessageState.FindingPath) {
                node.requestPath(message.peer)
                waitingSince[id] = now()
                setState(message, MessageState.FindingPath, "")
            }
            return
        }
        val cost = peerStampCost(message.peer)
        if (cost > 0 && id !in stamps) {
            if (cost > Lxmf.MAX_STAMP_COST) {
                fail(id, "The recipient asks for more proof of work ($cost bits) than ForkMesh computes")
                return
            }
            val cancel = AtomicBoolean(false)
            stampJobs[id] = cancel
            val messageId = Rns.fromHex(id) ?: ByteArray(0)
            thread(name = "reticulum-stamp", isDaemon = true, priority = Thread.MIN_PRIORITY) {
                val stamp = Lxmf.generateStamp(messageId, cost, cancel)
                synchronized(lock) {
                    stampJobs.remove(id)
                    if (cancel.get() || closed) return@synchronized
                    if (stamp == null) {
                        fail(id, "Could not compute the proof of work")
                        return@synchronized
                    }
                    stamps[id] = stamp.stamp
                    val current = find(id) ?: return@synchronized
                    setState(current, MessageState.Queued, "")
                    process(id)
                }
            }
            setState(message, MessageState.Stamping, "The recipient asks for $cost bits of proof of work")
            return
        }
        val packed = packedFor(message)?.packed ?: return fail(id, "Could not sign the message")
        val contentSize = packed.size - (2 * Rns.HASH_LENGTH + Rns.SIGNATURE_LENGTH) - 16
        if (contentSize <= Lxmf.OPPORTUNISTIC_MAX_CONTENT && message.attempts < 2) {
            deliverOpportunistic(message, packed, peer)
            return
        }
        val link = linkTo(message.peer) ?: openLink(message.peer) ?: return fail(id, "Could not open a link")
        if (link.active) {
            deliverOverLink(message, link)
        } else {
            if (id !in link.waiting) link.waiting.add(id)
            waitingSince[id] = now()
            setState(message, MessageState.Linking, "")
        }
    }

    private fun deliverOpportunistic(message: ChatMessage, packed: ByteArray, peer: RnsIdentity) {
        val ratchet = node.destination(message.peer)?.ratchetKey ?: ByteArray(0)
        val data = peer.encrypt(packed.copyOfRange(Rns.HASH_LENGTH, packed.size), ratchet)
        if (data.isEmpty()) return fail(message.id, "Could not encrypt the message")
        val raw = node.sendPacket(Packet(packetType = Rns.PACKET_DATA, destinationType = Rns.DESTINATION_SINGLE, destinationHash = message.peer, data = data))
        val hash = Rns.packetHash(raw)
        receipts[Rns.hex(hash.copyOf(Rns.HASH_LENGTH))] = Receipt(message.id, hash, peer.signingPublicKey, now())
        message.attempts++
        message.method = "opportunistic"
        sentAt[message.id] = now()
        setState(message, MessageState.Sent, WAITING_CONFIRMATION)
    }

    private fun deliverOverLink(message: ChatMessage, link: Link) {
        val packed = packedFor(message)?.packed ?: return fail(message.id, "Could not sign the message")
        val contentSize = packed.size - (2 * Rns.HASH_LENGTH + Rns.SIGNATURE_LENGTH) - 16
        message.attempts++
        if (contentSize <= Lxmf.LINK_PACKET_MAX_CONTENT) {
            val raw = linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_NONE, packed)
            val hash = Rns.packetHash(raw)
            receipts[Rns.hex(hash.copyOf(Rns.HASH_LENGTH))] = Receipt(message.id, hash, link.peerSigningPublic, now())
            message.method = "link"
            sentAt[message.id] = now()
            setState(message, MessageState.Sent, WAITING_CONFIRMATION)
            return
        }
        if (packed.size > Lxmf.RESOURCE_MAX_SIZE) return fail(message.id, "The message is too large")
        val blob = RnsCrypto.tokenEncrypt(link.key, Rns.randomBytes(Lxmf.RESOURCE_RANDOM_LENGTH) + packed)
        val sdu = link.mtu - Rns.HEADER_MAX_SIZE - 1
        val parts = ArrayList<ByteArray>()
        var offset = 0
        while (offset < blob.size) {
            parts.add(blob.copyOfRange(offset, minOf(blob.size, offset + sdu)))
            offset += sdu
        }
        var randomHash = ByteArray(0)
        var mapHashes: List<ByteArray> = emptyList()
        for (attempt in 0 until 8) {
            randomHash = Rns.randomBytes(Lxmf.RESOURCE_RANDOM_LENGTH)
            val random = randomHash
            mapHashes = parts.map { Lxmf.resourceMapHash(it, random) }
            if (mapHashes.map(Rns::hex).toSet().size == mapHashes.size) break
        }
        val hash = Rns.fullHash(packed, randomHash)
        val resource = OutgoingResource(
            linkId = link.hex, messageId = message.id, hash = hash, randomHash = randomHash, expectedProof = Rns.fullHash(packed, hash),
            parts = parts, mapHashes = mapHashes, dataSize = packed.size.toLong(), transferSize = blob.size.toLong(), lastActivity = now(),
        )
        message.method = "resource"
        message.progress = 0
        resource.queued = outgoingResources.values.any { it.linkId == link.hex && !it.queued }
        outgoingResources[Rns.hex(hash)] = resource
        if (resource.queued) {
            setState(message, MessageState.Sending, "Waiting for the transfer before it")
        } else {
            advertiseResource(resource)
            setState(message, MessageState.Sending, "Offering ${packed.size} bytes")
        }
    }

    private fun finishResource(key: String): OutgoingResource? {
        val resource = outgoingResources.remove(key) ?: return null
        val next = outgoingResources.values.firstOrNull { it.linkId == resource.linkId && it.queued }
        if (next != null && links.containsKey(next.linkId)) {
            next.queued = false
            next.lastActivity = now()
            advertiseResource(next)
            find(next.messageId)?.let { setState(it, MessageState.Sending, "Offering ${next.dataSize} bytes") }
        }
        return resource
    }

    private fun advertiseResource(resource: OutgoingResource) {
        val link = links[resource.linkId] ?: return
        val advertisement = ResourceAdvertisement(
            transferSize = resource.transferSize, dataSize = resource.dataSize, parts = resource.parts.size, hash = resource.hash,
            randomHash = resource.randomHash, originalHash = resource.hash, flags = 0x01,
            hashmap = resource.mapHashes.fold(ByteArray(0)) { acc, bytes -> acc + bytes },
        )
        linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_RESOURCE_ADVERTISEMENT, Lxmf.packResourceAdvertisement(advertisement, 0))
        resource.advertisements++
        resource.lastActivity = now()
    }

    private fun linkTo(peer: ByteArray): Link? = links.values.firstOrNull { it.initiator && it.peer.contentEquals(peer) }

    private fun openLink(peer: ByteArray): Link? {
        val identity = node.identityFor(peer) ?: return null
        if (links.size >= MAX_LINKS) return null
        val ephemeral = RnsIdentity.generate()
        val data = ephemeral.encryptionPublicKey + ephemeral.signingPublicKey
        val raw = node.sendPacket(Packet(packetType = Rns.PACKET_LINK_REQUEST, destinationType = Rns.DESTINATION_SINGLE, destinationHash = peer, data = data))
        val link = Link(
            id = Rns.packetHash(raw).copyOf(Rns.HASH_LENGTH),
            initiator = true,
            peer = peer,
            interfaceId = node.destination(peer)?.interfaceId.orEmpty(),
            ownEncryptionPrivate = ephemeral.privateKey.copyOf(32),
            ownEncryptionPublic = ephemeral.encryptionPublicKey,
            ownSigningPrivate = ephemeral.privateKey.copyOfRange(32, 64),
            peerSigningPublic = identity.signingPublicKey,
            requestedAt = now(),
        )
        links[link.hex] = link
        node.addEvent("link", "Opening a link to ${peerName(peer)}", peer)
        updateTicking()
        return link
    }

    private fun linkPacket(link: Link, type: Int, context: Int, plaintext: ByteArray, encrypt: Boolean = true): ByteArray {
        val raw = Rns.packPacket(
            Packet(
                packetType = type,
                destinationType = Rns.DESTINATION_LINK,
                destinationHash = link.id,
                context = context,
                data = if (encrypt) RnsCrypto.tokenEncrypt(link.key, plaintext) else plaintext,
            ),
        )
        node.sendRaw(raw, link.interfaceId)
        link.lastOutbound = now()
        return raw
    }

    private fun closeLink(linkId: String, notifyPeer: Boolean) {
        val link = links[linkId] ?: return
        if (notifyPeer && link.key.isNotEmpty()) linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_LINK_CLOSE, link.id)
        val waiting = link.waiting.toList()
        links.remove(linkId)
        incomingResources.entries.removeAll { it.value.linkId == linkId }
        val failed = outgoingResources.entries.filter { it.value.linkId == linkId }
        for (entry in failed) {
            outgoingResources.remove(entry.key)
            fail(entry.value.messageId, "The link closed before the message arrived")
        }
        for (id in waiting) fail(id, "The link closed")
        notifyChanged()
    }

    fun handlePacket(interfaceId: String, packet: Packet, raw: ByteArray, hops: Int) = synchronized(lock) {
        val own = node.identity()
        if (closed || own == null || !own.hasPrivateKey) return@synchronized
        when (packet.packetType) {
            Rns.PACKET_PROOF -> handleProof(interfaceId, packet)
            Rns.PACKET_LINK_REQUEST -> if (packet.destinationHash.contentEquals(address())) handleLinkRequest(interfaceId, packet, raw)
            Rns.PACKET_DATA -> {
                if (packet.destinationType == Rns.DESTINATION_LINK) {
                    handleLinkData(packet, raw)
                } else if (packet.destinationType == Rns.DESTINATION_SINGLE && packet.destinationHash.contentEquals(address()) && packet.context == Rns.CONTEXT_NONE) {
                    val plaintext = own.decrypt(packet.data) ?: return@synchronized
                    val hash = Rns.packetHash(raw)
                    node.sendRaw(RnsCrypto.buildProof(signingPrivate(), hash, false, hash.copyOf(Rns.HASH_LENGTH), Rns.DESTINATION_SINGLE), interfaceId)
                    receiveLxmf(address() + plaintext, "opportunistic")
                }
            }
        }
    }

    private fun handleProof(interfaceId: String, packet: Packet) {
        if (packet.destinationType == Rns.DESTINATION_LINK && packet.context == Rns.CONTEXT_LINK_REQUEST_PROOF) {
            val link = links[Rns.hex(packet.destinationHash)] ?: return
            if (!link.initiator || link.active) return
            if (packet.data.size != 96 && packet.data.size != 99) return
            var signalBytes = ByteArray(0)
            var mtu = Rns.MTU
            if (packet.data.size == 99) {
                signalBytes = packet.data.copyOfRange(96, 99)
                if ((signalBytes[0].toInt() and 0xff) shr 5 != 1) return
                mtu = (((signalBytes[0].toInt() and 0xff) shl 16) or ((signalBytes[1].toInt() and 0xff) shl 8) or (signalBytes[2].toInt() and 0xff)) and 0x1FFFFF
            }
            val peerPublic = packet.data.copyOfRange(64, 96)
            if (!RnsCrypto.ed25519Verify(link.peerSigningPublic, packet.data.copyOf(64), link.id + peerPublic + link.peerSigningPublic + signalBytes)) return
            val shared = RnsCrypto.x25519Exchange(link.ownEncryptionPrivate, peerPublic)
            if (shared.isEmpty()) return
            link.key = Rns.hkdf(64, shared, link.id)
            link.active = true
            link.mtu = mtu.coerceIn(Rns.MTU, Rns.STREAM_HARDWARE_MTU)
            link.interfaceId = interfaceId
            link.lastInbound = now()
            val rtt = (now() - link.requestedAt) / 1000.0
            linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_LINK_RTT, MsgPackWriter().real(rtt).bytes())
            node.addEvent("link", "Link to ${peerName(link.peer)} is up (${(rtt * 1000).toLong()} ms)", link.peer)
            val waiting = link.waiting.toList()
            link.waiting.clear()
            for (messageId in waiting) {
                val message = find(messageId)
                val current = links[link.hex]
                if (message != null && current != null) deliverOverLink(message, current)
            }
            return
        }
        if (packet.destinationType == Rns.DESTINATION_LINK && packet.context == Rns.CONTEXT_RESOURCE_PROOF) {
            if (packet.data.size != 2 * Rns.FULL_HASH_LENGTH) return
            val key = Rns.hex(packet.data.copyOf(Rns.FULL_HASH_LENGTH))
            val resource = outgoingResources[key] ?: return
            if (!packet.data.copyOfRange(Rns.FULL_HASH_LENGTH, packet.data.size).contentEquals(resource.expectedProof)) return
            finishResource(key)
            val message = find(resource.messageId) ?: return
            message.progress = 100
            setState(message, MessageState.Delivered, "")
            node.addEvent("message", "Message delivered to ${peerName(message.peer)}", message.peer)
            return
        }
        var key = packet.destinationHash
        if (packet.destinationType == Rns.DESTINATION_LINK) {
            if (packet.data.size != Rns.FULL_HASH_LENGTH + Rns.SIGNATURE_LENGTH) return
            key = packet.data.copyOf(Rns.HASH_LENGTH)
        }
        val receiptKey = Rns.hex(key)
        val receipt = receipts[receiptKey] ?: return
        if (!RnsCrypto.validateProof(receipt.signingPublic, receipt.packetHash, packet.data)) return
        receipts.remove(receiptKey)
        val message = find(receipt.messageId) ?: return
        setState(message, MessageState.Delivered, "")
        node.addEvent("message", "Message delivered to ${peerName(message.peer)}", message.peer)
    }

    private fun signalling(mtu: Int): ByteArray {
        val value = (mtu and 0x1FFFFF) + (((1 shl 5) and 0xE0) shl 16)
        return byteArrayOf((value ushr 16).toByte(), (value ushr 8).toByte(), value.toByte())
    }

    private fun handleLinkRequest(interfaceId: String, packet: Packet, raw: ByteArray) {
        if (packet.data.size != 64 && packet.data.size != 67) return
        if (links.size >= MAX_LINKS) return
        var mtu = Rns.MTU
        if (packet.data.size == 67) {
            if (((packet.data[64].toInt() and 0xE0) shr 5) != 1) return
            mtu = (((packet.data[64].toInt() and 0xff) shl 16) or ((packet.data[65].toInt() and 0xff) shl 8) or (packet.data[66].toInt() and 0xff)) and 0x1FFFFF
            mtu = mtu.coerceIn(Rns.MTU, Rns.STREAM_HARDWARE_MTU)
        }
        val start = if (packet.headerType == Rns.HEADER_2) Rns.HASH_LENGTH + 2 else 2
        var hashable = byteArrayOf((raw[0].toInt() and 0x0F).toByte()) + raw.copyOfRange(start, raw.size)
        if (packet.data.size > 64) hashable = hashable.copyOf(hashable.size - (packet.data.size - 64))
        val linkId = Rns.truncatedHash(hashable)
        val linkKey = Rns.hex(linkId)
        if (linkKey in links) return
        val ephemeral = RnsIdentity.generate()
        val shared = RnsCrypto.x25519Exchange(ephemeral.privateKey.copyOf(32), packet.data.copyOf(32))
        if (shared.isEmpty()) return
        val own = node.identity() ?: return
        val link = Link(
            id = linkId,
            initiator = false,
            peer = ByteArray(0),
            interfaceId = interfaceId,
            ownEncryptionPrivate = ephemeral.privateKey.copyOf(32),
            ownEncryptionPublic = ephemeral.encryptionPublicKey,
            ownSigningPrivate = signingPrivate(),
            peerSigningPublic = packet.data.copyOfRange(32, 64),
            key = Rns.hkdf(64, shared, linkId),
            mtu = mtu,
            requestedAt = now(),
            lastInbound = now(),
        )
        val signalBytes = signalling(mtu)
        val signature = RnsCrypto.ed25519Sign(link.ownSigningPrivate, linkId + link.ownEncryptionPublic + own.signingPublicKey + signalBytes)
        val proof = Packet(
            packetType = Rns.PACKET_PROOF,
            destinationType = Rns.DESTINATION_LINK,
            destinationHash = linkId,
            context = Rns.CONTEXT_LINK_REQUEST_PROOF,
            data = signature + link.ownEncryptionPublic + signalBytes,
        )
        node.sendRaw(Rns.packPacket(proof), interfaceId)
        links[linkKey] = link
        updateTicking()
    }

    private fun handleLinkData(packet: Packet, raw: ByteArray) {
        val link = links[Rns.hex(packet.destinationHash)] ?: return
        if (link.key.isEmpty()) return
        link.lastInbound = now()
        when (packet.context) {
            Rns.CONTEXT_KEEPALIVE -> {
                if (!link.initiator && packet.data.size == 1 && packet.data[0] == 0xFF.toByte()) {
                    linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_KEEPALIVE, byteArrayOf(0xFE.toByte()), false)
                }
                return
            }
            Rns.CONTEXT_RESOURCE -> {
                handleResourcePart(link, packet.data)
                return
            }
        }
        val plaintext = RnsCrypto.tokenDecrypt(link.key, packet.data) ?: return
        when (packet.context) {
            Rns.CONTEXT_LINK_RTT -> if (!link.initiator) link.active = true
            Rns.CONTEXT_LINK_CLOSE -> if (plaintext.contentEquals(link.id)) closeLink(link.hex, false)
            Rns.CONTEXT_NONE -> {
                val hash = Rns.packetHash(raw)
                val proof = Packet(
                    packetType = Rns.PACKET_PROOF,
                    destinationType = Rns.DESTINATION_LINK,
                    destinationHash = link.id,
                    data = hash + RnsCrypto.ed25519Sign(link.ownSigningPrivate, hash),
                )
                node.sendRaw(Rns.packPacket(proof), link.interfaceId)
                if (!link.initiator) receiveLxmf(plaintext, "link")
            }
            Rns.CONTEXT_RESOURCE_ADVERTISEMENT -> handleResourceAdvertisement(link, plaintext)
            Rns.CONTEXT_RESOURCE_REQUEST -> handleResourceRequest(link, plaintext)
            Rns.CONTEXT_RESOURCE_HASHMAP_UPDATE -> handleHashmapUpdate(plaintext)
            Rns.CONTEXT_RESOURCE_INITIATOR_CANCEL -> incomingResources.remove(Rns.hex(plaintext))
            Rns.CONTEXT_RESOURCE_RECEIVER_CANCEL -> {
                val key = Rns.hex(plaintext)
                val resource = finishResource(key)
                if (resource != null) fail(resource.messageId, "The recipient refused the message")
            }
        }
    }

    private fun handleResourceAdvertisement(link: Link, plaintext: ByteArray) {
        val advertisement = Lxmf.unpackResourceAdvertisement(plaintext) ?: return
        val key = Rns.hex(advertisement.hash)
        if (key in incomingResources) return
        if (link.initiator || advertisement.compressed || advertisement.segments > 1 || advertisement.isRequest ||
            advertisement.isResponse || advertisement.transferSize > MAX_INCOMING_RESOURCE + 4096 || incomingResources.size >= MAX_INCOMING_RESOURCES
        ) {
            linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_RESOURCE_RECEIVER_CANCEL, advertisement.hash)
            return
        }
        val hashmap = Array(advertisement.parts) { ByteArray(0) }
        var i = 0
        while (i * Lxmf.RESOURCE_MAP_HASH_LENGTH < advertisement.hashmap.size && i < advertisement.parts) {
            hashmap[i] = advertisement.hashmap.copyOfRange(i * Lxmf.RESOURCE_MAP_HASH_LENGTH, (i + 1) * Lxmf.RESOURCE_MAP_HASH_LENGTH)
            i++
        }
        val resource = IncomingResource(link.hex, advertisement, hashmap, Array(advertisement.parts) { ByteArray(0) }, lastActivity = now())
        incomingResources[key] = resource
        requestNextParts(resource)
        updateTicking()
    }

    private fun requestNextParts(resource: IncomingResource) {
        val link = links[resource.linkId] ?: return
        if (resource.waitingForHashmap) return
        var hashes = ByteArray(0)
        var exhausted = false
        var requested = 0
        for (i in resource.parts.indices) {
            if (requested >= REQUEST_WINDOW) break
            if (resource.parts[i].isNotEmpty()) continue
            if (resource.hashmap[i].isEmpty()) {
                exhausted = true
                break
            }
            hashes += resource.hashmap[i]
            requested++
        }
        if (requested == 0 && !exhausted) return
        var request = byteArrayOf(if (exhausted) 0xFF.toByte() else 0x00)
        if (exhausted) {
            var height = 0
            while (height < resource.hashmap.size && resource.hashmap[height].isNotEmpty()) height++
            if (height == 0) return
            request += resource.hashmap[height - 1]
            resource.waitingForHashmap = true
        }
        request += resource.advertisement.hash + hashes
        resource.requested = requested
        resource.lastActivity = now()
        linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_RESOURCE_REQUEST, request)
    }

    private fun handleResourcePart(link: Link, data: ByteArray) {
        for ((key, resource) in incomingResources) {
            if (resource.linkId != link.hex) continue
            val mapHash = Lxmf.resourceMapHash(data, resource.advertisement.randomHash)
            for (i in resource.parts.indices) {
                if (resource.parts[i].isEmpty() && resource.hashmap[i].contentEquals(mapHash)) {
                    resource.parts[i] = data
                    resource.received++
                    resource.requested--
                    resource.lastActivity = now()
                    resource.retries = 0
                    if (resource.received == resource.parts.size) assembleResource(key) else if (resource.requested <= 0) requestNextParts(resource)
                    return
                }
            }
        }
    }

    private fun handleHashmapUpdate(plaintext: ByteArray) {
        if (plaintext.size <= Rns.FULL_HASH_LENGTH) return
        val resource = incomingResources[Rns.hex(plaintext.copyOf(Rns.FULL_HASH_LENGTH))] ?: return
        val update = Rns.decodeMsgPack(plaintext.copyOfRange(Rns.FULL_HASH_LENGTH, plaintext.size)).value as? List<*> ?: return
        if (update.size < 2) return
        val segment = MsgPack.toInt(update[0])
        val hashmap = MsgPack.toBytes(update[1])
        var i = 0
        while (i * Lxmf.RESOURCE_MAP_HASH_LENGTH < hashmap.size) {
            val index = segment * Lxmf.RESOURCE_HASHMAP_MAX_LENGTH + i
            val end = minOf(hashmap.size, (i + 1) * Lxmf.RESOURCE_MAP_HASH_LENGTH)
            if (index >= 0 && index < resource.hashmap.size) resource.hashmap[index] = hashmap.copyOfRange(i * Lxmf.RESOURCE_MAP_HASH_LENGTH, end)
            i++
        }
        resource.waitingForHashmap = false
        resource.lastActivity = now()
        requestNextParts(resource)
    }

    private fun assembleResource(key: String) {
        val resource = incomingResources.remove(key) ?: return
        val link = links[resource.linkId] ?: return
        val joined = java.io.ByteArrayOutputStream()
        resource.parts.forEach { joined.write(it) }
        val plain = RnsCrypto.tokenDecrypt(link.key, joined.toByteArray()) ?: return
        if (plain.size < Lxmf.RESOURCE_RANDOM_LENGTH) return
        val data = plain.copyOfRange(Lxmf.RESOURCE_RANDOM_LENGTH, plain.size)
        val hash = resource.advertisement.hash
        if (!Rns.fullHash(data, resource.advertisement.randomHash).contentEquals(hash)) return
        linkPacket(link, Rns.PACKET_PROOF, Rns.CONTEXT_RESOURCE_PROOF, hash + Rns.fullHash(data, hash), false)
        var payload = data
        if (resource.advertisement.hasMetadata && payload.size >= 3) {
            val size = ((payload[0].toInt() and 0xff) shl 16) or ((payload[1].toInt() and 0xff) shl 8) or (payload[2].toInt() and 0xff)
            payload = if (3 + size <= payload.size) payload.copyOfRange(3 + size, payload.size) else ByteArray(0)
        }
        receiveLxmf(payload, "resource")
    }

    private fun handleResourceRequest(link: Link, plaintext: ByteArray) {
        if (plaintext.isEmpty()) return
        val exhausted = plaintext[0] == 0xFF.toByte()
        val pad = if (exhausted) 1 + Lxmf.RESOURCE_MAP_HASH_LENGTH else 1
        if (plaintext.size < pad + Rns.FULL_HASH_LENGTH) return
        val resource = outgoingResources[Rns.hex(plaintext.copyOfRange(pad, pad + Rns.FULL_HASH_LENGTH))] ?: return
        if (resource.linkId != link.hex) return
        resource.requested = true
        resource.lastActivity = now()
        val requested = plaintext.copyOfRange(pad + Rns.FULL_HASH_LENGTH, plaintext.size)
        var offset = 0
        while (offset + Lxmf.RESOURCE_MAP_HASH_LENGTH <= requested.size) {
            val wanted = requested.copyOfRange(offset, offset + Lxmf.RESOURCE_MAP_HASH_LENGTH)
            val index = resource.mapHashes.indexOfFirst { it.contentEquals(wanted) }
            if (index >= 0) {
                linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_RESOURCE, resource.parts[index], false)
                resource.sent.add(index)
            }
            offset += Lxmf.RESOURCE_MAP_HASH_LENGTH
        }
        if (exhausted) {
            val last = plaintext.copyOfRange(1, 1 + Lxmf.RESOURCE_MAP_HASH_LENGTH)
            val index = resource.mapHashes.indexOfFirst { it.contentEquals(last) }
            val partIndex = index + 1
            if (index >= 0 && partIndex % Lxmf.RESOURCE_HASHMAP_MAX_LENGTH == 0) {
                val segment = partIndex / Lxmf.RESOURCE_HASHMAP_MAX_LENGTH
                val from = segment * Lxmf.RESOURCE_HASHMAP_MAX_LENGTH
                val slice = resource.mapHashes.drop(from).take(Lxmf.RESOURCE_HASHMAP_MAX_LENGTH)
                if (slice.isNotEmpty()) {
                    val hashmap = slice.fold(ByteArray(0)) { acc, bytes -> acc + bytes }
                    linkPacket(link, Rns.PACKET_DATA, Rns.CONTEXT_RESOURCE_HASHMAP_UPDATE, resource.hash + MsgPackWriter().array(2).integer(segment).bin(hashmap).bytes())
                }
            }
        }
        val message = find(resource.messageId) ?: return
        message.progress = 100 * resource.sent.size / maxOf(1, resource.parts.size)
        setState(message, MessageState.Sending, "${message.progress}% sent")
    }

    private fun receiveLxmf(packed: ByteArray, method: String) {
        val lxmf = Lxmf.unpack(packed).value ?: return
        if (!lxmf.destinationHash.contentEquals(address())) return
        val id = Rns.hex(lxmf.hash)
        if (id in seenIds) return
        val message = ChatMessage(
            id = id,
            peer = lxmf.sourceHash,
            outgoing = false,
            title = lxmf.title,
            content = lxmf.content,
            timestamp = if (lxmf.timestamp > 0) lxmf.timestamp else now() / 1000.0,
            state = MessageState.Received,
            method = method,
            read = false,
            attachments = lxmf.attachments,
        )
        val source = node.identityFor(lxmf.sourceHash)
        if (source != null) {
            message.verified = Lxmf.verify(lxmf, source.publicKey)
            if (!message.verified) message.note = "The signature does not match the sender's announced key"
        } else {
            message.note = "Sender not yet heard on the network, so the signature is unchecked"
        }
        seenIds.add(id)
        messages.add(message)
        if (messages.size > MAX_MESSAGES + 500) messages.subList(0, messages.size - MAX_MESSAGES).clear()
        val name = peerName(message.peer)
        if (!name.startsWith("<")) names[message.peerHex] = name
        node.addEvent("message", "Message from $name", message.peer)
        scheduleSave()
        notifyChanged()
        val copy = message.copy()
        timers.schedule(0) { incomingHandler?.invoke(copy) }
    }

    private fun notifyChanged() {
        if (changePending) return
        changePending = true
        timers.schedule(CHANGE_DELAY_MS) {
            synchronized(lock) { changePending = false }
            changedHandler?.invoke()
        }
    }

    private fun scheduleSave() {
        if (savePending) return
        savePending = true
        timers.schedule(SAVE_DELAY_MS) {
            val json = synchronized(lock) {
                savePending = false
                saveJson()
            }
            saveHandler?.invoke(json)
        }
    }

    private fun updateTicking() {
        val pending = links.isNotEmpty() || incomingResources.isNotEmpty() || outgoingResources.isNotEmpty() ||
            messages.any { it.outgoing && (it.state.ordinal < MessageState.Sent.ordinal || (it.state == MessageState.Sent && it.attempts < 3)) }
        if (pending && tickTimer == null && !closed) {
            tickTimer = timers.every(1000) { synchronized(lock) { if (tickTimer != null && !closed) tick() } }
        } else if (!pending && tickTimer != null) {
            tickTimer?.cancel()
            tickTimer = null
        }
    }

    private fun tick() {
        val now = now()
        val toProcess = ArrayList<String>()
        val toFail = ArrayList<String>()
        for (message in messages) {
            if (!message.outgoing) continue
            when {
                message.state == MessageState.Queued && node.onlineCount() > 0 -> toProcess.add(message.id)
                message.state == MessageState.FindingPath -> {
                    if (node.identityFor(message.peer) != null) toProcess.add(message.id)
                    else if (now - (waitingSince[message.id] ?: now) > PATH_WAIT_SECONDS * 1000L) toFail.add(message.id)
                }
                message.state == MessageState.Sent && message.method == "opportunistic" && now - (sentAt[message.id] ?: now) > 15000 && message.attempts < 3 ->
                    toProcess.add(message.id)
                message.state == MessageState.Linking && now - (waitingSince[message.id] ?: now) > LINK_WAIT_SECONDS * 1000L -> toFail.add(message.id)
            }
        }
        for (id in toFail) {
            val message = find(id) ?: continue
            if (message.state == MessageState.FindingPath) fail(id, "No path to this contact. They may be offline; retry later.")
            else fail(id, "Could not open a link to this contact")
        }
        for (id in toProcess) process(id)
        val stale = ArrayList<String>()
        for (link in links.values) {
            if (!link.active && link.initiator && now - link.requestedAt > LINK_WAIT_SECONDS * 1000L) {
                stale.add(link.hex)
                continue
            }
            val busy = link.waiting.isNotEmpty() || outgoingResources.values.any { it.linkId == link.hex } || incomingResources.values.any { it.linkId == link.hex }
            val idle = now - maxOf(link.lastInbound, link.lastOutbound)
            when {
                !busy && link.initiator && link.active && idle > LINK_IDLE_SECONDS * 1000L -> stale.add(link.hex)
                !link.initiator && !link.active && now - link.requestedAt > 60_000 -> stale.add(link.hex)
                !link.initiator && now - link.lastInbound > 15 * 60_000 -> stale.add(link.hex)
            }
        }
        for (id in stale) closeLink(id, true)
        val dropped = ArrayList<String>()
        for ((key, resource) in incomingResources) {
            if (now - resource.lastActivity < RESOURCE_STALL_SECONDS * 1000L) continue
            if (++resource.retries > 5) {
                dropped.add(key)
                continue
            }
            resource.waitingForHashmap = false
            requestNextParts(resource)
        }
        dropped.forEach { incomingResources.remove(it) }
        val failed = ArrayList<String>()
        for ((key, resource) in outgoingResources) {
            if (resource.queued) continue
            if (!resource.requested && now - resource.lastActivity > 10_000) {
                if (resource.advertisements >= 4) failed.add(key) else advertiseResource(resource)
            } else if (resource.requested && now - resource.lastActivity > 60_000) {
                failed.add(key)
            }
        }
        for (key in failed) finishResource(key)?.let { fail(it.messageId, "The transfer stalled") }
        receipts.entries.removeAll { now - it.value.sentAt > 10 * 60_000 }
        for (message in messages) {
            if (message.outgoing && message.state == MessageState.Sent && message.attempts >= 3 && message.note == WAITING_CONFIRMATION) {
                message.note = "No delivery confirmation yet"
                notifyChanged()
            }
        }
        updateTicking()
    }

    companion object {
        const val MAX_MESSAGES = 5000
        const val MAX_CONTENT_BYTES = 64 * 1024
        const val MAX_INCOMING_RESOURCE = Lxmf.RESOURCE_MAX_SIZE
        const val PATH_WAIT_SECONDS = 30
        const val LINK_WAIT_SECONDS = 30
        const val RESOURCE_STALL_SECONDS = 15
        const val LINK_IDLE_SECONDS = 120
        const val REQUEST_WINDOW = 64
        const val MAX_LINKS = 64
        const val MAX_INCOMING_RESOURCES = 8
        const val OWN_ANNOUNCE_GAP_MS = 30 * 60 * 1000L
        const val CHANGE_DELAY_MS = 100L
        const val SAVE_DELAY_MS = 1500L
        const val WAITING_CONFIRMATION = "Waiting for delivery confirmation"
    }
}
