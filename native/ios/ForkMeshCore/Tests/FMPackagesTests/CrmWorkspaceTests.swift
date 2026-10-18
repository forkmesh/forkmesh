import FMBase
import XCTest

@testable import FMPackages

let hexC = String(repeating: "c", count: 32)
let hexD = String(repeating: "d", count: 32)
let listA = String(repeating: "1", count: 32)
let listB = String(repeating: "2", count: 32)
let pipeA = String(repeating: "3", count: 32)

extension FakePackagesTransport {
    func set(_ method: String, _ path: String, status: Int = 200, _ json: JSONObject) {
        routes[method + " " + path] = [PackagesReply(status: status, json: json)]
    }
}

final class CrmWorkspaceTests: XCTestCase {
    func contact(_ id: String, _ name: String, flow: String, stage: String, views: [String] = [], value: Int64 = 0,
                 owner: String = "", tags: [String] = [], updated: Int64 = 0) -> JSONValue {
        .object([
            "id": .string(id), "organization": "acme", "name": .string(name), "email": .string(name.lowercased() + "@x.test"),
            "flow": .string(flow), "stage": .string(stage), "views": .array(views.map { .string($0) }), "value": .int(value),
            "owner": .string(owner), "tags": .array(tags.map { .string($0) }), "updatedAt": .int(updated),
            "fields": ["twitter_followers": "120", "empty": ""],
        ])
    }

    func summary() -> JSONObject {
        [
            "organization": "acme", "role": "owner", "canManageSettings": true,
            "contacts": [
                contact(hexA, "Ada", flow: "investment", stage: "sourced", views: [listA], value: 500, owner: "alice", tags: ["vip"], updated: 3),
                contact(hexB, "Bob", flow: "investment", stage: "meeting", value: 100, owner: "bob", updated: 2),
                contact(hexC, "Cy", flow: "customers", stage: "lead", views: [listA, listB], updated: 1),
                contact(hexD, "Dee", flow: "retired", stage: "lead"),
            ],
            "emails": [["id": .string(hexA), "contactId": .string(hexA), "status": "accepted", "subject": "Hi", "sentAt": 9]],
            "mail": [["id": .string(hexB), "box": "Inbox", "subject": "Re: deck", "sender": "Ada"]],
            "settings": [
                "flows": [
                    ["id": "investment", "name": "Investment", "icon": "investors", "stages": ["sourced", "meeting", "committed"]],
                    ["id": "customers", "name": "Customers", "icon": "customers", "stages": ["lead", "customer"]],
                ],
                "mailBoxes": ["Inbox", "Archive"], "fromEmail": "team@acme.test", "mailProvider": "gmail",
                "zoho": ["connected": false, "oneClick": true],
            ],
            "outreach": [
                "allowed": true, "canManage": true, "dailyLimit": 50, "sentToday": 4, "fromEmail": "f@acme.test",
                "templates": [["key": "invest", "label": "Investment", "subject": "Hi {firstName}", "body": "About {company}"]],
                "recent": [["ts": 5, "sender": "alice", "ok": true, "to": "x@y.z", "subject": "Hello"]],
                "members": [["name": "carol", "addedBy": "alice", "addedAt": 1]],
            ],
            "views": [
                ["id": .string(listA), "name": "Press", "columns": [["field": "name"], ["field": "twitter_followers", "custom": true], ["field": "phone", "hidden": true]], "canManage": true],
                ["id": .string(pipeA), "name": "Investors", "flow": "investment", "canManage": true],
                ["id": .string(listB), "name": "Private", "visibility": "private"],
            ],
            "sharedViews": [["id": .string(hexD), "name": "Their list", "organization": "other", "canEdit": true, "shared": true]],
            "members": ["alice", "bob", "carol"],
            "activity": [
                "days": ["2026-10-01", "2026-10-02"],
                "members": [["name": "alice", "emails": [1, 2], "outreach": [0, 3], "contacts": [4, 0]], ["name": "bob", "emails": [0, 1], "outreach": [0, 0], "contacts": [0, 0]]],
            ],
            "decks": [["token": "abcdefghij12", "contactId": .string(hexA), "views": 3, "seconds": 75, "pagesViewed": 5, "pageCount": 10]],
            "summary": ["contacts": 4],
            "revision": "r1",
        ]
    }

