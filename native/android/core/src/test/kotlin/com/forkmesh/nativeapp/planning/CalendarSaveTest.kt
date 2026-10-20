package com.forkmesh.nativeapp.planning

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.testing.assertEq
import java.time.Instant
import java.time.ZoneId

class CalendarSaveTest {
    private val utc = ZoneId.of("UTC")
    private fun ms(text: String) = Instant.parse(text).toEpochMilli()

    private fun event(id: String, title: String) = linkedMapOf<String, Any?>(
        "id" to id, "owner" to "alice", "calendar" to "personal", "scope" to "personal",
        "startAt" to ms("2026-09-24T12:00:00Z"), "endAt" to ms("2026-09-24T13:00:00Z"), "title" to title,
    )

    private class LateApi : ApiService(TestSettings()) {
        var list: List<Map<String, Any?>> = emptyList()
        var whileListing: (() -> Unit)? = null
        var saved: Map<String, Any?>? = null

        override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
            if (method == "GET" && path.startsWith("/api/calendar?")) {
                val reply = mapOf("ok" to true, "actor" to "alice", "events" to list)
                whileListing?.let { run -> whileListing = null; run() }
                return reply
            }
            if (method == "POST") return mapOf("ok" to true, "event" to saved)
            return mapOf("ok" to true)
        }
    }

    private fun model(api: ApiService) = CalendarModel(api, CalendarMode.DAY, zone = utc, firstWeekday = 2, clock = { ms("2026-09-24T10:30:00Z") })

    fun testALateListKeepsANewEventAndDropsADeletedOne() {
        val old = "a".repeat(32)
        val fresh = "c".repeat(32)
        val api = LateApi()
        api.list = listOf(event(old, "Old"))
        val model = model(api)
        model.load()
        assertEq(listOf("Old"), model.events.map { it.title })
        api.saved = event(fresh, "New")
        api.whileListing = {
            model.create(model.newDraft().copy(title = "New"))
            model.delete(old)
        }
        model.load()
        assertEq(listOf("New"), model.events.map { it.title })
        api.list = listOf(event(fresh, "New"))
        model.load()
        assertEq(listOf("New"), model.events.map { it.title })
    }

    fun testAnOlderLoadThatFinishesLastIsIgnored() {
        val api = LateApi()
        api.list = listOf(event("a".repeat(32), "Stale"))
        val model = model(api)
        api.whileListing = {
            api.list = listOf(event("b".repeat(32), "Current"))
            model.load()
        }
        model.load()
        assertEq(listOf("Current"), model.events.map { it.title })
    }
}
