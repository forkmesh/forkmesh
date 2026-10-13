package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.api.AccountEventLoop
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.EventTicket
import com.forkmesh.nativeapp.api.EventTicketStatus
import com.forkmesh.nativeapp.api.FakeTransport
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.concurrent.CopyOnWriteArrayList

class BackgroundReconnectTest {
    private class TicketApi : ApiService(TestSettings(), FakeTransport { error("no network") }) {
        val calls = CopyOnWriteArrayList<Long>()

        @Volatile
        var status = EventTicketStatus.UNAVAILABLE

        init {
            sessionTokenProvider = { "token" }
        }

        override fun accountEventTicket(): EventTicket {
            calls.add(System.nanoTime())
            return if (status == EventTicketStatus.OK) EventTicket(EventTicketStatus.OK, "t") else EventTicket(status)
        }
    }

    private class Socket : AccountEventLoop.Socket {
        override fun send(text: String): Boolean = true

        override fun close() = Unit
    }

    private fun waitFor(timeoutMs: Long = 3_000, condition: () -> Boolean): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            if (condition()) return true
            Thread.sleep(2)
        }
        return condition()
    }

    fun testTheLoopWaitsWhatTheBackoffSaysBetweenFailedTickets() {
        val api = TicketApi()
        val backoff = ReconnectBackoff(initialMs = 40, maxMs = 160, jitter = 0.0)
        val loop = AccountEventLoop(api, sessionToken = { "token" }, connector = { _, _ -> Socket() }, backoff = backoff)
        try {
            loop.start()
            expect(waitFor { api.calls.size >= 4 })
            val gapsMs = api.calls.zipWithNext { a, b -> (b - a) / 1_000_000 }
            expect(gapsMs[0] >= 35, "first wait ${gapsMs[0]}")
            expect(gapsMs[1] >= 75, "second wait ${gapsMs[1]}")
            expect(gapsMs[2] >= 150, "capped wait ${gapsMs[2]}")
            expect(backoff.failures >= 3)
        } finally {
            loop.dispose()
        }
    }

    fun testAConnectResetsTheBackoff() {
        val api = TicketApi()
        val backoff = ReconnectBackoff(initialMs = 20, maxMs = 80, jitter = 0.0)
        val loop = AccountEventLoop(api, sessionToken = { "token" }, connector = { _, _ -> Socket() }, backoff = backoff)
        try {
            loop.start()
            expect(waitFor { backoff.failures >= 2 })
            api.status = EventTicketStatus.OK
            expect(waitFor { loop.connected })
            assertEq(0, backoff.failures)
        } finally {
            loop.dispose()
        }
    }

    fun testANetworkKickSkipsTheRemainingWait() {
        val api = TicketApi()
        val backoff = ReconnectBackoff(initialMs = 10_000, maxMs = 60_000, jitter = 0.0)
        val loop = AccountEventLoop(api, sessionToken = { "token" }, connector = { _, _ -> Socket() }, backoff = backoff)
        try {
            loop.start()
            expect(waitFor { api.calls.size == 1 && backoff.failures == 1 })
            api.status = EventTicketStatus.OK
            loop.setForeground(false)
            loop.setForeground(true)
            expect(waitFor(1_000) { loop.connected }, "reconnected without serving the 10 s wait")
            assertEq(0, backoff.failures)
        } finally {
            loop.dispose()
        }
    }

    fun testWhilePausedNothingRetries() {
        val api = TicketApi()
        val backoff = ReconnectBackoff(initialMs = 20, maxMs = 20, jitter = 0.0)
        val loop = AccountEventLoop(api, sessionToken = { "token" }, connector = { _, _ -> Socket() }, backoff = backoff)
        try {
            loop.start()
            expect(waitFor { api.calls.size >= 1 })
            loop.setForeground(false)
            expect(loop.awaitIdle())
            val before = api.calls.size
            Thread.sleep(150)
            assertEq(before, api.calls.size)
        } finally {
            loop.dispose()
        }
    }
}