    func testParsesEveryDesktopSurface() {
        let data = CrmOrganizationData(json: summary())
        XCTAssertEqual(data.contact(id: hexA)?.fields, ["twitter_followers": "120"])
        XCTAssertEqual(data.contact(id: hexA)?.views, [listA])
        XCTAssertEqual(data.mail.first?.who, "Ada")
        XCTAssertEqual(data.settings.mailBoxes, ["Inbox", "Archive"])
        XCTAssertTrue(data.settings.zoho.oneClick)
        XCTAssertEqual(data.outreach.remaining, 46)
        XCTAssertEqual(data.outreach.templates.first?.filled(for: data.contact(id: hexA)).subject, "Hi Ada")
        XCTAssertEqual(data.outreach.members.map(\.name), ["carol"])
        XCTAssertEqual(data.views.map(\.name), ["Press", "Investors", "Private"])
        XCTAssertEqual(data.views[0].summaryColumns.map(\.field), ["twitter_followers"])
        XCTAssertEqual(data.sharedViews.first?.canEdit, true)
        XCTAssertEqual(data.team.busiestDay, 6)
        XCTAssertEqual(data.team.totals(for: 1).emails, 3)
        XCTAssertEqual(data.team.members.first?.total, 10)
        XCTAssertEqual(data.decks(for: hexA).first?.pageShare, 0.5)
        XCTAssertEqual(data.pipelineValue, 600)
        XCTAssertEqual(data.owners, ["alice", "bob", "carol"])
        XCTAssertEqual(data.customFieldKeys(), ["twitter_followers"])
    }

    func testZohoStatusCarriesSendingAndMissingSteps() {
        let readOnly = CrmZohoStatus(json: [
            "connected": true, "canSend": false, "lastSyncAt": 5, "lastSentAt": 0,
            "steps": [["key": "send", "action": "reconnect", "text": "Reconnect Zoho Mail."], ["key": "empty"]],
        ])
        XCTAssertFalse(readOnly.canSend)
        XCTAssertEqual(readOnly.steps.map(\.action), ["reconnect"])
        let olderServer = CrmZohoStatus(json: ["connected": true])
        XCTAssertTrue(olderServer.canSend)
        XCTAssertTrue(olderServer.steps.isEmpty)
    }

    func testTabsFollowTheDesktopRules() {
        let data = CrmOrganizationData(json: summary())
        let tabs = data.contactTabs
        XCTAssertEqual(tabs.map(\.tab), [.list(listA), .list(pipeA), .list(listB), .pipeline("customers"), .noPipeline])
        XCTAssertEqual(tabs.map(\.count), [2, 2, 1, 1, 1])
        XCTAssertEqual(data.contacts(for: .list(pipeA)).map(\.name), ["Ada", "Bob"])
        XCTAssertEqual(data.contacts(for: .noPipeline).map(\.name), ["Dee"])
        XCTAssertEqual(data.flow(for: .list(pipeA))?.id, "investment")
        XCTAssertNil(data.flow(for: .list(listA)))
        XCTAssertEqual(data.tab(forFlow: "customers"), .pipeline("customers"))
        XCTAssertEqual(data.workspaceTabs.last?.tab, .shared)
    }

    func testTwoListsOnOnePipelineSplitByMembership() {
        var json = summary()
        var views = json.list("views")
        views.append(["id": .string(hexC), "name": "Angels", "flow": "investment"])
        json["views"] = .array(views)
        var contacts = json.list("contacts")
        contacts[1] = contact(hexB, "Bob", flow: "investment", stage: "meeting", views: [hexC])
        json["contacts"] = .array(contacts)
        let data = CrmOrganizationData(json: json)
        XCTAssertEqual(data.contacts(for: .list(pipeA)).map(\.name), ["Ada"])
        XCTAssertEqual(data.contacts(for: .list(hexC)).map(\.name), ["Bob"])
    }

