package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.PI
import kotlin.math.abs

class DirectLiveTest {
    private val conversationId = "d".repeat(32)
    private val conversation = "direct:$conversationId"
    private val room = "chat-direct:$conversationId:v1"

    private class Clock(var ms: Long)

    private class Rig(val engine: DirectLiveEngine, val clock: Clock, val loop: EventLoop, val sent: MutableList<Pair<String, Map<String, Any?>>>) {
        fun settle() = loop.advance(1)
    }

    private fun rig(start: Long = 1_000_000, interval: Long = 0): Rig {
        val clock = Clock(start)
        val loop = EventLoop("direct-live-test", virtualTime = true)
        val sent = java.util.Collections.synchronizedList(ArrayList<Pair<String, Map<String, Any?>>>())
        val engine = DirectLiveEngine(
            selfId = { "me-node" },
            send = { conversation, fields ->
                sent.add(conversation to fields)
                true
            },
            interval = { interval },
            clock = { clock.ms },
            loop = loop,
        )
        return Rig(engine, clock, loop, sent)
    }

    private fun peerFrame(vararg fields: Pair<String, Any?>, sender: String = "pat-node", ts: Long = 10_000): Map<String, Any?> =
        mapOf("type" to "live", "id" to "pat-live", "senderId" to sender, "sender" to "pat", "channel" to conversation, "ts" to ts) + fields

    private fun close(expected: Double, actual: Double?, accuracy: Double = 0.0001) {
        expect(actual != null && abs(expected - actual) <= accuracy, "expected $expected but was $actual")
    }

    private fun ink(fields: Map<String, Any?>): Map<String, Any?>? = (fields["ink"] as? List<*>)?.firstOrNull() as? Map<String, Any?>

    fun testIntervalsFollowTheRelayBudget() {
        assertEq(88L, DirectLiveFrames.interval(150, 10_000))
        assertEq(DirectLiveFrames.MIN_INTERVAL_MS, DirectLiveFrames.interval(100_000, 10_000))
        assertEq(DirectLiveFrames.FALLBACK_INTERVAL_MS, DirectLiveFrames.interval(0, 10_000))
        assertEq(null, DirectLiveFrames.budget(mapOf("kind" to "forkmesh-live", "perWindow" to 0L, "windowMs" to 10L)))
        assertEq(DirectLiveBudget(150, 10_000), DirectLiveFrames.budget(Json.parseObject("""{"kind":"forkmesh-live","v":1,"perWindow":150,"windowMs":10000}""")))
    }

    fun testFramesRoundLikeIos() {
        assertEq(listOf(250L, -500L), DirectLiveFrames.encodePoints(listOf(DirectLivePoint(0.25, -0.5))))
        assertEq(-3L, DirectLiveFrames.scaled(-0.025, 100.0))
        assertEq(3L, DirectLiveFrames.scaled(0.025, 100.0))
        assertEq(listOf(DirectLivePoint(0.5, DirectLiveFrames.MAX_Y)), DirectLiveFrames.decodePoints(listOf(500L, 9000L, 1L), 4))
        assertEq("", DirectLiveFrames.strokeId("bad id!"))
        assertEq("s-1", DirectLiveFrames.strokeId("s-1"))
        assertEq(0xFFFFFF, DirectLiveFrames.color(0x1000000L))
        assertEq(0x64D2FF, DirectLiveFrames.color(0x64D2FFL))
        assertEq(null, DirectLiveFrames.decodeMotion(listOf(70_000L, 0L, 0L, 0L), 0))
    }

    fun testMotionSamplesGiveTiltAndInterpolate() {
        val upright = DirectLiveMotionSample(0, 0.0, -1.0, 0.0)
        close(0.0, upright.roll)
        close(0.0, upright.pitch)
        close(PI / 2, DirectLiveMotionSample(0, 0.0, 0.0, -1.0).pitch)
        val right = DirectLiveMotionSample(0, 1.0, 0.0, 0.0)
        close(PI / 2, right.roll)
        close(PI / 2, right.steadyRoll)
        close(0.0449, DirectLiveMotionSample(0, 0.01, 0.0, -1.0).steadyRoll, 0.001)
        assertEq(0.0, DirectLiveMotionSample(0, Double.NaN, 99.0, 0.0).gx)
        assertEq(DirectLiveFrames.MAX_MOTION, DirectLiveMotionSample(0, 0.0, 99.0, 0.0).gy)
        val track = DirectLiveMotionTrack(
            samples = listOf(DirectLiveMotionSample(100, 0.0, -1.0, 0.0, 0.0), DirectLiveMotionSample(200, 1.0, 0.0, 0.0, 2.0)),
            lastHeard = 200,
        )
        assertEq(-1.0, track.sample(50)?.gy)
        close(0.5, track.sample(150)?.gx)
        close(1.0, track.sample(150)?.ax)
        assertEq(1.0, track.sample(900)?.gx)
        expect(track.isSharing(200 + DirectLiveFrames.MOTION_QUIET_MS - 1))
        expect(!track.isSharing(200 + DirectLiveFrames.MOTION_QUIET_MS))
    }

