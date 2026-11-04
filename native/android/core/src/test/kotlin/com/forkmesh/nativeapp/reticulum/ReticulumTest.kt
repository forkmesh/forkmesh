package com.forkmesh.nativeapp.reticulum

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertHex
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.Collections
import kotlin.concurrent.thread

private const val PRIVATE_KEY =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
private const val PUBLIC_KEY =
    "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7"
private const val ANNOUNCE =
    "0100fae321c442e3c9bdcd7a3e79d850e03c008f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd76ec60bc318e2c0f0d9080102030405006955b900b5fd2a0dce06f324f4ba40cef2dd50b2fde097047ac7a137b4feb5fe8277af2ddeb6608cb297548ddc310df93bd21fa3fc8524730d84d220e455e337fc0cbd0492c40d4669656c6420726164696f203308"
private const val DISCOVERY =
    "008b00b14261636b626f6e65496e7465726661636501c3ccfec410aca31af0441d81dbec71e82da0b4b5f5ccfda3524e53ccfca5312e352e37ccffa8546573742068756203cb404a2f5c28f5c28f04cb40138f5c28f5c28f05cb402400000000000002af6875622e6578616d706c652e6f726706cd1092d83b79d7a76c6bf37b3a640a739f67f92b7c3cf9efac45e303030a5cfaf7b0cc"
private const val LXMF_DESTINATION = "fae321c442e3c9bdcd7a3e79d850e03c"

private fun waitFor(timeoutMs: Long = 5000, condition: () -> Boolean): Boolean {
    val deadline = System.currentTimeMillis() + timeoutMs
    while (!condition()) {
        if (System.currentTimeMillis() > deadline) return false
        Thread.sleep(10)
    }
    return true
}

private class FakeTransport : ReticulumTransport {
    val writes: MutableList<ByteArray> = Collections.synchronizedList(ArrayList())

    override fun openTcp(host: String, port: Int, timeoutMs: Int, events: ReticulumConnectionEvents): ReticulumConnection {
        thread(isDaemon = true) { events.connected() }
        return object : ReticulumConnection {
            override fun write(bytes: ByteArray): Boolean = writes.add(bytes)
            override fun close() {}
        }
    }

    override fun openUdp(listenAddress: String, listenPort: Int, forwardAddress: String, forwardPort: Int, maxDatagram: Int, events: ReticulumConnectionEvents): ReticulumConnection =
        throw java.io.IOException("no UDP here")
}

private class FakeNode {
    val server = ServerSocket(0, 10, InetAddress.getLoopbackAddress())
    @Volatile var client: Socket? = null
    @Volatile var connections = 0
    val received: MutableList<ByteArray> = Collections.synchronizedList(ArrayList())

    init {
        thread(isDaemon = true) {
            while (!server.isClosed) {
                val socket = runCatching { server.accept() }.getOrNull() ?: break
                client = socket
                connections++
                thread(isDaemon = true) {
                    val decoder = FrameDecoder()
                    val buffer = ByteArray(4096)
                    runCatching {
                        val input = socket.getInputStream()
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            received.addAll(decoder.feed(buffer, count))
                        }
                    }
                }
            }
        }
    }

    val port: Int get() = server.localPort

    fun send(raw: ByteArray) {
        val socket = client ?: return
        synchronized(this) {
            socket.getOutputStream().write(Rns.hdlcFrame(raw))
            socket.getOutputStream().flush()
        }
    }

    fun find(type: Int, context: Int): Packet? = synchronized(received) {
        received.mapNotNull { Rns.parsePacket(it).value }.firstOrNull { it.packetType == type && it.context == context }
    }

    fun close() {
        runCatching { client?.close() }
        runCatching { server.close() }
    }
}

