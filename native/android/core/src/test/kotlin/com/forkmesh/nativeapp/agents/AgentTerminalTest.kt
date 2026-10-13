package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentTerminalTest {
    private class FakeChannel : RelayFrameChannel {
        override var onFrame: ((Map<String, Any?>) -> Unit)? = null
        override var onConnected: (() -> Unit)? = null
        override var onSignedOut: (() -> Unit)? = null
        val sent = ArrayList<Map<String, Any?>>()
        var started = false
        var disposed = false

        override fun start() {
            started = true
            onConnected?.invoke()
        }

        override fun send(frame: Map<String, Any?>): Boolean {
            sent.add(frame)
            return true
        }

        override fun dispose() {
            disposed = true
        }
    }

    fun testFeedCursorColoursAndErase() {
        val screen = TerminalScreen(4, 10)
        screen.feed("ab\u001b[31mc\u001b[0m\r\nxyz\u001b[2;2H!")
        assertEq("abc", screen.grid[0].text)
        assertEq(1, screen.grid[0].cells[2].fg)
        assertEq("x!z", screen.grid[1].text)
        screen.feed("\u001b[2K")
        assertEq("", screen.grid[1].text)
        screen.feed("\u001b[38;2;1;2;3mQ")
        assertEq(TERMINAL_TRUE_COLOR + 0x010203, screen.grid[1].cells[2].fg)
    }

    fun testKeyboardModeSequencesLeaveTextUnstyled() {
        val screen = TerminalScreen(4, 10)
        screen.feed("\u001b[>4;2m\u001b[>1u\u001b[?u\u001b[2;3Hab")
        assertEq("  ab", screen.grid[1].text)
        expect(!screen.grid[1].cells[2].underline)
        expect(!screen.grid[1].cells[2].dim)
        screen.feed("\u001b[4:3mc\u001b[4:0md\u001b[38:2::1:2:3me")
        expect(screen.grid[1].cells[4].underline)
        expect(!screen.grid[1].cells[5].underline)
        assertEq(TERMINAL_TRUE_COLOR + 0x010203, screen.grid[1].cells[6].fg)
    }

    fun testWrapScrollbackAndAltScreen() {
        val screen = TerminalScreen(2, 4)
        screen.feed("abcdefgh\r\nij")
        expect(screen.scrollback.isNotEmpty())
        assertEq("abcd", screen.scrollback.first().text)
        expect(screen.scrollback.first().wrapped)
        screen.feed("\u001b[?1049h")
        expect(screen.onAltScreen)
        screen.feed("zz\u001b[?1049l")
        expect(!screen.onAltScreen)
        assertEq("ij", screen.grid[1].text)
    }

    fun testLinksFromOscAndWrappedUrl() {
        val screen = TerminalScreen(3, 20)
        screen.feed("\u001b]8;;https://a.example/x\u0007click\u001b]8;;\u0007 go\r\n")
        screen.feed("https://claude.ai/oauth?code=abcdefghijklmnop.")
        val links = screen.links()
        assertEq("https://a.example/x", links[0])
        assertEq("https://claude.ai/oauth?code=abcdefghijklmnop", links[1])
        val unsafe = TerminalScreen(1, 40)
        unsafe.feed("\u001b]8;;javascript:alert(1)\u0007x\u001b]8;;\u0007")
        expect(unsafe.links().isEmpty())
    }

    fun testResizeKeepsCursorLine() {
        val screen = TerminalScreen(4, 10)
        screen.feed("1\r\n2\r\n3\r\n4")
        val smaller = screen.resized(2, 5)
        assertEq("3", smaller.grid[0].text)
        assertEq("4", smaller.grid[1].text)
        assertEq(1, smaller.cy)
        expect(smaller.scrollback.map { it.text }.containsAll(listOf("1", "2")))
    }

    fun testKeysAndControlBytes() {
        assertEq("\u001b[A", terminalKeyBytes(TerminalKey.Up))
        assertEq("\u001bOA", terminalKeyBytes(TerminalKey.Up, appCursorKeys = true))
        assertEq("\u0003", terminalControlBytes("c"))
        assertEq("\u001d", terminalControlBytes("]"))
        assertEq("", terminalControlBytes("ab"))
    }

    fun testUtf8SplitAcrossFrames() {
        val decoder = StreamingUtf8()
        val bytes = "é✓".toByteArray()
        assertEq("", decoder.decode(bytes.copyOfRange(0, 1)))
        assertEq("é✓", decoder.decode(bytes.copyOfRange(1, bytes.size)))
    }

    fun testSessionOpensAndFeedsOutput() {
        val channel = FakeChannel()
        val terminal = AgentLoginTerminal("mirror35", "claude-code", channel, session = "abc", openTimeoutMs = 60_000, inputFlushMs = 0, notifyDelayMs = 0)
        var notified = 0
        terminal.addListener { notified++ }
        terminal.start(cols = 40, rows = 10)
        val open = channel.sent.single()
        assertEq("terminal", open["type"])
        assertEq("open", open["action"])
        assertEq("mirror35", open["node"])
        assertEq(40, open["cols"])
        channel.onFrame!!(mapOf("type" to "terminal", "session" to "abc", "from" to "node", "action" to "ready"))
        expect(terminal.live)
        assertEq("Live on mirror35. Complete Claude Code's own sign-in below.", terminal.statusText)
        channel.onFrame!!(mapOf("type" to "terminal", "session" to "abc", "from" to "node", "action" to "output", "data" to B64.encode("Visit https://x.example/login\r\n".toByteArray())))
        assertEq("https://x.example/login", terminal.signInLink)
        assertEq("Visit https://x.example/login", terminal.plainText)
        expect(!terminal.canRestart)
        channel.onFrame!!(mapOf("type" to "terminal", "session" to "other", "from" to "node", "action" to "exit", "code" to 1))
        assertEq(null, terminal.exitCode)
        terminal.input("hi")
        assertEq("input", channel.sent.last()["action"])
        assertEq(B64.encode("hi".toByteArray()), channel.sent.last()["data"])
        channel.onFrame!!(mapOf("type" to "terminal", "session" to "abc", "from" to "node", "action" to "exit", "code" to 0))
        assertEq(0, terminal.exitCode)
        expect(terminal.canRestart)
        expect(notified > 0)
        terminal.close()
        assertEq("close", channel.sent.last()["action"])
        expect(channel.disposed)
    }
}