    private fun extent(points: List<DirectLivePlanePoint>): Pair<Double, Double> =
        (points.maxOf { it.x } - points.minOf { it.x }) to (points.maxOf { it.y } - points.minOf { it.y })

    fun testPhonePoseShowsAFlatPhoneAsALine() {
        val upright = extent(DirectLivePhonePose(DirectLiveMotionSample(0, 0.0, -1.0, 0.0)).outline())
        close(DirectLivePhonePose.WIDTH, upright.first, 0.01)
        close(DirectLivePhonePose.HEIGHT, upright.second, 0.01)
        val faceUp = DirectLivePhonePose(DirectLiveMotionSample(0, 0.0, 0.0, -1.0))
        val line = extent(faceUp.outline())
        close(DirectLivePhonePose.WIDTH, line.first, 0.01)
        close(DirectLivePhonePose.DEPTH, line.second, 0.01)
        expect(!faceUp.faceShowing)
        close(0.0, faceUp.faceMap.d)
        close(DirectLivePhonePose.DEPTH, extent(DirectLivePhonePose(DirectLiveMotionSample(0, 0.0, 0.0, 1.0)).outline()).second, 0.01)
        val halfway = extent(DirectLivePhonePose(0.0, PI / 4).outline())
        expect(halfway.second > DirectLivePhonePose.DEPTH && halfway.second < DirectLivePhonePose.HEIGHT)
        val sideways = extent(DirectLivePhonePose(DirectLiveMotionSample(0, 1.0, 0.0, 0.0)).outline())
        close(DirectLivePhonePose.HEIGHT, sideways.first, 0.01)
        close(DirectLivePhonePose.WIDTH, sideways.second, 0.01)
        assertEq(DirectLiveFaceMap(1.0, 0.0, -0.0, 1.0, 0.0, -0.0), DirectLivePhonePose.still.faceMap)
    }

    fun testPhonePoseHoldsEveryBounceLongEnoughToSee() {
        val track = DirectLiveMotionTrack(
            samples = listOf(
                DirectLiveMotionSample(0, 0.0, 0.0, -1.0),
                DirectLiveMotionSample(16, 0.0, 0.0, -1.0, az = 1.5),
                DirectLiveMotionSample(33, 0.0, 0.0, -1.0),
            ),
            lastHeard = 33,
        )
        val soon = DirectLivePhonePose.live(track, 116)
        expect(soon != null && soon.offset.y < 0 && soon.jolt > 0.5)
        close(0.0, DirectLivePhonePose.live(track, 16 + DirectLivePhonePose.KICK_HOLD_MS)?.jolt)
        expect(DirectLivePhonePose(DirectLiveMotionSample(0, 0.0, -1.0, 0.0, 1.0)).offset.x > 0)
        close(DirectLivePhonePose.MAX_KICK, DirectLivePhonePose(DirectLiveMotionSample(0, 0.0, -1.0, 0.0, ay = 8.0)).jolt)
    }

    fun testTapsAndStrokesAreBatchedIntoLiveFrames() {
        val r = rig()
        r.engine.tap(conversation, DirectLivePoint(0.25, -0.5))
        assertEq(listOf(true), r.engine.taps(conversation).map { it.isSelf })
        r.settle()
        assertEq(1, r.sent.size)
        assertEq(listOf(listOf(250L, -500L)), r.sent[0].second["taps"])
        assertEq(1_000_000L, r.sent[0].second["ts"])

        val id = r.engine.beginStroke(conversation, DirectLivePoint(0.1, 0.1), 0xFF453A, 6.0)
        r.engine.extendStroke(conversation, id, DirectLivePoint(0.1001, 0.1))
        r.engine.extendStroke(conversation, id, DirectLivePoint(0.2, 0.2))
        assertEq(2, r.engine.strokes(conversation).first().points.size)
        r.settle()
        val first = r.sent.mapNotNull { ink(it.second) }.first()
        assertEq(id, first["s"])
        assertEq(0xFF453AL, first["c"])
        assertEq(60L, first["w"])
        r.engine.extendStroke(conversation, id, DirectLivePoint(0.3, 0.3))
        r.engine.endStroke(conversation, id)
        r.settle()
        val chunks = r.sent.mapNotNull { ink(it.second) }
        assertEq(true, chunks.last()["e"])
        assertEq(listOf(100L, 100L, 200L, 200L, 300L, 300L), chunks.flatMap { it["p"] as List<*> })
        assertEq(null, chunks.last()["c"])

        r.engine.clearInk(conversation)
        expect(r.engine.strokes(conversation).isEmpty())
        r.settle()
        assertEq(true, r.sent.last().second["clear"])
    }

