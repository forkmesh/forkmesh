package com.forkmesh.nativeapp.genie

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.market.CrmContact
import com.forkmesh.nativeapp.market.PackagesClient
import com.forkmesh.nativeapp.market.PackagesTransport
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneId
import java.time.ZoneOffset

class GenieAutoSendTest {
    private val sam = "a".repeat(32)
    private val start = 1_790_000_000.0
    private val utc: () -> ZoneId = { ZoneOffset.UTC }

    private val pick = GenieOutreachPick(
        "acme", CrmContact(id = sam, name = "Sam Dev", email = "sam@example.com"), "Customers", GenieOutreachChannel.Email, "alice",
    )

    private val sent = ApiJsonResult(
        201,
        mapOf(
            "ok" to true, "subject" to "Quick idea", "text" to "Hi Sam,\n\nhttps://forkmesh.com/r/alice", "via" to "zoho",
            "fromEmail" to "founders@acme.example", "model" to "host-model", "sentToday" to 7, "dailyLimit" to 100,
            "email" to mapOf("id" to "e".repeat(32), "to" to "sam@example.com"),
        ),
    )

    private class FakeTransport(val replies: MutableList<ApiJsonResult>) : PackagesTransport {
        val requests = mutableListOf<Triple<String, String, Map<String, Any?>?>>()

        override fun send(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult {
            requests.add(Triple(method, path, body?.toMap()))
            return if (replies.isEmpty()) ApiJsonResult(500, mapOf("error" to "unexpected")) else replies.removeAt(0)
        }
    }

    fun testPaceSendsOneEveryFourteenMinutesUpToAHundredADay() {
        val store = MemoryPrefStore()
        val pace = GenieAutoSendPace(store, utc)
        assertEq(864.0, GenieAutoSendPace.EVERY)
        expect(!pace.due(start))
        pace.setOn(true)
        expect(pace.due(start))
        pace.sent(start)
        assertEq(1, pace.sentToday(start))
        expect(!pace.due(start + 863))
        assertEq(64.0, pace.wait(start + 800))
        expect(pace.due(start + 864))
        val again = GenieAutoSendPace(store, utc)
        expect(again.on)
        assertEq(1, again.sentToday(start))
        assertEq(0, again.sentToday(start + 24 * 60 * 60))
    }

    fun testAFullDayWaitsForMidnight() {
        val pace = GenieAutoSendPace(MemoryPrefStore(), utc)
        pace.setOn(true)
        val midnight = Math.floor(start / 86400) * 86400
        repeat(GenieAutoSendPace.PER_DAY) { pace.sent(midnight + it) }
        val later = midnight + 2 * 60 * 60
        expect(!pace.due(later))
        assertEq(22 * 60 * 60.0, pace.wait(later))
        expect(pace.due(midnight + 24 * 60 * 60))
    }

    fun testCountdownRunsFromTheLastSendToTheNext() {
        val pace = GenieAutoSendPace(MemoryPrefStore(), utc)
        assertEq(null, pace.countdown(start))
        pace.setOn(true)
        assertEq(null, pace.countdown(start))
        pace.sent(start)
        val window = pace.countdown(start + 200)
        assertEq(start, window?.start)
        assertEq(start + 864, window?.end)
        assertEq(1 - 200.0 / 864, window?.left(start + 200))
        assertEq(null, pace.countdown(start + 864))
        pace.setOn(false)
        assertEq(null, pace.countdown(start + 200))
    }

    fun testOutcomesSkipTheContactOrPauseTheSender() {
        val outcome = GenieAutoSend.outcome(sent, pick) as GenieAutoSendOutcome.Sent
        assertEq("Quick idea", outcome.receipt.subject)
        assertEq("sam@example.com", outcome.receipt.to)
        assertEq("Sam Dev", outcome.receipt.contactName)
        assertEq("Zoho Mail", outcome.receipt.viaLabel)
        assertEq(7, outcome.receipt.sentToday)
        assertEq(GenieAutoSendOutcome.Skip("recently_emailed"), GenieAutoSend.outcome(ApiJsonResult(409, mapOf("error" to "recently_emailed")), pick))
        assertEq(
            GenieAutoSendOutcome.Pause("The CRM's limit of 100 emails a day is reached."),
            GenieAutoSend.outcome(ApiJsonResult(429, mapOf("error" to "daily_limit_reached", "dailyLimit" to 100)), pick),
        )
        assertEq(
            GenieAutoSendOutcome.Pause("Reconnect Zoho Mail."),
            GenieAutoSend.outcome(ApiJsonResult(409, mapOf("error" to "zoho_send_not_allowed", "detail" to "Reconnect Zoho Mail.")), pick),
        )
    }

    fun testAgentRepliesParseIntoSubjectAndBody() {
        val parsed = GenieAutoSend.parseReply("Here it is:\n**Subject:** \"Quick idea\"\n\nHi Sam,\n\nThanks\n")
        assertEq("Quick idea", parsed?.first)
        assertEq("Hi Sam,\n\nThanks", parsed?.second)
        assertEq(null, GenieAutoSend.parseReply("Hi Sam, no subject here"))
    }