class ReticulumTest {
    fun testFraming() {
        val payload = byteArrayOf(0x01, 0x7e, 0x7d, 0x02, 0xc0.toByte(), 0xdb.toByte()) + ByteArray(30) { 'x'.code.toByte() }
        val framed = Rns.hdlcFrame(payload)
        expect(framed.copyOfRange(1, framed.size - 1).none { it == 0x7e.toByte() }, "HDLC escapes flag bytes inside a frame")
        val decoder = FrameDecoder()
        val frames = ArrayList<ByteArray>()
        for (byte in framed + Rns.hdlcKeepalive() + framed) frames += decoder.feed(byteArrayOf(byte))
        expect(frames.size == 2 && frames.all { it.contentEquals(payload) }, "HDLC decoder rebuilds frames fed one byte at a time and skips keepalives")

        val small = FrameDecoder(FrameDecoder.Framing.Hdlc, 40)
        val dropped = small.feed(Rns.hdlcFrame(ByteArray(100) { 'a'.code.toByte() }) + Rns.hdlcFrame(ByteArray(25) { 'b'.code.toByte() }))
        expect(dropped.size == 1 && dropped[0].contentEquals(ByteArray(25) { 'b'.code.toByte() }) && small.invalidFrames == 1L, "An oversized HDLC frame is dropped and the next one still decodes")

        val kiss = FrameDecoder(FrameDecoder.Framing.Kiss)
        val kissFrames = kiss.feed(Rns.kissFrame(payload) + Rns.kissFrame(payload))
        expect(kissFrames.size == 2 && kissFrames[0].contentEquals(payload), "KISS framing round trips")

        val shortFrames = FrameDecoder()
        expect(shortFrames.feed(Rns.hdlcFrame(ByteArray(10))).isEmpty() && shortFrames.invalidFrames == 1L, "Frames no longer than a header are counted as invalid")

        val streamLimit = FrameDecoder()
        val huge = streamLimit.feed(Rns.hdlcFrame(ByteArray(Rns.STREAM_HARDWARE_MTU + 1) { 1 }))
        expect(huge.isEmpty() && streamLimit.invalidFrames == 1L, "Stream frames over 256 KiB are dropped")
    }

    fun testVectors() {
        assertEq("7926bbe7dd7f9aba88b0", Rns.hex(Rns.nameHash("rnstransport.path.request")), "Name hash matches Python RNS")
        assertEq("6b9f66014d9853faab220fba47d02761", Rns.hex(Rns.pathRequestDestination), "Path request destination matches Python RNS")
        val identity = RnsIdentity.fromPrivateKey(bytesOfHex(PRIVATE_KEY))!!
        assertHex(PUBLIC_KEY, identity.publicKey)
        assertEq("aca31af0441d81dbec71e82da0b4b5f5", Rns.hex(identity.hash), "Identity hash matches Python RNS")
        assertEq(LXMF_DESTINATION, Rns.hex(Rns.destinationHashForName("lxmf.delivery", identity.hash)), "Destination hash matches Python RNS")
        assertEq(
            "ffae909e84651ee55e1826c11f1a9e722af61742efc3d00b94d3067fce9e548268c612d3465a2ad08bac2eec009bb36d4fc86cb070ff64e60b45ef0fe2e76eab0cb34a87e3cabea1b0089d14dbc204de",
            Rns.hex(Rns.hkdf(80, "abc".toByteArray(), "salt".toByteArray(), "ctx".toByteArray())),
            "HKDF matches Python RNS",
        )

        val raw = bytesOfHex(ANNOUNCE)
        assertEq("498d563bd52778f53e58a33d05de475df0aa2042044fa6b087e5ce2691b9e2cb", Rns.hex(Rns.packetHash(raw)), "Packet hash matches Python RNS")
        val packet = Rns.parsePacket(raw).value!!
        expect(packet.packetType == Rns.PACKET_ANNOUNCE && packet.hops == 0, "Announce header parses")
        expect(Rns.packPacket(packet).contentEquals(raw), "Packet packs back to the same bytes")
        val announce = Rns.parseAnnounce(packet).value!!
        assertEq(null, Rns.validateAnnounce(announce), "Python-signed announce validates")
        assertEq(1767225600L, announce.emittedAt, "Announce emission time is read from the random hash")
        assertEq("Field radio 3", Rns.displayNameFromAppData("peer", announce.appData), "LXMF msgpack display name is decoded")
        assertEq("lxmf.delivery", Rns.knownAspect(announce.nameHash)?.name, "Aspect is recognised from its name hash")

        val tampered = raw.copyOf()
        tampered[tampered.size - 3] = (tampered[tampered.size - 3].toInt() xor 0x01).toByte()
        val bad = Rns.parseAnnounce(Rns.parsePacket(tampered).value!!).value!!
        expect(Rns.validateAnnounce(bad) != null, "A tampered announce fails validation")

        val tag = ByteArray(16) { (it + 16).toByte() }
        assertHex(
            "08006b9f66014d9853faab220fba47d0276100fae321c442e3c9bdcd7a3e79d850e03c101112131415161718191a1b1c1d1e1f",
            Rns.buildPathRequest(bytesOfHex(LXMF_DESTINATION), tag),
        )
        val parsedRequest = Rns.parsePathRequest(Rns.parsePacket(Rns.buildPathRequest(bytesOfHex(LXMF_DESTINATION), tag)).value!!)!!
        expect(parsedRequest.destination.contentEquals(bytesOfHex(LXMF_DESTINATION)) && parsedRequest.tag.contentEquals(tag), "Path requests parse back")

        val built = Rns.buildAnnounce(identity, "lxmf.delivery", "hello".toByteArray(), false, 1767225600, byteArrayOf(1, 2, 3, 4, 5))
        val builtPacket = Rns.parsePacket(built).value!!
        val builtAnnounce = Rns.parseAnnounce(builtPacket).value!!
        expect(Rns.validateAnnounce(builtAnnounce) == null && Rns.hex(builtPacket.destinationHash) == LXMF_DESTINATION, "Announces built here validate and use the RNS destination hash")
        assertEq(1767225600L, builtAnnounce.emittedAt, "Built announces carry their emission time")

        val fresh = RnsIdentity.generate()
        expect(fresh.hasPrivateKey && RnsIdentity.fromPrivateKey(fresh.privateKey)!!.publicKey.contentEquals(fresh.publicKey), "A generated identity reloads from its private key")

        val discovered = Rns.parseDiscoveryAppData(bytesOfHex(DISCOVERY)).value!!
        expect(
            discovered.name == "Test hub" && discovered.type == "BackboneInterface" && discovered.reachableOn == "hub.example.org" &&
                discovered.port == 4242 && discovered.transport && discovered.hasLocation,
            "Interface discovery data is decoded: $discovered",
        )
        expect(discovered.stampValue == 11 && !discovered.stampValid, "Discovery proof of work matches LXMF's stamp value (11) and is below the required 16")
        expect(discovered.connectable, "A backbone with an address and port is connectable")

        expect(!Rns.parsePacket(ByteArray(10)).ok, "Short frames are refused")
        val hops = raw.copyOf().also { it[1] = 200.toByte() }
        expect(!Rns.parsePacket(hops).ok, "Hop counts over 127 are refused")
        val maxHops = raw.copyOf().also { it[1] = 127 }
        expect(Rns.parsePacket(maxHops).ok, "A hop count of 127 is accepted")
        val ifac = raw.copyOf().also { it[0] = (it[0].toInt() or 0x80).toByte() }
        assertEq("packet carries an interface access code", Rns.parsePacket(ifac).error, "Packets with an access code are refused")
        assertEq("<$LXMF_DESTINATION>", Rns.prettyHash(bytesOfHex(LXMF_DESTINATION)))
        expect(Rns.parseHash("<fae321c4:42e3c9bd cd7a3e79d850e03c>")!!.contentEquals(bytesOfHex(LXMF_DESTINATION)), "Hashes parse with brackets, colons and spaces")
        expect(Rns.parseHash("fae321") == null, "Short hashes are refused")
    }

