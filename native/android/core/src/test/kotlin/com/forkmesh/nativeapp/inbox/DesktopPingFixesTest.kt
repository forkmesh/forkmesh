package com.forkmesh.nativeapp.inbox

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

private class FakePingsChannel : ControlCalling {
    val calls = ArrayList<Pair<String, Map<String, Any?>>>()
    var reply: Map<String, Any?> = emptyMap()
    var failure: Exception? = null

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        calls.add(method to params)
        failure?.let { throw it }
        return reply
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = Any()

    override fun removeEventTap(token: Any) {}
}

class DesktopPingFixesTest {
    private fun desktopPing(pingId: String = "1790000000000-7") = InboxItem.fromPing(
        ForkNotification(
            id = "n1", kind = "desktop_ping", title = "Action failed",
            body = "Native iOS ad hoc build on the local Mac · forkmesh/forkmesh",
            ts = 1_790_000_000_500L, meta = linkedMapOf("node" to "studio", "pingId" to pingId),
        ),
    )

    private val reply: Map<String, Any?> = linkedMapOf(
        "found" to true, "event" to "action-failed", "cause" to "host-offline",
        "fixes" to listOf(
            linkedMapOf("id" to "workflow.disable", "label" to "Turn off this action", "icon" to "pause", "score" to 0.85, "primary" to true),
            linkedMapOf("id" to "run.rerun", "label" to "Run it again", "icon" to "rerun", "score" to 0.35),
        ),
    )

    fun testInboxItemKeepsTheDesktopPingIdentity() {
        val item = desktopPing()
        expect(item.fromDesktop, "a desktop ping is from the desktop")
        assertEq(item.desktopPingId, "1790000000000-7")
        assertEq(item.desktopNode, "studio")
        val web = InboxItem.fromPing(ForkNotification(id = "n2", kind = "pull_submitted", title = "PR"))
        expect(!web.fromDesktop, "a web ping is not from the desktop")
        expect(DesktopPingFixes.params(web) == null, "a web ping has no fix params")
    }

    fun testParamsFallBackToTextWithoutAPingId() {
        val params = DesktopPingFixes.params(desktopPing(pingId = ""))!!
        assertEq(params["title"], "Action failed")
        assertEq(params["ts"], 1_790_000_000_500L)
        expect(!params.containsKey("pingId"), "no ping id is sent when the server did not keep one")
    }

    fun testFixesAskThePingsFamily() {
        val channel = FakePingsChannel().also { it.reply = reply }
        val fixes = DesktopPingFixes(channel).fixes(desktopPing())
        assertEq(channel.calls.first().first, "pings.fixes")
        assertEq(channel.calls.first().second["pingId"], "1790000000000-7")
        expect(fixes.found, "fixes were found")
        assertEq(fixes.fixes.map { it.id }, listOf("workflow.disable", "run.rerun"))
        assertEq(fixes.primary?.label, "Turn off this action")
        assertEq(fixes.primary?.materialIcon, "pause_circle_outlined")
    }

    fun testApplySendsTheFix() {
        val channel = FakePingsChannel().also { it.reply = reply + mapOf("ok" to true, "message" to "Turned off it.") }
        val result = DesktopPingFixes(channel).apply(PingQuickFix("workflow.disable", "Turn off this action"), desktopPing())
        assertEq(channel.calls.first().first, "pings.fix")
        assertEq(channel.calls.first().second["fix"], "workflow.disable")
        assertEq(result.message, "Turned off it.")
    }

    fun testAnOlderDesktopExplainsItself() {
        val channel = FakePingsChannel().also { it.failure = LocalControlException("unsupported") }
        val error = runCatching { DesktopPingFixes(channel).fixes(desktopPing()) }.exceptionOrNull()
        expect(error != null, "the call fails")
        assertEq(DesktopPingFixes.explain(error!!), "Update the desktop to use quick fixes.")
    }
}