    func testFilterAndSort() {
        let data = CrmOrganizationData(json: summary())
        let flow = data.flow("investment")
        var filter = CrmFilter()
        XCTAssertEqual(filter.apply(data.contacts(for: .list(pipeA)), flow: flow).map(\.name), ["Ada", "Bob"])
        filter.sort = .value
        XCTAssertEqual(filter.apply(data.contacts, flow: flow).map(\.name).prefix(2), ["Ada", "Bob"])
        filter.sort = .name
        filter.owner = "BOB"
        XCTAssertEqual(filter.apply(data.contacts, flow: flow).map(\.name), ["Bob"])
        filter = CrmFilter()
        filter.tag = "vip"
        XCTAssertEqual(filter.apply(data.contacts, flow: flow).map(\.name), ["Ada"])
        filter = CrmFilter()
        filter.query = "120"
        XCTAssertEqual(filter.apply(data.contacts, flow: flow).count, 4)
        let stats = flow.stats(data.contacts(for: .list(pipeA)))
        XCTAssertEqual(stats.map(\.count), [1, 1, 0])
        XCTAssertEqual(stats.map(\.value), [500, 100, 0])
        XCTAssertEqual(flow.next(after: "sourced"), "meeting")
        XCTAssertNil(flow.next(after: "committed"))
        XCTAssertEqual(flow.progress(of: "meeting"), 0.5)
    }

    func testEditSendsSocialsAndCustomFields() {
        let ada = CrmContact(json: contact(hexA, "Ada", flow: "investment", stage: "sourced").objectValue!)
        var edit = CrmContactEdit(contact: ada)
        edit.linkedin = "ada"
        edit.fields["twitter_followers"] = ""
        edit.fields["region"] = "EU"
        let changes = edit.changes(from: ada)
        XCTAssertEqual(changes.str("linkedin"), "ada")
        XCTAssertEqual(changes.obj("fields"), ["region": "EU", "twitter_followers": ""])
        XCTAssertNil(changes["name"])
    }

    func testDuplicatesAndPrompt() {
        var json = summary()
        var contacts = json.list("contacts")
        contacts.append(contact(String(repeating: "e", count: 32), "Ada", flow: "customers", stage: "lead"))
        json["contacts"] = .array(contacts)
        let data = CrmOrganizationData(json: json)
        let ada = data.contact(id: hexA)!
        XCTAssertEqual(data.duplicates(of: ada).count, 1)
        let prompt = CrmPrompt.outreach(ada, sheet: "Investors", flowName: "Investment")
        XCTAssertTrue(prompt.contains("as an investor"))
        XCTAssertTrue(prompt.contains("look it up"))
    }

