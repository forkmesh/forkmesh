package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.Collections

class DesktopTranscriptStreamTest {
    private fun waitUntil(predicate: () -> Boolean): Boolean {
        val deadline = System.currentTimeMillis() + 5_000
        while (System.currentTimeMillis() < deadline) {
            if (predicate()) return true
            Thread.sleep(10)
        }
        return predicate()
    }

    private fun output(id: Int, text: String, from: Int, reset: Boolean = false, status: String = "running") = mapOf(
        "id" to id, "transcript" to text, "from" to from, "next" to from + text.length, "reset" to reset,
        "active" to (status == "running"), "status" to status,
    )

    private fun hub(desktop: LocalControlTest.FakeDesktop, channels: MutableList<LocalControlTest.LoopChannel>) = DesktopControlHub(
        null,
        { target ->
            val channel = LocalControlTest.LoopChannel(desktop)
            channels.add(channel)
            RelayControlConnection.open(channel, Identity.fromSeed(ByteArray(32) { 8 }), target.key, target.name, timeoutMs = 5000)
        },
        accountDesktopsLoader = { listOf(PairedDesktop(desktop.key, "studio", "alice")) },
    ) { emptyList() }

    fun testTheDesktopPushesOutputAsItIsWritten() {
        val desktop = LocalControlTest.FakeDesktop()
        desktop.followAnswer = output(7, "abc", from = 3) + ("streaming" to true)
        val channels = ArrayList<LocalControlTest.LoopChannel>()
        val hub = hub(desktop, channels)
        val deltas = Collections.synchronizedList(ArrayList<AgentTranscriptDelta>())
        var ended = ""
        val stream = DesktopTranscriptStream(hub, 7, since = 3, onEnded = { ended = it }) { deltas.add(it) }
        expect(stream.start())
        val followed = desktop.requests.last { it["method"] == "agents.follow" }["params"] as Map<*, *>
        assertEq(7, (followed["id"] as Number).toInt())
        assertEq(3, (followed["since"] as Number).toInt())
        assertEq(listOf("abc"), deltas.map { it.text })

        val channel = channels.single()
        channel.push("agents.output", output(7, "de", from = 6))
        channel.push("agents.output", output(8, "other run", from = 6))
        channel.push("agents.output", output(7, "stale", from = 2))
        channel.push("agents.output", output(7, "tail", from = 40))
        channel.push("agents.output", output(7, "", from = 44, status = "completed"))
        expect(waitUntil { deltas.size == 4 })
        assertEq(listOf("abc", "de", "tail", ""), deltas.map { it.text })
        assertEq(listOf(false, false, true, false), deltas.map { it.reset })
        assertEq("completed", deltas.last().status)
        assertEq(44, stream.next)

        channel.push("agents.output", mapOf("id" to 7, "error" to "unknown_session"))
        expect(waitUntil { ended == "unknown_session" })
        expect(!stream.isStreaming)
        expect(waitUntil { desktop.requests.any { it["method"] == "agents.unfollow" } })
        hub.disconnectRelay()
    }

    fun testRefollowPicksUpWhatAQuietFollowMissed() {
        val desktop = LocalControlTest.FakeDesktop()
        desktop.followAnswer = output(7, "abc", from = 0) + ("streaming" to true)
        val channels = ArrayList<LocalControlTest.LoopChannel>()
        val hub = hub(desktop, channels)
        val deltas = Collections.synchronizedList(ArrayList<AgentTranscriptDelta>())
        val stream = DesktopTranscriptStream(hub, 7) { deltas.add(it) }
        expect(stream.start())
        assertEq(3, stream.next)

        desktop.followAnswer = output(7, "reply", from = 3) + ("streaming" to true)
        expect(stream.refollow())
        val follows = desktop.requests.filter { it["method"] == "agents.follow" }
            .map { ((it["params"] as Map<*, *>)["since"] as Number).toInt() }
        assertEq(listOf(0, 3), follows)
        assertEq(listOf("abc", "reply"), deltas.map { it.text })
        assertEq(8, stream.next)
        expect(desktop.requests.none { it["method"] == "agents.unfollow" })

        channels.single().push("agents.output", output(7, "!", from = 8))
        expect(waitUntil { deltas.size == 3 })
        assertEq(9, stream.next)
        stream.stop()
        expect(!stream.refollow())
        hub.disconnectRelay()
    }

    fun testAnOlderDesktopLeavesTheCallerReading() {
        val desktop = LocalControlTest.FakeDesktop()
        val hub = hub(desktop, ArrayList())
        val deltas = ArrayList<AgentTranscriptDelta>()
        val stream = DesktopTranscriptStream(hub, 7) { deltas.add(it) }
        expect(!stream.start())
        expect(deltas.isEmpty())
        stream.stop()
        hub.disconnectRelay()
    }

    fun testTheFastestDesktopWinsTheRace() {
        val desktop = LocalControlTest.FakeDesktop()
        val identity = Identity.fromSeed(ByteArray(32) { 8 })
        val hub = DesktopControlHub(
            null,
            { target ->
                if (target.name == "asleep") {
                    Thread.sleep(3_000)
                    throw LocalControlException("timeout")
                }
                RelayControlConnection.open(LocalControlTest.LoopChannel(desktop), identity, target.key, target.name, timeoutMs = 5000)
            },
            accountDesktopsLoader = {
                listOf(PairedDesktop(controlB64(ByteArray(32) { 1 }), "asleep", "alice"), PairedDesktop(desktop.key, "studio", "alice"))
            },
        ) { emptyList() }
        var changes = 0
        hub.addLinkListener { changes++ }
        val started = System.currentTimeMillis()
        hub.link()
        expect(System.currentTimeMillis() - started < 2_000)
        assertEq(DesktopReach.Connected(ControlTransport.Relay, "studio"), hub.reach)
        assertEq(1, changes)
        hub.disconnectRelay()
        assertEq(2, changes)
    }
}
