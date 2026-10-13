import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMDesktopRemote

final class FleetStoreTests: XCTestCase {
    static let summaryJSON: JSONObject = [
        "costs": [
            "hosts": 3, "vultr": 2, "monthlyCost": 15.0,
            "byRegion": [["region": "ewr", "count": 2, "monthlyCost": 15.0]],
            "byPlan": [["plan": "vc2-1c-2gb", "count": 1, "monthlyCost": 10.0]],
        ],
        "fleet": [
            "available": true, "enabled": true, "desired": 3, "status": "Target: 3", "nextCheckInMs": 125000,
            "reconciling": false,
        ],
        "provision": [
            "active": true, "node": "mirror7", "stage": 4, "stageCount": 6, "state": "active",
            "detail": "Waiting for SSH", "resumable": false,
            "stages": [
                ["number": 1, "name": "Credentials", "status": "done"],
                ["number": 2, "name": "Plan + image", "status": "done"],
                ["number": 3, "name": "Create server", "status": "done"],
                ["number": 4, "name": "Boot + connect", "status": "active", "detail": "probe 3"],
                ["number": 5, "name": "Install", "status": "pending"],
                ["number": 6, "name": "Live traffic", "status": "pending"],
            ],
        ],
        "gateways": [
            ["host": "mirror2", "downSinceMs": 1000, "downForMs": 7_200_000, "retired": false],
            ["host": "mirror1", "downSinceMs": 10, "downForMs": 180_000_000, "retired": true],
        ],
        "mirrorClient": ["status": "Running", "health": "Healthy"],
        "mirrorPush": ["pushing": 1, "pending": 0],
        "canCreate": true, "busy": true, "deploying": 0, "admin": true,
        "nodeDelete": ["inProgress": false, "target": ""],
        "running": ["agentClis:mirror3"],
    ]

