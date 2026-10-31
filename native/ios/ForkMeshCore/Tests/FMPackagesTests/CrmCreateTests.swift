import FMBase
import XCTest

@testable import FMPackages

final class CrmCreateTests: XCTestCase {
    let request = String(repeating: "d", count: 32)

    func person(_ id: String, name: String) -> JSONValue {
        .object([
            "id": .string(id), "organization": "acme", "name": .string(name), "email": .string(name.lowercased() + "@x.test"),
            "company": "Acme", "flow": "customers", "stage": "lead", "tags": ["vip"],
        ])
    }

    @MainActor func loaded(_ transport: FakePackagesTransport) async -> CrmModel {
        transport.on("GET", "/api/crm", ["organizations": [["name": "acme", "role": "owner", "installed": true]]])
        transport.on("GET", "/api/crm/acme", [
            "organization": "acme", "role": "owner", "contacts": [person(hexA, name: "Ada")],
            "settings": ["flows": [["id": "customers", "name": "Customers", "stages": ["lead", "customer"]]]],
            "revision": "r1",
        ])
        let model = CrmModel(client: PackagesClient(transport: transport))
        model.retryDelays = [0, 0]
        await model.refresh()
        return model
    }

    func edit(name: String, email: String) -> CrmContactEdit {
        var edit = CrmContactEdit()
        edit.name = name
        edit.email = email
        return edit
    }

    @MainActor func testCreateSendsTheRequestIdAndRetriesAServerBlip() async {
        let transport = FakePackagesTransport()
        let model = await loaded(transport)
        transport.routes["POST /api/crm/acme/contacts"] = [
            PackagesReply(status: 503, json: ["error": "unavailable"]),
            PackagesReply(status: 201, json: ["contact": person(request, name: "Dee")]),
        ]
        let saved = await model.save(edit(name: "Dee", email: "dee@x.test"), original: nil, requestId: request)
        XCTAssertEqual(saved?.id, request)
        XCTAssertEqual(transport.paths.filter { $0 == "POST /api/crm/acme/contacts" }.count, 2)
        XCTAssertEqual(transport.last("POST", "/api/crm/acme/contacts")?.body?["requestId"], .string(request))
        XCTAssertEqual(model.contact(request)?.name, "Dee")
        XCTAssertEqual(model.errorText, "")
    }

    @MainActor func testCreateWithoutARequestIdIsNotRetried() async {
        let transport = FakePackagesTransport()
        let model = await loaded(transport)
        transport.on("POST", "/api/crm/acme/contacts", status: 503, ["error": "unavailable"])
        let saved = await model.save(edit(name: "Dee", email: ""), original: nil)
        XCTAssertNil(saved)
        XCTAssertEqual(transport.paths.filter { $0 == "POST /api/crm/acme/contacts" }.count, 1)
        XCTAssertFalse(model.errorText.isEmpty)
    }

    @MainActor func testAnEmailAlreadyInTheCrmNamesThatContact() async {
        let transport = FakePackagesTransport()
        let model = await loaded(transport)
        transport.on(
            "POST", "/api/crm/acme/contacts", status: 409,
            ["error": "contact_email_exists", "contact": person(hexA, name: "Ada")])
        let saved = await model.save(edit(name: "Ada L", email: "ada@x.test"), original: nil, requestId: request)
        XCTAssertNil(saved)
        XCTAssertEqual(model.existing?.id, hexA)
        XCTAssertEqual(model.existing?.company, "Acme")
        XCTAssertTrue(model.errorText.hasPrefix("Ada already uses that email address"))
        XCTAssertEqual(transport.paths.filter { $0 == "POST /api/crm/acme/contacts" }.count, 1)
    }

    @MainActor func testAddDetailsFillsOnlyBlanksAndAddsTheList() async {
        let transport = FakePackagesTransport()
        let model = await loaded(transport)
        transport.on("PATCH", "/api/crm/acme/contacts/" + hexA, ["contact": person(hexA, name: "Ada")])
        var details = edit(name: "Ada L", email: "ada@x.test")
        details.phone = "+1 555"
        details.company = "Other"
        details.tags = "vip, new"
        let found = model.contact(hexA)!
        model.existing = found
        let saved = await model.addDetails(details, to: found, addTo: hexB)
        XCTAssertNotNil(saved)
        XCTAssertNil(model.existing)
        let body = transport.last("PATCH", "/api/crm/acme/contacts/" + hexA)?.body
        XCTAssertEqual(body?["phone"], .string("+1 555"))
        XCTAssertNil(body?["company"])
        XCTAssertNil(body?["name"])
        XCTAssertNil(body?["stage"])
        XCTAssertEqual(body?["addTags"], .array([.string("new")]))
        XCTAssertEqual(body?["views"], .array([.string(hexB)]))
    }

    @MainActor func testShowingASectionPicksAMatchingTab() async {
        let transport = FakePackagesTransport()
        let model = await loaded(transport)
        model.show(.activity)
        XCTAssertEqual(model.activeTab, .activity)
        model.show(.contacts)
        XCTAssertTrue(model.activeTab.isContacts)
        model.show(.activity)
        model.show(.pipeline)
        XCTAssertEqual(model.activeFlow?.id, "customers")
        XCTAssertEqual(model.section, .pipeline)
    }

    func testRequestIdsAreHex() {
        XCTAssertTrue(PackagesText.isHexId(CrmModel.newRequestId()))
    }

    func testIsTransient() {
        XCTAssertTrue(PackagesError.isTransient(PackagesError("x", status: 502)))
        XCTAssertFalse(PackagesError.isTransient(PackagesError("contact_email_exists", status: 409)))
    }
}
