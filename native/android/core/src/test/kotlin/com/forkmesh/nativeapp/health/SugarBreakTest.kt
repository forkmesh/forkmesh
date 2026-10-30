package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant
import java.time.ZoneOffset

class SugarBreakTest {
    private class FakeApi : HealthCall {
        val replies = HashMap<String, HealthJson>()
        val failures = HashMap<String, Exception>()
        val calls = ArrayList<Triple<String, String, HealthJson?>>()

        override fun call(method: String, suffix: String, body: HealthJson?): HealthJson {
            calls.add(Triple(method, suffix, body))
            failures["$method $suffix"]?.let { throw it }
            return replies[method] ?: mapOf("ok" to true)
        }

        fun bodies(method: String, suffix: String): List<HealthJson?> = calls.filter { it.first == method && it.second == suffix }.map { it.third }
    }

    private class MemoryPrefs : com.forkmesh.nativeapp.api.PrefStore {
        val values = HashMap<String, Any>()
        override fun getString(key: String): String? = values[key] as? String
        override fun putString(key: String, value: String) { values[key] = value }
        override fun getBool(key: String): Boolean? = values[key] as? Boolean
        override fun putBool(key: String, value: Boolean) { values[key] = value }
        override fun getDouble(key: String): Double? = values[key] as? Double
        override fun putDouble(key: String, value: Double) { values[key] = value }
        override fun contains(key: String): Boolean = key in values
        override fun remove(key: String) { values.remove(key) }
    }

    private fun at(day: String, hour: Int = 12): Instant = Instant.parse("${day}T00:00:00Z").plusSeconds(hour * 3600L)

    private fun activeOverview(): HealthJson = linkedMapOf(
        "ok" to true,
        "installed" to true,
        "actor" to "alice",
        "day" to "2026-10-11",
        "profile" to mapOf("defaultLengthDays" to 21, "tzOffsetMinutes" to 0),
        "lengths" to listOf(7, 14, 21, 30, 60, 90),
        "current" to mapOf(
            "id" to "b1", "startDay" to "2026-10-02", "endDay" to "2026-10-31", "lengthDays" to 30,
            "startedAt" to 1, "endedAt" to 0, "endedDay" to "", "status" to "active",
        ),
        "days" to listOf(
            mapOf("day" to "2026-10-09", "status" to "slip", "skips" to 1, "slips" to 1),
            mapOf("day" to "2026-10-10", "status" to "sugar-free", "skips" to 3, "slips" to 0),
            mapOf("day" to "2026-10-11", "status" to "open", "skips" to 0, "slips" to 0),
            mapOf("day" to "2026-10-12", "status" to "upcoming", "skips" to 0, "slips" to 0),
        ),
        "today" to mapOf("day" to "2026-10-11", "skips" to 0, "slips" to 0, "status" to "open", "inBreak" to true),
        "events" to emptyList<Any>(),
        "summary" to mapOf(
            "skips" to 12, "slips" to 1, "sugarFreeDays" to 8, "slipDays" to 1, "openDays" to 1, "streak" to 1,
            "bestStreak" to 6, "dayNumber" to 10, "lengthDays" to 30, "remainingDays" to 20, "percent" to 26,
        ),
        "history" to listOf(
            mapOf(
                "id" to "b0", "startDay" to "2026-09-01", "endDay" to "2026-09-14", "lengthDays" to 14, "status" to "completed",
                "summary" to mapOf("skips" to 20, "sugarFreeDays" to 12, "bestStreak" to 9),
            ),
        ),
    )

    private fun model(api: FakeApi, prefs: MemoryPrefs? = null): SugarBreakModel =
        SugarBreakModel(api, prefs, ZoneOffset.UTC) { at("2026-10-11") }

    fun testNotInstalledShowsTheInstallPanel() {
        val api = FakeApi()
        api.replies["GET"] = mapOf("ok" to true, "installed" to false, "actor" to "alice")
        val sugar = model(api)
        sugar.load()
        expect(sugar.showsInstall)
        expect(!sugar.installed)
        assertEq("?day=2026-10-11", api.calls.first().second)
        api.failures["GET ?day=2026-10-11"] = OrgApiException("plugin_not_installed", "plugin not installed")
        sugar.load()
        expect(sugar.showsInstall)
        sugar.install()
        assertEq(1, api.bodies("POST", "/install").size)
    }