    func testTextHelpers() {
        XCTAssertEqual(CrmText.slug("  Angel Investors! 2026 "), "angel-investors-2026")
        XCTAssertEqual(CrmText.slug("2026"), "")
        XCTAssertEqual(CrmText.money(1500), "1.5k")
        XCTAssertEqual(CrmText.money(25000), "25k")
        XCTAssertEqual(CrmText.money(2_000_000), "2M")
        XCTAssertEqual(CrmText.profileURL("mastodon", "@ada@mastodon.social")?.absoluteString, "https://mastodon.social/@ada")
        XCTAssertEqual(CrmText.profileURL("github", "ada")?.absoluteString, "https://github.com/ada")
        XCTAssertEqual(CrmText.strippingTags("<p>Hi&nbsp;there</p><p>Bye</p>"), "Hi there\nBye")
        XCTAssertTrue(CrmText.isDeckToken("abcdefghij12"))
        XCTAssertFalse(CrmText.isDeckToken("../x"))
        let split = CrmCSV.split("name,notes\r\nAda,\"two\nlines\"\nBob,x\n", rowsPerPart: 1)
        XCTAssertEqual(split.rows, 2)
        XCTAssertEqual(split.parts, ["name,notes\nAda,\"two\nlines\"", "name,notes\nBob,x"])
        var draft = CrmFlowDraft(name: "Angel Round")
        draft.stages = ["Intro", "Pitch", "intro", ""]
        XCTAssertEqual(draft.flow?.id, "angel-round")
        XCTAssertEqual(draft.flow?.stages, ["intro", "pitch"])
        let settings = CrmOrganizationData(json: summary()).settings
        XCTAssertTrue(settings.composeURL(to: "a@b.c", subject: "Hi & bye")!.absoluteString.hasPrefix("https://mail.google.com/"))
        XCTAssertTrue(settings.composeURL(to: "a@b.c", subject: "Hi & bye")!.absoluteString.contains("Hi%20%26%20bye"))
        XCTAssertEqual(
            CrmSettings.zohoComposeURL(region: "eu", to: "a@b.c", subject: "Hi & bye", body: "x")?.absoluteString,
            "https://mail.zoho.eu/zm/comp.do?ct=mailto%3Aa%40b.c%3Fsubject%3DHi%2520%2526%2520bye%26body%3Dx")
        XCTAssertTrue(CrmSettings.zohoComposeURL(region: "", to: "a@b.c")!.absoluteString.hasPrefix("https://mail.zoho.com/zm/comp.do?ct=mailto%3A"))
    }