    fun testSendsWaitForTheRelayPace() {
        val r = rig(interval = 88)
        r.engine.tap(conversation, DirectLivePoint(0.5, 0.0))
        r.settle()
        assertEq(1, r.sent.size)
        r.engine.tap(conversation, DirectLivePoint(0.5, 0.0))
        r.engine.tap(conversation, DirectLivePoint(0.6, 0.0))
        r.loop.advance(50)
        assertEq(1, r.sent.size)
        r.loop.advance(40)
        assertEq(2, r.sent.size)
        assertEq(2, (r.sent[1].second["taps"] as List<*>).size)
    }

    fun testLongStrokesAreSplitAcrossFrames() {
        val r = rig()
        val id = r.engine.beginStroke(conversation, DirectLivePoint(0.0, 0.0), 0xFFFFFF, 4.0)
        for (step in 1 until 400) {
            r.engine.extendStroke(conversation, id, DirectLivePoint(0.5 + (step % 2) / 100.0, step / 400.0))
        }
        r.engine.endStroke(conversation, id)
        repeat(5) { r.settle() }
        val counts = r.sent.mapNotNull { (ink(it.second)?.get("p") as? List<*>)?.size }
        expect(counts.all { it <= DirectLiveFrames.MAX_INK_POINTS_PER_FRAME * 2 })
        assertEq(800, counts.sum())
        assertEq(true, r.sent.mapNotNull { ink(it.second) }.last()["e"])
    }

    fun testPeerFramesBecomeTapsInkAndMotion() {
        val r = rig(start = 50_000)
        val felt = ArrayList<DirectLiveTap>()
        val drawn = ArrayList<String>()
        r.engine.onTap = { _, tap -> felt.add(tap) }
        r.engine.onStroke = { _, stroke -> drawn.add(stroke.id) }
        r.engine.receive(peerFrame("taps" to listOf(listOf(500L, 0L)), sender = "me-node"), conversation)
        r.engine.receive(peerFrame("taps" to listOf(listOf(500L, 0L))), "direct:" + "e".repeat(32))
        expect(felt.isEmpty())

        r.engine.receive(
            peerFrame(
                "taps" to listOf(listOf(500L, 9000L)),
                "ink" to listOf(
                    mapOf("s" to "s-1", "c" to 0x64D2FFL, "w" to 80L, "p" to listOf(100L, 100L, 200L, 200L)),
                    mapOf("s" to "bad id!", "p" to listOf(1L, 1L)),
                ),
                "motion" to listOf(listOf(-80L, 0L, -100L, 0L, 0L, 0L, 0L), listOf(0L, 100L, 0L, 0L, 50L, 0L, 0L)),
            ),
            conversation,
        )
        assertEq(1, felt.size)
        assertEq(DirectLivePoint(0.5, DirectLiveFrames.MAX_Y), felt[0].point)
        expect(!felt[0].isSelf)
        assertEq(listOf("pat-node|s-1"), drawn)
        val stroke = r.engine.strokes(conversation).first()
        assertEq(0x64D2FF, stroke.color)
        assertEq(8.0, stroke.width)
        assertEq(listOf(DirectLivePoint(0.1, 0.1), DirectLivePoint(0.2, 0.2)), stroke.points)

        r.engine.receive(peerFrame("ink" to listOf(mapOf("s" to "s-1", "p" to listOf(300L, 300L), "e" to true))), conversation)
        assertEq(3, r.engine.strokes(conversation).first().points.size)
        assertEq(true, r.engine.strokes(conversation).first().ended)

        val track = r.engine.partnerMotion(conversation)!!
        val newest = 50_000 + DirectLiveFrames.MOTION_PLAYBACK_MARGIN_MS
        assertEq(listOf(newest - 80, newest), track.samples.map { it.at })
        close(0.5, track.sample(newest - 40)?.gx)

        r.engine.receive(peerFrame("motionOff" to true), conversation)
        assertEq(null, r.engine.partnerMotion(conversation))
        r.engine.receive(peerFrame("clear" to true), conversation)
        expect(r.engine.strokes(conversation).isEmpty())
        r.settle()
        expect(r.sent.isEmpty())
    }

