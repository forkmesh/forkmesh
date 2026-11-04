package com.forkmesh.nativeapp.reticulum

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertHex
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.Collections
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

private const val ANNOUNCE_HEX =
    "0100fae321c442e3c9bdcd7a3e79d850e03c008f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd76ec60bc318e2c0f0d9080102030405006955b900b5fd2a0dce06f324f4ba40cef2dd50b2fde097047ac7a137b4feb5fe8277af2ddeb6608cb297548ddc310df93bd21fa3fc8524730d84d220e455e337fc0cbd0492c40d4669656c6420726164696f203308"
private const val IFAC_HEX =
    "8987de53fc094c1651140853650234adc90c7f92c3561c02f4f7bdb0b1aa5b672d57d930ead724bf75210cd8c5ca654cc852e78d3619fe8a8a38edea430e28caacc28baa2d751f2e9f9d913bd508c62212464a252fa9a79b1327f5db10b3e591f9e09497a158d34a3d41070d2ddab3f371cb643c7e9b6b1fb7449d236224ddb2804f3253382aeea2cca4ab33e4d02b6cc7cff11f81b25e3664dfb55d0372d967470b94ba417d937564e631fedbc0dca27fa0ea338a3955f54245d1c912e09b24b3878e889563514f"
private const val TOKEN_HEX =
    "6465666768696a6b6c6d6e6f707172734a2e1cee965671c7b3f3bb5d40b699b325050fc0dc0720ae87a7d35c464cbc030aa3e48331a87aab70bae65e973c64e7"
private const val LXMF_HEX =
    "00000000000000000000000000000000fae321c442e3c9bdcd7a3e79d850e03c31912fa052aa7a972361c0167df3054b97d9f98705a4148862aba5a5f7305397af3e2a6738f9293e45136dfcbb089364645f72012efb5834d0652b91fbaa430194cb41da556e40200000c40174c40568656c6c6f80"
private const val PRIVATE_KEY_HEX =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"

private fun waitUntil(timeoutMs: Long = 10000, condition: () -> Boolean): Boolean {
    val deadline = System.currentTimeMillis() + timeoutMs
    while (!condition()) {
        if (System.currentTimeMillis() > deadline) return false
        Thread.sleep(20)
    }
    return true
}

private class Hub {
    val server = ServerSocket(0, 10, InetAddress.getLoopbackAddress())
    val clients: MutableList<Socket> = Collections.synchronizedList(ArrayList())
    private val outputs = HashMap<Socket, OutputStream>()