    fun testMsgPackBounds() {
        expect(!Rns.decodeMsgPack(byteArrayOf(0xdd.toByte(), -1, -1, -1, -1)).ok, "A msgpack array claiming four billion items is refused")
        expect(!Rns.decodeMsgPack(byteArrayOf(0xdf.toByte(), 0, 0, 0, 3, 1)).ok, "A map claiming more pairs than bytes left is refused")
        expect(!Rns.decodeMsgPack(byteArrayOf(0xc6.toByte(), 0, 0, 1, 0, 1, 2)).ok, "A binary claiming more bytes than are left is refused")
        expect(!Rns.decodeMsgPack(byteArrayOf(0xd9.toByte())).ok, "A truncated string length is refused")
        val nine = ByteArray(9) { 0x91.toByte() } + byteArrayOf(0x01)
        expect(!Rns.decodeMsgPack(nine).ok, "Nesting deeper than 8 is refused")
        val eight = ByteArray(8) { 0x91.toByte() } + byteArrayOf(0x01)
        expect(Rns.decodeMsgPack(eight).ok, "Nesting 8 deep is accepted")
        val many = byteArrayOf(0xdc.toByte(), 0x10, 0x00) + ByteArray(4096) { 0x01 }
        expect(!Rns.decodeMsgPack(many).ok, "More than 4096 items are refused")
        val decoded = Rns.decodeMsgPack(bytesOfHex("83a16101a162c4027879a1639201cb3ff8000000000000")).value as Map<*, *>
        assertEq(1L, decoded["a"])
        expect((decoded["b"] as ByteArray).contentEquals("xy".toByteArray()), "Binary values decode to bytes")
        assertEq(listOf(1L, 1.5), decoded["c"])
        val keyed = Rns.decodeMsgPack(bytesOfHex("82ccff01d0fe02")).value as Map<*, *>
        assertEq(setOf("255", "-2"), keyed.keys, "Integer map keys become decimal text, as on the desktop")
        expect(!Rns.decodeMsgPack(ByteArray(0)).ok, "Empty input is refused")
        assertEq("Hill node", Rns.displayNameFromAppData("node", "Hill\u0001 \tnode".toByteArray()))
        assertEq("", Rns.displayNameFromAppData("node", byteArrayOf(0xff.toByte(), 0xfe.toByte())))
        assertEq("", Rns.displayNameFromAppData("discovery", "x".toByteArray()))
    }

