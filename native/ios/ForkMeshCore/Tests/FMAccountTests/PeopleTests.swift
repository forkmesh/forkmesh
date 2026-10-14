import FMBase
import Foundation
import XCTest
@testable import FMAccount

final class PeopleRingsTests: XCTestCase {
    static func fixture() throws -> JSONObject {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { url.deleteLastPathComponent() }
        url.appendPathComponent("marketplace/packages/people/tests/fixtures/rings.json")
        let value = try JSON.parse(Data(contentsOf: url))
        return try XCTUnwrap(value.objectValue)
    }

    func testEveryCaseMatchesTheSharedFixture() throws {
        let fixture = try Self.fixture()
        let now = fixture.int64("now")
        XCTAssertEqual(fixture.strings("rings"), PeopleRing.allCases.map(\.rawValue))
        for item in fixture["cases"]?.objects ?? [] {
            let user = item["user"]?.objectValue ?? JSONObject()
            let member = PeopleMember.parse(user)
            guard let expected = item["member"]?.objectValue else {
                XCTAssertNil(member, "\(user.str("name")) should be skipped")
                continue
            }
            let parsed = try XCTUnwrap(member)
            XCTAssertEqual(parsed.name, expected.str("name"))
            XCTAssertEqual(parsed.contributions, expected.int("contributions"))
            XCTAssertEqual(parsed.nodeCount, expected.int("nodeCount"))
            XCTAssertEqual(parsed.hasPayoutAddress, expected.bool("hasPayoutAddress"))
            XCTAssertEqual(parsed.avatar, expected.str("avatar"))
            XCTAssertEqual(PeopleRings.score(parsed), item.int("score"), parsed.name)
            XCTAssertEqual(PeopleRings.ring(parsed, now: now).rawValue, item.str("ring"), parsed.name)
        }
    }

    func testSectionsKeepTheFixtureOrder() throws {
        let fixture = try Self.fixture()
        let members = (fixture["cases"]?.objects ?? []).compactMap { PeopleMember.parse($0["user"]?.objectValue ?? JSONObject()) }
        let sections = PeopleDirectory.sections(members, now: fixture.int64("now"))
        let expected = fixture["order"]?.objects ?? []
        XCTAssertEqual(sections.map(\.ring.rawValue), expected.map { $0.str("ring") })
        for (section, want) in zip(sections, expected) {
            XCTAssertEqual(section.people.map(\.member.name), want.strings("names"))
        }
    }

    func testLayoutMatchesTheFixture() throws {
        for layout in try Self.fixture()["layout"]?.objects ?? [] {
            let size = layout.double("size"), seat = layout.double("seat"), label = layout.double("label")
            let radii = layout["radii"]?.arrayOrEmpty.map { $0.doubleValue ?? 0 } ?? []
            let capacities = layout["capacities"]?.arrayOrEmpty.map { $0.intValue ?? 0 } ?? []
            for ring in 0..<PeopleRing.allCases.count {
                XCTAssertEqual(PeopleRingLayout.radius(ring: ring, size: size), radii[ring], accuracy: 0.0001)
                XCTAssertEqual(PeopleRingLayout.capacity(ring: ring, size: size, seat: seat, label: label), capacities[ring])
            }
        }
    }

    func testAFullRingEndsWithAnOverflowSeat() {
        let people = (0..<40).map { PeopleMember(name: "p\($0)", activityBucket: "120d") }
        let sections = PeopleDirectory.sections(people, now: 0)
        let seats = PeopleRingLayout.seats(sections, size: 360, seat: 30, label: 72)
        let capacity = PeopleRingLayout.capacity(ring: PeopleRing.embers.order, size: 360, seat: 30, label: 72)
        XCTAssertEqual(seats.count, capacity)
        XCTAssertEqual(seats.last?.person, nil)
        XCTAssertEqual(seats.last?.overflow, 40 - (capacity - 1))
        for seat in seats {
            let distance = ((seat.x - 180) * (seat.x - 180) + (seat.y - 180) * (seat.y - 180)).squareRoot()
            XCTAssertEqual(distance, PeopleRingLayout.radius(ring: 4, size: 360), accuracy: 0.001)
            XCTAssertGreaterThan(seat.y - 180, -PeopleRingLayout.radius(ring: 4, size: 360) * cos(PeopleRingLayout.labelGap(ring: 4, size: 360, seat: 30, label: 72)) - 0.001)
        }
    }

