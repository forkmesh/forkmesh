package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

class CallDiagnosticsTest {
    private val fixture: Map<String, Any?> by lazy { Json.parseObject(resourceText("chat/call-diagnostics-parity.json")) }

    private fun num(value: Any?): Double? = (value as? Number)?.toDouble()

    private fun sample(entry: Map<String, Any?>): CallQualitySample = CallQualitySample(
        live = true,
        at = num(entry["at"]) ?: 0.0,
        rttMs = num(entry["rttMs"]),
        lossPct = num(entry["lossPct"]),
        jitterMs = num(entry["jitterMs"]),
        sendKbps = num(entry["sendKbps"]),
        recvKbps = num(entry["recvKbps"]),
        inWidth = num(entry["inWidth"])?.toInt(),
        inHeight = num(entry["inHeight"])?.toInt(),
        inFps = num(entry["inFps"]),
        outWidth = num(entry["outWidth"])?.toInt(),
        outHeight = num(entry["outHeight"])?.toInt(),
        outFps = num(entry["outFps"]),
        limit = entry.str("limit"),
        faceFps = num(entry["faceFps"]),
        faceMs = num(entry["faceMs"]),
        faceLevel = num(entry["faceLevel"]),
    )

    private fun memory(value: Any?): CallDiagnosticsMemory = CallDiagnosticsMemory.parse(Json.stringify(value))

    private fun environment(entry: Map<String, Any?>) = CallDiagnosticsEnvironment(
        userAgent = entry.str("userAgent"),
        cores = entry.int("cores"),
        memory = entry.double("memory"),
        connection = entry.str("connection"),
    )

    private fun same(expected: Any?, actual: Double?, label: String) {
        val want = num(expected)
        if (want == null) {
            assertEq(null, actual, label)
            return
        }
        expect(actual != null && Math.abs(actual - want) < 1e-9, "$label: expected $want but was $actual")
    }

    fun testTheFixtureCoversTheWebModule() {
        val cases = fixture.maps("cases")
        expect(cases.size >= 15, "run node tools/call_diagnostics_parity.mjs")
        assertEq(setOf("unknown", "ok", "warn", "bad"), cases.map { it.obj("diagnosis").str("severity") }.toSet())
        val ids = cases.flatMap { case -> case.obj("diagnosis").maps("issues").map { it.str("id") } }.toSet()
        assertEq(CallDiagnostics.KINDS.keys, ids)
        val constants = fixture.obj("constants")
        assertEq(CallDiagnostics.WINDOW, constants.int("window"))
        assertEq(CallDiagnostics.MIN_SAMPLES, constants.int("minSamples"))
        assertEq(CallDiagnostics.REPORTS, constants.int("reports"))
        assertEq(CallDiagnostics.NOTES, constants.int("notes"))
        assertEq(CallDiagnostics.NOTE_CHARS, constants.int("noteChars"))
        assertEq(CallDiagnostics.VIDEO_MIN_FPS, constants.double("videoMinFps"))
        assertEq(CallDiagnostics.VIDEO_MIN_HEIGHT, constants.double("videoMinHeight"))
        assertEq(CallDiagnostics.VIDEO_MIN_KBPS, constants.double("videoMinKbps"))
        assertEq(CallDiagnostics.FACE_SLOW_MS, constants.double("faceSlowMs"))
        assertEq(CallDiagnostics.FACE_BAD_MS, constants.double("faceBadMs"))
        assertEq(CallDiagnostics.KEY, constants.str("key"))
    }