    fun testNoBreakOffersLengthsWithAMonthByDefault() {
        val api = FakeApi()
        api.replies["GET"] = mapOf(
            "ok" to true, "installed" to true, "profile" to mapOf("defaultLengthDays" to 30),
            "lengths" to listOf(7, 14, 21, 30, 60, 90), "current" to null, "days" to emptyList<Any>(),
            "events" to emptyList<Any>(), "history" to emptyList<Any>(),
            "summary" to mapOf("skips" to 0, "sugarFreeDays" to 0, "lengthDays" to 0),
        )
        val sugar = model(api)
        sugar.load()
        expect(sugar.showsStart)
        expect(!sugar.isActive)
        assertEq(30, sugar.selectedLength)
        assertEq(listOf(7, 14, 21, 30, 60, 90), sugar.lengths.map { it.days })
        assertEq("1 month (30 days)", sugar.lengths.first { it.days == 30 }.label)
        assertEq("2 weeks (14 days)", sugar.lengths.first { it.days == 14 }.label)
        assertEq("3 months (90 days)", sugar.lengths.first { it.days == 90 }.label)
        sugar.startBreak()
        val body = api.bodies("POST", "/breaks").first()
        assertEq(30L, body?.get("lengthDays"))
        assertEq("2026-10-11", body?.get("startDay"))
        sugar.startBreak(500)
        assertEq(365L, api.bodies("POST", "/breaks").last()?.get("lengthDays"))
    }

    fun testActiveBreakShowsProgressStatsAndDays() {
        val api = FakeApi()
        api.replies["GET"] = activeOverview()
        val sugar = model(api)
        sugar.load()
        expect(sugar.isActive)
        expect(!sugar.showsStart)
        assertEq(21, sugar.selectedLength)
        assertEq("Day 10 of 30", sugar.headline)
        assertEq("8 of 30 days sugar-free", sugar.progressLine)
        assertEq("20 days left · ends Sat Oct 31", sugar.remainingLine)
        assertEq(8.0 / 30.0, sugar.progressRatio)
        assertEq(SugarBreakModel.EMPTY_TODAY, sugar.todayLine)
        assertEq(listOf("12", "8 of 30", "1 day", "6 days", "20"), sugar.stats.map { it.value })
        val days = sugar.days
        assertEq(listOf(SugarDayStatus.Slip, SugarDayStatus.SugarFree, SugarDayStatus.Open, SugarDayStatus.Upcoming), days.map { it.status })
        assertEq(listOf(9, 10, 11, 12), days.map { it.number })
        assertEq(true, days[2].isToday)
        assertEq("Sat Oct 10, sugar-free, +3", days[1].caption)
        assertEq("Sep 1 – Sep 14", sugar.history.first().title)
        assertEq("12 of 14 days sugar-free · +20 · best streak 9 days", sugar.history.first().detail)
        assertEq("Completed", sugar.history.first().status)
    }

    fun testSkipSlipUndoAndEndEarlyCallTheApi() {
        val api = FakeApi()
        api.replies["GET"] = activeOverview()
        val sugar = model(api)
        sugar.load()
        sugar.skip()
        assertEq("skip", api.bodies("POST", "/events").first()?.get("kind"))
        assertEq("2026-10-11", api.bodies("POST", "/events").first()?.get("day"))
        assertEq(0, sugar.pendingSkips)
        sugar.slip()
        assertEq("slip", api.bodies("POST", "/events").last()?.get("kind"))
        sugar.undo("e1")
        expect(api.calls.any { it.first == "DELETE" && it.second == "/events/e1" })
        sugar.endEarly()
        assertEq(true, api.bodies("PATCH", "/breaks/b1").first()?.get("end"))
        sugar.deleteBreak("b0")
        expect(api.calls.any { it.first == "DELETE" && it.second == "/breaks/b0" })
        api.failures["POST /events"] = OrgApiException("no_active_break", "no active break")
        sugar.skip()
        assertEq("Start a sugar break first.", sugar.notice?.message)
    }

    fun testTodayEventsAreNewestFirst() {
        val api = FakeApi()
        val overview = LinkedHashMap(activeOverview())
        overview["today"] = mapOf("day" to "2026-10-11", "skips" to 2, "slips" to 0, "status" to "sugar-free")
        overview["events"] = listOf(
            mapOf("id" to "e1", "kind" to "skip", "at" to at("2026-10-11", 9).toEpochMilli()),
            mapOf("id" to "e2", "kind" to "skip", "at" to at("2026-10-11", 15).toEpochMilli()),
        )
        api.replies["GET"] = overview
        val sugar = model(api)
        sugar.load()
        assertEq(listOf("e2", "e1"), sugar.events.map { it.id })
        assertEq("3:00 PM", sugar.events.first().time)
        assertEq("+1 Skipped sugar", sugar.events.first().title)
        assertEq("+2 today. Sugar-free so far.", sugar.todayLine)
    }

    fun testFinishedBreakOffersANewOne() {
        val api = FakeApi()
        val overview = LinkedHashMap(activeOverview())
        overview["current"] = mapOf(
            "id" to "b1", "startDay" to "2026-10-02", "endDay" to "2026-10-31", "lengthDays" to 30, "endedDay" to "2026-10-09",
            "status" to "ended",
        )
        api.replies["GET"] = overview
        val sugar = model(api)
        sugar.load()
        expect(sugar.showsStart)
        assertEq("Sugar break ended", sugar.headline)
        assertEq("Oct 2 – ended early on Oct 9", sugar.remainingLine)
        assertEq("You stopped with 8 of 30 days sugar-free and +12 in all. Best streak 6 days.", sugar.resultLine)
    }

