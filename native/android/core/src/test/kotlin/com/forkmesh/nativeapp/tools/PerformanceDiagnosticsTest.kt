package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.Locale

class PerformanceDiagnosticsTest {
    private val now = 1_800_000_000_000L

    private fun sample(kind: PerformanceEventKind, ms: Long, agoMs: Long) = PerformanceAlert.Sample(kind, ms, now - agoMs)

    fun testQuietAppIsNeutral() {
        val alert = PerformanceAlert.evaluate(
            listOf(sample(PerformanceEventKind.EventLoop, 249, 10_000), sample(PerformanceEventKind.Operation, 3000, 1000)),
            emptyList(), null, now,
        )
        expect(!alert.detected)
        assertEq("Send diagnostics", alert.label)
    }

    fun testAQuarterSecondFreezeTurnsItRed() {
        val alert = PerformanceAlert.evaluate(
            listOf(sample(PerformanceEventKind.EventLoop, 420, 30_000), sample(PerformanceEventKind.Sync, 300, 60_000)),
            emptyList(), null, now,
        )
        assertEq("The app froze for 0.4 s (2 freezes)", alert.reason)
        assertEq("Slow, send report", alert.label)
    }

    fun testOldOrAlreadySentFreezesDoNotCount() {
        expect(!PerformanceAlert.evaluate(listOf(sample(PerformanceEventKind.EventLoop, 900, 601_000)), emptyList(), null, now).detected)
        expect(!PerformanceAlert.evaluate(listOf(sample(PerformanceEventKind.EventLoop, 900, 60_000)), emptyList(), now - 30_000, now).detected)
    }

    fun testFiveHitchesTurnItRed() {
        val four = (1..4).map { sample(PerformanceEventKind.Frame, 60, it * 10_000L) }
        expect(!PerformanceAlert.evaluate(four, emptyList(), null, now).detected)
        val five = four + sample(PerformanceEventKind.Frame, 60, 100_000)
        assertEq("5 dropped-frame hitches in 10 min", PerformanceAlert.evaluate(five, emptyList(), null, now).reason)
    }

    fun testAnAndroidExitReasonSinceTheLastSendWins() {
        val anr = PerformanceAlert.Diagnostic("The app stopped responding (ANR)", now - 3_600_000)
        assertEq("Android reported: The app stopped responding (ANR)", PerformanceAlert.evaluate(emptyList(), listOf(anr), null, now).reason)
        expect(!PerformanceAlert.evaluate(emptyList(), listOf(anr), now - 60_000, now).detected)
    }

    fun testScrubRemovesSecretsAndContents() {
        val raw = """
            GET https://app.forkmesh.com/api/chat?token=abc123&room=7 200
            Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJqZXR0In0.c2lnbmF0dXJlLXZhbHVl
            api_key=sk-live-1234 password: hunter2
            from jett@forkmesh.com {"text":"meet at 5pm","id":4}
            node 3f9a0c1d2e4b5a6978877665544332211aabbccdd done
            call +1 (415) 555-0100 at 2026-10-13 19:20:15 took 420 ms
            /data/user/0/com.forkmesh.nativeapp/files/stalls.log
        """.trimIndent()
        val clean = DiagnosticsBundle.scrub(raw)
        for (leaked in listOf("abc123", "eyJhbGci", "sk-live-1234", "hunter2", "jett@forkmesh.com", "meet at 5pm",
            "3f9a0c1d2e4b5a6978877665544332211aabbccdd", "555-0100", "com.forkmesh.nativeapp/files")) {
            expect(!clean.contains(leaked), leaked)
        }
        expect(clean.contains("https://app.forkmesh.com/api/chat?[query] 200"))
        expect(clean.contains("\"text\":\"[redacted]\",\"id\":4"))
        expect(clean.contains("2026-10-13 19:20:15 took 420 ms"))
    }

    fun testScrubsConversationAndRoomNames() {
        val raw = """
            relay.send.edit {"type":"edit","conversation":"#layoffs-plan","to":"node-9"}
            stall conversation=#layoffs-plan room=secret-room channel=@pat took 300 ms
        """.trimIndent()
        val clean = DiagnosticsBundle.scrub(raw)
        for (leaked in listOf("layoffs-plan", "secret-room", "@pat", "node-9")) expect(!clean.contains(leaked), leaked)
        expect(clean.contains("took 300 ms"))
    }

    fun testThePerformanceReportIsScrubbed() {
        val report = DiagnosticsBundle.report(
            listOf("screen" to "chat conversation=#layoffs-plan", "note" to "token=abc123def from pat@example.com"),
            null,
            emptyList(),
            emptyList(),
            now,
        )
        for (leaked in listOf("layoffs-plan", "abc123def", "pat@example.com")) expect(!report.contains(leaked), leaked)
    }