    fun testDiagnosesMatchTheWebModule() {
        val memories = fixture.list("memories")
        val environments = fixture.maps("environments").map(::environment)
        assertEq(3, memories.size)
        for (entry in fixture.maps("cases")) {
            val name = entry.str("name")
            val options = entry.obj("options")
            val samples = entry.maps("samples").map(::sample)
            val diagnosis = CallDiagnostics.diagnose(
                samples,
                video = options.bool("video"),
                sending = options.optBool("sending") ?: true,
                window = options.int("window", CallDiagnostics.WINDOW),
            )
            val expected = entry.obj("diagnosis")
            same(expected["at"], diagnosis.at, "$name at")
            assertEq(expected.str("severity"), diagnosis.severity.wire, name)
            val issues = expected.maps("issues")
            assertEq(issues.map { it.str("id") }, diagnosis.issues.map { it.id }, name)
            for ((issue, want) in diagnosis.issues.zip(issues)) {
                assertEq(want.str("severity"), issue.severity.wire, "$name ${issue.id}")
                assertEq(want.str("title"), issue.title)
                assertEq(want.str("area"), issue.area)
                assertEq(want.str("cause"), issue.cause)
                assertEq(want.str("detail"), issue.detail, "$name ${issue.id}")
                assertEq(want.maps("fixes").map { it.str("id") }, issue.fixes.map { it.id })
                assertEq(want.maps("fixes").map { it.str("text") }, issue.fixes.map { it.text })
            }
            val metrics = expected.obj("metrics")
            assertEq(metrics.int("samples"), diagnosis.metrics.samples, name)
            for ((key, value) in diagnosis.metrics.entries) same(metrics[key], value, "$name $key")
            assertEq(entry.strings("lines"), CallDiagnostics.describe(diagnosis.metrics), name)
            entry.list("ranked").forEachIndexed { index, ranked ->
                val lists = ranked as? List<*> ?: emptyList<Any?>()
                for ((issue, list) in diagnosis.issues.zip(lists)) {
                    val want = jsonMaps(list)
                    val mine = CallDiagnostics.rankFixes(issue, memory(memories[index]))
                    assertEq(want.map { it.str("id") }, mine.map { it.id }, "$name ${issue.id} memory $index")
                    assertEq(want.map { it.int("worked") }, mine.map { it.worked })
                    assertEq(want.map { it.int("failed") }, mine.map { it.failed })
                }
            }
            entry.strings("prompts").forEachIndexed { index, prompt ->
                val mine = CallDiagnostics.prompt(diagnosis, memory(memories[index]), environments[index % environments.size])
                assertEq(prompt, mine, "$name prompt $index")
            }
            val remembered = memory(memories[1])
            CallDiagnostics.rememberReport(remembered, diagnosis, now = -1.0)
            val report = entry.obj("remembered")
            val last = remembered.reports.last()
            if (diagnosis.at != null) same(report["at"], last.at, "$name remembered at")
            assertEq(report.strings("issues"), last.issues, name)
            val wanted = report.obj("metrics")
            assertEq(wanted.keys, last.metrics.keys, name)
            for (key in wanted.keys) same(wanted[key], num(last.metrics[key]), "$name remembered $key")
        }
    }

    fun testLessonsAndMemoryMatchTheWebModule() {
        val memory = memory(fixture.list("memories")[1])
        for (entry in fixture.maps("notes")) {
            val ok = CallDiagnostics.rememberNote(memory, entry.str("text"))
            assertEq(entry.bool("ok"), ok, entry.str("text"))
            assertEq(entry.strings("notes"), memory.notes.toList(), entry.str("text"))
        }
        val crowded = CallDiagnosticsMemory()
        for (index in 0 until 25) {
            val samples = (0 until 3).map { CallQualitySample(at = (index * 10 + it * 1000).toDouble(), rttMs = 400.0) }
            CallDiagnostics.rememberReport(crowded, CallDiagnostics.diagnose(samples))
        }
        for (index in 0 until 10) CallDiagnostics.rememberNote(crowded, "note $index")
        val expected = fixture.obj("crowded")
        assertEq(expected.int("reports"), crowded.reports.size)
        assertEq(expected.double("firstAt"), crowded.reports.first().at)
        assertEq(expected.strings("notes"), crowded.notes.toList())
        val stored = CallDiagnosticsMemory.parse(fixture.str("stored"))
        assertEq(2, stored.notes.size)
        assertEq(CallFixRecord(worked = 2, failed = 0), stored.record("latency", "vpn"))
        assertEq(listOf(1.0, 2.0, 27000.0), stored.reports.map { it.at })
        assertEq(stored, CallDiagnosticsMemory.parse(stored.json))
        assertEq(CallDiagnosticsMemory(), CallDiagnosticsMemory.parse("not json"))
        assertEq(CallDiagnosticsMemory(), CallDiagnosticsMemory.parse(null))
    }