    fun testConfigImport() {
        val text = "[reticulum]\n  share_instance = Yes\n  instance_name = lab\n\n[interfaces]\n" +
            "  [[Default Interface]]\n    type = AutoInterface\n    enabled = yes\n" +
            "  [[RNS Testnet Dublin]]\n    type = TCPClientInterface\n    enabled = yes\n" +
            "    target_host = dublin.connect.reticulum.network\n    target_port = 4965\n" +
            "  [[Private]]\n    type = TCPClientInterface\n    enabled = yes\n    target_host = 10.0.0.2\n" +
            "    target_port = 4242\n    network_name = secret\n" +
            "  [[LAN UDP]]\n    type = UDPInterface\n    interface_enabled = no\n    port = 4243 # both\n"
        val skipped = ArrayList<String>()
        val configs = ReticulumConfig.parse(text, skipped)
        assertEq(4, configs.size, "Config import finds shared instance, two TCP and UDP")
        expect(configs[0].kind == InterfaceKind.SharedInstance && configs[0].instanceName == "lab" && configs[0].port == 37428, "Shared instance name comes from [reticulum]")
        expect(configs[1].host == "dublin.connect.reticulum.network" && configs[1].port == 4965 && configs[1].enabled, "TCP client entry is imported")
        expect(configs[2].networkName == "secret" && configs[2].usesAccessCode && configs[2].accessCodeSize == 16, "An entry with a network name keeps it for IFAC")
        expect(configs[3].kind == InterfaceKind.Udp && configs[3].listenPort == 4243 && configs[3].forwardPort == 4243 && !configs[3].enabled, "UDP entry keeps its port and disabled state")
        assertEq(listOf("Default Interface (AutoInterface)"), skipped)
        val sized = ReticulumConfig.parse("[reticulum]\nshare_instance = no\n[interfaces]\n[[Lab]]\ntype = TCPClientInterface\ntarget_host = 10.0.0.9\ntarget_port = 4242\npassphrase = \"p w\"\nifac_size = 64\n")
        expect(sized.single().passphrase == "p w" && sized.single().accessCodeSize == 8, "ifac_size is read in bits")
        expect(ReticulumConfig.parse("[reticulum]\nshare_instance = no\n").isEmpty(), "No shared instance when sharing is off")
        val backbone = ReticulumConfig.parse("[reticulum]\nshare_instance = no\n[interfaces]\n[[Hub]]\ntype = BackboneInterface\nremote = \"hub.example.org\"\nport = 4343\nenabled = true\n")
        expect(backbone.size == 1 && backbone[0].host == "hub.example.org" && backbone[0].port == 4343, "A Backbone client with a remote is imported as TCP")

        expect(InterfaceConfig(name = "x", host = "bad host!").validationError.isNotEmpty(), "An invalid host is refused")
        expect(InterfaceConfig(name = "x", host = "::1").validationError.isEmpty(), "IPv6 literals are hosts")
        expect(InterfaceConfig(name = "x", host = "rmap.world", port = 70000).validationError.isNotEmpty(), "Ports above 65535 are refused")
        expect(InterfaceConfig(name = "x", kind = InterfaceKind.Udp, listenAddress = "lan", forwardAddress = "255.255.255.255").validationError.isNotEmpty(), "UDP needs IP addresses")
        assertEq("[::1]:4242", InterfaceConfig(name = "x", host = "::1").endpoint)
        val ifac = InterfaceConfig.fromJson(Json.parseObject(Json.stringify(configs[2].copy(passphrase = "pw").toJson())))
        expect(ifac != null && ifac.networkName == "secret" && ifac.passphrase == "pw" && ifac.accessCodeSize == 16, "Access code settings round trip through JSON")
        expect("passphrase" !in configs[2].copy(passphrase = "pw").toJson(includePassphrase = false), "The passphrase can be left out of stored JSON")
        expect(InterfaceConfig(name = "x", host = "h", networkName = "n", accessCodeSize = 65).validationError.isNotEmpty(), "Access codes above 64 bytes are refused")
        val round = InterfaceConfig.fromJson(Json.parseObject(Json.stringify(configs[1].toJson())))
        expect(round != null && round.sameTarget(configs[1]) && round.id == configs[1].id, "Interface settings round trip through JSON")
        val udp = InterfaceConfig.fromJson(Json.parseObject(Json.stringify(configs[2].toJson())))
        assertEq(configs[2], udp, "UDP settings round trip through JSON")
        expect(InterfaceConfig.fromJson(mapOf("kind" to "serial", "name" to "x")) == null, "Unknown kinds are refused")
        val desktopJson = Json.parseObject("""{"id":"a","name":"Daemon","kind":"shared","enabled":true,"instanceName":"default","sharedOverTcp":false,"port":37428}""")
        assertEq("127.0.0.1:37428", InterfaceConfig.fromJson(desktopJson)?.endpoint, "A desktop shared instance connects over TCP on the phone")
        assertEq(listOf("rmap.world:4242", "rmap.world:4343"), ReticulumConfig.publicEntryPoints().map { it.endpoint })
        assertEq("127.0.0.1:37428", ReticulumConfig.defaultInterfaces().single().endpoint)
    }

