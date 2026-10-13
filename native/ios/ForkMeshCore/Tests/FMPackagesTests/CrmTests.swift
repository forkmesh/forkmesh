import FMBase
import XCTest

@testable import FMPackages

final class CrmTests: XCTestCase {
    func contactJSON(_ id: String, name: String, stage: String, flow: String = "customers", value: Int64 = 0) -> JSONValue {
        .object([
            "id": .string(id), "organization": "acme", "name": .string(name), "email": .string(name.lowercased() + "@x.test"),
            "company": "Acme", "title": "CTO", "flow": .string(flow), "stage": .string(stage), "value": .int(value),
            "tags": ["vip"],
        ])
    }

    func summaryJSON(revision: String = "r1") -> JSONObject {
        [
            "organization": "acme", "role": "owner", "canManageSettings": true,
            "contacts": [
                contactJSON(hexA, name: "Ada", stage: "lead", value: 100),
                contactJSON(hexB, name: "Bob", stage: "customer", value: 50),
                contactJSON(String(repeating: "c", count: 32), name: "Cy", stage: "mystery"),
                contactJSON("not-hex", name: "Bad", stage: "lead"),
            ],
            "emails": [
                ["id": "e1", "contactId": .string(hexA), "status": "failed", "subject": "Hi", "createdAt": 5],
                ["id": "e2", "contactId": .string(hexA), "status": "sent", "subject": "Again", "sentAt": 9],
            ],
            "settings": ["flows": [["id": "customers", "name": "Customers", "stages": ["lead", "customer"]]]],
            "members": ["alice", ["name": "bob"]],
            "summary": ["contacts": 3, "customers": 1, "sent": 2, "failed": 1],
            "revision": .string(revision),
        ]
    }

    func testSummaryParsingAndBoard() {
        let data = CrmOrganizationData(json: summaryJSON())
        XCTAssertEqual(data.contacts.count, 3)
        XCTAssertEqual(data.members, ["alice", "bob"])
        let flow = data.flow(nil)
        let columns = data.stageColumns(flow)
        XCTAssertEqual(columns.map(\.stage), ["lead", "customer", "other"])
        XCTAssertEqual(columns[0].contacts.map(\.name), ["Ada"])
        XCTAssertEqual(data.stageValue(flow, stage: "lead"), 100)
        XCTAssertEqual(data.activity(for: hexA).map(\.id), ["e2", "e1"])
        XCTAssertTrue(data.activity.last!.failed)
        XCTAssertEqual(data.stageColumns(flow, query: "bob")[1].contacts.map(\.name), ["Bob"])
    }

    func testFallbackFlowWhenSettingsEmpty() {
        let data = CrmOrganizationData(json: ["contacts": []])
        XCTAssertEqual(data.flows, [CrmFlow.fallback])
    }

    func testContactRouteIds() {
        let ref = CrmContactRef(routeId: "acme/" + hexA)
        XCTAssertEqual(ref?.organization, "acme")
        XCTAssertEqual(ref?.routeId, "acme/" + hexA)
        XCTAssertNil(CrmContactRef(routeId: hexA))
        XCTAssertNil(CrmContactRef(routeId: "Acme/" + hexA))
        XCTAssertNil(CrmContactRef(routeId: "acme/" + hexA + "/x"))
    }

    func testEditChangesOnlySendsDifferences() {
        let contact = CrmContact(json: contactJSON(hexA, name: "Ada", stage: "lead").objectValue!)
        var edit = CrmContactEdit(contact: contact)
        XCTAssertTrue(edit.changes(from: contact).isEmpty)
        edit.stage = "customer"
        edit.tags = "vip, Hot , vip, "
        let changes = edit.changes(from: contact)
        XCTAssertEqual(changes.str("stage"), "customer")
        XCTAssertEqual(changes.strings("tags"), ["vip", "Hot"])
        XCTAssertNil(changes["name"])
        let created = CrmContactEdit().changes(from: nil)
        XCTAssertEqual(created.str("flow"), "customers")
        XCTAssertFalse(CrmContactEdit().isValid)
    }

