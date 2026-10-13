package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger

class AccountEventLoopTest {
    private class TicketApi(@Volatile var status: EventTicketStatus) : ApiService(TestSettings(), FakeTransport { error("no network") }) {
        val ticketCalls = AtomicInteger()

        init {
            sessionTokenProvider = { "session-token" }
        }

        override fun accountEventTicket(): EventTicket {
            ticketCalls.incrementAndGet()
            return if (status == EventTicketStatus.OK) EventTicket(EventTicketStatus.OK, "ticket-abc") else EventTicket(status)
        }
    }

    private class FakeSocket : AccountEventLoop.Socket {
        val sent = CopyOnWriteArrayList<String>()

        @Volatile
        var closed = false

        @Volatile
        var failSends = false
        lateinit var events: AccountEventLoop.Events

        fun emit(frame: Any?) = events.onText(Json.stringify(frame))

        fun closeFromServer() = events.onClosed()

        override fun send(text: String): Boolean {
            if (failSends) return false
            sent.add(text)
            return true
        }

        override fun close() {
            closed = true
        }
    }

    private class Connector : AccountEventLoop.Connector {
        val sockets = CopyOnWriteArrayList<FakeSocket>()
        val urls = CopyOnWriteArrayList<String>()

        @Volatile
        var fail = false

        @Volatile
        var onConnect: (FakeSocket) -> Unit = {}

        override fun connect(url: String, events: AccountEventLoop.Events): AccountEventLoop.Socket {
            urls.add(url)
            if (fail) throw java.io.IOException("refused")
            val socket = FakeSocket()
            socket.events = events
            sockets.add(socket)
            onConnect(socket)
            return socket
        }
    }