    func testFilterMatchesNamesWithoutTheAtSign() {
        let sections = PeopleDirectory.sections([PeopleMember(name: "Ada"), PeopleMember(name: "bob")], now: 0)
        let filtered = PeopleDirectory.filter(sections, query: " @ad ")
        XCTAssertEqual(filtered.flatMap(\.people).map(\.member.name), ["Ada"])
    }
}

@MainActor
final class PeopleModelTests: XCTestCase {
    func testLoadDropsDuplicatesAndNodes() async {
        let api = FakeAccountEndpoints()
        api.answers["GET \(PeopleModel.directoryPath)"] = .success([
            "ok": true,
            "users": [["name": "ada", "activityBucket": "hour"], ["name": "Ada"], ["name": "n1", "kind": "node"]],
        ])
        let model = PeopleModel(client: api, viewer: { "Ada" }, clock: FixedClock(epochMillis: 0))
        await model.load()
        XCTAssertTrue(model.loaded)
        XCTAssertEqual(model.members.map(\.name), ["ada"])
        XCTAssertEqual(model.viewerName, "ada")
        model.toggleFocus(.glowing)
        XCTAssertEqual(model.visibleSections.map(\.ring), [.glowing])
        model.toggleFocus(.glowing)
        XCTAssertEqual(model.visibleSections.count, PeopleRing.allCases.count)
    }

    func testLoadFailureKeepsAnError() async {
        let api = FakeAccountEndpoints()
        api.answers["GET \(PeopleModel.directoryPath)"] = .failure(AccountEndpointError("offline"))
        let model = PeopleModel(client: api, viewer: { "" })
        await model.load()
        XCTAssertFalse(model.loaded)
        XCTAssertFalse(model.error.isEmpty)
    }

    func testProfileAsksAsTheViewerAndFollowsAndUnfollows() async {
        let api = FakeAccountEndpoints()
        api.answers["GET /api/accounts/bob?viewer=ada"] = .success([
            "ok": true, "name": "bob", "profileBio": "hi", "createdAt": 5,
            "social": ["followers": 2, "following": 1, "isFollowing": false],
        ])
        api.answers["POST /api/accounts/bob/follow"] = .success(["ok": true, "followers": 3, "isFollowing": true])
        api.answers["DELETE /api/accounts/bob/follow"] = .success(["ok": true, "followers": 2, "isFollowing": false])
        let model = PeopleProfileModel(name: "Bob", viewer: "Ada", client: api)
        await model.load()
        XCTAssertEqual(model.profile?.bio, "hi")
        XCTAssertEqual(model.profile?.followers, 2)
        XCTAssertFalse(model.isSelf)
        await model.toggleFollow()
        XCTAssertEqual(model.profile?.isFollowing, true)
        XCTAssertEqual(model.profile?.followers, 3)
        await model.toggleFollow()
        XCTAssertEqual(model.profile?.isFollowing, false)
        XCTAssertEqual(api.calls.map { "\($0.method) \($0.path)" }, [
            "GET /api/accounts/bob?viewer=ada", "POST /api/accounts/bob/follow", "DELETE /api/accounts/bob/follow",
        ])
    }

    func testNobodyFollowsThemselvesOrWhileSignedOut() async {
        let api = FakeAccountEndpoints()
        api.answers["GET /api/accounts/ada?viewer=ada"] = .success(["ok": true, "name": "ada"])
        let me = PeopleProfileModel(name: "ada", viewer: "ada", client: api)
        await me.load()
        XCTAssertTrue(me.isSelf)
        await me.toggleFollow()
        api.answers["GET /api/accounts/bob"] = .success(["ok": true, "name": "bob"])
        let guest = PeopleProfileModel(name: "bob", viewer: "", client: api)
        await guest.load()
        await guest.toggleFollow()
        XCTAssertEqual(api.calls.map(\.method), ["GET", "GET"])
    }

    func testUnverifiedEmailExplainsTheRefusal() async {
        let api = FakeAccountEndpoints()
        api.answers["GET /api/accounts/bob?viewer=ada"] = .success(["ok": true, "name": "bob"])
        api.answers["POST /api/accounts/bob/follow"] = .failure(AccountEndpointError("email_not_verified", status: 403))
        let model = PeopleProfileModel(name: "bob", viewer: "ada", client: api)
        await model.load()
        await model.toggleFollow()
        XCTAssertEqual(model.actionError, "Verify your email to follow people.")
        XCTAssertEqual(model.profile?.isFollowing, false)
    }
}
