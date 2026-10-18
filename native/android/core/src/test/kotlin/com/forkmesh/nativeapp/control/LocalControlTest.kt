package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentQueueApi
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.RelayFrameChannel
import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.crypto.X25519
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.localrepos.LocalScratch
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class LocalControlTest {
    class FakeDesktop(val paired: Boolean = true, val name: String = "studio") {
        val seed = ByteArray(32) { (it * 7 + 3).toByte() }
        val key: String = controlB64(Ed25519.publicKey(seed))
        private val ephPrivate = X25519.generatePrivateKey()
        private var keys: ControlKeys? = null
        private var transcript = ByteArray(0)
        private var sendCounter = 0L
        private var receiveCounter = 0L
        val requests = ArrayList<Map<String, Any?>>()
        var clientKey = ""
        var transcriptError: String? = null
        var followAnswer: Map<String, Any?>? = null
        var takeoverError: String? = null
        var queueLimit = 2
        val queueOrder = mutableListOf(11, 12, 13)
        val handlers = HashMap<String, (Map<*, *>) -> Map<String, Any?>>()

        fun queuePayload(): Map<String, Any?> =
            mapOf("running" to 2, "limit" to queueLimit, "limitMax" to 8, "queue" to queueOrder.toList())

        fun queueChange(method: String, params: Map<*, *>): String? {
            if (method == "agents.queue.limit") {
                val limit = (params["limit"] as? Number)?.toInt() ?: 0
                if (limit < 1 || limit > 99) return "bad_limit"
                queueLimit = limit
                return null
            }
            val id = (params["id"] as? Number)?.toInt() ?: 0
            val to = (params["to"] as? Number)?.toInt() ?: -1
            if (id !in queueOrder || to < 0) return "not_queued"
            queueOrder.remove(id)
            queueOrder.add(minOf(to, queueOrder.size), id)
            return null
        }

        fun onHello(hello: Map<String, Any?>): ByteArray {
            clientKey = jsonText(hello["key"])
            val eph = controlB64(X25519.publicKey(ephPrivate))
            transcript = controlTranscript(clientKey, jsonText(hello["eph"]), jsonText(hello["nonce"]), key, eph)
            val sig = controlB64(Ed25519.sign(seed, controlServerSignedBytes(transcript)))
            val shared = X25519.sharedSecret(ephPrivate, controlFromB64(hello["eph"], 32)!!)
            keys = deriveControlKeys(shared, transcript)
            return Json.stringify(linkedMapOf("t" to CONTROL_WELCOME_TAG, "v" to 1, "key" to key, "eph" to eph, "name" to name, "user" to "fran", "sig" to sig))
                .toByteArray()
        }

        fun onAuth(auth: Map<String, Any?>): Boolean {
            val sig = controlFromB64(auth["sig"], 64) ?: return false
            return Ed25519.verify(controlFromB64(clientKey, 32)!!, controlClientSignedBytes(transcript), sig)
        }

        fun seal(message: Map<String, Any?>): ByteArray = sealControlFrame(keys!!.serverToClient, sendCounter++, Json.stringify(message).toByteArray())

        fun sessionEvent(): ByteArray = seal(mapOf("event" to "session", "data" to mapOf("paired" to paired, "code" to "123456")))

        fun answer(sealed: ByteArray): ByteArray? {
            val plain = openControlFrame(keys!!.clientToServer, receiveCounter++, sealed) ?: return null
            val request = Json.parse(String(plain)).asJsonMap()
            requests.add(request)
            handlers[jsonText(request["method"])]?.let { handler ->
                val params = request["params"] as? Map<*, *> ?: emptyMap<String, Any?>()
                return try {
                    seal(mapOf("id" to request["id"], "ok" to true, "result" to handler(params)))
                } catch (refusal: LocalControlException) {
                    seal(mapOf("id" to request["id"], "ok" to false, "error" to refusal.code))
                }
            }
            if (request["method"] == "agents.transcript" && transcriptError != null) {
                return seal(mapOf("id" to request["id"], "ok" to false, "error" to transcriptError))
            }
            if (request["method"] == "agents.takeover" && takeoverError != null) {
                return seal(mapOf("id" to request["id"], "ok" to false, "error" to takeoverError))
            }
            if (request["method"] == "agents.queue.limit" || request["method"] == "agents.queue.move") {
                val refusal = queueChange(jsonText(request["method"]), request["params"] as? Map<*, *> ?: emptyMap<String, Any?>())
                if (refusal != null) return seal(mapOf("id" to request["id"], "ok" to false, "error" to refusal))
            }
            val result = when (request["method"]) {
                "agents.queue", "agents.queue.limit", "agents.queue.move" -> queuePayload()
                "agents.list" -> mapOf(
                    "sessions" to listOf(mapOf("id" to 5, "status" to "running", "repositoryOwner" to "a", "repositoryName" to "b", "prompt" to "do it", "stoppable" to true)),
                    "repositories" to listOf(mapOf("owner" to "a", "name" to "b")),
                    "providers" to listOf(mapOf("id" to "claude-code", "label" to "Claude Code")),
                    "running" to 1,
                    "queue" to queuePayload(),
                )
                "agents.start" -> mapOf("id" to 9)
                "agents.stop" -> if (((((request["params"] as? Map<*, *>)?.get("id")) as? Number)?.toInt() ?: 0) < 0) {
                    mapOf("status" to "interrupted", "key" to "esc")
                } else {
                    null
                }
                "agents.prompt" -> when ((request["params"] as? Map<*, *>)?.get("text")) {
                    "gone" -> null
                    "type" -> mapOf("status" to "typed", "via" to "terminal")
                    "wait" -> mapOf("status" to "held_for_approval", "cause" to "no-mode-asserted")
                    else -> mapOf("status" to "delivered")
                }
                "agents.transcript" -> mapOf("transcript" to "hello", "next" to 5, "attachments" to listOf(mapOf("id" to "ab".repeat(16), "name" to "p.png")))
                "agents.follow" -> followAnswer
                "agents.takeover" -> if ((request["params"] as? Map<*, *>)?.get("check") == true) {
                    mapOf("id" to 0, "check" to true)
                } else {
                    mapOf("id" to 42, "branch" to "agent/9-fix-login")
                }
                else -> null
            }
            if (request["method"] == "agents.prompt" && (request["params"] as? Map<*, *>)?.get("text") == "busy") {
                return seal(mapOf("id" to request["id"], "ok" to false, "error" to "terminal_user_typing"))
            }
            if (request["method"] == "agents.prompt" && result == null) {
                return seal(mapOf("id" to request["id"], "ok" to false, "error" to "not_steerable"))
            }
            return if (result == null) seal(mapOf("id" to request["id"], "ok" to false, "error" to "unsupported"))
            else seal(mapOf("id" to request["id"], "ok" to true, "result" to result))
        }
    }

    private fun serve(desktop: FakeDesktop, events: Int = 0): Pair<ServerSocket, Thread> {
        val server = ServerSocket(0)
        val thread = Thread {
            server.accept().use { socket ->
                val reader = ControlFrameReader(socket.getInputStream())
                val out = socket.getOutputStream()
                out.write(controlFrame("{\"t\":\"mesh-key\"}".toByteArray()))
                val hello = Json.parse(String(reader.next()!!)).asJsonMap()
                out.write(controlFrame(desktop.onHello(hello)))
                val authFrame = try {
                    reader.next()
                } catch (_: Exception) {
                    null
                } ?: return@use
                val auth = Json.parse(String(authFrame)).asJsonMap()
                if (!desktop.onAuth(auth)) return@use
                out.write(controlFrame(desktop.sessionEvent()))
                repeat(events) { out.write(controlFrame(desktop.seal(mapOf("event" to "agents.changed", "data" to emptyMap<String, Any?>())))) }
                while (true) {
                    val frame = try {
                        reader.next()
                    } catch (_: Exception) {
                        null
                    } ?: break
                    out.write(controlFrame(desktop.answer(frame) ?: break))
                }
            }
        }.apply {
            isDaemon = true
            start()
        }
        return server to thread
    }

    fun testCryptoPrimitivesAreStable() {
        val transcript = controlTranscript("a", "b", "c", "d", "e")
        assertEq("forkmesh-local-control-v1\na\nb\nc\nd\ne", String(transcript))
        assertEq(6, controlPairingCode(transcript).length)
        val keys = deriveControlKeys(ByteArray(32) { 1 }, transcript)
        expect(!keys.clientToServer.contentEquals(keys.serverToClient))
        val sealed = sealControlFrame(keys.clientToServer, 3, "hi".toByteArray())
        assertEq("hi", String(openControlFrame(keys.clientToServer, 3, sealed)!!))
        assertEq(null, openControlFrame(keys.clientToServer, 4, sealed))
        assertEq(null, controlFromB64("abc=", 2))
        assertEq(32, controlFromB64(controlB64(ByteArray(32) { 9 }), 32)!!.size)
        assertEq(listOf(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2).map { it.toByte() }, controlNonce(258).toList())
    }

    fun testLanHandshakeAndRequests() {
        val desktop = FakeDesktop()
        val (server, _) = serve(desktop)
        val identity = Identity.fromSeed(ByteArray(32) { 5 })
        val connection = LocalControlConnection.open("127.0.0.1", server.localPort, identity, "phone", "fran", "android", desktop.key)
        val session = CountDownLatch(1)
        connection.addEventTap { if (it.event == "session" && it.data["paired"] == true) session.countDown() }
        expect(session.await(5, TimeUnit.SECONDS))
        assertEq("studio", connection.serverName)
        val list = LocalAgentsSnapshot.fromJson(connection.request("agents.list"))
        assertEq("do it", list.sessions.single().title)
        assertEq("a/b", list.repositories.single().label)
        val error = assertFails<LocalControlException> { connection.request("nope") }
        assertEq("unsupported", error.code)
        connection.close()
        expect(connection.isClosed)
        server.close()
    }

    fun testWrongDesktopRefused() {
        val desktop = FakeDesktop()
        val (server, _) = serve(desktop)
        val identity = Identity.fromSeed(ByteArray(32) { 6 })
        val other = controlB64(ByteArray(32) { 1 })
        val error = assertFails<LocalControlException> { LocalControlConnection.open("127.0.0.1", server.localPort, identity, expectedKey = other) }
        assertEq("wrong_desktop", error.code)
        server.close()
    }

    fun testServicePairsAndRemembers() {
        val desktop = FakeDesktop()
        val (server, _) = serve(desktop, events = 1)
        val prefs = MemoryPrefStore()
        val identity = Identity.fromSeed(ByteArray(32) { 7 })
        val dir = LocalScratch.create("control-files")
        val service = LocalControlService(identity, prefs, null, "p/x/", filesDirectory = { File(dir, "in") })
        val connected = CountDownLatch(1)
        val listed = CountDownLatch(1)
        service.addListener {
            if (service.connected) connected.countDown()
            if (service.agents != null) listed.countDown()
        }
        service.connect(MeshControlHost("", "127.0.0.1", server.localPort, name = "desk"))
        expect(connected.await(5, TimeUnit.SECONDS))
        expect(listed.await(5, TimeUnit.SECONDS))
        assertEq(desktop.key, service.paired.single().key)
        expect(prefs.getString("p/x/" + LocalControlService.PAIRED_KEY)!!.contains(desktop.key))
        assertEq(9, service.startAgent("a", "b", "task"))
        val detail = service.transcriptDetail(5)
        assertEq("hello", detail.text)
        assertEq("desktop:5:" + "ab".repeat(16), detail.attachments.single().cacheKey)
        val hub = DesktopControlHub(service, null)
        assertEq(DesktopReach.Connected(ControlTransport.Lan, "studio"), hub.reach)
        val center = AgentControlCenter(hub, null)
        center.refresh()
        assertEq("desktop:5", center.rows.single().id)
        assertEq(AgentTransportBadge.DesktopLan, center.rows.single().badge)
        center.dispose()
        service.dispose()
        server.close()
        LocalScratch.delete(dir)
    }

    private class ProbedHosts : ControlHostSource {
        override val displayName: String = "phone"
        var provider: (() -> List<String>)? = null

        override fun controlHosts(): List<MeshControlHost> = emptyList()

        override fun addListener(listener: () -> Unit) {}

        override fun removeListener(listener: () -> Unit) {}

        override fun setRememberedTargets(provider: () -> List<String>) {
            this.provider = provider
        }
    }

    private fun waitFor(done: () -> Boolean): Boolean {
        val deadline = System.nanoTime() + 5_000_000_000L
        while (!done() && System.nanoTime() < deadline) Thread.sleep(10)
        return done()
    }

    fun testTheDesktopsLastAddressIsProbedUntilItIsBack() {
        val desktop = FakeDesktop()
        val (server, _) = serve(desktop)
        val prefs = MemoryPrefStore()
        val identity = Identity.fromSeed(ByteArray(32) { 6 })
        val dir = LocalScratch.create("control-probe")
        val mesh = ProbedHosts()
        val service = LocalControlService(identity, prefs, mesh, "p/x/", filesDirectory = { File(dir, "in") })
        service.connect(MeshControlHost("", "127.0.0.1", server.localPort, name = "desk"))
        expect(waitFor { service.connected })
        expect(prefs.getString("p/x/" + LocalControlService.LAN_ADDRESSES_KEY)!!.contains(desktop.key))
        assertEq(emptyList<String>(), mesh.provider!!())
        expect(!service.lanPending)

        service.disconnect(hold = false)
        expect(waitFor { service.status == LocalControlStatus.Idle })
        expect(!service.lanPending)
        assertEq(listOf("127.0.0.1"), mesh.provider!!())
        expect(service.lanPending)

        service.disconnect()
        expect(waitFor { mesh.provider!!().isEmpty() })
        service.dispose()
        server.close()
        LocalScratch.delete(dir)
    }

    fun testDesktopRowsShowBeforeTheCloudAnswers() {
        val release = CountDownLatch(1)
        val transport = HttpTransport {
            release.await(5, TimeUnit.SECONDS)
            HttpClient.Response(200, emptyMap(), """{"sessions":[{"id":"run","status":"running","updatedAt":1700000003000}],"finishedBefore":0}""".toByteArray())
        }
        val queue = AgentQueueApi({ "https://x" }, { "t" }, transport)
        val desktop = FakeDesktop()
        val identity = Identity.fromSeed(ByteArray(32) { 5 })
        val hub = DesktopControlHub(
            null,
            { target -> RelayControlConnection.open(LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000) },
            accountDesktopsLoader = { listOf(PairedDesktop(desktop.key, "studio", "alice")) },
        ) { emptyList() }
        val center = AgentControlCenter(hub, queue)
        center.refreshAsync()
        expect(waitFor { center.rows.isNotEmpty() })
        assertEq(listOf("desktop:5"), center.rows.map { it.id })
        release.countDown()
        expect(waitFor { center.rows.size == 2 })
        assertEq(setOf("desktop:5", "node:run"), center.rows.map { it.id }.toSet())
        center.dispose()
        hub.disconnectRelay()
    }

    private class FixedHosts(val hosts: List<MeshControlHost>) : ControlHostSource {
        override val displayName: String = "phone"

        override fun controlHosts(): List<MeshControlHost> = hosts

        override fun addListener(listener: () -> Unit) {}

        override fun removeListener(listener: () -> Unit) {}
    }

    fun testADesktopOnThePhonesAccountConnectsWithoutBeingPickedFirst() {
        val desktop = FakeDesktop()
        val (server, _) = serve(desktop)
        val prefs = MemoryPrefStore()
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val dir = LocalScratch.create("control-auto")
        val mine = MeshControlHost(desktop.key, "127.0.0.1", server.localPort, nodeName = "desk", ownerUser = "fran")
        val service = LocalControlService(identity, prefs, FixedHosts(listOf(mine)), "p/y/", userName = { " Fran " }, filesDirectory = { File(dir, "in") })
        val connected = CountDownLatch(1)
        service.addListener { if (service.connected) connected.countDown() }
        if (!service.connected) expect(connected.await(5, TimeUnit.SECONDS))
        assertEq(desktop.key, service.paired.single().key)
        service.dispose()
        server.close()
        LocalScratch.delete(dir)

        val theirs = MeshControlHost("theirs", "127.0.0.1", 1, ownerUser = "mallory")
        val paired = MeshControlHost("paired", "127.0.0.1", 1)
        assertEq(null, automaticControlTarget(listOf(theirs), emptySet(), "fran", emptySet()))
        assertEq(null, automaticControlTarget(listOf(mine), emptySet(), "", emptySet()))
        assertEq(null, automaticControlTarget(listOf(mine), emptySet(), "fran", setOf(desktop.key)))
        assertEq(mine, automaticControlTarget(listOf(theirs, mine), emptySet(), "fran", emptySet()))
        assertEq(paired, automaticControlTarget(listOf(mine, paired), setOf("paired"), "fran", setOf("paired")))
    }

    fun testADesktopTheAccountListsIsDialledOnTheLanWithoutAnOwnerMatch() {
        val desktop = FakeDesktop()
        val (server, _) = serve(desktop)
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val dir = LocalScratch.create("control-account")
        val owned = MeshControlHost(desktop.key, "127.0.0.1", server.localPort, nodeName = "desk")
        val service = LocalControlService(identity, MemoryPrefStore(), FixedHosts(listOf(owned)), "p/y/", userName = { "" }, filesDirectory = { File(dir, "in") })
        expect(!service.connected && service.status == LocalControlStatus.Idle)
        val connected = CountDownLatch(1)
        service.addListener { if (service.connected) connected.countDown() }
        service.setAccountDesktops(setOf(desktop.key))
        if (!service.connected) expect(connected.await(5, TimeUnit.SECONDS))
        assertEq(desktop.key, service.paired.single().key)
        service.dispose()
        server.close()
        LocalScratch.delete(dir)

        val stranger = MeshControlHost("stranger", "127.0.0.1", 1)
        val paired = MeshControlHost("paired", "127.0.0.1", 1)
        assertEq(owned, automaticControlTarget(listOf(stranger, owned), emptySet(), "", setOf(desktop.key), setOf(desktop.key)))
        assertEq(paired, automaticControlTarget(listOf(owned, paired), setOf("paired"), "fran", emptySet(), setOf(desktop.key)))
        assertEq(null, automaticControlTarget(listOf(stranger), emptySet(), "fran", emptySet(), setOf(desktop.key)))
    }

    fun testSafeNameAndPairedJson() {
        assertEq("passwd", LocalControlService.safeName("../../etc/passwd"))
        assertEq("a_b.txt", LocalControlService.safeName("a?b.txt"))
        assertEq("file", LocalControlService.safeName("..."))
        assertEq(null, PairedDesktop.fromJson(mapOf("key" to "short")))
        assertEq("Sign this phone and", "pair_on_lan".let { ControlErrorText.describe(it) }.take(19))
    }

    class LoopChannel(val desktop: FakeDesktop) : RelayFrameChannel {
        override var onFrame: ((Map<String, Any?>) -> Unit)? = null
        override var onConnected: (() -> Unit)? = null
        override var onSignedOut: (() -> Unit)? = null
        private var seq = 0
        private val pending = StringBuilder()
        private var stage = 0
        var closedWith: String? = null

        @Volatile
        var silent = false

        @Volatile
        var forgotten = false

        override fun start() {
            onConnected?.invoke()
        }

        private fun reply(payload: ByteArray) {
            val encoded = B64.encode(payload)
            var at = 0
            do {
                val end = minOf(encoded.length, at + 48)
                onFrame?.invoke(
                    mapOf(
                        "type" to "control", "action" to "data", "session" to session, "from" to "node",
                        "seq" to seq++, "data" to encoded.substring(at, end), "more" to (end < encoded.length),
                    ),
                )
                at = end
            } while (at < encoded.length)
        }

        private var session = ""

        override fun send(frame: Map<String, Any?>): Boolean {
            session = jsonText(frame["session"])
            if (frame["action"] == "close") {
                closedWith = jsonText(frame["reason"])
                return true
            }
            if (silent) return true
            if (forgotten) {
                val gone = session
                Thread {
                    onFrame?.invoke(mapOf("type" to "control", "action" to "close", "session" to gone, "from" to "node", "reason" to "channel_gone"))
                }.start()
                return true
            }
            pending.append(jsonText(frame["data"]))
            if (frame["more"] == true) return true
            val payload = B64.decode(pending.toString())
            pending.setLength(0)
            val current = stage++
            Thread {
                runCatching { dispatch(current, payload) }
            }.start()
            return true
        }

        fun push(event: String, data: Map<String, Any?>) = synchronized(this) {
            reply(desktop.seal(mapOf("event" to event, "data" to data)))
        }

        private fun dispatch(current: Int, payload: ByteArray) = synchronized(this) {
            run {
                when (current) {
                    0 -> reply(desktop.onHello(Json.parse(String(payload)).asJsonMap()))
                    1 -> {
                        desktop.onAuth(Json.parse(String(payload)).asJsonMap())
                        reply(desktop.sessionEvent())
                    }
                    else -> desktop.answer(payload)?.let { reply(it) }
                }
            }
        }

        override fun dispose() {}
    }

    fun testRelayConnectionRunsTheSameProtocol() {
        val desktop = FakeDesktop()
        val channel = LoopChannel(desktop)
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val connection = RelayControlConnection.open(channel, identity, desktop.key, "Studio", timeoutMs = 5000)
        expect(connection.isOpen)
        assertEq("studio", connection.node)
        assertEq("studio", connection.desktopName)
        assertEq(9, (connection.request("agents.start", mapOf("owner" to "a"), 5000)["id"] as Number).toInt())
        connection.close()
        assertEq("closed", channel.closedWith)
        expect(!connection.isOpen)

        val unpaired = FakeDesktop(paired = false)
        val refused = assertFails<LocalControlException> {
            RelayControlConnection.open(LoopChannel(unpaired), identity, unpaired.key, "x", timeoutMs = 5000)
        }
        assertEq("pair_on_lan", refused.code)
        val wrong = assertFails<LocalControlException> {
            RelayControlConnection.open(LoopChannel(desktop), identity, controlB64(Bytes.random(32)), "x", timeoutMs = 5000)
        }
        assertEq("wrong_desktop", wrong.code)
    }

    private fun waitUntil(timeoutMs: Long = 5_000, done: () -> Boolean): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (!done() && System.currentTimeMillis() < deadline) Thread.sleep(20)
        return done()
    }

    fun testRelayKeepaliveNoticesADesktopThatStopsAnswering() {
        val desktop = FakeDesktop()
        val channel = LoopChannel(desktop)
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val connection = RelayControlConnection.open(channel, identity, desktop.key, "studio", timeoutMs = 5000, keepaliveMs = 200)
        expect(waitUntil { synchronized(channel) { desktop.requests.any { it["method"] == "ping" } } })
        expect(connection.isOpen)
        channel.silent = true
        expect(waitUntil { !connection.isOpen })
        assertEq("desktop_unreachable", connection.closeReason)
    }

    fun testADesktopThatForgotTheChannelEndsItAtOnce() {
        val desktop = FakeDesktop()
        val channel = LoopChannel(desktop)
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val connection = RelayControlConnection.open(channel, identity, desktop.key, "studio", timeoutMs = 5000)
        channel.forgotten = true
        expect(!connection.confirmAlive(5_000))
        expect(waitUntil { !connection.isOpen })
        assertEq("channel_gone", connection.closeReason)
    }

    fun testTheHubChecksAnIdleRelayBeforeUsingIt() {
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val desktops = java.util.Collections.synchronizedList(ArrayList<FakeDesktop>())
        val channels = java.util.Collections.synchronizedList(ArrayList<LoopChannel>())
        val key = FakeDesktop().key
        val hub = DesktopControlHub(
            null,
            { target ->
                val desktop = FakeDesktop()
                desktops.add(desktop)
                val channel = LoopChannel(desktop)
                channels.add(channel)
                RelayControlConnection.open(channel, identity, target.key, target.name, timeoutMs = 5000)
            },
            accountDesktopsLoader = { listOf(PairedDesktop(key, "studio", "alice")) },
            relayIdleProbeMs = 0,
        ) { emptyList() }
        hub.request("agents.list")
        assertEq(1, channels.size)
        channels[0].forgotten = true
        assertEq(9, (hub.request("agents.start", mapOf("owner" to "a"))["id"] as Number).toInt())
        assertEq(2, channels.size)
        assertEq(0, synchronized(channels[0]) { desktops[0].requests.count { it["method"] == "agents.start" } })
        assertEq(1, synchronized(channels[1]) { desktops[1].requests.count { it["method"] == "agents.start" } })
        hub.disconnectRelay()
    }

    fun testWithNothingPairedTheHubDialsTheAccountsDesktops() {
        val desktop = FakeDesktop()
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val dialled = java.util.Collections.synchronizedList(ArrayList<String>())
        var loads = 0
        var now = 1_000L
        val hub = DesktopControlHub(
            null,
            { target ->
                dialled.add(target.name)
                if (target.name == "old-laptop") throw LocalControlException("timeout")
                RelayControlConnection.open(LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000)
            },
            accountDesktopsLoader = {
                loads++
                listOf(PairedDesktop(controlB64(Bytes.random(32)), "old-laptop", "alice"), PairedDesktop(desktop.key, "studio", "alice"))
            },
            clock = { now },
        ) { emptyList() }
        assertEq(DesktopReach.NotPaired, hub.reach)
        hub.link()
        assertEq(setOf("old-laptop", "studio"), dialled.toSet())
        assertEq(DesktopReach.Connected(ControlTransport.Relay, "studio"), hub.reach)
        assertEq("studio", hub.desktop?.name)
        hub.refreshAccountDesktops()
        assertEq(1, loads)
        now += 700_000
        hub.refreshAccountDesktops()
        assertEq(2, loads)
        hub.disconnectRelay()
    }

    fun testUnifiedRowsMergeAndCloudTranscript() {
        val transport = HttpTransport { request ->
            val body = if (request.url.endsWith("/api/account/agent-sessions")) {
                """{"sessions":[{"id":"c1","org":"o","repo":"r","status":"running","targetNode":"studio","localAgentId":5,"updatedAt":10},{"id":"c2","org":"o","repo":"r","status":"completed","updatedAt":20}]}"""
            } else {
                """{"session":{"id":"c2","status":"completed","history":[{"role":"user","text":"go"},{"role":"agent","text":"done"}]}}"""
            }
            HttpClient.Response(200, emptyMap(), body.toByteArray())
        }
        val queue = AgentQueueApi({ "https://x" }, { "t" }, transport)
        val cloud = queue.accountSessions().sessions.map(UnifiedAgentRow::fromCloud)
        val desk = listOf(UnifiedAgentRow("desktop:5", AgentTransportBadge.DesktopLan, "t", "o", "r", "studio", "running", "Running", true, localId = 5))
        val merged = mergeAgentRows(desk, cloud, "Studio")
        assertEq(listOf("desktop:5", "node:c2"), merged.map { it.id })
        val hub = DesktopControlHub(null, null) { emptyList() }
        assertEq(DesktopReach.NotPaired, hub.reach)
        val center = AgentControlCenter(hub, queue)
        val delta = center.transcript(merged[1], 0)
        assertEq("> go\n\ndone", delta.text)
        expect(delta.reset)
        val follower = TranscriptFollower(limit = 4)
        follower.apply(AgentTranscriptDelta("abc", 3, false, true, ""))
        follower.apply(AgentTranscriptDelta("def", 6, false, true, ""))
        assertEq("cdef", follower.text)
        val older = AgentTranscriptDelta.fromDesktop(mapOf("transcript" to "all"), 10)
        expect(older.reset)
        assertEq(DesktopControlFamilyName.FilesBrowse, DesktopControlFamilyName.owning("files.browse.list"))
        center.dispose()
    }

    fun testDesktopAgentsStartedOutsideForkMeshListAndStreamTheirTranscripts() {
        val snapshot = LocalAgentsSnapshot.fromJson(
            Json.parse(
                """{"sessions":[{"id":4,"repositoryOwner":"o","repositoryName":"r","status":"running"},{"id":7,"repositoryOwner":"o","repositoryName":"s","status":"running"},""" +
                    """{"id":9,"repositoryOwner":"acme","repositoryName":"tools","status":"completed","finishedAtMs":1700000000100},""" +
                    """{"id":10,"repositoryOwner":"acme","repositoryName":"tools","status":"failed","finishedAtMs":1700000000200}],""" +
                    """"outside":[{"id":-1001,"kind":"codex","label":"Codex","provider":"codex","model":"gpt-5","title":"scratch","cwd":"/home/tester/scratch"},""" +
                    """{"id":-1002,"kind":"claude","label":"Claude Code","title":"Fix login","cwd":"/home/tester/r","repositoryOwner":"o","repositoryName":"r"},{"kind":"aider"}]}""",
            ).asJsonMap(),
        )
        assertEq(listOf(-1001, -1002), snapshot.outside.map { it.id })
        val rows = snapshot.sessions.map { UnifiedAgentRow.fromDesktop(it, ControlTransport.Relay, "studio") } +
            snapshot.outside.map { UnifiedAgentRow.fromOutside(it, ControlTransport.Relay, "studio") }
        val codex = rows.first { it.id == "outside:-1001" }
        expect(codex.outside && codex.isActive && codex.isDesktop)
        expect(!codex.canStop && !codex.canPrompt && !codex.canContinue)
        assertEq(-1001, codex.localId)
        assertEq("scratch", codex.title)
        assertEq("o/r", rows.first { it.id == "outside:-1002" }.repoLabel)
        val live = desktopAgents(rows, listOf("R" to 4))
        assertEq(listOf("desktop:7", "outside:-1001", "outside:-1002", "desktop:10", "desktop:9"), live.map { it.id })
        assertEq(listOf("desktop:10"), desktopAgents(rows, emptyList(), idle = 1).filter { !it.isActive }.map { it.id })
        val desktop = FakeDesktop()
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val hub = DesktopControlHub(
            null,
            { target -> RelayControlConnection.open(LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000) },
            accountDesktopsLoader = { listOf(PairedDesktop(desktop.key, "studio", "alice")) },
        ) { emptyList() }
        val center = AgentControlCenter(hub, null)
        desktop.transcriptError = "no_transcript"
        val delta = center.transcript(codex, 0)
        expect(delta.text.startsWith("Codex was started outside ForkMesh on studio, in /home/tester/scratch."))
        expect(!delta.active && delta.reset)
        desktop.transcriptError = null
        val claude = rows.first { it.id == "outside:-1002" }
        val following = center.transcript(claude, 0)
        assertEq("hello", following.text)
        val asked = desktop.requests.last()
        assertEq("agents.transcript", asked["method"])
        assertEq(-1002, ((asked["params"] as Map<*, *>)["id"] as Number).toInt())
        desktop.transcriptError = "unknown_session"
        val ended = center.transcript(claude, following.next)
        expect(!ended.reset && !ended.active)
        assertEq(following.next, ended.next)
        assertFails<LocalControlException> { center.stop(codex) }
        assertFails<LocalControlException> { center.prompt(codex, "hi") }
        center.dispose()
        hub.disconnectRelay()
    }

    fun testOutsideClaudeCodeIsSteeredThroughTheDesktop() {
        val snapshot = LocalAgentsSnapshot.fromJson(
            Json.parse(
                """{"sessions":[],"outside":[{"id":-2001,"kind":"claude","label":"Claude Code","title":"Fix login","cwd":"/home/tester/app","steer":true},""" +
                    """{"id":-2002,"kind":"claude","label":"Claude Code","cwd":"/home/tester/old"},""" +
                    """{"id":-2003,"kind":"codex","label":"Codex","cwd":"/home/tester/scratch","steer":true}]}""",
            ).asJsonMap(),
        )
        assertEq(listOf(true, false, true), snapshot.outside.map { it.steer })
        val rows = snapshot.outside.map { UnifiedAgentRow.fromOutside(it, ControlTransport.Relay, "studio") }
        val steerable = rows.first { it.id == "outside:-2001" }
        val terminalOnly = rows.first { it.id == "outside:-2002" }
        val codex = rows.first { it.id == "outside:-2003" }
        expect(steerable.canPrompt && !steerable.canStop && !steerable.canContinue)
        expect(!terminalOnly.canPrompt && !codex.canPrompt)
        assertEq("Claude Code started outside ForkMesh on studio in app. You can watch and steer it here.", steerable.outsideSummary)
        expect(terminalOnly.outsideSummary.endsWith("steer it in the terminal that started it."))
        expect(steerable.outsideNote.endsWith("you can still steer it here."))
        val desktop = FakeDesktop()
        val identity = Identity.fromSeed(ByteArray(32) { 9 })
        val hub = DesktopControlHub(
            null,
            { target -> RelayControlConnection.open(LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000) },
            accountDesktopsLoader = { listOf(PairedDesktop(desktop.key, "studio", "alice")) },
        ) { emptyList() }
        val center = AgentControlCenter(hub, null)
        center.prompt(steerable, "  also run the tests ")
        val sent = desktop.requests.last { it["method"] == "agents.prompt" }["params"] as Map<*, *>
        assertEq(-2001, (sent["id"] as Number).toInt())
        assertEq("also run the tests", sent["text"])
        expect("images" !in sent)
        assertEq("", center.promptNotice)
        center.prompt(steerable, "wait")
        expect(center.promptNotice.startsWith("Sent. Claude Code is holding it for approval"))
        expect(center.promptNotice.contains("crossSessionInbound"))
        val before = desktop.requests.count { it["method"] == "agents.prompt" }
        val photo = PromptAttachment("a", "a.png", byteArrayOf(1), "image/png")
        assertEq("outside_images_unsupported", assertFails<LocalControlException> { center.prompt(steerable, "look", listOf(photo)) }.code)
        assertEq("outside_cli_unsupported", assertFails<LocalControlException> { center.prompt(terminalOnly, "hi") }.code)
        assertEq("outside_cli_unsupported", assertFails<LocalControlException> { center.prompt(codex, "hi") }.code)
        assertEq(before, desktop.requests.count { it["method"] == "agents.prompt" })
        val refused = assertFails<LocalControlException> { center.prompt(steerable, "gone") }
        assertEq("not_steerable", refused.code)
        expect(ControlErrorText.describe(refused).contains("terminal that started it"))
        assertEq("outside_stop_unsupported", assertFails<LocalControlException> { center.stop(steerable) }.code)
        center.dispose()
        hub.disconnectRelay()
    }

    fun testOutsideTerminalsTakeTypedPromptsAndInterrupts() {
        val snapshot = LocalAgentsSnapshot.fromJson(
            Json.parse(
                """{"sessions":[],"outside":[{"id":-3001,"kind":"codex","label":"Codex","cwd":"/home/tester/tool","typeable":true,"stoppable":true},""" +
                    """{"id":-3002,"kind":"claude","label":"Claude Code","cwd":"/home/tester/app","steer":true,"typeable":true},""" +
                    """{"id":-3003,"kind":"claude","label":"Claude Code","cwd":"/home/tester/peer","steer":true},""" +
                    """{"id":-3004,"kind":"aider","label":"Aider","cwd":"/home/tester/other","stoppable":true}]}""",
            ).asJsonMap(),
        )
        assertEq(listOf(true, true, false, false), snapshot.outside.map { it.typeable })
        assertEq(listOf(true, false, false, true), snapshot.outside.map { it.stoppable })
        val old = LocalOutsideAgent.fromJson(Json.parse("""{"id":-1,"kind":"claude","steer":true}""").asJsonMap())
        expect(!old.typeable && !old.stoppable)
        val rows = snapshot.outside.map { UnifiedAgentRow.fromOutside(it, ControlTransport.Relay, "studio") }
        val codex = rows.first { it.id == "outside:-3001" }
        val claude = rows.first { it.id == "outside:-3002" }
        val peer = rows.first { it.id == "outside:-3003" }
        val aider = rows.first { it.id == "outside:-3004" }
        assertEq(
            listOf(OutsidePromptPath.Terminal, OutsidePromptPath.Terminal, OutsidePromptPath.Peer, OutsidePromptPath.None),
            rows.map { it.outsidePath },
        )
        assertEq(listOf(true, true, true, false), rows.map { it.canPrompt })
        assertEq(listOf(true, false, false, true), rows.map { it.canStop })
        expect(rows.none { it.canContinue })
        assertEq("Types into your terminal", codex.composerHint)
        assertEq("Message (arrives as another session)", peer.composerHint)
        assertEq("Codex started outside ForkMesh on studio in tool. What you send here is typed into its terminal.", codex.outsideSummary)
        assertEq("Claude Code started outside ForkMesh on studio in app. What you send here is typed into its terminal.", claude.outsideSummary)
        assertEq("Claude Code started outside ForkMesh on studio in peer. You can watch and steer it here.", peer.outsideSummary)
        expect(aider.outsideSummary.endsWith("steer it in the terminal that started it."))
        expect(codex.outsideNote.endsWith("what you send here is typed into its terminal."))
        val desktop = FakeDesktop()
        val identity = Identity.fromSeed(ByteArray(32) { 10 })
        val hub = DesktopControlHub(
            null,
            { target -> RelayControlConnection.open(LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000) },
            accountDesktopsLoader = { listOf(PairedDesktop(desktop.key, "studio", "alice")) },
        ) { emptyList() }
        val center = AgentControlCenter(hub, null)
        center.prompt(codex, " type ")
        val sent = desktop.requests.last { it["method"] == "agents.prompt" }["params"] as Map<*, *>
        assertEq(-3001, (sent["id"] as Number).toInt())
        assertEq("type", sent["text"])
        assertEq("Typed into your terminal.", center.promptNotice)
        center.prompt(peer, "hello")
        assertEq("", center.promptNotice)
        val photo = PromptAttachment("a", "a.png", byteArrayOf(1), "image/png")
        assertEq("outside_images_unsupported", assertFails<LocalControlException> { center.prompt(codex, "look", listOf(photo)) }.code)
        assertEq("outside_cli_unsupported", assertFails<LocalControlException> { center.prompt(aider, "hi") }.code)
        val typing = assertFails<LocalControlException> { center.prompt(claude, "busy") }
        assertEq("terminal_user_typing", typing.code)
        assertEq("Someone is typing in that terminal right now. Try again in a moment.", ControlErrorText.describe(typing))
        center.stop(codex)
        val stopped = desktop.requests.last { it["method"] == "agents.stop" }["params"] as Map<*, *>
        assertEq(-3001, (stopped["id"] as Number).toInt())
        val stops = desktop.requests.count { it["method"] == "agents.stop" }
        assertEq("outside_stop_unsupported", assertFails<LocalControlException> { center.stop(claude) }.code)
        assertEq("outside_stop_unsupported", assertFails<LocalControlException> { center.stop(peer) }.code)
        assertEq(stops, desktop.requests.count { it["method"] == "agents.stop" })
        center.dispose()
        hub.disconnectRelay()
    }

    fun testTerminalTypingErrorsReadAsSentences() {
        assertEq(
            "Too many prompts from this phone in the last minute. Wait a moment and try again.",
            ControlErrorText.describe("typing_rate_limited"),
        )
        assertEq(
            "Phone typing is turned off in that terminal. Press Ctrl-^ there (or restart it without --no-remote) to turn it back on.",
            ControlErrorText.describe("terminal_remote_off"),
        )
        assertEq("That terminal can no longer be typed into. It may have closed.", ControlErrorText.describe("not_typeable"))
        assertEq("Someone is typing in that terminal right now. Try again in a moment.", ControlErrorText.describe("terminal_user_typing"))
    }

    fun testAnAgentInAnotherLoginIsListedWithANote() {
        val agent = LocalOutsideAgent.fromJson(
            mapOf("id" to -3005, "kind" to "claude", "label" to "Claude Code", "title" to "Claude Code (b)", "otherUser" to "b"),
        )
        assertEq("b", agent.otherUser)
        val row = UnifiedAgentRow.fromOutside(agent, ControlTransport.Lan, "desk")
        assertEq("Claude Code (b)", row.title)
        expect(!row.canPrompt && !row.canStop, "another login's agent takes no prompt or stop")
        assertEq(
            "Claude Code is running in b's login on desk. ForkMesh cannot read or steer another login's agents; " +
                "open ForkMesh in that login to follow it.",
            row.outsideSummary,
        )
        expect(row.outsideNote.endsWith("Open ForkMesh in b's login to watch and steer it there."), row.outsideNote)
        expect(ControlErrorText.describe("outside_other_user").startsWith("That agent runs in another login"))
        assertEq("", UnifiedAgentRow.fromOutside(LocalOutsideAgent.fromJson(mapOf("id" to -3006, "kind" to "claude")), ControlTransport.Lan, "desk").outsideUser)
    }

    fun testAnAgentStartedFromThePhoneInADesktopTerminalIsMarked() {
        val snapshot = LocalAgentsSnapshot.fromJson(
            Json.parse(
                """{"sessions":[],"outside":[{"id":-4001,"kind":"claude","label":"Claude Code","cwd":"/home/tester/app","typeable":true,"stoppable":true,""" +
                    """"terminal":"0123456789abcdef0123456789abcdef"},{"id":-4002,"kind":"codex","label":"Codex","cwd":"/home/tester/tool","typeable":true}]}""",
            ).asJsonMap(),
        )
        assertEq(listOf("0123456789abcdef0123456789abcdef", ""), snapshot.outside.map { it.terminal })
        val rows = snapshot.outside.map { UnifiedAgentRow.fromOutside(it, ControlTransport.Relay, "studio") }
        val phone = rows.first { it.id == "outside:-4001" }
        val wrapped = rows.first { it.id == "outside:-4002" }
        expect(phone.isPhoneTerminal && phone.canPrompt && phone.canStop)
        assertEq("0123456789abcdef0123456789abcdef", phone.terminal)
        expect(!wrapped.isPhoneTerminal)
        assertEq(
            "Claude Code started from your phone in a terminal on studio in app. Open the terminal for full control; " +
                "what you send here is typed into it.",
            phone.outsideSummary,
        )
        expect(phone.outsideNote.startsWith("Claude Code was started from your phone in a terminal on studio, in /home/tester/app."), phone.outsideNote)
        assertEq("Codex started outside ForkMesh on studio in tool. What you send here is typed into its terminal.", wrapped.outsideSummary)
        expect(!UnifiedAgentRow("desktop:1", AgentTransportBadge.DesktopLan, "t", "o", "r", "studio", "running", "Running", true, terminal = "x").isPhoneTerminal)
    }

    fun testDesktopTerminalObjectsDecode() {
        val info = DesktopTerminalInfo.fromJson(
            Json.parse(
                """{"terminal":"0123456789abcdef0123456789abcdef","provider":"codex","label":"Codex","cli":"/usr/bin/codex","repositoryOwner":"alice",""" +
                    """"repositoryName":"app","cwd":"/home/alice/app","startedAtMs":1700000000123,"device":"Pixel 9","running":true,"cols":80,"rows":24,"replay":""}""",
            ).asJsonMap(),
        )
        assertEq(
            DesktopTerminalInfo(
                "0123456789abcdef0123456789abcdef", "codex", "Codex", "/usr/bin/codex", "alice", "app", "/home/alice/app",
                1700000000123, "Pixel 9", true, 80, 24,
            ),
            info,
        )
        assertEq("alice/app", info.repoLabel)
        val listed = DesktopTerminalInfo.list(
            Json.parse("""{"terminals":[{"terminal":"a1","running":false},{"provider":"codex"},{"terminal":"b2"}]}""").asJsonMap(),
        )
        assertEq(listOf("a1", "b2"), listed.map { it.terminal })
        assertEq(listOf(false, true), listed.map { it.running })
        expect(DesktopTerminalInfo.list(emptyMap()).isEmpty())
    }

    fun testDesktopTerminalErrorsReadAsSentences() {
        val codes = listOf(
            "unknown_repository", "unknown_terminal", "unsupported_provider", "cli_missing", "guardrails_on",
            "terminal_vm", "terminal_limit", "terminal_failed", "bad_input",
        )
        for (code in codes) {
            val sentence = ControlErrorText.describe(code)
            expect(sentence != code.replace('_', ' ') && sentence.endsWith("."), "$code reads as a sentence: $sentence")
        }
        assertEq("That agent's command-line tool is not installed on the desktop.", ControlErrorText.describe("cli_missing"))
        assertEq("Six terminals (agents and SSH sessions) already run on the desktop. End one first.", ControlErrorText.describe("terminal_limit"))
        assertEq("That terminal has ended on the desktop.", ControlErrorText.describe(LocalControlException("unknown_terminal")))
    }

    fun testDesktopQueueParsesAndToleratesMissingFields() {
        val queue = LocalAgentQueue.fromJson(
            Json.parse("""{"running":2,"limit":5,"limitMax":8,"queue":[7,3,9]}""").asJsonMap(),
        )
        assertEq(2, queue.running)
        assertEq(5, queue.limit)
        assertEq(8, queue.limitMax)
        assertEq(listOf(7, 3, 9), queue.order)
        assertEq(3, queue.waiting)
        assertEq("2 of 5 running · 3 waiting", queue.summary)
        assertEq(1, queue.position(3))
        assertEq(-1, queue.position(4))
        assertEq(0, queue.target(3, QueueMove.Top))
        assertEq(0, queue.target(3, QueueMove.Up))
        assertEq(2, queue.target(3, QueueMove.Down))
        assertEq(3, queue.target(3, QueueMove.Bottom))
        assertEq(null, queue.target(7, QueueMove.Top))
        assertEq(null, queue.target(7, QueueMove.Up))
        assertEq(null, queue.target(9, QueueMove.Down))
        assertEq(null, queue.target(9, QueueMove.Bottom))
        assertEq(null, queue.target(4, QueueMove.Top))
        val empty = LocalAgentQueue.fromJson(emptyMap())
        assertEq(LocalAgentQueue(running = 0, limit = 1, limitMax = 99, order = emptyList()), empty)
        assertEq("0 of 1 running", empty.summary)
        val odd = LocalAgentQueue.fromJson(
            Json.parse("""{"running":-1,"limit":120,"limitMax":3,"queue":[4,"x",0,4,-2,6]}""").asJsonMap(),
        )
        assertEq(0, odd.running)
        assertEq(99, odd.limit)
        assertEq(99, odd.limitMax)
        assertEq(listOf(4, 6), odd.order)
        assertEq(null, LocalAgentQueue.fromJsonOrNull("queue"))
        val snapshot = LocalAgentsSnapshot.fromJson(
            Json.parse(
                """{"sessions":[{"id":7,"repositoryOwner":"o","repositoryName":"r","status":"queued"}],"running":1,""" +
                    """"queue":{"running":1,"limit":1,"limitMax":8,"queue":[7]}}""",
            ).asJsonMap(),
        )
        assertEq(listOf(7), snapshot.queue?.order)
        assertEq(8, snapshot.queue?.limitMax)
        val older = LocalAgentsSnapshot.fromJson(Json.parse("""{"sessions":[],"running":0}""").asJsonMap())
        assertEq(null, older.queue)
        assertEq("Choose between 1 and 99 agents at once.", ControlErrorText.describe("bad_limit"))
        assertEq("That agent is no longer waiting in the desktop's queue.", ControlErrorText.describe("not_queued"))
    }

    fun testDesktopQueueLimitAndOrderAreChangedFromThePhone() {
        val desktop = FakeDesktop()
        val identity = Identity.fromSeed(ByteArray(32) { 12 })
        val hub = DesktopControlHub(
            null,
            { target -> RelayControlConnection.open(LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000) },
            accountDesktopsLoader = { listOf(PairedDesktop(desktop.key, "studio", "alice")) },
        ) { emptyList() }
        val center = AgentControlCenter(hub, null)
        expect(!center.supportsQueue)
        center.refresh()
        expect(center.supportsQueue)
        assertEq(listOf(11, 12, 13), center.queue?.order)
        assertEq(2, center.queue?.limit)
        fun queued(id: Int) = UnifiedAgentRow(
            id = "desktop:$id",
            badge = AgentTransportBadge.DesktopRelay,
            title = "t$id",
            repoOwner = "a",
            repoName = "b",
            node = "studio",
            status = "queued",
            statusLabel = "Queued",
            isActive = true,
            localId = id,
        )
        assertEq(2, center.queuePosition(queued(13)))
        assertEq(-1, center.queuePosition(queued(5)))
        val raised = center.setQueueLimit(3)
        assertEq(3, raised.limit)
        val limitParams = desktop.requests.last { it["method"] == "agents.queue.limit" }["params"] as Map<*, *>
        assertEq(3, (limitParams["limit"] as Number).toInt())
        assertEq(3, center.queue?.limit)
        val requestsBefore = desktop.requests.size
        assertEq("bad_limit", assertFails<LocalControlException> { center.setQueueLimit(0) }.code)
        assertEq("bad_limit", assertFails<LocalControlException> { center.setQueueLimit(100) }.code)
        assertEq(requestsBefore, desktop.requests.size)
        val top = center.moveQueued(queued(13), QueueMove.Top)
        assertEq(listOf(13, 11, 12), top?.order)
        val moveParams = desktop.requests.last { it["method"] == "agents.queue.move" }["params"] as Map<*, *>
        assertEq(13, (moveParams["id"] as Number).toInt())
        assertEq(0, (moveParams["to"] as Number).toInt())
        assertEq(listOf(13, 11, 12), center.queue?.order)
        assertEq(listOf(11, 12, 13), center.moveQueued(queued(13), QueueMove.Bottom)?.order)
        assertEq(listOf(12, 11, 13), center.moveQueued(queued(12), QueueMove.Up)?.order)
        assertEq(listOf(11, 12, 13), center.moveQueued(queued(12), QueueMove.Down)?.order)
        assertEq(null, center.moveQueued(queued(11), QueueMove.Top))
        assertEq(listOf(12, 11, 13), center.moveQueued(queued(11), 1).order)
        assertEq("not_queued", assertFails<LocalControlException> { center.moveQueued(queued(5), 0) }.code)
        val outside = queued(13).copy(id = "outside:-13", outside = true, localId = -13)
        assertEq("not_queued", assertFails<LocalControlException> { center.moveQueued(outside, 0) }.code)
        center.dispose()
        hub.disconnectRelay()
    }

    fun testOpeningADesktopAgentMarksItReadOnTheDesktop() {
        val desktop = FakeDesktop()
        val identity = Identity.fromSeed(ByteArray(32) { 13 })
        val hub = DesktopControlHub(
            null,
            { target -> RelayControlConnection.open(LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000) },
            accountDesktopsLoader = { listOf(PairedDesktop(desktop.key, "studio", "alice")) },
        ) { emptyList() }
        val center = AgentControlCenter(hub, null)
        center.refresh()
        expect(!center.markRead(5))
        assertEq(5, ((desktop.requests.last { it["method"] == "agents.markRead" }["params"] as Map<*, *>)["id"] as Number).toInt())
        desktop.handlers["agents.markRead"] = { params ->
            if ((params["id"] as? Number)?.toInt() != 5) throw LocalControlException("unknown_session")
            emptyMap()
        }
        expect(center.markRead(5))
        expect(!center.markRead(6))
        val sent = desktop.requests.filter { it["method"] == "agents.markRead" }.map { ((it["params"] as Map<*, *>)["id"] as Number).toInt() }
        assertEq(listOf(5, 5, 6), sent)
        val before = desktop.requests.size
        expect(!center.markRead(0))
        assertEq(before, desktop.requests.size)
        center.dispose()
        hub.disconnectRelay()
    }

    fun testDesktopListCarriesTheUnreadFlag() {
        fun row(id: Int, extra: Map<String, Any?> = emptyMap()) =
            mapOf("id" to id, "status" to "done", "finishedAtMs" to 1000L * id, "repositoryOwner" to "a", "repositoryName" to "b") + extra
        val flagged = LocalAgentsSnapshot.fromJson(
            mapOf("sessions" to listOf(row(1, mapOf("unread" to true)), row(2, mapOf("unread" to false)), row(4))),
        )
        assertEq(listOf(true, false, null), flagged.sessions.map { it.unread })
        val older = LocalAgentsSnapshot.fromJson(mapOf("sessions" to listOf(row(1), row(2))))
        assertEq(listOf<Boolean?>(null, null), older.sessions.map { it.unread })
        val unified = UnifiedAgentRow.fromDesktop(flagged.sessions.first(), ControlTransport.Lan, "studio")
        assertEq(true, unified.unread)
        assertEq(1000L, unified.finishedAtMs)
        assertEq(false, UnifiedAgentRow.fromDesktop(flagged.sessions[1], ControlTransport.Lan, "studio").unread)
        assertEq(null, UnifiedAgentRow.fromDesktop(flagged.sessions[2], ControlTransport.Lan, "studio").unread)
        assertEq(null, UnifiedAgentRow.fromDesktop(older.sessions.first(), ControlTransport.Lan, "studio").unread)
        val running = LocalAgentRow.fromJson(row(3, mapOf("status" to "running", "unread" to true)))
        assertEq(0L, UnifiedAgentRow.fromDesktop(running, ControlTransport.Relay, "studio").finishedAtMs)
    }

    fun testCloudRowsSurviveTheListWindowAndFailedRefresh() {
        var answer = 200 to """{"sessions":[{"id":"run","status":"running","updatedAt":1700000003000},{"id":"old","status":"completed","updatedAt":1700000001000},{"id":"gone","status":"failed","updatedAt":1700000002000},{"id":"late","status":"completed","updatedAt":1700000005000}],"finishedBefore":0}"""
        val transport = HttpTransport { HttpClient.Response(answer.first, emptyMap(), answer.second.toByteArray()) }
        val queue = AgentQueueApi({ "https://x" }, { "t" }, transport)
        val center = AgentControlCenter(DesktopControlHub(null, null) { emptyList() }, queue)
        center.refresh()
        assertEq(setOf("node:run", "node:old", "node:gone", "node:late"), center.rows.map { it.id }.toSet())
        answer = 200 to """{"sessions":[{"id":"gone","status":"failed","updatedAt":1700000002000}],"finishedBefore":1700000002000}"""
        center.refresh()
        assertEq(setOf("node:gone", "node:old"), center.rows.map { it.id }.toSet())
        answer = 500 to """{"error":"down"}"""
        center.refresh()
        assertEq(setOf("node:gone", "node:old"), center.rows.map { it.id }.toSet())
        expect(center.cloudNote.isNotEmpty())
        answer = 200 to """{"sessions":[],"finishedBefore":0}"""
        center.refresh()
        expect(center.rows.isEmpty())
        assertEq("", center.cloudNote)
        center.dispose()
    }
}
