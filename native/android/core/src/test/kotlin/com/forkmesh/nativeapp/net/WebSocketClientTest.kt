package com.forkmesh.nativeapp.net

import com.forkmesh.nativeapp.net.TestWebSocketServer.Companion.OP_BINARY
import com.forkmesh.nativeapp.net.TestWebSocketServer.Companion.OP_CLOSE
import com.forkmesh.nativeapp.net.TestWebSocketServer.Companion.OP_CONTINUATION
import com.forkmesh.nativeapp.net.TestWebSocketServer.Companion.OP_PING
import com.forkmesh.nativeapp.net.TestWebSocketServer.Companion.OP_PONG
import com.forkmesh.nativeapp.net.TestWebSocketServer.Companion.OP_TEXT
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.IOException
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

class WebSocketClientTest {
    private class Closed(val code: Int, val reason: String, val error: Throwable?)

    private class Recorder : WebSocketClient.Listener {
        val texts = LinkedBlockingQueue<String>()
        val binaries = LinkedBlockingQueue<ByteArray>()
        val pings = LinkedBlockingQueue<ByteArray>()
        val pongs = LinkedBlockingQueue<ByteArray>()
        val closes = LinkedBlockingQueue<Closed>()
        val closeCount = AtomicInteger()

        @Volatile
        var onTextHook: ((WebSocketClient, String) -> Unit)? = null

        override fun onText(socket: WebSocketClient, text: String) {
            texts.add(text)
            onTextHook?.invoke(socket, text)
        }

        override fun onBinary(socket: WebSocketClient, data: ByteArray) {
            binaries.add(data)
        }

        override fun onPing(socket: WebSocketClient, payload: ByteArray) {
            pings.add(payload)
        }

        override fun onPong(socket: WebSocketClient, payload: ByteArray) {
            pongs.add(payload)
        }

        override fun onClosed(socket: WebSocketClient, code: Int, reason: String, error: Throwable?) {
            closeCount.incrementAndGet()
            closes.add(Closed(code, reason, error))
        }
    }

    private fun <T> LinkedBlockingQueue<T>.next(timeoutMs: Long = 3_000): T =
        poll(timeoutMs, TimeUnit.MILLISECONDS) ?: throw AssertionError("timed out waiting for an event")