    fun testWithoutAServerModelThePhoneWritesAndSendsAgain() {
        val transport = FakeTransport(mutableListOf(ApiJsonResult(503, mapOf("error" to "ai_unavailable", "prompt" to "Write to Sam.")), sent))
        val crm = GenieCrm(PackagesClient(transport))
        val seen = mutableListOf<String>()
        val writer = GenieOutreachWriter("Phone model", 5.0) { messages ->
            seen.add(messages.single().content)
            "Subject: Hello Sam\n\nA note from the phone."
        }
        val stages = mutableListOf<GenieAutoSendStage>()
        val outcome = crm.autoSend(pick, listOf(writer)) { stages.add(it) }
        expect(outcome is GenieAutoSendOutcome.Sent)
        assertEq(listOf("Write to Sam."), seen)
        assertEq(listOf(GenieAutoSendStage.Writing, GenieAutoSendStage.Sending, GenieAutoSendStage.Sent), stages)
        assertEq(2, transport.requests.size)
        assertEq("/api/crm/acme/outreach/auto", transport.requests[1].second)
        assertEq(mapOf("contactId" to sam, "subject" to "Hello Sam", "text" to "A note from the phone.", "model" to "Phone model"), transport.requests[1].third)
    }

    fun testWithoutAnyWriterThePauseSaysWhatIsMissing() {
        val unavailable = ApiJsonResult(503, mapOf("error" to "ai_unavailable", "prompt" to "Write to Sam."))
        val alone = GenieCrm(PackagesClient(FakeTransport(mutableListOf(unavailable)))).autoSend(pick, emptyList())
        assertEq(GenieAutoSendOutcome.Pause(GenieAutoSend.NO_WRITER), alone)
        val failing = GenieOutreachWriter("Desktop", 5.0) { "Sorry, I can't." }
        val tried = GenieCrm(PackagesClient(FakeTransport(mutableListOf(unavailable)))).autoSend(pick, listOf(failing))
        assertEq(GenieAutoSendOutcome.Pause(GenieAutoSend.UNWRITTEN), tried)
    }

    fun testTheDesktopWriterAsksTheDesktopsAgentWithTheWholePrompt() {
        val asked = mutableListOf<Triple<String, Map<String, Any?>, Long>>()
        val writer = GenieDesktopWriter.make("Studio") { method, params, timeout ->
            asked.add(Triple(method, params, timeout))
            mapOf("text" to "Subject: Hi\n\nBody", "agent" to "claude-code")
        }
        assertEq("Claude Code or Codex on Studio", writer.name)
        assertEq(300.0, writer.timeoutSeconds)
        assertEq("Subject: Hi\n\nBody", writer.run(listOf(GenieMessage.system("Be brief."), GenieMessage.user("Write to Sam."))))
        assertEq("agents.write", asked.single().first)
        assertEq(mapOf<String, Any?>("prompt" to "Be brief.\n\nWrite to Sam."), asked.single().second)
        assertEq(300_000L, asked.single().third)
        assertEq("Claude Code or Codex on your desktop", GenieDesktopWriter.name(" "))
    }

    fun testAServerWrittenEmailGoesFromWritingStraightToSent() {
        val transport = FakeTransport(mutableListOf(sent))
        val stages = mutableListOf<GenieAutoSendStage>()
        GenieCrm(PackagesClient(transport)).autoSend(pick, emptyList()) { stages.add(it) }
        assertEq(listOf(GenieAutoSendStage.Writing, GenieAutoSendStage.Sent), stages)
        assertEq(mapOf("contactId" to sam), transport.requests.single().third)
    }

    fun testRatingsPostTheStarsAndTheNote() {
        val transport = FakeTransport(mutableListOf(ApiJsonResult(200, mapOf("ok" to true))))
        val receipt = (GenieAutoSend.outcome(sent, pick) as GenieAutoSendOutcome.Sent).receipt
        GenieCrm(PackagesClient(transport)).rate(receipt, 4, "Shorter please")
        assertEq("/api/crm/acme/outreach/rate", transport.requests.single().second)
        assertEq(mapOf("emailId" to "e".repeat(32), "rating" to 4, "note" to "Shorter please"), transport.requests.single().third)
    }

    fun testZohoOnlySkipsOrganizationsWithoutZoho() {
        val contact = mapOf("id" to sam, "name" to "Sam Dev", "email" to "sam@example.com", "flow" to "customers", "stage" to "lead")
        val transport = object : PackagesTransport {
            override fun send(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult = when (path) {
                "/api/crm" -> ApiJsonResult(200, mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true), mapOf("name" to "beta", "installed" to true))))
                "/api/crm/acme" -> ApiJsonResult(200, mapOf("organization" to "acme", "contacts" to listOf(contact)))
                "/api/crm/beta" -> ApiJsonResult(200, mapOf("organization" to "beta", "contacts" to listOf(contact), "settings" to mapOf("zoho" to mapOf("connected" to true))))
                else -> ApiJsonResult(404, mapOf("error" to "not_found"))
            }
        }
        val crm = GenieCrm(PackagesClient(transport)) { "sam" }
        assertEq("acme", crm.best(emptySet())?.organization)
        assertEq("beta", crm.best(emptySet(), GenieOutreachChannel.Email, zohoOnly = true)?.organization)
        assertEq("sam", crm.best(emptySet())?.sender)
    }
}
