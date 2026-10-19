import FMBase
import XCTest

@testable import FMPackages

final class CrmParityTests: XCTestCase {
    let hexC = String(repeating: "c", count: 32)

    func testZohoReconnectRetainsClientIDAndUsesSavedSecretOnlyForThatClient() {
        let status = CrmZohoStatus(json: [
            "connected": true, "region": "eu", "clientId": "saved-id", "hasClientSecret": true,
        ])
        var code = CrmZohoCode(status: status)
        XCTAssertEqual(code.clientId, "saved-id")
        XCTAssertEqual(code.region, "eu")
        XCTAssertTrue(code.clientSecret.isEmpty)
        XCTAssertTrue(code.usesSavedSecret)
        XCTAssertFalse(code.isComplete)
        code.code = "fresh-code"
        XCTAssertTrue(code.isComplete)
        XCTAssertEqual(code.body.str("clientSecret"), "")
        code.clientId = "different-id"
        XCTAssertFalse(code.usesSavedSecret)
        XCTAssertFalse(code.isComplete)
        code.clientSecret = "new-secret"
        XCTAssertTrue(code.isComplete)
        let legacy = CrmZohoStatus(json: ["connected": true])
        XCTAssertFalse(CrmZohoCode(status: legacy).usesSavedSecret)
    }

    func summary() -> JSONObject {
        [
            "organization": "acme", "role": "owner", "canManageSettings": true,
            "contacts": [
                ["id": .string(hexA), "name": "Ada", "email": "ada@x.test", "flow": "customers", "stage": "lead",
                 "notes": "Met at a meetup.\nDRAFT, not sent.\nSubject: Hello\nHi Ada,\nwant a demo?\n\nOlder note", "lastContactedAt": 5],
                ["id": .string(hexB), "name": "Bob", "mastodon": "@bob@social.test", "flow": "customers", "stage": "lead",
                 "notes": "DRAFT, not sent. Mastodon reply\nLoved your post!"],
                ["id": .string(hexC), "name": "Cy", "flow": "customers", "stage": "lead",
                 "notes": "Says DRAFT, not sent. mid-line, so no draft"],
            ],
            "emails": [["id": "m1", "contactId": .string(hexB), "provider": "mastodon",
                        "providerUrl": "https://social.test/@acme/1234567", "status": "sent"]],
            "settings": ["flows": [["id": "customers", "name": "Customers", "stages": ["lead"]]],
                         "removedColumns": ["phone", "twitter"]],
            "revision": "r1",
        ]
    }

    func testNextContactSplitsReachOutFromFollowUp() {
        let day: Int64 = 86_400_000
        let now: Int64 = 1_800_000_000_000
        func contact(_ id: String, _ extra: JSONObject) -> CrmContact {
            var json: JSONObject = ["id": .string(id), "flow": "customers", "stage": "lead"]
            for (key, value) in extra { json[key] = value }
            return CrmContact(json: json)
        }
        var contacts = [
            contact("fresh", ["email": "f@x.test", "createdAt": .int(now - 3 * day)]),
            contact("quiet", ["email": "q@x.test", "lastContactedAt": .int(now - 40 * day)]),
            contact("recent", ["email": "r@x.test", "lastContactedAt": .int(now - 2 * day)]),
            contact("won", ["email": "w@x.test", "stage": "customer", "lastContactedAt": .int(now - 90 * day)]),
            contact("unreachable", ["lastContactedAt": .int(now - 90 * day)]),
        ]
        var follow = CrmNextContact.pick(.followUp, contacts: contacts, activity: [], now: now)
        XCTAssertEqual(follow.contact?.id, "quiet")
        XCTAssertEqual(follow.due, 1)
        XCTAssertTrue(follow.reason.contains("40 days ago"))
        var reach = CrmNextContact.pick(.reachOut, contacts: contacts, activity: [], now: now)
        XCTAssertEqual(reach.contact?.id, "fresh")
        contacts += [
            contact("ranked", ["mastodon": "@r@mastodon.social", "fields": ["priority_rank": "4"]]),
            contact("ranked-better", ["email": "rb@x.test", "fields": ["rank": "2", "coding_fit_score": "70"]]),
            contact("valued", ["email": "v@x.test", "value": 5000]),
            contact("lost", ["email": "l@x.test", "stage": "lost"]),
        ]
        reach = CrmNextContact.pick(.reachOut, contacts: contacts, activity: [], now: now)
        XCTAssertEqual(reach.contact?.id, "valued")
        XCTAssertEqual(reach.due, 4)
        reach = CrmNextContact.pick(.reachOut, contacts: contacts, activity: [], skipped: ["valued"], now: now)
        XCTAssertEqual(reach.contact?.id, "ranked-better")
        XCTAssertTrue(reach.reason.contains("ranked 2 (rank)") && reach.reason.contains("score 70"))
        contacts.append(contact("new-reply", ["email": "n@x.test"]))
        let activity = [
            CrmActivity(json: ["contactId": "new-reply", "direction": "inbound", "subject": "Hello", "sentAt": .int(now - 1000)]),
            CrmActivity(json: ["contactId": "new-reply", "direction": "outbound", "status": "draft", "sentAt": .int(now)]),
        ]
        follow = CrmNextContact.pick(.followUp, contacts: contacts, activity: activity, now: now)
        XCTAssertEqual(follow.contact?.id, "new-reply")
        XCTAssertTrue(follow.reason.contains("Hello"))
        reach = CrmNextContact.pick(.reachOut, contacts: contacts, activity: activity, now: now)
        XCTAssertNotEqual(reach.contact?.id, "new-reply")
    }