    fun testSkipShowsUpAtOnceAsAnOptimisticDay() {
        val api = FakeApi()
        api.replies["GET"] = activeOverview()
        val sugar = model(api)
        sugar.load()
        var seen = ""
        sugar.onChange = { if (sugar.pendingSkips > 0 && seen.isEmpty()) seen = "${sugar.todaySkips}/${sugar.sugarFreeDays}/${sugar.days[2].status}" }
        sugar.skip()
        assertEq("1/9/SugarFree", seen)
        assertEq(8, sugar.sugarFreeDays)
    }

    fun testUninstallClearsThePreference() {
        val api = FakeApi()
        api.replies["GET"] = activeOverview()
        val prefs = MemoryPrefs()
        prefs.putBool(HealthPrefs.SUGAR_BREAK, true)
        val sugar = model(api, prefs)
        sugar.load()
        expect(sugar.uninstall())
        assertEq(1, api.bodies("POST", "/uninstall").size)
        assertEq(false, prefs.getBool(HealthPrefs.SUGAR_BREAK))
    }

    fun testSugarBreakText() {
        assertEq("1 week (7 days)", SugarBreakText.length(7))
        assertEq("3 weeks (21 days)", SugarBreakText.length(21))
        assertEq("2 months (60 days)", SugarBreakText.length(60))
        assertEq("45 days", SugarBreakText.length(45))
        assertEq("1 day", SugarBreakText.days(1))
        assertEq("Dec 5", SugarBreakText.monthDay("2026-12-05"))
        assertEq("nope", SugarBreakText.monthDay("nope"))
    }

    fun testContractCoversTheRestOfAnActiveBreak() {
        val api = FakeApi()
        api.replies["GET"] = activeOverview()
        val sugar = model(api)
        sugar.load()
        val contract = sugar.contract()
        assertEq("2026-10-11", contract.startDay)
        assertEq("2026-10-31", contract.endDay)
        assertEq(21, contract.days)
        assertEq(2, contract.signers)
        assertEq("Oct 11 – Oct 31, 2026", contract.dateLine)
        assertEq("21 days", contract.lengthLine)
        assertEq("We, the undersigned, promise to eat no added sugar from Oct 11 through Oct 31, 2026.", contract.terms)
        assertEq("No sugar for Oct 11 – Oct 31, 2026 (21 days). Sign the contract and go sugar-free with me.", contract.message)
        assertEq("no-sugar-contract-2026-10-11.png", contract.fileName)
    }

    fun testContractWithoutABreakUsesTheSelectedLength() {
        val api = FakeApi()
        api.replies["GET"] = mapOf(
            "ok" to true, "installed" to true, "profile" to mapOf("defaultLengthDays" to 7), "current" to null,
            "days" to emptyList<Any>(), "events" to emptyList<Any>(), "history" to emptyList<Any>(), "summary" to emptyMap<String, Any>(),
        )
        val sugar = model(api)
        sugar.load()
        val contract = sugar.contract(4)
        assertEq("2026-10-11", contract.startDay)
        assertEq("2026-10-17", contract.endDay)
        assertEq(7, contract.days)
        assertEq(4, contract.lines.size)
    }

    fun testContractSignersNamesAndDates() {
        var contract = SugarContract("2026-12-24", "2026-12-20", 0)
        assertEq("2026-12-24", contract.endDay)
        expect(contract.isOneDay)
        assertEq(1, contract.signers)
        assertEq("Dec 24, 2026", contract.dateLine)
        assertEq("1 day", contract.lengthLine)
        assertEq("I, the undersigned, promise to eat no added sugar on Dec 24, 2026.", contract.terms)
        assertEq("No sugar on Dec 24, 2026. Hold me to it.", contract.message)
        contract = contract.withName(0, "  Sam  ").withSigners(3)
        assertEq(listOf("Sam", "Signature", "Signature"), contract.lines.map { it.caption })
        contract = contract.withName(2, "x".repeat(99))
        assertEq(SugarContract.MAX_NAME_LENGTH, contract.names[2].length)
        contract = contract.withSigners(40)
        assertEq(12, contract.signers)
        contract = contract.withSigners(2)
        assertEq(listOf("  Sam  ", ""), contract.names)
        contract = contract.withEnd("2027-01-02")
        assertEq(10, contract.days)
        assertEq("Dec 24, 2026 – Jan 2, 2027", contract.dateLine)
        contract = contract.withStart("2027-01-05")
        assertEq("2027-01-05", contract.endDay)
        contract = contract.withEnd("2027-01-01")
        assertEq("2027-01-05", contract.endDay)
    }
}