    fun testNumbersFormatLikeJavaScript() {
        assertEq("3.3", CallDiagnostics.fixed(3.25, 1))
        assertEq("0.13", CallDiagnostics.fixed(0.125, 2))
        assertEq("1.00", CallDiagnostics.fixed(1.005, 2))
        assertEq("10.0", CallDiagnostics.fixed(9.96, 1))
        assertEq("100", CallDiagnostics.fixed(99.5, 0))
        assertEq("0.0", CallDiagnostics.fixed(0.0, 1))
        assertEq("-0.0", CallDiagnostics.fixed(-0.01, 1))
        assertEq(301, CallDiagnostics.round(300.5))
        assertEq(2, CallDiagnostics.round(2.4999))
        assertEq("150", CallDiagnostics.text(150.0))
        assertEq("5.5", CallDiagnostics.text(5.5))
        assertEq("1.3 Mb/s", CallDiagnostics.formatKbps(1250.0))
        assertEq("1000 kb/s", CallDiagnostics.formatKbps(999.6))
        assertEq("3.3% loss", CallQuality.describeNetwork(CallQualitySample(live = true, rttMs = 10.0, lossPct = 3.25)).substringAfter(" · "))
    }

    fun testFixesThatClearAProblemAreRememberedAsWorking() {
        val bad = (0 until 4).map { CallQualitySample(at = it.toDouble(), rttMs = 600.0, lossPct = 0.0, jitterMs = 4.0) }
        val diagnosis = CallDiagnostics.diagnose(bad)
        assertEq(listOf("latency"), diagnosis.issues.map { it.id })
        expect(diagnosis.problem)
        assertEq("1 problem found: high latency", diagnosis.summary)
        assertEq("1 problem found", diagnosis.heading)
        val memory = CallDiagnosticsMemory()
        val tracker = CallDiagnosticsTracker()
        tracker.opened(diagnosis, memory)
        tracker.opened(diagnosis, memory)
        assertEq(1, memory.reports.size)
        assertEq(listOf("wired"), tracker.pending.map { it.second })
        expect(!tracker.refreshed(CallDiagnostics.diagnose(emptyList()), memory))
        expect(!tracker.refreshed(diagnosis, memory))
        val good = (0 until 4).map { CallQualitySample(at = (10 + it).toDouble(), rttMs = 40.0, lossPct = 0.0, jitterMs = 4.0) }
        val cleared = CallDiagnostics.diagnose(good)
        assertEq("No problems found", cleared.summary)
        expect(tracker.refreshed(cleared, memory))
        assertEq(CallFixRecord(worked = 1, failed = 0), memory.record("latency", "wired"))
        expect(tracker.pending.isEmpty())
        tracker.opened(diagnosis, memory)
        assertEq(listOf("wired"), tracker.pending.map { it.second })
        expect(tracker.finished(memory))
        assertEq(CallFixRecord(worked = 1, failed = 1), memory.record("latency", "wired"))
        expect(!tracker.finished(memory))
        assertEq(
            "Use a wired connection, or move closer to the Wi-Fi router. (worked here before)",
            CallDiagnostics.fixLabel(CallDiagnostics.rankFixes(diagnosis.issues[0], memory)[0]),
        )
        assertEq("Checking the call", CallDiagnosis.EMPTY.summary)
        assertEq("Not enough measurements yet", CallDiagnosis.EMPTY.heading)
        assertEq("Prompt for an AI assistant · v1, learned from 0 earlier reports", CallDiagnostics.promptLabel(diagnosis, memory))
    }

    fun testAQualityEventCarriesTheFaceFilterTimings() {
        val event = CallQuality.parse(
            Json.parseObject("""{"event":"quality","live":true,"rttMs":40,"faceMs":42.5,"faceFps":20,"faceLevel":2}"""),
            at = 12.0,
        ) ?: error("not a quality event")
        assertEq(12.0, event.sample.at)
        assertEq(42.5, event.sample.faceMs)
        assertEq(20.0, event.sample.faceFps)
        assertEq(2.0, event.sample.faceLevel)
    }
}