    fun testReportAndPromptLeaveLogsOutWhenAsked() {
        val log = AppLog(now = { now })
        log.add(LogCategory.entries.first(), "secret line")
        val telemetry = AppTelemetry(log = log, now = { now }, rssMegabytes = { 100.0 })
        val events = listOf(PerformanceEvent(PerformanceEventKind.EventLoop, "loop", 420_000, now - 5_000, stackTrace = "a.B.c(B.kt:1)\nd.E.f(E.kt:2)"))
        val report = DiagnosticsBundle.report(listOf("app" to "1.4.1+10401"), telemetry, events, emptyList(), now)
        expect(report.contains("eventLoop loop 420ms 5s ago"))
        expect(report.contains("stack: a.B.c(B.kt:1) < d.E.f(E.kt:2)"))
        val without = DiagnosticsBundle.prompt("The app froze for 0.4 s", report, includeLogs = false)
        expect(!without.contains("secret line"))
        expect(without.startsWith("The app detected a performance issue: The app froze for 0.4 s."))
        expect(without.contains("First read ${DiagnosticsBundle.PLAYBOOK}") && without.contains("add this check to it"))
        expect(DiagnosticsBundle.prompt("", report, includeLogs = true).contains("secret line"))
        val long = DiagnosticsBundle.prompt("x", "line\n".repeat(5000), includeLogs = true)
        expect(long.length <= DiagnosticsBundle.MAX_PROMPT_CHARS && long.endsWith("[cut to fit the prompt]"))
    }

    fun testStallsAreGroupedByNameAndByTheScreenThatWasShowing() {
        val utc = java.time.ZoneOffset.UTC
        val noon = java.time.LocalDateTime.of(2026, 10, 17, 12, 0, 0).toInstant(utc).toEpochMilli()
        val events = listOf(
            PerformanceEvent(PerformanceEventKind.Frame, "frame", 354_000, noon + 30_000, mapOf("screen" to "Inbox > Shortcuts")),
            PerformanceEvent(PerformanceEventKind.EventLoop, "ui-isolate blocked", 324_000, noon + 30_000, mapOf("screen" to "Inbox > Shortcuts")),
            PerformanceEvent(PerformanceEventKind.Operation, "api.GET /api/tasks", 2_000_000, noon + 10_000),
            PerformanceEvent(PerformanceEventKind.EventLoop, "ui-isolate blocked", 250_000, noon),
        )
        val report = DiagnosticsBundle.report(emptyList(), null, events, emptyList(), noon + 60_000, utc)
        expect(
            report.contains(
                "## Stalls by screen\n2x     total 678ms max 354ms last 12:00:30 Inbox > Shortcuts\n" +
                    "1x     total 250ms max 250ms last 12:00:00 not recorded\n",
            ),
            report,
        )
        expect(report.contains("## Stalls by name\n2x     total 574ms max 324ms last 12:00:30 eventLoop ui-isolate blocked\n1x     total 354ms max 354ms last 12:00:30 frame\n"), report)
        expect(report.contains("## Slow operations by name\n1x     total 2000ms max 2000ms operation api.GET /api/tasks\n"), report)
    }
}

class AppChangelogTest {
    private val sample = """
        {"schema": 1, "entries": [
          {"version": "1.4.1", "build": "10401", "date": "2026-10-13",
           "sections": [{"title": "Calls", "items": ["Blur your background."]}, {"title": "Empty", "items": []}]},
          {"version": "1.4.0", "build": "10400", "date": "2026-10-01", "sections": [{"title": "Market", "items": ["One tap."]}]}
        ]}
    """.trimIndent()

    fun testParsesAndDropsEmptySections() {
        val log = AppChangelog.parse(sample)
        assertEq(listOf("10401", "10400"), log.entries.map { it.build })
        assertEq(listOf(AppChangelog.Section("Calls", listOf("Blur your background."))), log.entries[0].sections)
        assertEq("Version 1.4.1 (10401)", log.entries[0].heading)
        assertEq(0, AppChangelog.parse("not json").entries.size)
    }

    fun testTheBundledChangelogIsShownAsItIs() {
        assertEq(listOf("10401", "10400"), AppChangelog.load(sample, "1.4.1", "10401").entries.map { it.build })
        assertEq(listOf("10401", "10400"), AppChangelog.load(sample, "1.4.2", "10402").entries.map { it.build })
        assertEq(listOf(AppChangelog.Entry("1.4.1", "10401")), AppChangelog.load("""{"entries": []}""", "1.4.1", "10401").entries)
    }

    fun testSharedEntriesUseTheirTitleOrDate() {
        val shared = """
            {"schema": 1, "entries": [
              {"version": "", "build": "", "date": "2026-10-21", "title": "", "pitch": "",
               "sections": [{"title": "Chat", "items": ["Stick stickers on messages."]}]},
              {"version": "0.7.0", "build": "", "date": "2026-07", "title": "The Living Code City",
               "pitch": "A shared world.", "sections": [{"title": "Highlights", "items": ["Everyone enters the World."]}]}
            ]}
        """.trimIndent()
        val log = AppChangelog.load(shared, "1.4.1", "303")
        assertEq(2, log.entries.size)
        assertEq("October 21, 2026", log.entries[0].formattedDate(Locale.US))
        assertEq("", log.entries[0].detail(Locale.US))
        assertEq("The Living Code City", log.entries[1].heading)
        assertEq("A shared world.", log.entries[1].pitch)
        assertEq("Version 0.7.0 · July 2026", log.entries[1].detail(Locale.US))
    }

    fun testLabelsAndDates() {
        assertEq("Version 1.4.1", AppChangelog.label("1.4.1", ""))
        assertEq("October 13, 2026", AppChangelog.Entry("1", "2", "2026-10-13").formattedDate(Locale.US))
        assertEq("soon", AppChangelog.Entry("1", "2", "soon").formattedDate(Locale.US))
    }
}