    @MainActor func testModelUsesRevisionAndKeepsDataInMemoryOnly() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "role": "owner", "installed": true], ["name": "Bad"]]])
        transport.on("GET", "/api/crm/acme", summaryJSON())
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        XCTAssertEqual(model.organization, "acme")
        XCTAssertEqual(model.organizations.map(\.name), ["acme"])
        XCTAssertEqual(model.data?.contacts.count, 3)
        transport.routes["GET /api/crm/acme"] = [PackagesReply(status: 200, json: ["unchanged": true, "revision": "r1"])]
        await model.load()
        XCTAssertEqual(transport.last("GET", "/api/crm/acme")?.query.first?.1, "r1")
        XCTAssertEqual(model.data?.contacts.count, 3)
        model.forget()
        XCTAssertNil(model.data)
    }

    @MainActor func testMoveContactPatchesStage() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "installed": true]]])
        transport.on("GET", "/api/crm/acme", summaryJSON())
        transport.on("PATCH", "/api/crm/acme/contacts/" + hexA, ["contact": contactJSON(hexA, name: "Ada", stage: "customer")])
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        let ada = model.contact(hexA)!
        let moved = await model.move(ada, toStage: "customer")
        XCTAssertTrue(moved)
        XCTAssertEqual(transport.last("PATCH", "/api/crm/acme/contacts/" + hexA)?.body, ["stage": "customer"])
        XCTAssertEqual(model.contact(hexA)?.stage, "customer")
    }

    @MainActor func testDeleteContactRemovesIt() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "installed": true]]])
        transport.on("GET", "/api/crm/acme", summaryJSON())
        transport.on("DELETE", "/api/crm/acme/contacts/" + hexA, ["deleted": true])
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        let deleted = await model.delete(model.contact(hexA)!)
        XCTAssertTrue(deleted)
        XCTAssertNotNil(transport.last("DELETE", "/api/crm/acme/contacts/" + hexA))
        XCTAssertNil(model.contact(hexA))
        XCTAssertEqual(model.data?.contacts.count, 2)
    }

    @MainActor func testDeleteFailureKeepsContact() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "installed": true]]])
        transport.on("GET", "/api/crm/acme", summaryJSON())
        transport.on("DELETE", "/api/crm/acme/contacts/" + hexA, status: 404, ["error": "contact_not_found"])
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        let deleted = await model.delete(model.contact(hexA)!)
        XCTAssertFalse(deleted)
        XCTAssertNotNil(model.contact(hexA))
        XCTAssertFalse(model.errorText.isEmpty)
    }

    @MainActor func testOwnerInstallsCrmForOrganization() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "role": "owner", "installed": false]]])
        transport.on("GET", "/api/crm/acme", status: 409, ["error": "plugin_not_installed"])
        transport.on("GET", "/api/crm/acme", summaryJSON())
        transport.on("POST", "/api/plugins/acme/crm/install", ["organization": "acme", "plugins": []])
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        XCTAssertTrue(model.needsInstall)
        XCTAssertTrue(model.canInstall)
        let installed = await model.install()
        XCTAssertTrue(installed)
        XCTAssertNotNil(transport.last("POST", "/api/plugins/acme/crm/install"))
        XCTAssertFalse(model.needsInstall)
        XCTAssertEqual(model.data?.contacts.count, 3)
        XCTAssertEqual(model.errorText, "")
    }

    @MainActor func testMemberCannotInstall() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "role": "member", "installed": false]]])
        transport.on("GET", "/api/crm/acme", status: 409, ["error": "plugin_not_installed"])
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        XCTAssertTrue(model.needsInstall)
        XCTAssertFalse(model.canInstall)
        let installed = await model.install()
        XCTAssertFalse(installed)
        XCTAssertNil(transport.last("POST", "/api/plugins/acme/crm/install"))
    }

    @MainActor func testNotInstalledShowsReason() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "installed": false]]])
        transport.on("GET", "/api/crm/acme", status: 409, ["error": "plugin_not_installed"])
        let model = CrmModel(client: PackagesClient(transport: transport))
        await model.refresh()
        XCTAssertEqual(model.errorText, "This package is not installed for that organization yet.")
    }
}