    fun testEchoesTextAndBinary() {
        val serverCloses = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            serverCloses.add(c.echoUntilClose())
        }.use { server ->
            val recorder = Recorder()
            val client = WebSocketClient.connect("${server.baseUrl}/echo", recorder)
            expect(client.isOpen, "open after connect")

            expect(client.sendText("hello"), "sendText")
            assertEq("hello", recorder.texts.next())
            val unicode = "héllo ✓ 😀 ForkMesh"
            client.sendText(unicode)
            assertEq(unicode, recorder.texts.next())
            val medium = "x".repeat(300)
            client.sendText(medium)
            assertEq(medium, recorder.texts.next())
            client.sendText("")
            assertEq("", recorder.texts.next())

            val bytes = ByteArray(256) { it.toByte() }
            expect(client.sendBinary(bytes), "sendBinary")
            assertBytes(bytes, recorder.binaries.next())
            val large = ByteArray(70_000) { (it * 31).toByte() }
            client.sendBinary(large)
            assertBytes(large, recorder.binaries.next())

            client.close()
            assertEq(1000, serverCloses.next().closeCode)
            val closed = recorder.closes.next()
            assertEq(1000, closed.code)
            expect(closed.error == null, "clean close has no error")
        }
    }

    fun testHeadersAndSubprotocolReachTheServer() {
        val requests = LinkedBlockingQueue<TestWebSocketServer.Connection>()
        TestWebSocketServer { c ->
            c.readRequest()
            requests.add(c)
            c.accept(protocol = if (c.header("Sec-WebSocket-Protocol") != null) "chat.v1" else null)
            c.echoUntilClose()
        }.use { server ->
            val options = WebSocketClient.Options()
                .header("Authorization", "Bearer abc")
                .header("X-ForkMesh-Test", "yes")
            options.protocols = listOf("chat.v2", "chat.v1")
            val client = WebSocketClient.connect("${server.baseUrl}/api/nodes/events?ticket=t%2B1", Recorder(), options)

            val request = requests.next()
            assertEq("GET /api/nodes/events?ticket=t%2B1 HTTP/1.1", request.requestLine)
            assertEq("127.0.0.1:${server.port}", request.header("Host"))
            assertEq("websocket", request.header("Upgrade"))
            assertEq("Upgrade", request.header("Connection"))
            assertEq("13", request.header("Sec-WebSocket-Version"))
            assertEq(24, request.header("Sec-WebSocket-Key")?.length)
            assertEq("Bearer abc", request.header("Authorization"))
            assertEq("yes", request.header("X-ForkMesh-Test"))
            assertEq("chat.v2, chat.v1", request.header("Sec-WebSocket-Protocol"))
            assertEq("chat.v1", client.protocol)
            assertEq("websocket", client.responseHeaders.entries.first { it.key.equals("upgrade", true) }.value.first())
            client.close()

            val bare = WebSocketClient.connect("http://127.0.0.1:${server.port}", Recorder())
            assertEq("GET / HTTP/1.1", requests.next().requestLine)
            assertEq("", bare.protocol)
            bare.close()
        }
    }

    fun testFrameSentWithTheHandshakeIsDelivered() {
        TestWebSocketServer { c ->
            c.readRequest().accept(trailing = TestWebSocketServer.encodeFrame(OP_TEXT, "early".toByteArray()))
            c.echoUntilClose()
        }.use { server ->
            val recorder = Recorder()
            val client = WebSocketClient.connect(server.baseUrl, recorder)
            assertEq("early", recorder.texts.next())
            client.close()
        }
    }

    fun testServerFragmentedTextIsAssembled() {
        val pongs = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            val bytes = "fragmented ✓".toByteArray()
            val split = bytes.size - 2
            c.sendFrame(OP_TEXT, bytes.copyOfRange(0, 4), fin = false)
            c.sendPing("mid".toByteArray())
            c.sendFrame(OP_CONTINUATION, bytes.copyOfRange(4, split), fin = false)
            c.sendFrame(OP_CONTINUATION, bytes.copyOfRange(split, bytes.size))
            pongs.add(c.readFrame())
            c.echoUntilClose()
        }.use { server ->
            val recorder = Recorder()
            val client = WebSocketClient.connect(server.baseUrl, recorder)
            assertEq("fragmented ✓", recorder.texts.next())
            val pong = pongs.next()
            assertEq(OP_PONG, pong.opcode)
            assertEq("mid", pong.text)
            assertEq("mid", String(recorder.pings.next()))
            client.close()
            expect(recorder.texts.isEmpty(), "one message only")
        }
    }

    fun testClientFragmentSizeSendsContinuationFrames() {
        val messages = LinkedBlockingQueue<List<TestWebSocketServer.Frame>>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            repeat(3) { messages.add(c.readMessageFrames()) }
            c.echoUntilClose()
        }.use { server ->
            val options = WebSocketClient.Options()
            options.fragmentSize = 4
            val client = WebSocketClient.connect(server.baseUrl, Recorder(), options)

            expect(client.sendText("abcdefghij"), "fragmented text")
            val text = messages.next()
            assertEq(listOf(OP_TEXT, OP_CONTINUATION, OP_CONTINUATION), text.map { it.opcode })
            assertEq(listOf(false, false, true), text.map { it.fin })
            assertEq("abcdefghij", text.joinToString("") { it.text })

            client.sendBinary(byteArrayOf(1, 2, 3, 4, 5))
            val binary = messages.next()
            assertEq(listOf(OP_BINARY, OP_CONTINUATION), binary.map { it.opcode })
            assertBytes(byteArrayOf(1, 2, 3, 4, 5), binary[0].payload + binary[1].payload)

            client.sendText("abcd")
            assertEq(1, messages.next().size)
            client.close()
        }
    }

    fun testServerPingIsAnsweredWithPong() {
        val replies = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            c.sendPing("hi".toByteArray())
            replies.add(c.readFrame())
            c.echoUntilClose()
        }.use { server ->
            val recorder = Recorder()
            val client = WebSocketClient.connect(server.baseUrl, recorder)
            val reply = replies.next()
            assertEq(OP_PONG, reply.opcode)
            expect(reply.masked, "pong is masked")
            assertEq("hi", reply.text)
            assertEq("hi", String(recorder.pings.next()))
            client.close()
        }
    }

    fun testClientPingReceivesPong() {
        val pings = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            val ping = c.readFrame()
            pings.add(ping)
            c.sendPong(ping.payload)
            c.echoUntilClose()
        }.use { server ->
            val recorder = Recorder()
            val client = WebSocketClient.connect(server.baseUrl, recorder)
            expect(!client.ping(ByteArray(126)), "a ping payload over 125 bytes is refused")
            expect(client.ping("yo".toByteArray()), "ping")
            val ping = pings.next()
            assertEq(OP_PING, ping.opcode)
            assertEq("yo", ping.text)
            assertEq("yo", String(recorder.pongs.next()))
            client.close()
        }
    }

    fun testServerInitiatedCloseIsEchoed() {
        val echoes = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            if (c.index == 0) c.sendClose(4000, "bye") else c.sendClose(null)
            echoes.add(c.readFrame())
        }.use { server ->
            val recorder = Recorder()
            val client = WebSocketClient.connect(server.baseUrl, recorder)
            val closed = recorder.closes.next()
            assertEq(4000, closed.code)
            assertEq("bye", closed.reason)
            expect(closed.error == null, "handshake close has no error")
            val echo = echoes.next()
            assertEq(OP_CLOSE, echo.opcode)
            assertEq(4000, echo.closeCode)
            expect(client.awaitClosed(2_000), "reader finished")
            expect(!client.isOpen, "closed")
            expect(!client.sendText("late"), "no sends after close")

            val bare = Recorder()
            WebSocketClient.connect(server.baseUrl, bare)
            assertEq(1005, bare.closes.next().code)
            assertEq(0, echoes.next().payload.size)
        }
    }

    fun testClientCloseHandshakeCompletes() {
        val serverCloses = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            serverCloses.add(c.echoUntilClose())
        }.use { server ->
            val recorder = Recorder()
            val options = WebSocketClient.Options()
            options.closeTimeoutMs = 4_000
            val client = WebSocketClient.connect(server.baseUrl, recorder, options)

            val started = System.nanoTime()
            client.close(1000, "done")
            val elapsedMs = (System.nanoTime() - started) / 1_000_000
            expect(elapsedMs < 3_000, "close returned after the handshake, not the timeout ($elapsedMs ms)")

            val received = serverCloses.next()
            assertEq(1000, received.closeCode)
            assertEq("done", received.closeReason)
            val closed = recorder.closes.poll() ?: throw AssertionError("onClosed before close() returned")
            assertEq(1000, closed.code)
            expect(closed.error == null, "no error")

            expect(!client.isOpen, "closed")
            expect(!client.sendText("late"), "sendText after close")
            expect(!client.sendBinary(byteArrayOf(1)), "sendBinary after close")
            expect(!client.ping(), "ping after close")
            expect(client.awaitClosed(0), "reader thread finished")
            client.close()
            client.cancel()
            Thread.sleep(50)
            assertEq(1, recorder.closeCount.get())
        }
    }

    fun testMessageLargerThanTheLimitClosesWith1009() {
        val replies = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            if (c.index == 0) {
                c.sendText("0123456789")
                c.sendText("0123456789A")
            } else {
                c.sendFrame(OP_BINARY, ByteArray(6), fin = false)
                c.sendFrame(OP_CONTINUATION, ByteArray(6))
            }
            replies.add(c.readFrame())
        }.use { server ->
            val options = WebSocketClient.Options()
            options.maxMessageBytes = 10

            val single = Recorder()
            WebSocketClient.connect(server.baseUrl, single, options)
            assertEq("0123456789", single.texts.next())
            val closed = single.closes.next()
            assertEq(1009, closed.code)
            expect(closed.error != null, "oversized message is an error")
            assertEq(1009, replies.next().closeCode)

            val fragmented = Recorder()
            WebSocketClient.connect(server.baseUrl, fragmented, options)
            assertEq(1009, fragmented.closes.next().code)
            assertEq(1009, replies.next().closeCode)
            expect(fragmented.binaries.isEmpty(), "nothing delivered")
        }
    }

    fun testInvalidUtf8ClosesWith1007() {
        val replies = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            c.sendFrame(OP_TEXT, byteArrayOf(0xC3.toByte(), 0x28))
            replies.add(c.readFrame())
        }.use { server ->
            val recorder = Recorder()
            WebSocketClient.connect(server.baseUrl, recorder)
            val closed = recorder.closes.next()
            assertEq(1007, closed.code)
            expect(closed.error != null, "error reported")
            assertEq(1007, replies.next().closeCode)
            expect(recorder.texts.isEmpty(), "nothing delivered")
        }
    }

    fun testProtocolViolationsCloseWith1002() {
        val scripts: List<Pair<String, (TestWebSocketServer.Connection) -> Unit>> = listOf(
            "masked server frame" to { c -> c.sendFrame(OP_TEXT, "hi".toByteArray(), mask = true) },
            "reserved bit" to { c -> c.sendFrame(OP_TEXT, "hi".toByteArray(), rsv = 0x40) },
            "unknown opcode" to { c -> c.sendFrame(0x3, ByteArray(0)) },
            "fragmented control frame" to { c -> c.sendFrame(OP_PING, ByteArray(0), fin = false) },
            "oversized control frame" to { c -> c.sendFrame(OP_PING, ByteArray(126)) },
            "continuation without a message" to { c -> c.sendFrame(OP_CONTINUATION, "x".toByteArray()) },
            "data frame inside a fragmented message" to { c ->
                c.sendFrame(OP_TEXT, "a".toByteArray(), fin = false)
                c.sendFrame(OP_TEXT, "b".toByteArray())
            },
            "one-byte close payload" to { c -> c.sendFrame(OP_CLOSE, byteArrayOf(3)) },
            "reserved close code" to { c -> c.sendClose(1005) },
            "64-bit length with the top bit set" to { c ->
                c.sendRaw(byteArrayOf(0x81.toByte(), 127, 0x80.toByte(), 0, 0, 0, 0, 0, 0, 0))
            },
        )
        for ((name, script) in scripts) {
            val replies = LinkedBlockingQueue<TestWebSocketServer.Frame>()
            TestWebSocketServer { c ->
                c.readRequest().accept()
                script(c)
                replies.add(c.readFrame())
            }.use { server ->
                val recorder = Recorder()
                WebSocketClient.connect(server.baseUrl, recorder)
                val closed = recorder.closes.next()
                assertEq(1002, closed.code, name)
                expect(closed.error != null, "$name: error reported")
                assertEq(1002, replies.next().closeCode, name)
                assertEq(1, recorder.closeCount.get(), name)
            }
        }
    }

    fun testNon101AnswerThrowsWithStatus() {
        TestWebSocketServer { c ->
            c.readRequest()
            c.respond("HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}")
        }.use { server ->
            val error = assertFails<WebSocketException> { WebSocketClient.connect(server.baseUrl, Recorder()) }
            assertEq(401, error.status)
            expect(error.message.orEmpty().contains("401"), "message names the status")
        }
    }

    fun testBadHandshakeAnswersThrow() {
        TestWebSocketServer { c ->
            c.readRequest()
            when (c.index) {
                0 -> c.accept(acceptKey = "d3JvbmcgYWNjZXB0IGtleQ==")
                1 -> c.accept(protocol = "not-offered")
                else -> c.accept(upgrade = null)
            }
        }.use { server ->
            val wrongAccept = assertFails<WebSocketException> { WebSocketClient.connect(server.baseUrl, Recorder()) }
            expect(wrongAccept.message.orEmpty().contains("Accept"), "wrong accept: ${wrongAccept.message}")
            val options = WebSocketClient.Options()
            options.protocols = listOf("chat.v1")
            assertFails<WebSocketException> { WebSocketClient.connect(server.baseUrl, Recorder(), options) }
            assertFails<WebSocketException> { WebSocketClient.connect(server.baseUrl, Recorder()) }
        }
        assertFails<WebSocketException> { WebSocketClient.connect("ftp://127.0.0.1/", Recorder()) }
    }

    fun testConnectionRefusedThrows() {
        val port = ServerSocket(0).use { it.localPort }
        assertFails<IOException> { WebSocketClient.connect("ws://127.0.0.1:$port/", Recorder()) }
    }

    fun testCancelReportsAbnormalClosureOnce() {
        TestWebSocketServer { c ->
            c.readRequest().accept()
            c.readFrame()
        }.use { server ->
            val recorder = Recorder()
            val client = WebSocketClient.connect(server.baseUrl, recorder)
            client.cancel()
            val closed = recorder.closes.next()
            assertEq(1006, closed.code)
            expect(closed.error != null, "abnormal closure carries an error")
            expect(client.awaitClosed(2_000), "reader finished")
            client.cancel()
            client.close()
            expect(!client.isOpen, "not open")
            expect(!client.sendText("late"), "no sends after cancel")
            Thread.sleep(50)
            assertEq(1, recorder.closeCount.get())
        }
    }

    fun testPeerDisconnectWithoutCloseIsAbnormal() {
        TestWebSocketServer { c ->
            c.readRequest().accept()
            c.close()
        }.use { server ->
            val recorder = Recorder()
            WebSocketClient.connect(server.baseUrl, recorder)
            val closed = recorder.closes.next()
            assertEq(1006, closed.code)
            expect(closed.error != null, "error reported")
        }
    }

    fun testCloseFromInsideOnTextDoesNotDeadlock() {
        val serverCloses = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            c.sendText("trigger")
            serverCloses.add(c.echoUntilClose())
        }.use { server ->
            val recorder = Recorder()
            val hookReturned = CountDownLatch(1)
            recorder.onTextHook = { socket, _ ->
                socket.close(1000, "from listener")
                hookReturned.countDown()
            }
            val client = WebSocketClient.connect(server.baseUrl, recorder)
            expect(hookReturned.await(2, TimeUnit.SECONDS), "close() inside onText returned")
            val received = serverCloses.next()
            assertEq(1000, received.closeCode)
            assertEq("from listener", received.closeReason)
            val closed = recorder.closes.next()
            assertEq(1000, closed.code)
            expect(closed.error == null, "clean close")
            expect(client.awaitClosed(2_000), "reader finished")
        }
    }

    fun testCloseTimesOutWhenThePeerNeverAnswers() {
        val received = LinkedBlockingQueue<TestWebSocketServer.Frame>()
        TestWebSocketServer { c ->
            c.readRequest().accept()
            if (c.index == 1) c.sendText("trigger")
            received.add(c.readFrame())
            Thread.sleep(3_000)
        }.use { server ->
            val options = WebSocketClient.Options()
            options.closeTimeoutMs = 300

            val recorder = Recorder()
            val client = WebSocketClient.connect(server.baseUrl, recorder, options)
            val started = System.nanoTime()
            client.close()
            val elapsedMs = (System.nanoTime() - started) / 1_000_000
            expect(elapsedMs < 2_000, "close() gave up after the timeout ($elapsedMs ms)")
            assertEq(1000, received.next().closeCode)
            val closed = recorder.closes.next()
            assertEq(1006, closed.code)
            expect(closed.error != null, "timeout reported")

            val fromListener = Recorder()
            fromListener.onTextHook = { socket, _ -> socket.close() }
            WebSocketClient.connect(server.baseUrl, fromListener, options)
            assertEq(1000, received.next().closeCode)
            assertEq(1006, fromListener.closes.next(2_000).code)
        }
    }

    fun testTargetParsing() {
        val relay = WebSocketTarget.parse("wss://app.forkmesh.com/api/nodes/events?ticket=abc")
        expect(relay.secure, "wss is TLS")
        assertEq("app.forkmesh.com", relay.host)
        assertEq(443, relay.port)
        assertEq("/api/nodes/events?ticket=abc", relay.resource)
        assertEq("app.forkmesh.com", relay.hostHeader)
        expect(!relay.isIpLiteral, "a name is not an IP literal")

        val v6 = WebSocketTarget.parse("ws://[::1]:8787")
        expect(!v6.secure, "ws is plain")
        assertEq("::1", v6.host)
        assertEq(8787, v6.port)
        assertEq("/", v6.resource)
        assertEq("[::1]:8787", v6.hostHeader)
        expect(v6.isIpLiteral, "IPv6 literal")

        assertEq("example.com:8443", WebSocketTarget.parse("https://example.com:8443/p").hostHeader)
        assertEq("example.com", WebSocketTarget.parse("ws://example.com:80/").hostHeader)
        expect(WebSocketTarget.parse("ws://10.0.2.2:8787/ws").isIpLiteral, "IPv4 literal")

        val underscore = WebSocketTarget.parse("ws://my_host:9000/x")
        assertEq("my_host", underscore.host)
        assertEq(9000, underscore.port)

        assertFails<WebSocketException> { WebSocketTarget.parse("ftp://example.com/") }
        assertFails<WebSocketException> { WebSocketTarget.parse("ws:///no-host") }
        assertFails<WebSocketException> { WebSocketTarget.parse("ws://bad host/") }
    }
}