    fun testPartnerMotionPlaysTheNewestSampleOneMarginBehind() {
        val r = rig(start = 60_000)
        val margin = DirectLiveFrames.MOTION_PLAYBACK_MARGIN_MS
        r.engine.receive(
            peerFrame("motion" to listOf(listOf(-66L, 0L, -100L, 0L), listOf(-33L, 20L, -100L, 0L), listOf(0L, 40L, -100L, 0L)), ts = 5_000),
            conversation,
        )
        var track = r.engine.partnerMotion(conversation)!!
        assertEq(60_000 + margin, track.samples.last().at)
        close(0.4, track.sample(60_000 + margin)?.gx)
        r.clock.ms += 88
        val newest = r.engine.now + margin
        r.engine.receive(
            peerFrame("motion" to listOf(listOf(-55L, 60L, -100L, 0L), listOf(-22L, 80L, -100L, 0L), listOf(0L, 100L, -100L, 0L)), ts = 5_088),
            conversation,
        )
        track = r.engine.partnerMotion(conversation)!!
        assertEq(newest, track.samples.last().at)
        assertEq(track.samples.map { it.at }.sorted(), track.samples.map { it.at })
        close(0.4, track.sample(60_000 + margin)?.gx)
        close(1.0, track.sample(newest)?.gx)
        val between = track.sample(60_000 + margin + 10)!!.gx
        expect(between > 0.4 && between < 0.6)
    }

    fun testInkFadesAndIsPruned() {
        val r = rig(start = 1_000)
        r.engine.receive(peerFrame("ink" to listOf(mapOf("s" to "a", "p" to listOf(0L, 0L, 10L, 10L), "e" to true)), "taps" to listOf(listOf(1L, 1L))), conversation)
        val stroke = r.engine.strokes(conversation).first()
        assertEq(1.0, stroke.opacity(1_000))
        val fadeStart = 1_000 + DirectLiveFrames.INK_LIFE_MS - DirectLiveFrames.INK_FADE_MS
        close(0.5, stroke.opacity(fadeStart + DirectLiveFrames.INK_FADE_MS / 2), 0.001)
        r.clock.ms += 2_000
        r.engine.prune()
        expect(r.engine.taps(conversation).isEmpty())
        assertEq(1, r.engine.strokes(conversation).size)
        r.clock.ms += DirectLiveFrames.INK_LIFE_MS
        expect(!r.engine.prune())
        expect(r.engine.strokes(conversation).isEmpty())
    }

    fun testSharingMotionSendsSamplesAndAStop() {
        val r = rig(start = 2_000)
        r.engine.addMotion(DirectLiveMotionSample(1_990, 0.0, -1.0, 0.0))
        r.settle()
        expect(r.sent.isEmpty())
        r.engine.setSharingMotion(conversation, true)
        expect(r.engine.isSharingMotion(conversation))
        r.engine.addMotion(DirectLiveMotionSample(1_950, 0.5, -0.5, 0.0, 0.25))
        r.settle()
        assertEq(listOf(listOf(-50L, 50L, -50L, 0L, 25L, 0L, 0L)), r.sent.last().second["motion"])
        r.engine.stopSharingMotion()
        expect(!r.engine.isSharingMotion(conversation))
        r.settle()
        assertEq(true, r.sent.last().second["motionOff"])
    }