    fun testLiveSession() {
        val fake = FakeNode()
        val node = ReticulumNode()
        try {
            node.setIdentity(RnsIdentity.generate())
            node.setAnnounceName("Test phone")
            val asked = Collections.synchronizedList(ArrayList<String>())
            node.authorizer = { _, url -> asked.add(url); true }
            val pulses = Collections.synchronizedList(ArrayList<String>())
            node.announceHandler = { pulses.add(Rns.hex(it)) }
            val config = InterfaceConfig(id = "fake", name = "Fake node", host = "127.0.0.1", port = fake.port)
            node.setInterfaces(listOf(config))
            node.start()
            expect(waitFor { node.onlineCount() == 1 && fake.client != null }, "TCP interface comes online")
            assertEq(listOf("tcp://127.0.0.1:${fake.port}"), asked.toList(), "The authorizer is asked once for tcp://host:port")

            fake.send(bytesOfHex(ANNOUNCE))
            expect(waitFor { node.destination(bytesOfHex(LXMF_DESTINATION)) != null }, "Announce from the network is recorded")
            val record = node.destination(bytesOfHex(LXMF_DESTINATION))!!
            expect(record.displayName == "Field radio 3" && record.hops == 1 && record.kind == "peer" && record.interfaceId == "fake", "Recorded destination has name, kind, one hop and its interface: $record")
            expect(pulses.contains(LXMF_DESTINATION), "The announce handler is told about the new destination")
            fake.send(bytesOfHex(ANNOUNCE))
            expect(waitFor { node.counters().duplicates == 1L }, "A repeated announce counts as a duplicate")
            val status = node.status("fake")
            expect(status.rxBytes > 0 && status.announces == 2L && status.rxPackets == 2L, "Interface counts bytes, packets and announces: $status")

            expect(node.announce(), "Announcing this phone sends a packet")
            expect(waitFor { fake.find(Rns.PACKET_ANNOUNCE, Rns.CONTEXT_NONE) != null }, "Fake node receives our announce")
            val ours = Rns.parseAnnounce(fake.find(Rns.PACKET_ANNOUNCE, Rns.CONTEXT_NONE)!!).value!!
            expect(
                Rns.validateAnnounce(ours) == null && ours.destinationHash.contentEquals(node.destinationHash()) &&
                    String(ours.appData) == "Test phone" && ours.nameHash.contentEquals(Rns.nameHash("forkmesh.node")),
                "Our announce validates and carries the name",
            )
            expect(node.status("fake").txPackets == 1L && node.counters().announcesSent == 1L, "Sent announces are counted")

            val remote = RnsIdentity.generate()
            val remoteHash = Rns.destinationHashForName("nomadnetwork.node", remote.hash)
            expect(node.requestPath(remoteHash) != null, "Path request is sent")
            expect(waitFor { fake.find(Rns.PACKET_DATA, Rns.CONTEXT_NONE) != null }, "Fake node receives the path request")
            val request = Rns.parsePathRequest(fake.find(Rns.PACKET_DATA, Rns.CONTEXT_NONE)!!)!!
            expect(request.destination.contentEquals(remoteHash) && request.tag.size == 16, "Path request names the destination and carries a tag")
            val response = Rns.buildAnnounce(remote, "nomadnetwork.node", "Hill node".toByteArray(), true, System.currentTimeMillis() / 1000)
            response[1] = 2
            fake.send(response)
            expect(waitFor { (node.lookups()[Rns.hex(remoteHash)]?.answered ?: 0L) > 0L }, "Path response answers the lookup")
            expect(node.lookups()[Rns.hex(remoteHash)]?.hops == 3 && node.destination(remoteHash)?.displayName == "Hill node", "Lookup records three hops and the node name")
            expect(node.events().any { it.kind == "path-found" }, "The path answer is in the activity")

            fake.received.clear()
            fake.send(Rns.buildPathRequest(node.destinationHash(), ByteArray(16) { 'q'.code.toByte() }))
            expect(waitFor { fake.find(Rns.PACKET_ANNOUNCE, Rns.CONTEXT_PATH_RESPONSE) != null }, "A path request for this phone is answered with a path response")
            expect(waitFor { node.counters().pathRequestsAnswered == 1L }, "Answered path requests are counted")
            fake.send(Rns.buildPathRequest(node.destinationHash(), ByteArray(16) { 'r'.code.toByte() }))
            expect(waitFor { node.counters().pathRequestsSeen == 2L }, "The second path request is heard")
            assertEq(1L, node.counters().pathRequestsAnswered, "Path answers are limited to one every 10 seconds")

            val snapshot = node.snapshot()
            expect(snapshot.running && snapshot.onlineCount == 1 && snapshot.destinations.size == 2, "The snapshot carries the node state")

            fake.client!!.close()
            expect(waitFor { node.status("fake").state == LinkState.Waiting }, "A dropped connection waits to retry")
            expect(node.status("fake").nextAttempt > System.currentTimeMillis(), "The retry time is in the future")
            expect(node.events().any { it.text == "Fake node went offline: 127.0.0.1:${fake.port} closed the connection" }, "Going offline is in the activity")
            expect(waitFor(9000) { node.onlineCount() == 1 && fake.connections == 2 }, "The interface reconnects after 5 seconds")
            node.stop()
            assertEq(LinkState.Disabled, node.status("fake").state, "Stopping the node closes every interface")
            expect(node.announce().not(), "Nothing is announced while disconnected")
        } finally {
            node.close()
            fake.close()
        }
    }

