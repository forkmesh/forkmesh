package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.ChatDeviceKeys
import com.forkmesh.nativeapp.crypto.ChatNodeKey
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class GroupCallSessionTest {
    private val roomId = "b".repeat(32)
    private val publicId = "c".repeat(32)
    private val peerId = "0123456789abcdef0123456789abcdef"

    private class Rig(val service: OfficeChannelService, val api: FakeAccountApi, val socket: FakeOfficeSocket, val crypto: RoomCrypto, val peer: RoomSeal)

    private fun rig(): Rig {
        val api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = mapOf(
            "channels" to listOf(
                mapOf("id" to roomId, "name" to "team", "visibility" to "private", "members" to listOf("me", "pat")),
                mapOf("id" to publicId, "name" to "lobby", "visibility" to "public"),
            ),
        )
        val room = "chat-channel:$roomId:v1"
        api.replies["GET /api/chat/channels/$roomId/room-access"] = mapOf(
            "room" to room, "passphrase" to "team-pass", "webSocketUrl" to "/api/chat/channels/$roomId/ws?ticket=abc",
        )
        api.replies["POST /api/chat/channels/$roomId/call-ticket"] = mapOf(
            "ticket" to "t", "room" to "r", "peer" to peerId, "url" to "wss://n2.forkmesh.com/calls/ws",
        )
        val connector = FakeOfficeConnector()
        val me = ChatNodeKey.generate()
        val service = OfficeChannelService(api, connector, MemorySecretStore(), { "https://app.forkmesh.com" }, { me.node }, { "Me" }, nodeKey = { me })
        service.refresh()
        expect(service.open("#office/team"))
        val socket = connector.last!!
        val crypto = RoomCrypto.withPassphrase(room, "team-pass")
        val ownBundle = crypto.decrypt(Json.parseObject(socket.sent.first()))!!
        val peer = RoomSeal.create(room, room, SealPolicy.OPEN, "pat", ChatDeviceKeys.fromSeed(ByteArray(32) { 7 }), nodeKey = ChatNodeKey.generate())
        peer.acceptKeyFrame(ownBundle)
        socket.onText(Json.stringify(crypto.encrypt(peer.keyFrame())))
        return Rig(service, api, socket, crypto, peer)
    }

    fun testLiveFramesAreSealedAndNeverStored() {
        val rig = rig()
        expect(rig.service.sendLiveFrame("#office/team", "group-call-presence", mapOf("peer" to peerId, "call" to "in", "sender" to "mallory")))
        val wire = Json.parseObject(rig.socket.sent.last())
        expect(wire["persist"] == null && wire["notify"] == null)
        val plain = rig.peer.unseal(rig.crypto.decrypt(wire)!!)!!
        assertEq("group-call-presence", plain["type"])
        assertEq(peerId, plain["peer"])
        assertEq("in", plain["call"])
        assertEq("Me", plain["sender"])
        assertEq("#team", plain["channel"])
        assertEq(true, plain["private"])
        expect(!plain.containsKey("text"))
        expect(!rig.service.sendLiveFrame("#office/lobby", "group-call-presence", mapOf("peer" to peerId)), "public channels have no group calls")
        expect(!rig.service.sendLiveFrame("#office/team", "chat", mapOf("text" to "x")))
        assertEq(emptyList<String>(), rig.service.messages("#office/team").map { it.text })
    }

    fun testIncomingCallFramesGoToTheCallNotTheHistory() {
        val rig = rig()
        val seen = ArrayList<Pair<String, String>>()
        rig.service.onLiveFrame = { plain, conversation -> seen.add(jsonType(plain) to conversation) }
        val frame = rig.peer.seal(mapOf("type" to "group-call-key", "id" to "k1", "senderId" to rig.peer.node, "sender" to "pat",
            "channel" to "#team", "peer" to peerId, "keyId" to 1L, "key" to "AAAAAAAAAAAAAAAAAAAAAA==", "ts" to System.currentTimeMillis()))
        rig.socket.onText(Json.stringify(rig.crypto.encrypt(frame)))
        assertEq(listOf("group-call-key" to "#office/team"), seen)
        assertEq(0, rig.service.messages("#office/team").size)
    }

    fun testTicketsAreOnlyForPrivateChannels() {
        val rig = rig()
        val ticket = rig.service.callTicket("#office/team", mapOf("engines" to listOf("webrtc")))!!
        assertEq(peerId, ticket["peer"])
        expect(GroupCallTicket.from(ticket) != null)
        assertEq(null, rig.service.callTicket("#office/lobby"))
    }

    private fun jsonType(plain: Map<String, Any?>): String = plain["type"] as String

    fun testPageEventsAreParsedAndBounded() {
        val send = GroupCallPageEvent.parse(Json.parseObject("""{"event":"send","channel":"#office/team","type":"group-call-key","body":{"peer":"x"}}"""))
        assertEq(GroupCallPageEvent.Send("#office/team", "group-call-key", mapOf("peer" to "x")), send)
        assertEq(null, GroupCallPageEvent.parse(Json.parseObject("""{"event":"send","channel":"#office/team","type":"chat","body":{}}""")))
        val state = GroupCallPageEvent.parse(Json.parseObject("""{"event":"state","state":"live","others":99,"muted":true,"active":true}"""))
        assertEq(GroupCallPageEvent.State("live", 15, true, false, true, ""), state)
        assertEq(GroupCallPageEvent.Ticket(3, "c1", emptyMap()), GroupCallPageEvent.parse(Json.parseObject("""{"event":"ticket","id":3,"channel":"c1"}""")))
        assertEq(GroupCallPageEvent.Ready, GroupCallPageEvent.parse(Json.parseObject("""{"event":"ready"}""")))
        assertEq(null, GroupCallPageEvent.parse(Json.parseObject("""{"event":"other"}""")))
    }

    fun testTheStateLineMatchesTheIPhone() {
        val view = GroupCallView(conversation = "#office/team", state = "live", others = 2)
        assertEq("#team", view.title)
        assertEq("Connected · end-to-end encrypted · 3 in call", view.stateLine)
        assertEq("Reconnecting…", view.copy(state = "reconnecting").stateLine)
        assertEq("Connecting…", view.copy(state = "joining").stateLine)
        expect(!GroupCallView().active)
    }

    fun testRecentFramesAreKeptForAMinuteForTheNextJoin() {
        var now = 1_000_000L
        val buffer = GroupCallFrameBuffer(now = { now }, limit = 2)
        buffer.add(mapOf("type" to "group-call-presence", "peer" to "a"), "#office/team")
        buffer.add(mapOf("type" to "chat"), "#office/team")
        now += 30_000
        buffer.add(mapOf("type" to "group-call-key", "peer" to "b"), "#office/team")
        buffer.add(mapOf("type" to "group-call-presence", "peer" to "c"), "#office/team")
        assertEq(listOf("b", "c"), buffer.recent("#office/team").map { it["peer"] })
        now += 61_000
        assertEq(0, buffer.recent("#office/team").size)
        assertEq(0, buffer.recent("#office/other").size)
    }
}