    private fun preparedApi(): FakeAccountApi {
        val api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages"] = mapOf(
            "conversations" to listOf(mapOf("id" to conversationId, "otherUser" to "pat")),
            "nextCursor" to "",
        )
        api.replies["GET /api/chat/direct-messages/$conversationId/room-access"] = mapOf(
            "room" to room, "passphrase" to "direct-pass",
            "webSocketUrl" to "/api/chat/direct-messages/$conversationId/ws?ticket=abc",
        )
        return api
    }

    private class Room(val dm: DirectMessageService, val hub: ChatHub, val relay: FakeChatRelay, val socket: FakeOfficeSocket, val peer: RoomSeal, val crypto: RoomCrypto)

    private fun room(): Room {
        val store = MemorySecretStore()
        val connector = FakeOfficeConnector()
        val dm = DirectMessageService(
            preparedApi(), connector, store, { "https://app.forkmesh.com" }, { "me-node" }, { "Me" },
            loop = EventLoop("direct-live-room", virtualTime = true),
        )
        dm.refresh()
        val relay = FakeChatRelay()
        val hub = ChatHub(relay)
        hub.direct = dm
        hub.select(conversation)
        val socket = connector.last!!
        socket.onText("""{"kind":"forkmesh-history-end","v":1}""")
        val crypto = RoomCrypto.withPassphrase(room, "direct-pass")
        val peer = RoomSeal.loadOrCreate(room, scope = conversation, policy = SealPolicy.STRICT, acct = "pat", secureStore = MemorySecretStore(), node = "pat-node")
        peer.acceptKeyFrame(RoomSeal.loadOrCreate(room, scope = conversation, policy = SealPolicy.STRICT, acct = "me-node", secureStore = store).keyFrame())
        socket.onText(Json.stringify(crypto.encrypt(peer.keyFrame())))
        return Room(dm, hub, relay, socket, peer, crypto)
    }

    fun testLiveFramesTravelSealedAndUnstoredThroughTheDirectRoom() {
        val idle = DirectMessageService(
            preparedApi(), FakeOfficeConnector(), MemorySecretStore(), { "https://app.forkmesh.com" }, { "me-node" }, { "Me" },
            loop = EventLoop("direct-live-idle", virtualTime = true),
        )
        idle.refresh()
        assertEq(false, idle.sendLive(conversation, mapOf("taps" to listOf(listOf(1L, 1L)))))
        val r = room()
        assertEq(DirectLiveFrames.FALLBACK_INTERVAL_MS, r.dm.liveInterval(conversation))
        r.socket.onText("""{"kind":"forkmesh-live","v":1,"perWindow":150,"windowMs":10000}""")
        assertEq(88L, r.dm.liveInterval(conversation))

        val engine = r.dm.makeLiveEngine()
        var felt = 0
        engine.onTap = { _, _ -> felt++ }
        val before = r.socket.sent.size
        expect(r.dm.sendLive(conversation, mapOf("taps" to listOf(listOf(500L, 0L)), "ts" to 7L)))
        val envelope = r.socket.sent.drop(before).map { Json.parseObject(it) }.first { it["live"] == true }
        assertEq(null, envelope["persist"])
        assertEq(null, envelope["notify"])
        val plain = r.peer.unseal(r.crypto.decrypt(envelope)!!)!!
        assertEq("live", plain["type"])
        assertEq(conversation, plain["channel"])
        assertEq("me-node", plain["senderId"])
        assertEq(7L, plain["ts"])
        assertEq(listOf(listOf(500L, 0L)), plain["taps"])

        val fromPat = mapOf(
            "type" to "live", "id" to "pat-live-1", "senderId" to "pat-node", "sender" to "pat",
            "channel" to conversation, "ts" to 1000L, "taps" to listOf(listOf(100L, 100L)),
        )
        r.socket.onText(Json.stringify(r.crypto.encrypt(r.peer.seal(fromPat))))
        r.socket.onText(Json.stringify(r.crypto.encrypt(r.peer.seal(fromPat)) + ("historyReplay" to true)))
        assertEq(1, felt)
        expect(r.dm.messages(conversation).isEmpty())
        r.dm.closeAll()
    }

    fun testServerDirectMessagesFindThePersonOnTheRoster() {
        val r = room()
        assertEq(ChatPeerPresence.OFFLINE, r.hub.peerPresence(conversation))
        r.relay.peers.add(Member("pat-node", "Pat", online = true))
        assertEq(ChatPeerPresence.ONLINE, r.hub.peerPresence(conversation))
        expect(r.hub.peerOnline(conversation))
        r.relay.peers.clear()
        r.relay.peers.add(Member("pat-laptop", "Pat's laptop", online = true, owner = "PAT"))
        assertEq(ChatPeerPresence.ONLINE, r.hub.peerPresence(conversation))
        r.relay.peers.clear()
        r.relay.peers.add(Member("pat-laptop", "Pat's laptop", online = true, owner = "pat", userStatus = com.forkmesh.nativeapp.model.UserStatus(away = true)))
        assertEq(ChatPeerPresence.AWAY, r.hub.peerPresence(conversation))
        r.relay.peers.clear()
        assertEq(ChatPeerPresence.OFFLINE, r.hub.peerPresence(conversation))
        val engine = r.dm.makeLiveEngine()
        r.hub.live = engine
        engine.receive(peerFrame("taps" to listOf(listOf(1L, 1L))), conversation)
        assertEq(ChatPeerPresence.ONLINE, r.hub.peerPresence(conversation))
        r.relay.peers.add(Member("sam-node", "Sam", online = true, owner = "sam"))
        assertEq(ChatPeerPresence.ONLINE, r.hub.peerPresence("@sam-node"))
        r.dm.closeAll()
    }
}