    func testNoteDraftsParseLikeTheDesktop() {
        let data = CrmOrganizationData(json: summary())
        let drafts = CrmNoteDraft.drafts(data.contacts, activity: data.activity)
        XCTAssertEqual(drafts.map(\.contactId), [hexA, hexB])
        XCTAssertEqual(drafts[0].subject, "Hello")
        XCTAssertEqual(drafts[0].body, "Hi Ada,\nwant a demo?")
        XCTAssertFalse(drafts[0].mastodon)
        XCTAssertTrue(drafts[0].canSend)
        XCTAssertTrue(drafts[1].mastodon)
        XCTAssertEqual(drafts[1].to, "@bob@social.test")
        XCTAssertEqual(drafts[1].thread, "https://social.test/@acme/1234567")
        let email = drafts[0].emailDraft(data.contacts.first)
        XCTAssertTrue(email.noteDraft)
        XCTAssertEqual(email.to, "ada@x.test")
        XCTAssertEqual(CrmNoteDraft.markedSent(data.contacts[0].notes, day: "2026-10-07", by: "sam"),
                       "Met at a meetup.\nDRAFT, sent 2026-10-07 by sam.\nSubject: Hello\nHi Ada,\nwant a demo?\n\nOlder note")
        XCTAssertNil(CrmNoteDraft.markedSent("nothing here", day: "2026-10-07"))
    }

    func testEditorColumnsHonourRemovedColumnsAndLinks() {
        let columns = CrmColumnCatalog.editorColumns(
            [CrmColumn(field: "name", label: "Name", custom: false, hidden: false),
             CrmColumn(field: "phone", label: "Phone", custom: false, hidden: false),
             CrmColumn(field: "budget", label: "Budget", custom: true, hidden: false)],
            removed: ["phone", "twitter"])
        XCTAssertEqual(columns.prefix(2).map(\.field), ["name", "budget"])
        XCTAssertFalse(columns.contains { $0.field == "phone" || $0.field == "twitter" })
        XCTAssertTrue(columns.contains { $0.field == "email" && $0.hidden })
        XCTAssertEqual(CrmSettings(json: summary().obj("settings")).removedColumns, ["phone", "twitter"])
        XCTAssertEqual(CrmPrompt.contactLink(organization: "acme", contactId: hexA)?.absoluteString,
                       "https://app.forkmesh.com/dashboard/crm?org=acme&contact=" + hexA)
    }

