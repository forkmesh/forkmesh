package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CrmParityTest {
    private val hexA = "a".repeat(32)
    private val hexB = "b".repeat(32)
    private val hexC = "c".repeat(32)

    private fun summary(): Map<String, Any?> = mapOf(
        "organization" to "acme", "role" to "owner", "canManageSettings" to true,
        "contacts" to listOf(
            mapOf(
                "id" to hexA, "name" to "Ada", "email" to "ada@x.test", "flow" to "customers", "stage" to "lead", "lastContactedAt" to 5L,
                "notes" to "Met at a meetup.\nDRAFT, not sent.\nSubject: Hello\nHi Ada,\nwant a demo?\n\nOlder note",
            ),
            mapOf(
                "id" to hexB, "name" to "Bob", "mastodon" to "@bob@social.test", "flow" to "customers", "stage" to "lead",
                "notes" to "DRAFT, not sent. Mastodon reply\nLoved your post!",
            ),
            mapOf("id" to hexC, "name" to "Cy", "flow" to "customers", "stage" to "lead", "notes" to "Says DRAFT, not sent. mid-line, so no draft"),
        ),
        "emails" to listOf(mapOf("id" to "m1", "contactId" to hexB, "provider" to "mastodon", "providerUrl" to "https://social.test/@acme/1234567", "status" to "sent")),
        "settings" to mapOf(
            "flows" to listOf(mapOf("id" to "customers", "name" to "Customers", "stages" to listOf("lead"))),
            "removedColumns" to listOf("phone", "twitter"),
        ),
        "revision" to "r1",
    )

    fun testNextContactSplitsReachOutFromFollowUp() {
        val day = 86_400_000L
        val now = 1_800_000_000_000L
        val contacts = mutableListOf(
            CrmContact(id = "fresh", email = "f@x.test", createdAt = now - 3 * day),
            CrmContact(id = "quiet", email = "q@x.test", lastContactedAt = now - 40 * day),
            CrmContact(id = "recent", email = "r@x.test", lastContactedAt = now - 2 * day),
            CrmContact(id = "won", email = "w@x.test", stage = "customer", lastContactedAt = now - 90 * day),
            CrmContact(id = "unreachable", lastContactedAt = now - 90 * day),
        )
        var follow = CrmNextContact.pick(CrmNextContact.Mode.FOLLOW_UP, contacts, emptyList(), now = now)
        assertEq("quiet", follow.contact?.id)
        assertEq(1, follow.due)
        expect("40 days ago" in follow.reason, "the reason says how long it has been")
        var reach = CrmNextContact.pick(CrmNextContact.Mode.REACH_OUT, contacts, emptyList(), now = now)
        assertEq("fresh", reach.contact?.id)
        contacts += listOf(
            CrmContact(id = "ranked", mastodon = "@r@mastodon.social", fields = mapOf("priority_rank" to "4")),
            CrmContact(id = "ranked-better", email = "rb@x.test", fields = mapOf("rank" to "2", "coding_fit_score" to "70")),
            CrmContact(id = "valued", email = "v@x.test", value = 5000),
            CrmContact(id = "lost", email = "l@x.test", stage = "lost"),
        )
        reach = CrmNextContact.pick(CrmNextContact.Mode.REACH_OUT, contacts, emptyList(), now = now)
        assertEq("valued", reach.contact?.id)
        assertEq(4, reach.due)
        reach = CrmNextContact.pick(CrmNextContact.Mode.REACH_OUT, contacts, emptyList(), setOf("valued"), now)
        assertEq("ranked-better", reach.contact?.id)
        expect("ranked 2 (rank)" in reach.reason && "score 70" in reach.reason, "the reason names the rank and score columns")
        contacts += CrmContact(id = "new-reply", email = "n@x.test")
        val activity = listOf(
            CrmActivity.fromJson(mapOf("contactId" to "new-reply", "direction" to "inbound", "subject" to "Hello", "sentAt" to now - 1000)),
            CrmActivity.fromJson(mapOf("contactId" to "new-reply", "direction" to "outbound", "status" to "draft", "sentAt" to now)),
        )
        follow = CrmNextContact.pick(CrmNextContact.Mode.FOLLOW_UP, contacts, activity, now = now)
        assertEq("new-reply", follow.contact?.id)
        expect("Hello" in follow.reason, "the reason names the message")
        reach = CrmNextContact.pick(CrmNextContact.Mode.REACH_OUT, contacts, activity, now = now)
        expect(reach.contact?.id != "new-reply", "a lead who wrote in is a follow-up, not a new lead")
    }

    fun testNoteDraftsParseLikeTheDesktop() {
        val data = CrmOrganizationData.fromJson(summary())
        val drafts = CrmNoteDraft.drafts(data.contacts, data.activity)
        assertEq(listOf(hexA, hexB), drafts.map { it.contactId })
        assertEq("Hello", drafts[0].subject)
        assertEq("Hi Ada,\nwant a demo?", drafts[0].body)
        expect(!drafts[0].mastodon && drafts[0].canSend)
        expect(drafts[1].mastodon)
        assertEq("@bob@social.test", drafts[1].to)
        assertEq("https://social.test/@acme/1234567", drafts[1].thread)
        val email = drafts[0].emailDraft(data.contacts.first())
        expect(email.noteDraft)
        assertEq("ada@x.test", email.to)
        assertEq(
            "Met at a meetup.\nDRAFT, sent 2026-10-07 by sam.\nSubject: Hello\nHi Ada,\nwant a demo?\n\nOlder note",
            CrmNoteDraft.markedSent(data.contacts[0].notes, "2026-10-07", "sam"),
        )
        assertEq(null, CrmNoteDraft.markedSent("nothing here", "2026-10-07"))
    }

    fun testEditorColumnsHonourRemovedColumnsAndLinks() {
        val columns = CrmColumnCatalog.editorColumns(
            listOf(CrmColumn("name", "Name", false, false), CrmColumn("phone", "Phone", false, false), CrmColumn("budget", "Budget", true, false)),
            listOf("phone", "twitter"),
        )
        assertEq(listOf("name", "budget"), columns.take(2).map { it.field })
        expect(columns.none { it.field == "phone" || it.field == "twitter" })
        expect(columns.any { it.field == "email" && it.hidden })
        assertEq(listOf("phone", "twitter"), CrmOrganizationData.fromJson(summary()).settings.removedColumns)
        assertEq("https://app.forkmesh.com/dashboard/crm?org=acme&contact=$hexA", CrmPrompt.contactLink("acme", hexA))
    }

    fun testContactEditSendsLastContacted() {
        val contact = CrmOrganizationData.fromJson(summary()).contacts[0]
        var edit = CrmContactEdit.of(contact)
        assertEq(5L, edit.lastContactedAt)
        expect(edit.changes(contact).isEmpty())
        edit = edit.copy(lastContactedAt = 1_000)
        assertEq(1_000L, edit.changes(contact)["lastContactedAt"])
        assertEq(0L, edit.copy(lastContactedAt = 0).changes(contact)["lastContactedAt"])
        expect(!CrmContactEdit().changes(null).containsKey("lastContactedAt"))
    }

    fun testModelDraftsColumnsZohoAndTimeZone() {
        val transport = FakeTransport()
        transport.answer("GET", "/api/crm", mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true, "role" to "owner"))))
        transport.answer("GET", "/api/crm/acme", summary())
        val model = CrmModel(PackagesClient(transport))
        model.now = { 1_791_417_600_000L }
        model.refresh()
        assertEq("tz", transport.last("GET", "/api/crm/acme")?.query?.firstOrNull()?.first)
        assertEq(2, model.noteDrafts.size)

        transport.answer("PATCH", "/api/crm/acme/contacts/$hexB", mapOf("contact" to mapOf("id" to hexB, "name" to "Bob", "flow" to "customers", "stage" to "lead", "notes" to "DRAFT, sent today.")))
        expect(model.markNoteDraftSent(hexB, stampContacted = true))
        val body = transport.last("PATCH", "/api/crm/acme/contacts/$hexB")?.body
        expect(body.str("notes").startsWith("DRAFT, sent 2026-10-0"))
        assertEq(1_791_417_600_000L, body.long("lastContactedAt"))
        assertEq(listOf(hexA), model.noteDrafts.map { it.contactId })

        transport.answer("POST", "/api/crm/acme/emails/send", mapOf("email" to mapOf("id" to "e9", "contactId" to hexA, "status" to "sent")))
        transport.answer("PATCH", "/api/crm/acme/contacts/$hexA", mapOf("contact" to mapOf("id" to hexA, "name" to "Ada", "flow" to "customers", "stage" to "lead", "notes" to "DRAFT, sent.")))
        model.sendEmail(model.noteDrafts[0].emailDraft(model.contact(hexA)))
        expect(transport.last("PATCH", "/api/crm/acme/contacts/$hexA") != null)
        expect(model.noteDrafts.isEmpty())
        expect(model.notice.startsWith("Sent to ada@x.test. Draft marked sent"))

        transport.answer("PUT", "/api/crm/acme/settings", mapOf("settings" to mapOf("removedColumns" to listOf("twitter"))))
        expect(model.restoreColumn("phone"))
        assertEq(listOf("twitter"), transport.last("PUT", "/api/crm/acme/settings")?.body.strings("removedColumns"))
        assertEq(listOf("twitter"), model.data?.settings?.removedColumns)

        expect(!model.exchangeZoho(CrmZohoCode()))
        assertEq("Enter the Self Client's client ID, its client secret and the code.", model.errorText)
        val code = CrmZohoCode("1000.X", "s", "1000.c", "eu")
        transport.answer("POST", "/api/crm/acme/zoho/exchange", mapOf("error" to "zoho_code_rejected"), 400)
        expect(!model.exchangeZoho(code))
        expect(model.errorText.startsWith("Zoho rejected the code"))
        transport.answer("POST", "/api/crm/acme/zoho/exchange", mapOf("settings" to mapOf("zoho" to mapOf("connected" to true, "region" to "eu"))))
        expect(model.exchangeZoho(code))
        assertEq("eu", transport.last("POST", "/api/crm/acme/zoho/exchange")?.body.str("region"))
        expect(model.data?.settings?.zoho?.connected == true)
    }
}