    private fun waitFor(timeoutMs: Long = 3_000, condition: () -> Boolean): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            if (condition()) return true
            Thread.sleep(5)
        }
        return condition()
    }

    fun testAnEventFrameForTheInboxReachesOnTopic() {
        val api = TicketApi(EventTicketStatus.OK)
        val connector = Connector()
        val topics = CopyOnWriteArrayList<String>()
        val connects = AtomicInteger()
        val loop = AccountEventLoop(api, sessionToken = { "session-token" }, connector = connector)
        loop.onTopic = { topics.add(it) }
        loop.onConnected = { connects.incrementAndGet() }
        try {
            loop.start()
            expect(loop.awaitIdle())
            expect(loop.connected)
            assertEq(1, connects.get())

            val socket = connector.sockets.single()
            socket.emit(mapOf("type" to "hello", "accepts" to listOf("agent-status")))
            socket.emit(mapOf("type" to "event", "topic" to "pings"))
            socket.emit(mapOf("type" to "event", "topic" to "issues"))
            socket.emit(mapOf("type" to "event", "topic" to ""))
            socket.events.onText("not json")
            socket.events.onBinary(Json.stringify(mapOf("type" to "event", "topic" to "direct-messages")).toByteArray())
            expect(loop.awaitIdle())
            assertEq(listOf("pings", "issues", "direct-messages"), topics.toList())
        } finally {
            loop.dispose()
        }
    }

    fun testTheSocketCarriesTheTicketNotTheSessionToken() {
        val connector = Connector()
        val loop = AccountEventLoop(TicketApi(EventTicketStatus.OK), sessionToken = { "session-token" }, connector = connector)
        try {
            loop.start()
            expect(loop.awaitIdle())
            val url = java.net.URI(connector.urls.single())
            assertEq("/api/nodes/events", url.path)
            assertEq("ticket=ticket-abc", url.rawQuery)
            expect(url.scheme == "ws" || url.scheme == "wss")
            expect(!connector.urls.single().contains("session-token"))
        } finally {
            loop.dispose()
        }
    }

    fun testASessionThatProvesNobodyGoesDarkInsteadOfPolling() {
        val api = TicketApi(EventTicketStatus.SIGNED_OUT)
        val connector = Connector()
        val signedOut = AtomicInteger()
        val loop = AccountEventLoop(api, sessionToken = { "session-token" }, connector = connector, initialReconnectDelayMs = 10)
        loop.onSignedOut = { signedOut.incrementAndGet() }
        try {
            loop.start()
            expect(loop.awaitIdle())
            assertEq(1, signedOut.get())
            expect(!loop.connected)
            expect(!loop.started)
            Thread.sleep(60)
            assertEq(1, api.ticketCalls.get())
            assertEq(0, connector.urls.size)
        } finally {
            loop.dispose()
        }
    }

    fun testATransientTicketFailureKeepsTheChannelArmedAndBacksOff() {
        val api = TicketApi(EventTicketStatus.UNAVAILABLE)
        val connector = Connector()
        val loop = AccountEventLoop(api, sessionToken = { "session-token" }, connector = connector, initialReconnectDelayMs = 20, maxReconnectDelayMs = 40)
        try {
            loop.start()
            expect(loop.awaitIdle())
            expect(loop.started)
            expect(!loop.connected)
            expect(waitFor { api.ticketCalls.get() >= 3 })
            api.status = EventTicketStatus.OK
            expect(waitFor { loop.connected })
            assertEq(0, connector.urls.size - 1)
        } finally {
            loop.dispose()
        }
    }

    fun testStopClosesTheSocketAndStopsDeliveringTopics() {
        val connector = Connector()
        val topics = CopyOnWriteArrayList<String>()
        val loop = AccountEventLoop(TicketApi(EventTicketStatus.OK), sessionToken = { "session-token" }, connector = connector)
        loop.onTopic = { topics.add(it) }
        try {
            loop.start()
            expect(loop.awaitIdle())
            expect(loop.connected)
            loop.stop()
            expect(!loop.connected)
            expect(!loop.started)
            val socket = connector.sockets.single()
            socket.emit(mapOf("type" to "event", "topic" to "pings"))
            expect(loop.awaitIdle())
            assertEq(0, topics.size)
            expect(waitFor { socket.closed })
        } finally {
            loop.dispose()
        }
    }

    fun testAServerCloseReconnectsWithACatchUp() {
        val connector = Connector()
        val connects = AtomicInteger()
        val loop = AccountEventLoop(TicketApi(EventTicketStatus.OK), sessionToken = { "session-token" }, connector = connector, initialReconnectDelayMs = 10)
        loop.onConnected = { connects.incrementAndGet() }
        try {
            loop.start()
            expect(loop.awaitIdle())
            connector.sockets.single().closeFromServer()
            expect(waitFor { connects.get() == 2 && loop.connected })
            assertEq(2, connector.sockets.size)
            connector.fail = true
            connector.sockets.last().closeFromServer()
            expect(waitFor { connector.urls.size >= 4 })
            connector.fail = false
            expect(waitFor { loop.connected })
        } finally {
            loop.dispose()
        }
    }

    fun testKeepalivePingsAndAFailedWriteReconnects() {
        val connector = Connector()
        val loop = AccountEventLoop(TicketApi(EventTicketStatus.OK), sessionToken = { "session-token" }, connector = connector, keepaliveMs = 20, initialReconnectDelayMs = 10)
        try {
            loop.start()
            expect(loop.awaitIdle())
            val first = connector.sockets.single()
            expect(waitFor { first.sent.size >= 2 })
            assertEq("""{"type":"ping"}""", first.sent.first())
            first.failSends = true
            expect(waitFor { connector.sockets.size == 2 && loop.connected })
            expect(waitFor { first.closed })
        } finally {
            loop.dispose()
        }
    }

    fun testBackgroundPausesAndForegroundReconnectsAtOnce() {
        val connector = Connector()
        val api = TicketApi(EventTicketStatus.OK)
        val loop = AccountEventLoop(api, sessionToken = { "session-token" }, connector = connector, initialReconnectDelayMs = 60_000)
        try {
            loop.start()
            expect(loop.awaitIdle())
            loop.setForeground(false)
            expect(loop.awaitIdle())
            expect(!loop.connected)
            expect(loop.started)
            expect(waitFor { connector.sockets.single().closed })
            Thread.sleep(40)
            assertEq(1, connector.urls.size)

            loop.setForeground(true)
            expect(loop.awaitIdle())
            expect(loop.connected)
            assertEq(2, connector.urls.size)

            loop.setForeground(true)
            expect(loop.awaitIdle())
            assertEq(2, connector.urls.size)
            assertEq(listOf("""{"type":"ping"}"""), connector.sockets.last().sent.toList())
        } finally {
            loop.dispose()
        }
    }

    fun testRestartFollowsTheSession() {
        val connector = Connector()
        var token = ""
        val api = TicketApi(EventTicketStatus.OK)
        val loop = AccountEventLoop(api, sessionToken = { token }, connector = connector)
        try {
            loop.start()
            expect(loop.awaitIdle())
            expect(!loop.started)
            assertEq(0, api.ticketCalls.get())

            token = "session-token"
            loop.restart()
            expect(loop.awaitIdle())
            expect(loop.connected)

            loop.restart()
            expect(loop.awaitIdle())
            assertEq(2, connector.sockets.size)
            expect(waitFor { connector.sockets.first().closed })

            token = ""
            loop.stop()
            loop.restart()
            expect(loop.awaitIdle())
            expect(!loop.started)
        } finally {
            loop.dispose()
        }
    }

    fun testAFrameArrivingDuringTheConnectIsNotLost() {
        val connector = Connector()
        val topics = CopyOnWriteArrayList<String>()
        connector.onConnect = { socket -> socket.emit(mapOf("type" to "event", "topic" to "pings")) }
        val loop = AccountEventLoop(TicketApi(EventTicketStatus.OK), sessionToken = { "session-token" }, connector = connector)
        loop.onTopic = { topics.add(it) }
        try {
            loop.start()
            expect(loop.awaitIdle())
            assertEq(listOf("pings"), topics.toList())
        } finally {
            loop.dispose()
        }
    }

    fun testDisposeIsFinal() {
        val connector = Connector()
        val loop = AccountEventLoop(TicketApi(EventTicketStatus.OK), sessionToken = { "session-token" }, connector = connector)
        loop.start()
        expect(loop.awaitIdle())
        loop.dispose()
        expect(!loop.connected)
        loop.start()
        loop.restart()
        loop.setForeground(true)
        Thread.sleep(20)
        assertEq(1, connector.urls.size)
    }
}