    func testContactEditSendsLastContacted() {
        let contact = CrmOrganizationData(json: summary()).contacts[0]
        var edit = CrmContactEdit(contact: contact)
        XCTAssertEqual(edit.lastContactedAt, 5)
        XCTAssertTrue(edit.changes(from: contact).isEmpty)
        edit.lastContactedAt = 1_000
        XCTAssertEqual(edit.changes(from: contact)["lastContactedAt"], .int(1_000))
        edit.lastContactedAt = 0
        XCTAssertEqual(edit.changes(from: contact)["lastContactedAt"], .int(0))
        XCTAssertNil(CrmContactEdit().changes(from: nil)["lastContactedAt"])
    }

    @MainActor func testModelDraftsColumnsZohoAndTimeZone() async throws {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "installed": true, "role": "owner"]]])
        transport.on("GET", "/api/crm/acme", summary())
        let client = PackagesClient(transport: transport)
        let model = CrmModel(client: client)
        model.now = { Date(timeIntervalSince1970: 1_791_417_600) }
        await model.refresh()
        let query = try XCTUnwrap(transport.last("GET", "/api/crm/acme")?.query)
        XCTAssertEqual(query.first?.0, "tz")
        XCTAssertEqual(model.noteDrafts.count, 2)

        transport.on("PATCH", "/api/crm/acme/contacts/" + hexB, ["contact": [
            "id": .string(hexB), "name": "Bob", "flow": "customers", "stage": "lead", "notes": "DRAFT, sent today.",
        ]])
        let marked = await model.markNoteDraftSent(hexB, stampContacted: true)
        XCTAssertTrue(marked)
        let body = try XCTUnwrap(transport.last("PATCH", "/api/crm/acme/contacts/" + hexB)?.body)
        XCTAssertTrue(body.str("notes").hasPrefix("DRAFT, sent 2026-10-0"))
        XCTAssertEqual(body.int64("lastContactedAt"), 1_791_417_600_000)
        XCTAssertEqual(model.noteDrafts.map(\.contactId), [hexA])

        transport.on("POST", "/api/crm/acme/emails/send", ["email": ["id": "e9", "contactId": .string(hexA), "status": "sent"]])
        transport.on("PATCH", "/api/crm/acme/contacts/" + hexA, ["contact": [
            "id": .string(hexA), "name": "Ada", "flow": "customers", "stage": "lead", "notes": "DRAFT, sent.",
        ]])
        let draft = model.noteDrafts[0].emailDraft(model.contact(hexA))
        _ = await model.sendEmail(draft)
        XCTAssertNotNil(transport.last("PATCH", "/api/crm/acme/contacts/" + hexA))
        XCTAssertTrue(model.noteDrafts.isEmpty)

        transport.on("PUT", "/api/crm/acme/settings", ["settings": ["removedColumns": ["twitter"]]])
        let restored = await model.restoreColumn("phone")
        XCTAssertTrue(restored)
        XCTAssertEqual(transport.last("PUT", "/api/crm/acme/settings")?.body?.strings("removedColumns"), ["twitter"])
        XCTAssertEqual(model.data?.settings.removedColumns, ["twitter"])

        var code = CrmZohoCode()
        let incomplete = await model.exchangeZoho(code)
        XCTAssertFalse(incomplete)
        XCTAssertEqual(model.errorText, "Enter the Self Client's client ID, its client secret and the code.")
        code.clientId = "1000.X"
        code.clientSecret = "s"
        code.code = "1000.c"
        code.region = "eu"
        transport.on("POST", "/api/crm/acme/zoho/exchange", status: 400, ["error": "zoho_code_rejected"])
        let rejected = await model.exchangeZoho(code)
        XCTAssertFalse(rejected)
        XCTAssertTrue(model.errorText.hasPrefix("Zoho rejected the code"))
        transport.routes["POST /api/crm/acme/zoho/exchange"] = nil
        transport.on("POST", "/api/crm/acme/zoho/exchange", ["settings": ["zoho": ["connected": true, "region": "eu"]]])
        let connected = await model.exchangeZoho(code)
        XCTAssertTrue(connected)
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/zoho/exchange")?.body?.str("region"), "eu")
        XCTAssertTrue(model.data?.settings.zoho.connected ?? false)
    }
}