    init {
        thread(isDaemon = true) {
            while (!server.isClosed) {
                val socket = runCatching { server.accept() }.getOrNull() ?: break
                synchronized(outputs) { outputs[socket] = socket.getOutputStream() }
                clients.add(socket)
                thread(isDaemon = true) {
                    val buffer = ByteArray(65536)
                    runCatching {
                        val input = socket.getInputStream()
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            synchronized(outputs) {
                                for ((other, output) in outputs) {
                                    if (other !== socket) runCatching { output.write(buffer, 0, count); output.flush() }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    val port: Int get() = server.localPort

    fun close() {
        synchronized(outputs) { outputs.keys.forEach { runCatching { it.close() } } }
        runCatching { server.close() }
    }
}

private class Peer(name: String, port: Int, networkName: String = "", passphrase: String = "") {
    val node = ReticulumNode()
    val messenger = ReticulumMessenger(node)

    init {
        node.setIdentity(RnsIdentity.generate())
        messenger.setDisplayName(name)
        node.announceHandler = { messenger.destinationAnnounced(it) }
        node.packetHandler = { id, packet, raw, hops -> messenger.handlePacket(id, packet, raw, hops) }
        node.setInterfaces(listOf(InterfaceConfig(id = "hub", name = "Hub", host = "127.0.0.1", port = port, networkName = networkName, passphrase = passphrase)))
        node.start()
    }

    fun close() {
        messenger.close()
        node.close()
    }
}

class ReticulumMessagingTest {
    fun testCryptoMatchesPython() {
        val key = ByteArray(64) { it.toByte() }
        assertHex(TOKEN_HEX, RnsCrypto.tokenEncrypt(key, "secret message".toByteArray(), ByteArray(16) { (100 + it).toByte() }))
        assertEq("secret message", String(RnsCrypto.tokenDecrypt(key, bytesOfHex(TOKEN_HEX))!!), "Python tokens decrypt")
        val forged = bytesOfHex(TOKEN_HEX).also { it[20] = (it[20].toInt() xor 1).toByte() }
        expect(RnsCrypto.tokenDecrypt(key, forged) == null, "A tampered token is refused")
        expect(RnsCrypto.tokenDecrypt(key, ByteArray(40)) == null, "A short token is refused")

        val code = InterfaceAccessCode("net", "pass")
        val announce = bytesOfHex(ANNOUNCE_HEX)
        assertHex(IFAC_HEX, code.apply(announce))
        expect(code.remove(bytesOfHex(IFAC_HEX))!!.contentEquals(announce), "A Python IFAC frame unmasks to the announce")
        expect(InterfaceAccessCode("net", "wrong").remove(bytesOfHex(IFAC_HEX)) == null, "The wrong passphrase rejects the frame")
        expect(code.remove(announce) == null, "A frame without the IFAC flag is rejected")
        expect(!InterfaceAccessCode().enabled && InterfaceAccessCode("", "", 16).apply(announce).contentEquals(announce), "No name and passphrase means no code")

        val identity = RnsIdentity.fromPrivateKey(bytesOfHex(PRIVATE_KEY_HEX))!!
        val source = Rns.destinationHashForName("lxmf.delivery", identity.hash)
        val packed = Lxmf.pack(identity, source, ByteArray(16), 1767225600.5, "t", "hello")!!
        assertHex(LXMF_HEX, packed.packed)
        assertHex("bac2ea5d74932d9994fe7d268965ae2f162f5649c169d0b6ab84c42f23498efb", packed.hash)
        val unpacked = Lxmf.unpack(bytesOfHex(LXMF_HEX)).value!!
        expect(unpacked.title == "t" && unpacked.content == "hello" && unpacked.timestamp == 1767225600.5 && Lxmf.verify(unpacked, identity.publicKey), "Python LXMF unpacks and verifies")
        assertHex("93c40550686f6e65c090", Lxmf.announceData("Phone"))
        assertEq(0, Lxmf.stampCostFromAppData(Lxmf.announceData("Phone")))
        assertEq(8, Lxmf.stampCostFromAppData(Lxmf.announceData("Phone", 8)))
        assertEq("Phone", Rns.displayNameFromAppData("peer", Lxmf.announceData("Phone")))
        assertHex("61c97deae4780c352f6f46dbac5ac73d6e585a4ca8310870175f00d840b586ed", Rns.fullHash(Lxmf.stampWorkblock(packed.hash, Lxmf.STAMP_ROUNDS)))
    }

    fun testLxmfAndStamps() {
        val sender = RnsIdentity.generate()
        val source = Rns.destinationHashForName("lxmf.delivery", sender.hash)
        val stamped = Lxmf.pack(sender, source, ByteArray(16) { 'd'.code.toByte() }, 1767225600.5, "t", "hello", ByteArray(32) { 's'.code.toByte() })!!
        val unpacked = Lxmf.unpack(stamped.packed).value!!
        expect(unpacked.content == "hello" && unpacked.stamp.contentEquals(ByteArray(32) { 's'.code.toByte() }) && unpacked.hash.contentEquals(stamped.hash) && Lxmf.verify(unpacked, sender.publicKey),
            "LXMF messages with a stamp round trip and verify")
        val payload = Lxmf.payload(1767225600.5, "t", "hello")
        expect(payload[0] == 0x94.toByte() && payload[1] == 0xcb.toByte(), "LXMF payload is a four-item array starting with a float")
        val recipient = RnsIdentity.generate()
        expect(String(recipient.decrypt(recipient.encrypt("to you".toByteArray()))!!) == "to you" && RnsIdentity.generate().decrypt(recipient.encrypt("to you".toByteArray())) == null,
            "Identity encryption opens only for the recipient")
        val ratchet = ByteArray(32).also { java.security.SecureRandom().nextBytes(it) }
        expect(recipient.decrypt(recipient.encrypt("x".toByteArray(), RnsCrypto.x25519PublicFromPrivate(ratchet))) == null, "A ratchet-encrypted token needs the ratchet key")
        val id = Rns.fullHash("message".toByteArray())
        val stamp = Lxmf.generateStamp(id, 6)!!
        expect(stamp.stamp.size == 32 && stamp.value >= 6 && Lxmf.validateStamp(id, stamp.stamp, 6), "Stamps are generated and validated")
        expect(Lxmf.generateStamp(id, 30) == null && Lxmf.generateStamp(id, 20, AtomicBoolean(true)) == null, "Too costly or cancelled stamps stop")
        val signer = RnsIdentity.generate()
        val hash = Rns.fullHash("packet".toByteArray())
        val proof = Rns.parsePacket(RnsCrypto.buildProof(signer.privateKey.copyOfRange(32, 64), hash, true, ByteArray(16), Rns.DESTINATION_LINK)).value!!
        expect(RnsCrypto.validateProof(signer.signingPublicKey, hash, proof.data) && !RnsCrypto.validateProof(signer.signingPublicKey, Rns.fullHash("x".toByteArray()), proof.data), "Proofs validate for their packet only")
        val advertisement = ResourceAdvertisement(transferSize = 100, dataSize = 90, parts = 1, hash = ByteArray(32), randomHash = ByteArray(4), originalHash = ByteArray(32), flags = 1, hashmap = ByteArray(4))
        val round = Lxmf.unpackResourceAdvertisement(Lxmf.packResourceAdvertisement(advertisement))!!
        expect(round.parts == 1 && round.encrypted && !round.compressed && round.transferSize == 100L, "Resource advertisements round trip")
        expect(RnsCrypto.msgPackArrayItems(MsgPackWriter().array(2).integer(1).str("a").bytes())!!.map { it.toList() } == listOf(listOf(1, 2), listOf(2, 4)), "Array items are located")
        assertHex("cd1092", MsgPackWriter().integer(4242).bytes())
        assertHex("d0de", MsgPackWriter().integer(-34).bytes())
        assertHex("ff", MsgPackWriter().integer(-1).bytes())
        assertHex("da0100", MsgPackWriter().str("a".repeat(256)).bytes().copyOf(3))
    }

    fun testRouting() {
        val sent = Collections.synchronizedList(ArrayList<ByteArray>())
        val transport = object : ReticulumTransport {
            override fun openTcp(host: String, port: Int, timeoutMs: Int, events: ReticulumConnectionEvents): ReticulumConnection {
                thread(isDaemon = true) { events.connected() }
                return object : ReticulumConnection {
                    override fun write(bytes: ByteArray): Boolean = sent.add(bytes)
                    override fun close() {}
                }
            }

            override fun openUdp(listenAddress: String, listenPort: Int, forwardAddress: String, forwardPort: Int, maxDatagram: Int, events: ReticulumConnectionEvents): ReticulumConnection =
                throw java.io.IOException("none")
        }
        val node = ReticulumNode(transport = transport)
        try {
            node.setIdentity(RnsIdentity.generate())
            node.setInterfaces(listOf(InterfaceConfig(id = "t", name = "T", host = "h.example", port = 1)))
            node.start()
            expect(waitUntil { node.onlineCount() == 1 }, "Fake link is up")
            val remote = RnsIdentity.generate()
            val near = Rns.parsePacket(Rns.buildAnnounce(remote, "lxmf.delivery", ByteArray(0), false, 1767225600)).value!!
            val transportId = ByteArray(16) { 7 }
            val far = Rns.packPacket(Packet(headerType = Rns.HEADER_2, transportType = Rns.TRANSPORT_FORWARD, packetType = Rns.PACKET_ANNOUNCE, hops = 2, transportId = transportId, destinationHash = near.destinationHash, data = near.data))
            node.ingest("t", far)
            assertEq(3, node.destination(near.destinationHash)?.hops)
            node.sendPacket(Packet(packetType = Rns.PACKET_DATA, destinationHash = near.destinationHash, data = byteArrayOf(1, 2, 3)))
            val decoder = FrameDecoder()
            val routed = Rns.parsePacket(decoder.feed(sent.last()).single()).value!!
            expect(routed.headerType == Rns.HEADER_2 && routed.transportType == Rns.TRANSPORT_FORWARD && routed.transportId.contentEquals(transportId), "Packets for a far destination go to its next hop")
            val direct = RnsIdentity.generate()
            node.ingest("t", Rns.buildAnnounce(direct, "lxmf.delivery", ByteArray(0), false, 1767225600))
            val directHash = Rns.destinationHashForName("lxmf.delivery", direct.hash)
            node.sendPacket(Packet(packetType = Rns.PACKET_DATA, destinationHash = directHash, data = byteArrayOf(1)))
            expect(Rns.parsePacket(FrameDecoder().feed(sent.last()).single()).value!!.headerType == Rns.HEADER_1, "A one-hop neighbour gets a plain packet")
            val before = node.counters().duplicates
            node.ingest("t", Rns.packPacket(Packet(headerType = Rns.HEADER_2, packetType = Rns.PACKET_DATA, transportId = transportId, destinationHash = directHash, data = byteArrayOf(9))))
            assertEq(before + 1, node.counters().duplicates, "Transported data not addressed through the shared instance is dropped")
            var handled = 0
            node.packetHandler = { _, _, _, _ -> handled++ }
            val keepalive = Rns.packPacket(Packet(packetType = Rns.PACKET_DATA, destinationType = Rns.DESTINATION_LINK, destinationHash = ByteArray(16), context = Rns.CONTEXT_KEEPALIVE, data = byteArrayOf(0xFF.toByte())))
            node.ingest("t", keepalive)
            node.ingest("t", keepalive)
            assertEq(2, handled, "Keepalives are handed on every time")
            val plain = Rns.packPacket(Packet(packetType = Rns.PACKET_DATA, destinationHash = ByteArray(16) { 3 }, data = byteArrayOf(4)))
            node.ingest("t", plain)
            node.ingest("t", plain)
            assertEq(3, handled, "Repeated ordinary packets are handed on once")
            val messenger = ReticulumMessenger(node)
            sent.clear()
            expect(node.announce() && sent.size == 2, "Every local destination is announced")
            node.ingest("t", Rns.buildPathRequest(messenger.address(), ByteArray(16) { 5 }))
            expect(waitUntil { node.counters().pathRequestsAnswered == 1L }, "Path requests for the LXMF destination are answered")
            messenger.close()
        } finally {
            node.close()
        }
    }

    fun testMessaging() {
        for (ifac in listOf(false, true)) {
            val hub = Hub()
            val net = if (ifac) "testnet" else ""
            val pass = if (ifac) "secret" else ""
            val label = if (ifac) " (IFAC)" else ""
            val alice = Peer("Alice", hub.port, net, pass)
            val bob = Peer("Bob", hub.port, net, pass)
            val stranger = if (ifac) Peer("Stranger", hub.port, net, "wrong") else null
            try {
                expect(waitUntil { alice.node.onlineCount() == 1 && bob.node.onlineCount() == 1 && hub.clients.size >= (if (ifac) 3 else 2) }, "Messaging peers connect to the hub$label")
                alice.node.announce()
                bob.node.announce()
                expect(waitUntil { bob.node.destination(alice.messenger.address()) != null && alice.node.destination(bob.messenger.address()) != null }, "Peers hear each other's LXMF announces$label")
                if (stranger != null) {
                    expect(stranger.node.destinations().isEmpty() && stranger.node.status("hub").invalidFrames > 0, "A node with the wrong passphrase hears nothing")
                }
                val longText = (0 until 300).joinToString("") { "long line $it\n" }
                val shortId = bob.messenger.send(alice.messenger.address(), "hi alice")
                val mediumId = bob.messenger.send(alice.messenger.address(), "x".repeat(300))
                val longId = bob.messenger.send(alice.messenger.address(), longText)
                fun state(id: String) = bob.messenger.message(id)?.state ?: MessageState.Failed
                expect(waitUntil(30000) { listOf(shortId, mediumId, longId).all { state(it) == MessageState.Delivered } },
                    "Short, medium and long messages are delivered$label: ${listOf(shortId, mediumId, longId).map { bob.messenger.message(it)?.let { m -> "${m.state} ${m.method} ${m.note}" } }}")
                assertEq(listOf("opportunistic", "link", "resource"), listOf(shortId, mediumId, longId).map { bob.messenger.message(it)?.method }, "Each size uses its delivery method$label")
                val got = alice.messenger.messages(bob.messenger.address())
                expect(got.size == 3 && got.all { it.verified && !it.read } && got.any { it.content == longText }, "Alice receives all three, signed, unread$label")
                expect(alice.messenger.unreadCount() == 3 && alice.messenger.conversations().firstOrNull()?.name == "Bob", "Conversations list Bob by name with 3 unread$label")
                alice.messenger.markRead(bob.messenger.address())
                assertEq(0, alice.messenger.unreadCount(), "Opening a conversation marks it read$label")
                val restored = ReticulumMessenger(alice.node)
                restored.load(alice.messenger.saveJson())
                assertEq(3, restored.messages(bob.messenger.address()).size, "Messages survive a save and load$label")
                restored.close()
                val reply = alice.messenger.send(bob.messenger.address(), "hello bob")
                expect(waitUntil(15000) { alice.messenger.message(reply)?.state == MessageState.Delivered }, "Replies go the other way$label")
                expect(waitUntil { bob.messenger.messages(alice.messenger.address()).any { !it.outgoing && it.content == "hello bob" } }, "Bob receives the reply$label")
            } finally {
                alice.close()
                bob.close()
                stranger?.close()
                hub.close()
            }
        }
    }

    fun testPersistenceAndLimits() {
        val node = ReticulumNode()
        val messenger = ReticulumMessenger(node)
        try {
            node.setIdentity(RnsIdentity.generate())
            val peer = ByteArray(16) { 9 }
            assertEq("", messenger.send(peer, " "), "Empty messages are refused")
            assertEq("", messenger.send(peer, "x".repeat(ReticulumMessenger.MAX_CONTENT_BYTES + 1)), "Messages over 64 KB are refused")
            assertEq("", messenger.send(messenger.address(), "me"), "Messages to yourself are refused")
            val id = messenger.send(peer, "queued")
            expect(id.length == 64 && messenger.message(id)?.state == MessageState.Queued, "Without an interface a message waits")
            val json = messenger.saveJson()
            val restored = ReticulumMessenger(node)
            restored.load(json)
            val back = restored.message(id)!!
            expect(back.state == MessageState.Failed && back.note.startsWith("Not sent before ForkMesh closed"), "Unsent messages load as not sent")
            restored.retry(id)
            assertEq(MessageState.Queued, restored.message(id)?.state, "Retry queues it again")
            restored.forget(id)
            expect(restored.message(id) == null, "Delete removes it")
            val attachment = ChatMessage(id = "a".repeat(64), peer = peer, attachments = listOf(LxmfAttachment("f.txt", byteArrayOf(1, 2))))
            expect(ChatMessage.fromJson(com.forkmesh.nativeapp.json.Json.parseObject(com.forkmesh.nativeapp.json.Json.stringify(attachment.toJson())))!!.attachments.single().data.contentEquals(byteArrayOf(1, 2)), "Attachments are stored")
            restored.close()
        } finally {
            messenger.close()
            node.close()
        }
    }
}
