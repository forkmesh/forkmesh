package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CrmWorkspaceTest {
    private val hexA = "a".repeat(32)
    private val hexB = "b".repeat(32)
    private val hexC = "c".repeat(32)
    private val hexD = "d".repeat(32)
    private val listA = "1".repeat(32)
    private val listB = "2".repeat(32)
    private val pipeA = "3".repeat(32)

    private fun contact(id: String, name: String, flow: String, stage: String, views: List<String> = emptyList(), value: Long = 0, owner: String = "", tags: List<String> = emptyList(), updated: Long = 0) =
        mapOf(
            "id" to id, "organization" to "acme", "name" to name, "email" to name.lowercase() + "@x.test", "flow" to flow, "stage" to stage,
            "views" to views, "value" to value, "owner" to owner, "tags" to tags, "updatedAt" to updated,
            "fields" to mapOf("twitter_followers" to "120", "empty" to ""),
        )

    private fun summary(extraViews: List<Map<String, Any?>> = emptyList(), contacts: List<Map<String, Any?>>? = null, flows: List<Map<String, Any?>>? = null): Map<String, Any?> = mapOf(
        "organization" to "acme", "role" to "owner", "canManageSettings" to true,
        "contacts" to (contacts ?: listOf(
            contact(hexA, "Ada", "investment", "sourced", listOf(listA), 500, "alice", listOf("vip"), 3),
            contact(hexB, "Bob", "investment", "meeting", value = 100, owner = "bob", updated = 2),
            contact(hexC, "Cy", "customers", "lead", listOf(listA, listB), updated = 1),
            contact(hexD, "Dee", "retired", "lead"),
        )),
        "emails" to listOf(mapOf("id" to hexA, "contactId" to hexA, "status" to "accepted", "subject" to "Hi", "sentAt" to 9)),
        "mail" to listOf(mapOf("id" to hexB, "box" to "Inbox", "subject" to "Re: deck", "sender" to "Ada")),
        "settings" to mapOf(
            "flows" to (flows ?: listOf(
                mapOf("id" to "investment", "name" to "Investment", "icon" to "investors", "stages" to listOf("sourced", "meeting", "committed")),
                mapOf("id" to "customers", "name" to "Customers", "icon" to "customers", "stages" to listOf("lead", "customer")),
            )),
            "mailBoxes" to listOf("Inbox", "Archive"), "fromEmail" to "team@acme.test", "mailProvider" to "gmail",
            "zoho" to mapOf("connected" to false, "oneClick" to true),
        ),
        "outreach" to mapOf(
            "allowed" to true, "canManage" to true, "dailyLimit" to 50, "sentToday" to 4,
            "templates" to listOf(mapOf("key" to "invest", "label" to "Investment", "subject" to "Hi {firstName}", "body" to "About {company}")),
            "members" to listOf(mapOf("name" to "carol")),
        ),
        "views" to listOf(
            mapOf("id" to listA, "name" to "Press", "columns" to listOf(mapOf("field" to "name"), mapOf("field" to "twitter_followers", "custom" to true), mapOf("field" to "phone", "hidden" to true)), "canManage" to true),
            mapOf("id" to pipeA, "name" to "Investors", "flow" to "investment", "canManage" to true),
            mapOf("id" to listB, "name" to "Private", "visibility" to "private"),
        ) + extraViews,
        "sharedViews" to listOf(mapOf("id" to hexD, "name" to "Their list", "organization" to "other", "canEdit" to true)),
        "members" to listOf("alice", "bob", "carol"),
        "activity" to mapOf(
            "days" to listOf("2026-10-01", "2026-10-02"),
            "members" to listOf(mapOf("name" to "alice", "emails" to listOf(1, 2), "outreach" to listOf(0, 3), "contacts" to listOf(4, 0)), mapOf("name" to "bob", "emails" to listOf(0, 1))),
        ),
        "decks" to listOf(mapOf("token" to "abcdefghij12", "contactId" to hexA, "views" to 3, "seconds" to 75, "pagesViewed" to 5, "pageCount" to 10)),
        "revision" to "r1",
    )

    private fun loaded(transport: FakeTransport, json: Map<String, Any?> = summary()): CrmModel {
        transport.answer("GET", "/api/crm", mapOf("organizations" to listOf(mapOf("name" to "acme", "role" to "owner", "installed" to true))))
        transport.answer("GET", "/api/crm/acme", json)
        return CrmModel(PackagesClient(transport)).also { it.refresh() }
    }

    fun testParsesEveryDesktopSurface() {
        val data = CrmOrganizationData.fromJson(summary())
        assertEq(mapOf("twitter_followers" to "120"), data.contactById(hexA)?.fields)
        assertEq(listOf(listA), data.contactById(hexA)?.views)
        assertEq("Ada", data.mail.first().who)
        assertEq(listOf("Inbox", "Archive"), data.settings.mailBoxes)
        expect(data.settings.zoho.oneClick)
        assertEq(46, data.outreach.remaining)
        assertEq("Hi Ada", data.outreach.templates.first().filled(data.contactById(hexA)).first)
        assertEq(listOf("twitter_followers"), data.views[0].summaryColumns.map { it.field })
        expect(data.sharedViews.first().canEdit)
        assertEq(6, data.team.busiestDay)
        assertEq(3, data.team.totals(1).emails)
        assertEq(0.5, data.decksFor(hexA).first().pageShare)
        assertEq(600L, data.pipelineValue)
        assertEq(listOf("alice", "bob", "carol"), data.owners)
        assertEq(listOf("twitter_followers"), data.customFieldKeys())
    }

    fun testTabsFollowTheDesktopRules() {
        val data = CrmOrganizationData.fromJson(summary())
        assertEq(listOf(CrmTab.Sheet(listA), CrmTab.Sheet(pipeA), CrmTab.Sheet(listB), CrmTab.Pipeline("customers"), CrmTab.NoPipeline), data.contactTabs.map { it.tab })
        assertEq(listOf(2, 2, 1, 1, 1), data.contactTabs.map { it.count })
        assertEq(listOf("Dee"), data.contactsFor(CrmTab.NoPipeline).map { it.name })
        assertEq("investment", data.flowFor(CrmTab.Sheet(pipeA))?.id)
        assertEq(CrmTab.Shared, data.workspaceTabs.last().tab)
        val split = CrmOrganizationData.fromJson(
            summary(
                extraViews = listOf(mapOf("id" to hexC, "name" to "Angels", "flow" to "investment")),
                contacts = listOf(contact(hexA, "Ada", "investment", "sourced"), contact(hexB, "Bob", "investment", "meeting", listOf(hexC))),
            ),
        )
        assertEq(listOf("Ada"), split.contactsFor(CrmTab.Sheet(pipeA)).map { it.name })
        assertEq(listOf("Bob"), split.contactsFor(CrmTab.Sheet(hexC)).map { it.name })
    }

    fun testFilterSortAndHelpers() {
        val data = CrmOrganizationData.fromJson(summary())
        val flow = data.flow("investment")
        assertEq(listOf("Ada", "Bob"), CrmFilter().apply(data.contactsFor(CrmTab.Sheet(pipeA)), flow).map { it.name })
        assertEq(listOf("Bob"), CrmFilter(owner = "BOB", sort = CrmSort.NAME).apply(data.contacts, flow).map { it.name })
        assertEq(listOf("Ada"), CrmFilter(tag = "vip").apply(data.contacts, flow).map { it.name })
        assertEq(listOf(1, 1, 0), flow.stats(data.contactsFor(CrmTab.Sheet(pipeA))).map { it.count })
        assertEq("meeting", flow.next("sourced"))
        assertEq(null, flow.next("committed"))
        assertEq("angel-investors-2026", CrmText.slug("  Angel Investors! 2026 "))
        assertEq("1.5k", CrmText.money(1500))
        assertEq("https://mastodon.social/@ada", CrmText.profileUrl("mastodon", "@ada@mastodon.social"))
        assertEq("Hi there\nBye", CrmText.strippingTags("<p>Hi&nbsp;there</p><p>Bye</p>"))
        val split = CrmCsv.split("name,notes\r\nAda,\"two\nlines\"\nBob,x\n", rowsPerPart = 1)
        assertEq(2, split.rows)
        assertEq(listOf("name,notes\nAda,\"two\nlines\"", "name,notes\nBob,x"), split.parts)
        val draft = CrmFlowDraft(name = "Angel Round", stages = listOf("Intro", "Pitch", "intro", ""))
        assertEq("angel-round", draft.flow?.id)
        assertEq(listOf("intro", "pitch"), draft.flow?.stages)
        expect(CrmPrompt.outreach(data.contactById(hexA)!!, "Investors", "Investment").contains("as an investor"))
        val ada = data.contactById(hexA)!!
        val edit = CrmContactEdit.of(ada).copy(linkedin = "ada", fields = mapOf("twitter_followers" to "", "region" to "EU"))
        val changes = edit.changes(ada)
        assertEq("ada", changes["linkedin"])
        assertEq(mapOf("region" to "EU", "twitter_followers" to ""), changes["fields"])
        expect("name" !in changes)
    }

    fun testBulkMergeAndPipelineLists() {
        val transport = FakeTransport()
        val model = loaded(transport, summary(extraViews = listOf(mapOf("id" to hexC, "name" to "Angels", "flow" to "investment"))))
        transport.answer("POST", "/api/crm/acme/contacts/bulk", mapOf("contacts" to listOf(contact(hexA, "Ada", "customers", "lead")), "missing" to listOf(hexC)))
        expect(model.bulk(CrmBulkAction.Flow("customers"), listOf(hexA)))
        val edit = (transport.last("POST", "/api/crm/acme/contacts/bulk")?.body?.get("edits") as List<*>).first() as Map<*, *>
        assertEq("customers", edit["flow"])
        assertEq("lead", edit["stage"])
        assertEq("customers", model.contact(hexA)?.flow)
        assertEq(null, model.contact(hexC))
        val angels = model.data!!.views.first { it.id == hexC }
        expect(model.moveToPipelineList(listOf(hexD), angels))
        val moved = (transport.last("POST", "/api/crm/acme/contacts/bulk")?.body?.get("edits") as List<*>).first() as Map<*, *>
        assertEq("sourced", moved["stage"])
        assertEq(listOf(pipeA), moved["removeViews"])
        transport.answer("POST", "/api/crm/acme/contacts/merge", mapOf("contact" to contact(hexB, "Bob", "investment", "meeting")))
        assertEq(hexB, model.merge(model.contact(hexA)!!, model.contact(hexB)!!)?.id)
        assertEq(mapOf("from" to hexA, "into" to hexB), transport.last("POST", "/api/crm/acme/contacts/merge")?.body)
        assertEq(hexB, model.data?.activity?.first()?.contactId)
        assertEq(hexB, model.data?.decks?.first()?.contactId)
    }

    fun testPipelinesAndLists() {
        val transport = FakeTransport()
        val model = loaded(transport)
        assertEq(CrmTab.Sheet(listA), model.activeTab)
        model.select(CrmTab.Sheet(pipeA))
        assertEq("investment", model.flowId)
        model.filter = model.filter.copy(stage = "meeting")
        assertEq(listOf("Bob"), model.visibleContacts.map { it.name })
        transport.answer("PUT", "/api/crm/acme/settings", mapOf("settings" to mapOf("flows" to listOf(
            mapOf("id" to "investment", "stages" to listOf("sourced", "call", "committed")), mapOf("id" to "customers", "stages" to listOf("lead")),
        ))))
        transport.answer("POST", "/api/crm/acme/contacts/bulk", mapOf("contacts" to listOf(contact(hexB, "Bob", "investment", "call"))))
        val draft = CrmFlowDraft.of(model.data!!.flow("investment"))
        model.saveFlow(draft.copy(stages = draft.stages.toMutableList().also { it[1] = "Call" }))
        val edits = transport.last("POST", "/api/crm/acme/contacts/bulk")?.body?.get("edits") as List<*>
        assertEq(listOf(mapOf("id" to hexB, "stage" to "call")), edits)
        transport.answer("PUT", "/api/crm/acme/settings", mapOf("settings" to mapOf("flows" to listOf(mapOf("id" to "customers", "stages" to listOf("lead"))))))
        transport.answer("DELETE", "/api/crm/acme/views/$pipeA", mapOf("deleted" to true))
        expect(model.deleteFlow("investment"))
        expect(transport.last("DELETE", "/api/crm/acme/views/$pipeA") != null)
        expect(!model.deleteFlow("customers"))
        transport.answer("POST", "/api/crm/acme/views", mapOf("view" to mapOf("id" to hexC, "name" to "Leads")), 201)
        assertEq(hexC, model.createList("Leads")?.id)
        assertEq(CrmTab.Sheet(hexC), model.activeTab)
        transport.answer("POST", "/api/crm/acme/views/order", mapOf("views" to listOf(mapOf("id" to listB, "name" to "Private"), mapOf("id" to listA, "name" to "Press"))))
        expect(model.moveList(model.data!!.views.first { it.id == listA }, 1))
        assertEq(listOf(listB, listA), (transport.last("POST", "/api/crm/acme/views/order")?.body?.get("order") as List<*>).take(2))
    }

    fun testOutreachMailDecksImport() {
        val transport = FakeTransport()
        val model = loaded(transport)
        transport.answer("POST", "/api/crm/acme/outreach/send", mapOf("sentToday" to 5))
        expect(model.sendOutreach(CrmEmailDraft.of(model.contact(hexA)).copy(subject = "Hi", body = "Hello", template = "invest")))
        assertEq("invest", transport.last("POST", "/api/crm/acme/outreach/send")?.body?.get("template"))
        transport.answer("POST", "/api/crm/acme/emails/send", mapOf("email" to mapOf("id" to hexC, "contactId" to hexB, "status" to "accepted", "sentAt" to 99, "text" to "x")), 201)
        assertEq("accepted", model.sendEmail(CrmEmailDraft.of(model.contact(hexB)).copy(subject = "Deck", body = "Here"))?.status)
        assertEq(hexC, model.data?.activity?.first()?.id)
        assertEq("", model.data?.activity?.first()?.text)
        assertEq(99L, model.contact(hexB)?.lastContactedAt)
        transport.answer("POST", "/api/crm/acme/zoho/connect", mapOf("authorizationUrl" to "https://accounts.zoho.com/oauth/v2/auth?x=1"))
        assertEq("https://accounts.zoho.com/oauth/v2/auth?x=1", model.syncMail())
        transport.answer("POST", "/api/crm/acme/decks", mapOf("link" to mapOf("token" to "zzzzzzzzzz99", "contactId" to hexB)), 201)
        val link = model.deckLink(model.contact(hexB)!!)!!
        expect(model.revoke(link))
        assertEq(false, model.data?.decksFor(hexB)?.first()?.active)
        transport.answer("POST", "/api/crm/acme/outreach/team", mapOf("members" to listOf(mapOf("name" to "carol"), mapOf("name" to "dave"))))
        expect(model.setOutreachMember("Dave", true))
        assertEq(mapOf("action" to "add", "name" to "dave"), transport.last("POST", "/api/crm/acme/outreach/team")?.body)
        val rows = listOf("name,email") + (0 until 150).map { "P$it,p$it@x.test" }
        transport.answer("POST", "/api/crm/acme/contacts/import", mapOf("created" to 75, "viewId" to hexC))
        val seen = mutableListOf<Int>()
        val result = model.importCsv(rows.joinToString("\n"), "people.csv", "People", mapping = listOf("name", "email")) { part, _ -> seen.add(part) }
        assertEq(150, result?.created)
        assertEq(listOf(0, 1, 2), seen)
        val parts = transport.sent.filter { it.path == "/api/crm/acme/contacts/import" }
        assertEq(2, parts.size)
        assertEq(hexC, parts[1].body?.get("viewId"))
        assertEq(100, (parts[1].body?.get("import") as Map<*, *>)["offset"])
        assertEq("150 added.", model.notice)
    }
}