    fun testFirewallDeniedAndRetries() {
        val fake = FakeNode()
        val node = ReticulumNode()
        try {
            node.authorizer = { _, _ -> false }
            node.setInterfaces(listOf(InterfaceConfig(id = "denied", name = "Denied", host = "127.0.0.1", port = fake.port)))
            node.start()
            Thread.sleep(300)
            expect(node.status("denied").state == LinkState.Blocked && fake.connections == 0, "A connection the authorizer denies is never opened")
        } finally {
            node.close()
            fake.close()
        }
        val closedPort = ServerSocket(0, 1, InetAddress.getLoopbackAddress()).use { it.localPort }
        val refused = ReticulumNode()
        try {
            refused.setInterfaces(listOf(InterfaceConfig(id = "shared", name = "Shared", kind = InterfaceKind.SharedInstance, port = closedPort)))
            refused.start()
            expect(waitFor { refused.status("shared").state == LinkState.Waiting }, "A refused shared instance waits to retry")
            val status = refused.status("shared")
            expect("No Reticulum app on this phone" in status.detail, "The refused shared instance says no app shares one: ${status.detail}")
            val delay = status.nextAttempt - System.currentTimeMillis()
            expect(delay in 3000..5100, "The first retry comes after 5 seconds ($delay)")
            refused.setInterfaceEnabled("shared", false)
            assertEq(LinkState.Disabled, refused.status("shared").state, "Turning an interface off stops it")
        } finally {
            refused.close()
        }
    }

    fun testUdpSession() {
        val peer = DatagramSocket(InetSocketAddress(InetAddress.getLoopbackAddress(), 0))
        val listen = DatagramSocket(InetSocketAddress(InetAddress.getLoopbackAddress(), 0)).use { it.localPort }
        val node = ReticulumNode()
        try {
            node.setIdentity(RnsIdentity.generate())
            node.setInterfaces(listOf(InterfaceConfig(id = "udp", name = "LAN", kind = InterfaceKind.Udp, listenAddress = "127.0.0.1", listenPort = listen, forwardAddress = "127.0.0.1", forwardPort = peer.localPort)))
            node.start()
            expect(waitFor { node.onlineCount() == 1 }, "UDP interface listens")
            val announce = bytesOfHex(ANNOUNCE)
            peer.send(DatagramPacket(announce, announce.size, InetAddress.getLoopbackAddress(), listen))
            expect(waitFor { node.destination(bytesOfHex(LXMF_DESTINATION)) != null }, "A UDP announce is recorded")
            val huge = ByteArray(ReticulumNode.UDP_MTU + 10) { 1 }
            peer.send(DatagramPacket(huge, huge.size, InetAddress.getLoopbackAddress(), listen))
            expect(waitFor { node.status("udp").invalidFrames == 1L }, "Datagrams over the UDP MTU are dropped")
            expect(node.announce(), "Announcing over UDP sends a datagram")
            val buffer = ByteArray(2048)
            val packet = DatagramPacket(buffer, buffer.size)
            peer.soTimeout = 3000
            peer.receive(packet)
            val parsed = Rns.parseAnnounce(Rns.parsePacket(buffer.copyOf(packet.length)).value!!).value!!
            expect(Rns.validateAnnounce(parsed) == null && parsed.destinationHash.contentEquals(node.destinationHash()), "The UDP peer receives our unframed announce")
        } finally {
            node.close()
            peer.close()
        }
    }

