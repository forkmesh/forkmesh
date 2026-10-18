package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.DesktopTerminalInfo
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.control.LocalControlTest
import com.forkmesh.nativeapp.control.PairedDesktop
import com.forkmesh.nativeapp.control.RelayControlConnection
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.Collections

class DesktopAgentTerminalTest {
    private val id = "0123456789abcdef0123456789abcdef"
    private val other = "f".repeat(32)

    private class Desk(val configure: (LocalControlTest.FakeDesktop) -> Unit) {
        val desktops: MutableList<LocalControlTest.FakeDesktop> = Collections.synchronizedList(ArrayList())
        val channels: MutableList<LocalControlTest.LoopChannel> = Collections.synchronizedList(ArrayList())
        val key = LocalControlTest.FakeDesktop().key

        val hub = DesktopControlHub(
            null,
            { target ->
                val desktop = LocalControlTest.FakeDesktop().also(configure)
                desktops.add(desktop)
                val channel = LocalControlTest.LoopChannel(desktop)
                channels.add(channel)
                RelayControlConnection.open(channel, Identity.fromSeed(ByteArray(32) { 11 }), target.key, target.name, timeoutMs = 5000)
            },
            accountDesktopsLoader = { listOf(PairedDesktop(key, "studio", "alice")) },
        ) { emptyList() }

        fun requests(method: String): List<Map<*, *>> {
            val all = ArrayList<Map<String, Any?>>()
            for (desktop in synchronized(desktops) { desktops.toList() }) {
                all += runCatching { desktop.requests.toList() }.getOrDefault(emptyList())
            }
            return all.filter { it["method"] == method }.map { it["params"] as? Map<*, *> ?: emptyMap<String, Any?>() }
        }

        fun typed(): List<String> = requests("terminal.input").map { String(B64.decode(jsonText(it["data"]))) }

        fun push(event: String, data: Map<String, Any?>) = channels.last().push(event, data)
    }

