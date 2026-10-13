// Gate 2: a family client prefixes its methods and sees only its own bells.

import FMBase
import FMChat
import Foundation
import XCTest

@testable import FMControl

@MainActor
final class FakeControlChannel: DesktopControlCalling {
    var requests: [(String, JSONObject)] = []
    var reply: JSONObject = [:]
    var failure: Error?
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        if let failure { throw failure }
        return reply
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    var tapCount: Int { taps.count }

    func emit(_ event: String, _ data: JSONObject = [:]) {
        for tap in taps.values { tap(event, data) }
    }
}

final class DesktopControlFamilyTests: XCTestCase {
    @MainActor func testCallPrefixesTheMethod() async throws {
        let channel = FakeControlChannel()
        channel.reply = ["ok": .bool(true)]
        let family = DesktopControlFamily(.teammates, channel: channel)
        let reply = try await family.call("list", ["limit": .int(5)])
        XCTAssertEqual(channel.requests.first?.0, "teammates.list")
        XCTAssertEqual(reply["ok"], .bool(true))
        _ = try await family.call("teammates.get")
        XCTAssertEqual(channel.requests.last?.0, "teammates.get", "an already-prefixed name is kept")
    }

    @MainActor func testEventsAreFilteredToTheFamily() async {
        let channel = FakeControlChannel()
        let family = DesktopControlFamily(.fleet, channel: channel)
        var seen: [String] = []
        let id = family.addEventTap { suffix, _ in seen.append(suffix) }
        channel.emit("fleet.changed")
        channel.emit("agents.changed")
        channel.emit("files.browse.changed")
        XCTAssertEqual(seen, ["changed"])
        family.removeEventTap(id)
        XCTAssertEqual(channel.tapCount, 0)
    }

    @MainActor func testOwningPicksTheLongestPrefix() async {
        XCTAssertEqual(DesktopControlFamilyName.owning("files.browse.list"), .filesBrowse)
        XCTAssertEqual(DesktopControlFamilyName.owning("agentops.approve"), .agentOps)
        XCTAssertNil(DesktopControlFamilyName.owning("agents.list"))
        XCTAssertEqual(DesktopControlFamilyName.owning("stats.ratchet.prepare"), .stats)
    }

    @MainActor func testUnsupportedIsRecognised() async {
        XCTAssertTrue(DesktopControlFamily.isUnsupported(LocalControlException("unsupported")))
        XCTAssertFalse(DesktopControlFamily.isUnsupported(LocalControlException("unknown_session")))
    }
}