    fun testLimitsAndTimeouts() {
        var now = 1_000_000L
        val transport = FakeTransport()
        val node = ReticulumNode(transport = transport, clock = { now })
        try {
            node.setIdentity(RnsIdentity.generate())
            node.ingest("x", ByteArray(5))
            assertEq(1L, node.counters().invalid, "Short packets are counted as invalid")
            val ifac = bytesOfHex(ANNOUNCE).also { it[0] = (it[0].toInt() or 0x80).toByte() }
            node.ingest("x", ifac)
            assertEq(2L, node.counters().invalid, "IFAC packets are dropped")
            node.ingest("x", bytesOfHex(ANNOUNCE), fromSharedInstance = true)
            assertEq(0, node.destination(bytesOfHex(LXMF_DESTINATION))?.hops, "Shared instance packets keep their hop count")
            val other = RnsIdentity.generate()
            val forged = Rns.parsePacket(Rns.buildAnnounce(other, "lxmf.delivery", ByteArray(0), false, 1767225601)).value!!
            val swapped = Rns.packPacket(Packet(packetType = Rns.PACKET_ANNOUNCE, destinationHash = bytesOfHex(LXMF_DESTINATION), data = forged.data))
            node.ingest("x", swapped)
            assertEq(1L, node.counters().rejectedAnnounces, "An announce whose key does not match its destination is rejected")
            for (i in 0 until ReticulumNode.MAX_EVENTS + 20) node.ingest("x", Rns.buildAnnounce(RnsIdentity.generate(), "nomadnetwork.node", "n$i".toByteArray(), false, 1767225600L + i))
            expect(node.events().size == ReticulumNode.MAX_EVENTS, "The activity keeps at most 400 events")
            assertEq(ReticulumNode.MAX_EVENTS + 21, node.destinations().size)
            for (i in 0 until ReticulumNode.HISTORY_SECONDS + 15) node.tickForTest()
            assertEq(ReticulumNode.HISTORY_SECONDS, node.totalHistory().size, "Traffic history keeps 180 seconds")
            val lookup = bytesOfHex(LXMF_DESTINATION)
            expect(node.requestPath(lookup) == null, "No path request without an online interface")
            node.setInterfaces(listOf(InterfaceConfig(id = "t", name = "Fake", host = "node.example", port = 4242)))
            node.start()
            expect(waitFor { node.onlineCount() == 1 }, "The fake transport connects")
            expect(node.requestPath(lookup) != null && transport.writes.size == 1, "A path request is written once")
            expect(Rns.hdlcFrame(Rns.buildPathRequest(lookup, node.lookups()[LXMF_DESTINATION]!!.tag)).contentEquals(transport.writes[0]), "The path request is HDLC framed")
            now += (ReticulumNode.PATH_TIMEOUT_SECONDS - 1) * 1000L
            node.tickForTest()
            expect(node.lookups()[LXMF_DESTINATION]?.timedOut == false, "A lookup is still waiting before 20 seconds")
            now += 1000
            node.tickForTest()
            expect(node.lookups()[LXMF_DESTINATION]?.timedOut == true, "A lookup times out after 20 seconds")
            assertEq("No path to <$LXMF_DESTINATION> within 20 s", node.events().first().text)
            node.forgetDestination(lookup)
            expect(node.destination(lookup) == null && node.lookups().isEmpty(), "Forgetting removes a destination and its lookup")
            node.clearDestinations()
            expect(node.destinations().isEmpty(), "Clearing empties the table")
        } finally {
            node.close()
        }
    }

