package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant
import java.time.ZoneOffset

class HealthModelsTest {
    private class MemoryPrefs : PrefStore {
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

    fun testStepDayBodyRoundsAndClamps() {
        val day = HealthStepDay("2026-10-20", -4, distanceM = 1234.56, floors = 3, activeKcal = 88.04)
        assertEq(mapOf("day" to "2026-10-20", "steps" to 0L, "distanceM" to 1234.6, "floors" to 3L, "activeKcal" to 88.0), day.body)
        val merged = day.merged(HealthStepDay("2026-10-20", 900, distanceM = 10.0, floors = 5))
        assertEq(900, merged.steps)
        assertEq(1234.56, merged.distanceM)
        assertEq(5, merged.floors)
    }

    fun testDates() {
        val zone = ZoneOffset.UTC
        assertEq(listOf("2026-10-18", "2026-10-19", "2026-10-20"), HealthDates.days("2026-10-20", 3))
        assertEq(emptyList<String>(), HealthDates.days("nope", 3))
        val now = Instant.parse("2026-10-20T18:00:00Z")
        assertEq(Instant.parse("2026-10-19T12:00:00Z"), HealthDates.sampleDate("2026-10-19", 0, zone, now))
        assertEq(Instant.parse("2026-10-19T08:30:00Z"), HealthDates.sampleDate("2026-10-19", Instant.parse("2026-10-19T08:30:00Z").toEpochMilli(), zone, now))
        assertEq(Instant.parse("2026-10-20T12:00:00Z"), HealthDates.sampleDate("2026-10-20", 0, zone, now))
        assertEq(null, HealthDates.sampleDate("2026-10-21", 0, zone, now))
    }

    fun testStepsSyncChunksAndRespectsInterval() {
        val prefs = MemoryPrefs()
        val calls = ArrayList<HealthJson?>()
        val call = HealthCall { _, _, body -> calls += body; mapOf("synced" to ((body?.get("days") as List<*>).size).toLong()) }
        val days = (1..70).map { HealthStepDay("2026-08-%02d".format((it % 28) + 1), it) }
        assertEq(0, StepsHealthSync.runIfDue(true, { days }, prefs, call, 1000.0))
        prefs.putBool(HealthPrefs.STEPS, true)
        assertEq(70, StepsHealthSync.runIfDue(true, { days }, prefs, call, 1000.0))
        assertEq(2, calls.size)
        assertEq("android", calls[0]?.get("device"))
        assertEq(0, StepsHealthSync.runIfDue(true, { days }, prefs, call, 1000.0 + 60))
        assertEq(70, StepsHealthSync.runIfDue(true, { days }, prefs, call, 1000.0 + 60, force = true))
        expect(prefs.getDouble(HealthPrefs.STEPS_SYNCED_AT) == 1060.0, "stamps the sync time")
    }
}