    private fun waitUntil(timeoutMs: Long = 5_000, predicate: () -> Boolean): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            if (predicate()) return true
            Thread.sleep(10)
        }
        return predicate()
    }

    private fun terminalObject(terminal: String, replay: String = "") = mapOf(
        "terminal" to terminal, "provider" to "claude-code", "label" to "Claude Code", "cli" to "/usr/bin/claude",
        "repositoryOwner" to "alice", "repositoryName" to "app", "cwd" to "/home/alice/app", "startedAtMs" to 1700000000000L,
        "device" to "Pixel", "running" to true, "cols" to 40, "rows" to 10, "replay" to B64.encode(replay.toByteArray()),
    )

    private fun output(terminal: String, bytes: ByteArray, reset: Boolean = false) =
        mapOf("terminal" to terminal, "data" to B64.encode(bytes), "reset" to reset)

    private fun output(terminal: String, text: String, reset: Boolean = false) = output(terminal, text.toByteArray(), reset)

    private fun answering(desktop: LocalControlTest.FakeDesktop, replays: MutableList<String> = mutableListOf()) {
        desktop.handlers["terminal.start"] = { terminalObject(id) }
        desktop.handlers["terminal.attach"] = { params ->
            if (jsonText(params["terminal"]) != id) throw LocalControlException("unknown_terminal")
            terminalObject(id, synchronized(replays) { if (replays.isEmpty()) "" else replays.removeAt(0) })
        }
        for (method in listOf("terminal.input", "terminal.resize", "terminal.detach", "terminal.close")) {
            desktop.handlers[method] = { emptyMap() }
        }
    }

    fun testAnAccountSignInStartsInLoginModeForThatAccount() {
        val desk = Desk { answering(it) }
        val terminal = DesktopAgentTerminal(desk.hub, provider = "codex", mode = "login", account = "n1", notifyDelayMs = 0)
        expect(terminal.statusText.startsWith("Starting Codex sign-in"), terminal.statusText)
        terminal.start(40, 10)
        expect(waitUntil { desk.requests("terminal.start").isNotEmpty() }, terminal.statusText)
        val sent = desk.requests("terminal.start").single()
        assertEq("codex", sent["provider"])
        assertEq("login", sent["mode"])
        assertEq("n1", sent["account"])
        terminal.detach()
    }

    fun testStartFeedsTheScreenTypesInOrderAndEndsOnExit() {
        val desk = Desk { answering(it) }
        var started: DesktopTerminalInfo? = null
        val terminal = DesktopAgentTerminal(
            desk.hub, owner = "alice", name = "app", provider = "claude-code", onStarted = { started = it },
            inputFlushMs = 0, notifyDelayMs = 0, enterDelayMs = 0,
        )
        assertEq("", terminal.terminalId)
        terminal.start(40, 10)
        expect(waitUntil { terminal.live }, terminal.statusText)
        val sent = desk.requests("terminal.start").single()
        assertEq("alice", sent["owner"])
        assertEq("app", sent["name"])
        assertEq("claude-code", sent["provider"])
        assertEq(40, (sent["cols"] as Number).toInt())
        assertEq(10, (sent["rows"] as Number).toInt())
        expect(!sent.containsKey("mode") && !sent.containsKey("account"), "$sent")
        assertEq(id, terminal.terminalId)
        assertEq("Pixel", started?.device)
        assertEq("Claude Code · app", terminal.title)
        assertEq(TerminalStatusLevel.Ok, terminal.statusLevel)
        expect(terminal.statusText.startsWith("Live on studio: Claude Code in alice/app."), terminal.statusText)

        val accent = "é".toByteArray()
        desk.push("terminal.output", output(other, "not mine\r\n"))
        desk.push("terminal.output", output(id, "hello\r\n"))
        desk.push("terminal.output", output(id, byteArrayOf(*"caf".toByteArray(), accent[0])))
        desk.push("terminal.output", output(id, byteArrayOf(accent[1])))
        expect(waitUntil { terminal.plainText == "hello\ncafé" }, terminal.plainText)
        desk.push("terminal.output", output(id, "fresh", reset = true))
        expect(waitUntil { terminal.plainText == "fresh" }, terminal.plainText)

        terminal.input("a")
        terminal.input("b")
        terminal.key(TerminalKey.Enter)
        terminal.submit("go")
        expect(waitUntil { desk.typed().size == 5 }, desk.typed().toString())
        assertEq(listOf("a", "b", "\r", "go", "\r"), desk.typed())
        expect(desk.requests("terminal.input").all { it["terminal"] == id })

        terminal.resize(50, 12)
        expect(waitUntil { desk.requests("terminal.resize").isNotEmpty() })
        val resized = desk.requests("terminal.resize").single()
        assertEq(50, (resized["cols"] as Number).toInt())
        assertEq(12, (resized["rows"] as Number).toInt())

        desk.push("terminal.exit", mapOf("terminal" to other, "code" to 3))
        desk.push("terminal.exit", mapOf("terminal" to id, "code" to 2))
        expect(waitUntil { terminal.exitCode == 2 })
        expect(terminal.finished && !terminal.live)
        assertEq("Claude Code exited with status 2.", terminal.statusText)
        terminal.input("late")
        terminal.detach()
        expect(desk.requests("terminal.detach").isEmpty())
        assertEq(5, desk.typed().size)
        desk.hub.disconnectRelay()
    }

    fun testAttachReplaysBatchesInputAndAttachesAgainOnANewLink() {
        val replays = mutableListOf("first screen", "after reconnect")
        val desk = Desk { answering(it, replays) }
        val terminal = DesktopAgentTerminal(desk.hub, terminal = id, inputFlushMs = 40, notifyDelayMs = 0)
        terminal.start(30, 8)
        expect(waitUntil { terminal.live }, terminal.statusText)
        assertEq("first screen", terminal.plainText)
        val attach = desk.requests("terminal.attach").single()
        assertEq(id, attach["terminal"])
        assertEq(30, (attach["cols"] as Number).toInt())
        assertEq(8, (attach["rows"] as Number).toInt())
        expect(desk.requests("terminal.start").isEmpty())

        terminal.input("x")
        terminal.input("y")
        expect(waitUntil { desk.typed().isNotEmpty() })
        assertEq(listOf("xy"), desk.typed())

        desk.hub.disconnectRelay()
        expect(waitUntil(10_000) { desk.requests("terminal.attach").size == 2 && terminal.live }, terminal.statusText)
        assertEq("after reconnect", terminal.plainText)
        assertEq(2, desk.channels.size)
        desk.push("terminal.output", output(id, " and more"))
        expect(waitUntil { terminal.plainText == "after reconnect and more" }, terminal.plainText)

        terminal.detach()
        expect(waitUntil { desk.requests("terminal.detach").size == 1 })
        assertEq(id, desk.requests("terminal.detach").single()["terminal"])
        desk.hub.disconnectRelay()
    }

    fun testRefusalsAndEnd() {
        val missing = Desk { it.handlers["terminal.start"] = { throw LocalControlException("cli_missing") } }
        val refused = DesktopAgentTerminal(missing.hub, owner = "alice", name = "app", provider = "codex", notifyDelayMs = 0)
        refused.start(40, 10)
        expect(waitUntil { refused.failed })
        assertEq(ControlErrorText.describe("cli_missing"), refused.statusText)
        assertEq(TerminalStatusLevel.Error, refused.statusLevel)
        expect(refused.finished && refused.terminalId.isEmpty())
        refused.detach()
        missing.hub.disconnectRelay()

        val desk = Desk { answering(it) }
        val ended = DesktopAgentTerminal(desk.hub, terminal = other, notifyDelayMs = 0)
        ended.start(40, 10)
        expect(waitUntil { ended.gone })
        assertEq("This terminal has ended on the desktop.", ended.statusText)
        ended.detach()

        val running = DesktopAgentTerminal(desk.hub, terminal = id, inputFlushMs = 0, notifyDelayMs = 0)
        running.start(40, 10)
        expect(waitUntil { running.live })
        running.input("bye")
        running.end()
        assertEq(listOf("bye"), desk.typed())
        assertEq(id, desk.requests("terminal.close").single()["terminal"])
        expect(running.gone && running.detached && !running.live)
        expect(desk.requests("terminal.detach").isEmpty())
        desk.hub.disconnectRelay()
    }

    fun testResumeOpensTheAgentInATerminal() {
        val desk = Desk { desktop ->
            answering(desktop)
            desktop.handlers["terminal.resume"] = { terminalObject(id, "resumed screen") + ("session" to 17) }
        }
        val terminal = DesktopAgentTerminal(
            desk.hub, owner = "alice", name = "app", provider = "claude-code", session = 17, notifyDelayMs = 0,
        )
        assertEq("Opening the agent in a terminal on your desktop…", terminal.statusText)
        terminal.start(40, 10)
        expect(waitUntil { terminal.live }, terminal.statusText)
        val sent = desk.requests("terminal.resume").single()
        assertEq(17, (sent["id"] as Number).toInt())
        assertEq(40, (sent["cols"] as Number).toInt())
        assertEq(10, (sent["rows"] as Number).toInt())
        expect(desk.requests("terminal.start").isEmpty())
        assertEq(id, terminal.terminalId)
        assertEq("resumed screen", terminal.plainText)
        terminal.detach()
        desk.hub.disconnectRelay()

        val old = Desk { it.handlers["terminal.resume"] = { throw LocalControlException("unknown_terminal") } }
        val refused = DesktopAgentTerminal(old.hub, provider = "claude-code", session = 17, notifyDelayMs = 0)
        refused.start(40, 10)
        expect(waitUntil { refused.failed })
        expect(!refused.gone)
        assertEq(ControlErrorText.describe("unsupported"), refused.statusText)
        refused.detach()
        old.hub.disconnectRelay()
    }
}