    fun testRoutesAndFormat() {
        for (link in listOf("forkmesh://reticulum", "forkmesh://dashboard/reticulum", "https://app.forkmesh.com/dashboard/reticulum")) {
            assertEq(listOf(com.forkmesh.nativeapp.navigation.Route.Reticulum), com.forkmesh.nativeapp.navigation.RouteParser.parse(link)?.stack, link)
        }
        assertEq("Reticulum", com.forkmesh.nativeapp.navigation.RouteCatalog.title(com.forkmesh.nativeapp.navigation.Route.Kind.Reticulum))
        expect(com.forkmesh.nativeapp.navigation.Route.Reticulum in com.forkmesh.nativeapp.navigation.HubCatalog.personal.mapNotNull { it.route }, "Reticulum has a Home tile")

        assertEq("999 B", ReticulumFormat.bytes(999.0))
        assertEq("1.5 KB", ReticulumFormat.bytes(1500.0))
        assertEq("12 MB", ReticulumFormat.bytes(12_000_000.0))
        assertEq("2.0 KB/s", ReticulumFormat.rate(2000.0))
        assertEq("59 s", ReticulumFormat.duration(59))
        assertEq("1 h 1 min", ReticulumFormat.duration(3660))
        assertEq("2 d 3 h", ReticulumFormat.duration(2 * 86400 + 3 * 3600))
        assertEq("just now", ReticulumFormat.age(10_000, 12_000))
        assertEq("2 min ago", ReticulumFormat.age(10_000, 130_000))
        assertEq("never", ReticulumFormat.age(0, 130_000))
        assertEq(MapGroup.Peer, MapGroup.of("forkmesh"))
        assertEq(MapGroup.Node, MapGroup.of("service"))
        assertEq(MapGroup.Transport, MapGroup.of("discovery"))
        assertEq(MapGroup.Other, MapGroup.of("unknown"))

        val a = DestinationRecord(hash = ByteArray(16) { 1 }, displayName = "Zed", label = "LXMF messaging", kind = "peer", hops = 3, announces = 1, lastHeard = 10)
        val b = DestinationRecord(hash = ByteArray(16) { 2 }, displayName = "alpha", label = "Nomad Network node", kind = "node", hops = 1, announces = 5, lastHeard = 5)
        val c = DestinationRecord(hash = ByteArray(16) { 3 }, label = "Unknown application", kind = "unknown", hops = 2, announces = 2, lastHeard = 20)
        val all = listOf(a, b, c)
        assertEq(listOf(c, a, b), ReticulumFormat.visibleDestinations(all, "", null, DestinationSort.LastHeard))
        assertEq(listOf(b, c, a), ReticulumFormat.visibleDestinations(all, "", null, DestinationSort.Hops))
        assertEq(listOf(b, c, a), ReticulumFormat.visibleDestinations(all, "", null, DestinationSort.Announces))
        assertEq(listOf(b, a, c), ReticulumFormat.visibleDestinations(all, "", null, DestinationSort.Name))
        assertEq(listOf(b), ReticulumFormat.visibleDestinations(all, "nomad", null, DestinationSort.LastHeard))
        assertEq(listOf(a), ReticulumFormat.visibleDestinations(all, "", MapGroup.Peer, DestinationSort.LastHeard))
        assertEq(listOf(c), ReticulumFormat.visibleDestinations(all, "0303", null, DestinationSort.LastHeard))

        val snapshot = ReticulumSnapshot(
            running = true,
            interfaces = listOf(InterfaceView(InterfaceConfig(id = "i", name = "Hub", host = "rmap.world"), InterfaceStatus(state = LinkState.Online))),
            destinations = all.associateBy { it.hex },
            history = List(20) { TrafficSample(rx = 100, tx = 50, announces = 1) },
            at = 1_000_000,
        )
        val tiles = ReticulumFormat.tiles(snapshot)
        assertEq("Online", tiles.status)
        assertEq("1 of 1 interfaces up", tiles.statusDetail)
        assertEq("3", tiles.destinations)
        assertEq("20/min", tiles.announces)
        assertEq("150 B/s", tiles.traffic)
        assertEq("Ask for one in Destinations", tiles.pathsDetail)
        assertEq("Off", ReticulumFormat.tiles(snapshot.copy(running = false)).status)
        assertEq("Hub", snapshot.interfaceName("i"))
        assertEq("Removed interface", snapshot.interfaceName("gone"))
        val rows = ReticulumFormat.detailRows(a.copy(interfaceId = "i", emittedAt = 1767225600), snapshot, java.time.ZoneOffset.UTC).toMap()
        assertEq("2026-01-01 00:00:00", rows["Announced at"])
        assertEq("Hub", rows["Heard via"])
        assertEq("3 hops", rows["Distance"])
        val found = Rns.parseDiscoveryAppData(bytesOfHex(DISCOVERY)).value!!
        assertEq("hub.example.org:4242", ReticulumFormat.discoveredAddress(found))
        assertEq(InterfaceConfig(id = "x", name = "Test hub", host = "hub.example.org", port = 4242).endpoint, ReticulumFormat.interfaceFromDiscovery(found).endpoint)
    }

    fun testDestinationTableIsBounded() {
        var now = 5_000L
        val node = ReticulumNode(clock = { now }, maxDestinations = 50)
        try {
            val identity = RnsIdentity.generate()
            for (i in 0..50) {
                now += 1
                node.ingest("x", Rns.buildAnnounce(identity, "app.n$i", ByteArray(0), false, 1767225600L))
            }
            assertEq(50 - 50 / 10, node.destinations().size, "Past the limit the oldest tenth is dropped")
            expect(node.destination(Rns.destinationHashForName("app.n0", identity.hash)) == null, "The oldest destination goes first")
            expect(node.destination(Rns.destinationHashForName("app.n50", identity.hash)) != null, "The newest destination stays")
            assertEq(20000, ReticulumNode.MAX_DESTINATIONS)
        } finally {
            node.close()
        }
    }
}
