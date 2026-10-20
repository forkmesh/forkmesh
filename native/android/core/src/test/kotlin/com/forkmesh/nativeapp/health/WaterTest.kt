package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant
import java.time.ZoneOffset

class WaterTest {
    private class FakeCall : HealthCall {
        val replies = HashMap<String, HealthJson>()
        val calls = ArrayList<Triple<String, String, HealthJson?>>()

        override fun call(method: String, suffix: String, body: HealthJson?): HealthJson {
            calls += Triple(method, suffix, body)
            return replies["$method $suffix"] ?: replies[method] ?: mapOf("ok" to true)
        }

        fun bodies(method: String, suffix: String): List<HealthJson> = calls.filter { it.first == method && it.second == suffix }.mapNotNull { it.third }
    }

    private val now = Instant.parse("2026-09-16T10:41:00Z")

    private fun overview(units: String = "metric"): HealthJson = mapOf(
        "ok" to true, "installed" to true, "actor" to "alice",
        "profile" to mapOf("goalMl" to 2000L, "units" to units, "unitsPreference" to "auto", "presetsMl" to listOf(250L, 500L, 750L)),
        "today" to mapOf("day" to "2026-09-16", "totalMl" to 550L, "entries" to 2L),
        "entries" to listOf(
            mapOf("id" to "e1", "day" to "2026-09-16", "amountMl" to 250L, "drink" to "water", "source" to "quick", "at" to now.minusSeconds(600).toEpochMilli()),
            mapOf("id" to "e3", "day" to "2026-09-16", "amountMl" to 300L, "drink" to "water", "source" to "health", "externalId" to "HK-1", "at" to now.minusSeconds(600).toEpochMilli()),
        ),
        "days" to listOf(mapOf("day" to "2026-09-16", "totalMl" to 550L, "entries" to 2L)),
        "summary" to mapOf("averageMl" to 1800L, "goalDays" to 12L, "streak" to 2L),
    )

    private fun model(call: FakeCall) = WaterModel(call, null, ZoneOffset.UTC) { now }

    fun testModelFormatsTheDay() {
        val call = FakeCall()
        call.replies["GET ?day=2026-09-16"] = overview()
        val model = model(call)
        model.load()
        expect(model.installed, "installed")
        assertEq("550 ml", model.totalText)
        assertEq("1.45 L to go of 2 L", model.goalLine)
        assertEq(listOf("250 ml", "500 ml", "750 ml"), model.presets.map { it.label })
        assertEq("Water", model.rows.first().title)
        assertEq("10:31 AM · From Apple Health", model.rows.first().subtitle)
        assertEq("Average 1.8 L · goal met on 12 of 30 days · streak 2 days", model.summaryLine)
        assertEq(0.275, model.ratio)
        assertEq("ml", model.unit)
        assertEq(1, model.bars.size)
        assertEq("9/16", model.bars.first().label)
        assertEq("2026-09-16: 550 ml", model.bars.first().caption)
    }

    fun testAddSendsTheServerBody() {
        val call = FakeCall()
        call.replies["GET ?day=2026-09-16"] = overview()
        val model = model(call)
        model.load()
        model.add(500, "water")
        val body = call.bodies("POST", "/entries").first()
        assertEq(500L, body["amountMl"])
        assertEq("quick", body["source"])
        assertEq("water", body["drink"])
        assertEq("2026-09-16", body["day"])
        assertEq(now.toEpochMilli(), body["at"])
        assertEq("Logged 500 ml of water.", model.notice?.message)
    }

    fun testUnitsAndTypedAmounts() {
        assertEq("250 ml", WaterText.amount(250.0, "metric"))
        assertEq("1.5 L", WaterText.amount(1500.0, "metric"))
        assertEq("2 L", WaterText.amount(2000.0, "metric"))
        assertEq("8.0 fl oz", WaterText.amount(237.0, "imperial"))
        assertEq("24 fl oz", WaterText.amount(710.0, "imperial"))
        assertEq("1 cup", WaterText.amount(237.0, "cups"))
        assertEq("8.5 cups", WaterText.amount(2000.0, "cups"))
        val call = FakeCall()
        call.replies["GET ?day=2026-09-16"] = overview("imperial")
        val model = model(call)
        model.load()
        model.addTyped("12", "coffee")
        assertEq(355L, call.bodies("POST", "/entries").first()["amountMl"])
        assertEq("coffee", call.bodies("POST", "/entries").first()["drink"])
        assertEq("manual", call.bodies("POST", "/entries").first()["source"])
        model.addTyped("0", "water")
        assertEq(1, call.bodies("POST", "/entries").size)
        assertEq(true, model.notice?.isError)
    }

    fun testGoalInstallAndDelete() {
        val call = FakeCall()
        call.replies["GET ?day=2026-09-16"] = overview()
        val model = model(call)
        model.load()
        model.saveGoal(2500, "cups")
        val goal = call.bodies("PUT", "/profile").first()
        assertEq(2500L, goal["goalMl"])
        assertEq("cups", goal["unitsPreference"])
        model.delete("e1")
        expect(call.calls.any { it.first == "DELETE" && it.second == "/entries/e1" }, "deletes the entry")
        model.install()
        assertEq(0L, call.bodies("POST", "/install").first()["tzOffsetMinutes"])
    }

    fun testNotInstalledShowsInstall() {
        val call = FakeCall()
        call.replies["GET ?day=2026-09-16"] = mapOf("ok" to true, "installed" to false)
        val model = model(call)
        model.load()
        expect(model.showsInstall, "install panel")
        assertEq(null, model.overview)
        assertEq("No daily goal", model.goalLine)
    }
}