    @MainActor func testSummaryParses() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["fleet.summary"] = Self.summaryJSON
        let store = FleetStore(channel: channel)
        await store.refresh()
        let summary = try XCTUnwrap(store.summary)
        XCTAssertEqual(summary.costs.line, "2 Vultr servers · $15.00/month")
        XCTAssertEqual(summary.costs.yearlyCost, 180)
        XCTAssertEqual(summary.fleet.line, "Keeps 3 healthy mirrors · next check in 2m 5s")
        XCTAssertEqual(summary.provision.headline, "Deploying mirror7: Boot + connect")
        XCTAssertEqual(summary.provision.progress, 0.5, accuracy: 0.001)
        XCTAssertTrue(summary.provision.isVisible)
        XCTAssertEqual(summary.gatewaysDown, 1)
        XCTAssertEqual(summary.gateways[1].line, "Down 2d 2h · retired from the automatic push")
        XCTAssertEqual(summary.mirrorClientStatus, "Running")
        XCTAssertTrue(store.isRunning(action: "agentClis", subject: "mirror3"))
    }

    @MainActor func testUnsupportedDesktopIsRecognised() async {
        let channel = FakeRemoteChannel()
        channel.failures["fleet.summary"] = LocalControlException("unsupported")
        let store = FleetStore(channel: channel)
        await store.refresh()
        XCTAssertTrue(store.unsupported)
        XCTAssertEqual(store.error, "This desktop's ForkMesh is too old for that. Update it.")
    }

    @MainActor func testPrepareAndRunCarryTheTokenAndTypedPhrase() async throws {
        let channel = FakeRemoteChannel()
        channel.replies["fleet.prepare"] = [
            "token": "tok", "action": "deleteNode", "subject": "mirror4", "summary": "Delete",
            "typeToConfirm": "DELETE mirror4",
        ]
        channel.replies["fleet.run"] = ["started": true, "action": "deleteNode", "subject": "mirror4", "message": "Deleting"]
        let store = FleetStore(channel: channel)
        let confirmation = try await store.prepare(.deleteNode("mirror4"))
        XCTAssertEqual(channel.last("fleet.prepare")?.str("host"), "mirror4")
        XCTAssertTrue(confirmation.needsTypedConfirmation)
        XCTAssertTrue(confirmation.needsDeviceAuthentication)
        do {
            try await store.run(confirmation, typed: "delete mirror4")
            XCTFail("a wrong phrase must not reach the desktop")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "name_mismatch")
        }
        XCTAssertEqual(channel.count("fleet.run"), 0)
        let message = try await store.run(confirmation, typed: " DELETE mirror4 ")
        XCTAssertEqual(message, "Deleting")
        let sent = try XCTUnwrap(channel.last("fleet.run"))
        XCTAssertEqual(sent.str("token"), "tok")
        XCTAssertEqual(sent.str("action"), "deleteNode")
        XCTAssertEqual(sent.str("host"), "mirror4")
        XCTAssertEqual(sent.str("confirmText"), "DELETE mirror4")
        XCTAssertTrue(store.running.contains("deleteNode:mirror4"))
    }

    @MainActor func testActionBellsRecordOutcomesAndChangedRefreshes() async {
        let channel = FakeRemoteChannel()
        channel.replies["fleet.summary"] = Self.summaryJSON
        var clock = 1000
        let store = FleetStore(channel: channel, now: { clock })
        channel.emit("fleet.action", ["action": "agentClis", "subject": "mirror3", "ok": true, "message": "Installed"])
        XCTAssertEqual(store.outcomes.first?.message, "Installed")
        XCTAssertEqual(store.outcomes.first?.atMs, 1000)
        clock = 2000
        channel.emit("hosts.changed")
        await settle()
        XCTAssertEqual(channel.count("fleet.summary"), 0, "other families' bells are ignored")
        channel.emit("fleet.changed")
        await settle()
        XCTAssertEqual(channel.count("fleet.summary"), 1)
        store.stop()
        XCTAssertEqual(channel.tapCount, 0)
    }

    func testActionParams() {
        XCTAssertEqual(FleetAction.fleetTarget(enabled: true, desired: 4).params["desired"], .int(4))
        XCTAssertEqual(FleetAction.deployAll(.reinstall).params.str("mode"), "reinstall")
        XCTAssertEqual(FleetAction.actions(host: "m1", enabled: false).params["enabled"], .bool(false))
        XCTAssertEqual(FleetAction.add(name: " m9 ", address: "1.2.3.4", user: "root").params.str("host"), "m9")
        XCTAssertTrue(FleetAction.deployAll(.reinstall).isDestructive)
        XCTAssertFalse(FleetAction.deployAll(.install).isDestructive)
        XCTAssertTrue(FleetInput.validLinkCode("012345"))
        XCTAssertFalse(FleetInput.validLinkCode("12a456"))
        XCTAssertTrue(FleetInput.validHostName("mirror-9"))
        XCTAssertFalse(FleetInput.validHostName("-bad"))
        XCTAssertTrue(FleetInput.validAddress("2001:db8::1"))
        XCTAssertFalse(FleetInput.validAddress("1.2.3.4; reboot"))
        XCTAssertTrue(FleetInput.validUser("root"))
        XCTAssertFalse(FleetInput.validUser("Root"))
    }

    func testDiskUsage() {
        let usage = DiskUsage(json: [
            "host": "m1", "path": "/var/lib",
            "entries": [
                ["name": ".", "path": "/var/lib", "bytes": 1000.0, "total": true],
                ["name": "forkmesh", "path": "/var/lib/forkmesh", "bytes": 750.0],
            ],
        ])
        XCTAssertEqual(usage.total, 1000)
        XCTAssertEqual(usage.children.count, 1)
        XCTAssertEqual(usage.share(of: usage.children[0]), 0.75)
        XCTAssertEqual(usage.parentPath, "/var")
        XCTAssertNil(DiskUsage(json: ["path": "/"]).parentPath)
    }

    func testFormatting() {
        XCTAssertEqual(RemoteFormat.bytes(512), "512 B")
        XCTAssertEqual(RemoteFormat.bytes(1536), "1.5 KB")
        XCTAssertEqual(RemoteFormat.bytes(-1), "—")
        XCTAssertEqual(RemoteFormat.duration(ms: 59_000), "59s")
        XCTAssertEqual(RemoteFormat.duration(ms: 3_600_000), "1h 0m")
        XCTAssertEqual(RemoteFormat.money(5), "$5.00")
    }
}