    @MainActor func loadedModel(_ transport: FakePackagesTransport) async -> CrmModel {
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "role": "owner", "installed": true]]])
        transport.on("GET", "/api/crm/acme", summary())
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        return model
    }

    @MainActor func testActiveTabFollowsSelectionAndFlow() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        XCTAssertEqual(model.activeTab, .list(listA))
        model.flowId = "customers"
        XCTAssertEqual(model.activeTab, .pipeline("customers"))
        model.select(.list(pipeA))
        XCTAssertEqual(model.activeTab, .list(pipeA))
        XCTAssertEqual(model.flowId, "investment")
        XCTAssertEqual(model.visibleContacts.map(\.name), ["Ada", "Bob"])
        model.filter.stage = "meeting"
        XCTAssertEqual(model.visibleContacts.map(\.name), ["Bob"])
        model.tab = .list(String(repeating: "9", count: 32))
        XCTAssertEqual(model.activeTab, .list(pipeA))
    }

    @MainActor func testBulkActionsSendOneRequest() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        transport.on("POST", "/api/crm/acme/contacts/bulk", [
            "contacts": [contact(hexA, "Ada", flow: "customers", stage: "lead"), contact(hexB, "Bob", flow: "customers", stage: "lead")],
            "missing": [],
        ])
        let moved = await model.bulk(.flow("customers"), ids: [hexA, hexB])
        XCTAssertTrue(moved)
        let edits = transport.last("POST", "/api/crm/acme/contacts/bulk")?.body?.list("edits") ?? []
        XCTAssertEqual(edits.count, 2)
        XCTAssertEqual(edits[0].str("flow"), "customers")
        XCTAssertEqual(edits[0].str("stage"), "lead")
        XCTAssertEqual(model.contact(hexA)?.flow, "customers")

        transport.set("POST", "/api/crm/acme/contacts/bulk", ["contacts": [], "missing": [.string(hexC)]])
        await model.setOnlyList([hexC], viewId: listB)
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/contacts/bulk")?.body?.list("edits").first?.strings("setViews"), [listB])
        XCTAssertNil(model.contact(hexC))
    }

    @MainActor func testMoveToPipelineListDropsSiblings() async {
        let transport = FakePackagesTransport()
        var json = summary()
        var views = json.list("views")
        views.append(["id": .string(hexC), "name": "Angels", "flow": "investment"])
        json["views"] = .array(views)
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "role": "owner", "installed": true]]])
        transport.on("GET", "/api/crm/acme", json)
        transport.on("POST", "/api/crm/acme/contacts/bulk", ["contacts": [], "missing": []])
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        let view = model.data!.views.first { $0.id == hexC }!
        await model.moveToPipelineList([hexD], view: view)
        let edit = transport.last("POST", "/api/crm/acme/contacts/bulk")?.body?.list("edits").first
        XCTAssertEqual(edit?.str("flow"), "investment")
        XCTAssertEqual(edit?.str("stage"), "sourced")
        XCTAssertEqual(edit?.strings("views"), [hexC])
        XCTAssertEqual(edit?.strings("removeViews"), [pipeA])
    }

    @MainActor func testMergeMovesHistory() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        transport.on("POST", "/api/crm/acme/contacts/merge", ["merged": .string(hexA), "contact": contact(hexB, "Bob", flow: "investment", stage: "meeting")])
        let merged = await model.merge(model.contact(hexA)!, into: model.contact(hexB)!)
        XCTAssertEqual(merged?.id, hexB)
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/contacts/merge")?.body, ["from": .string(hexA), "into": .string(hexB)])
        XCTAssertNil(model.contact(hexA))
        XCTAssertEqual(model.data?.activity.first?.contactId, hexB)
        XCTAssertEqual(model.data?.decks.first?.contactId, hexB)
    }

    @MainActor func testNewPipelineAddsAListTab() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        transport.on("PUT", "/api/crm/acme/settings", ["settings": ["flows": [
            ["id": "investment", "stages": ["sourced"]], ["id": "customers", "stages": ["lead"]],
            ["id": "press", "name": "Press", "stages": ["pitched", "covered"]],
        ]]])
        transport.on("POST", "/api/crm/acme/views", status: 201, ["view": ["id": .string(hexC), "name": "Press", "flow": "press"]])
        var draft = CrmFlowDraft(name: "Press")
        draft.stages = ["Pitched", "Covered"]
        let flow = await model.saveFlow(draft)
        XCTAssertEqual(flow?.id, "press")
        XCTAssertEqual(transport.last("PUT", "/api/crm/acme/settings")?.body?.list("flows").count, 3)
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/views")?.body?.str("flow"), "press")
        XCTAssertEqual(model.activeTab, .list(hexC))
    }

    @MainActor func testRenamedStageMovesItsContacts() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        transport.on("PUT", "/api/crm/acme/settings", ["settings": ["flows": [
            ["id": "investment", "stages": ["sourced", "call", "committed"]], ["id": "customers", "stages": ["lead"]],
        ]]])
        transport.on("POST", "/api/crm/acme/contacts/bulk", ["contacts": [contact(hexB, "Bob", flow: "investment", stage: "call")], "missing": []])
        var draft = CrmFlowDraft(flow: model.data!.flow("investment"))
        draft.stages[1] = "Call"
        _ = await model.saveFlow(draft)
        let edits = transport.last("POST", "/api/crm/acme/contacts/bulk")?.body?.list("edits") ?? []
        XCTAssertEqual(edits.map { $0.str("id") }, [hexB])
        XCTAssertEqual(edits.first?.str("stage"), "call")
    }

    @MainActor func testLastPipelineCannotBeDeleted() async {
        let transport = FakePackagesTransport()
        var json = summary()
        json["settings"] = ["flows": [["id": "customers", "stages": ["lead"]]]]
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "role": "owner", "installed": true]]])
        transport.on("GET", "/api/crm/acme", json)
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        let deleted = await model.deleteFlow("customers")
        XCTAssertFalse(deleted)
        XCTAssertNil(transport.last("PUT", "/api/crm/acme/settings"))
        XCTAssertFalse(model.errorText.isEmpty)
    }

    @MainActor func testDeletePipelineRemovesItsLists() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        transport.on("PUT", "/api/crm/acme/settings", ["settings": ["flows": [["id": "customers", "stages": ["lead"]]]]])
        transport.on("DELETE", "/api/crm/acme/views/" + pipeA, ["deleted": true])
        let deleted = await model.deleteFlow("investment")
        XCTAssertTrue(deleted)
        XCTAssertEqual(transport.last("PUT", "/api/crm/acme/settings")?.body?.list("flows").map { $0.str("id") }, ["customers"])
        XCTAssertNotNil(transport.last("DELETE", "/api/crm/acme/views/" + pipeA))
        XCTAssertFalse(model.data!.views.contains { $0.id == pipeA })
    }

    @MainActor func testListsCreateShareReorderMergeAndDelete() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        transport.on("POST", "/api/crm/acme/views", status: 201, ["view": ["id": .string(hexC), "name": "Leads"]])
        let created = await model.createList(name: "Leads")
        XCTAssertEqual(created?.id, hexC)
        XCTAssertEqual(model.activeTab, .list(hexC))

        let customers = model.data!.contactTabs.first { $0.tab == .pipeline("customers") }!
        transport.set("POST", "/api/crm/acme/views", status: 201, ["view": ["id": .string(hexD), "name": "Customers", "flow": "customers", "sharedWith": ["carol"]]])
        let shared = await model.share(customers, with: ["Carol", "carol", " "], access: "edit", visibility: "organization")
        XCTAssertTrue(shared)
        let body = transport.last("POST", "/api/crm/acme/views")?.body
        XCTAssertEqual(body?.strings("sharedWith"), ["carol"])
        XCTAssertEqual(body?.str("flow"), "customers")

        transport.on("POST", "/api/crm/acme/views/order", ["views": [["id": .string(pipeA), "name": "Investors", "flow": "investment"], ["id": .string(listA), "name": "Press"], ["id": .string(listB), "name": "Private"]]])
        let press = model.data!.views.first { $0.id == listA }!
        let moved = await model.moveList(press, by: 1)
        XCTAssertTrue(moved)
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/views/order")?.body?.strings("order").prefix(2), [pipeA, listA])

        transport.on("POST", "/api/crm/acme/contacts/bulk", ["contacts": [], "missing": []])
        transport.on("PATCH", "/api/crm/acme/views/" + listB, ["view": ["id": .string(listB), "name": "Private"]])
        transport.on("DELETE", "/api/crm/acme/views/" + listA, ["deleted": true])
        let target = model.data!.views.first { $0.id == listB }!
        let merged = await model.mergeList(press, into: target)
        XCTAssertTrue(merged)
        let edits = transport.last("POST", "/api/crm/acme/contacts/bulk")?.body?.list("edits") ?? []
        XCTAssertEqual(edits.map { $0.str("id") }.sorted(), [hexA, hexC])
        XCTAssertEqual(edits.first?.strings("removeViews"), [listA])
        XCTAssertNotNil(transport.last("PATCH", "/api/crm/acme/views/" + listB))
        XCTAssertEqual(model.activeTab, .list(listB))
        XCTAssertFalse(model.contact(hexA)!.views.contains(listA))
    }

    @MainActor func testOutreachEmailAndDecks() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        transport.on("POST", "/api/crm/acme/outreach/send", ["sentToday": 5, "dailyLimit": 50])
        var draft = CrmEmailDraft(contact: model.contact(hexA))
        draft.subject = "Hi"
        draft.body = "Hello"
        draft.template = "invest"
        let sent = await model.sendOutreach(draft)
        XCTAssertTrue(sent)
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/outreach/send")?.body?.str("template"), "invest")

        transport.set("POST", "/api/crm/acme/emails/send", status: 201, ["email": ["id": .string(hexC), "contactId": .string(hexB), "status": "accepted", "sentAt": 99, "text": "body"]])
        draft = CrmEmailDraft(contact: model.contact(hexB))
        draft.subject = "Deck"
        draft.body = "Here"
        let email = await model.sendEmail(draft)
        XCTAssertEqual(email?.status, "accepted")
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/emails/send")?.body?.str("contactId"), hexB)
        XCTAssertEqual(model.data?.activity.first?.id, hexC)
        XCTAssertEqual(model.data?.activity.first?.text, "")
        XCTAssertEqual(model.contact(hexB)?.lastContactedAt, 99)

        transport.on("POST", "/api/crm/acme/decks", status: 201, ["created": true, "link": ["token": "zzzzzzzzzz99", "contactId": .string(hexB), "active": true]])
        let link = await model.deckLink(for: model.contact(hexB)!)
        XCTAssertEqual(link?.token, "zzzzzzzzzz99")
        transport.on("DELETE", "/api/crm/acme/decks/zzzzzzzzzz99", ["revoked": true])
        let revoked = await model.revoke(link!)
        XCTAssertTrue(revoked)
        XCTAssertEqual(model.data?.decks(for: hexB).first?.active, false)

        transport.on("POST", "/api/crm/acme/outreach/team", ["members": [["name": "carol"], ["name": "dave"]]])
        await model.setOutreachMember("Dave", allowed: true)
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/outreach/team")?.body, ["action": "add", "name": "dave"])
        XCTAssertEqual(model.data?.outreach.members.count, 2)
    }

    @MainActor func testMailSyncConnectsZohoFirst() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        transport.on("POST", "/api/crm/acme/zoho/connect", ["authorizationUrl": "https://accounts.zoho.com/oauth/v2/auth?x=1"])
        let url = await model.syncMail()
        XCTAssertEqual(url?.host, "accounts.zoho.com")
        transport.on("PATCH", "/api/crm/acme/mail/" + hexB, ["message": ["id": .string(hexB), "box": "Archive"]])
        let moved = await model.moveMail(model.data!.mail[0], to: "Archive")
        XCTAssertTrue(moved)
        XCTAssertEqual(model.data?.mail.first?.box, "Archive")
    }

    @MainActor func testCsvImportGoesInParts() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        var rows = ["name,email"]
        for index in 0..<150 { rows.append("P\(index),p\(index)@x.test") }
        transport.on("POST", "/api/crm/acme/contacts/import", ["created": 100, "updated": 0, "skipped": 0, "viewId": .string(hexC)])
        transport.on("POST", "/api/crm/acme/contacts/import", ["created": 49, "updated": 0, "skipped": 1, "viewId": .string(hexC), "errors": [["row": 7, "error": "invalid_email", "message": "Row 7 has an email address that is not valid"]]])
        var seen: [Int] = []
        let result = await model.importCSV(rows.joined(separator: "\n"), fileName: "people.csv", sheetName: "People", mapping: ["name", "email"]) { part, _ in seen.append(part) }
        XCTAssertEqual(result?.created, 149)
        XCTAssertEqual(result?.skipped, 1)
        XCTAssertEqual(result?.errors.count, 1)
        XCTAssertEqual(seen, [0, 1, 2])
        let imports = transport.sent.filter { $0.path == "/api/crm/acme/contacts/import" }
        XCTAssertEqual(imports.count, 2)
        XCTAssertEqual(imports[1].body?.str("viewId"), hexC)
        XCTAssertEqual(imports[1].body?.obj("import").int("offset"), 100)
        XCTAssertEqual(imports[1].body?.obj("import").int("created"), 100)
        XCTAssertEqual(model.notice, "149 added, 1 skipped.")
    }

    @MainActor func testSettingsSaveAndExport() async {
        let transport = FakePackagesTransport()
        let model = await loadedModel(transport)
        var edit = CrmSettingsEdit(settings: model.data!.settings)
        edit.mailBoxes = "Inbox, Hot, inbox"
        transport.on("PUT", "/api/crm/acme/settings", ["settings": ["mailBoxes": ["Inbox", "Hot"], "mailProvider": "gmail"]])
        let saved = await model.saveSettings(edit)
        XCTAssertTrue(saved)
        XCTAssertEqual(transport.last("PUT", "/api/crm/acme/settings")?.body?.strings("mailBoxes"), ["Inbox", "Hot"])
        XCTAssertEqual(model.data?.settings.mailBoxes, ["Inbox", "Hot"])
        transport.on("GET", "/api/crm/acme/contacts/export", ["filename": "forkmesh-crm-acme.csv", "csv": "name\r\nAda\r\n"])
        let export = await model.exportCSV()
        XCTAssertEqual(export?.filename, "forkmesh-crm-acme.csv")
    }
}
