import FMBase
import Foundation
import XCTest

@testable import FMTeammates

final class ShortcutStepsTests: XCTestCase {
    static let catalogJSON: JSONObject = [
        "repo": "f/r",
        "repos": ["f/r"],
        "entries": [
            [
                "file": "deploy-new-version.sh", "scope": "shortcut", "name": "Deploy new version", "kind": "script",
                "runnable": true,
                "steps": [
                    ["name": "Cut the new version"],
                    ["name": "Wait for the release candidate"],
                    ["name": "Reset build numbers to 1"],
                    ["name": "Deploy Android build", "shortcut": "deploy-build-android.sh"],
                    ["name": "Deploy Linux build", "shortcut": "deploy-build-linux.sh"],
                    ["name": "Deploy Windows build", "shortcut": "deploy-build-windows.sh"],
                    ["name": "Deploy macOS build", "shortcut": "deploy-new-version.sh"],
                ],
            ],
            [
                "file": "deploy-build-android.sh", "scope": "shortcut", "name": "Deploy Android build", "kind": "script",
                "steps": [["name": "Plan the build"], ["name": "Build, sign and publish"], ["name": "Check the homepage"]],
            ],
            ["file": "deploy-build-linux.sh", "scope": "repository", "name": "Linux skill"],
            ["file": "plain.sh", "scope": "shortcut", "name": "Plain", "kind": "script"],
        ],
    ]

    func testStepsDecodeInOrderWithTheirLinks() throws {
        let catalog = ShortcutCatalog(json: Self.catalogJSON)
        let deploy = try XCTUnwrap(catalog.entries.first { $0.file == "deploy-new-version.sh" })
        XCTAssertEqual(deploy.steps.count, 7)
        XCTAssertEqual(deploy.steps.map(\.number), Array(1...7))
        XCTAssertEqual(deploy.steps[0], ShortcutStep(number: 1, name: "Cut the new version"))
        XCTAssertEqual(deploy.steps[3], ShortcutStep(number: 4, name: "Deploy Android build", shortcut: "deploy-build-android.sh"))
        XCTAssertEqual(deploy.stepsText, "7 steps")
        XCTAssertEqual(catalog.entries.first { $0.file == "deploy-build-android.sh" }?.stepsText, "3 steps")
        XCTAssertEqual(catalog.entries.first { $0.file == "plain.sh" }?.steps, [])
        XCTAssertNil(catalog.entries.first { $0.file == "plain.sh" }?.stepsText)
    }

    func testLinksResolveOnlyToAnotherShortcutInTheSameScope() throws {
        let catalog = ShortcutCatalog(json: Self.catalogJSON)
        let deploy = try XCTUnwrap(catalog.entries.first { $0.file == "deploy-new-version.sh" })
        XCTAssertNil(catalog.linked(deploy.steps[0], from: deploy))
        XCTAssertEqual(catalog.linked(deploy.steps[3], from: deploy)?.name, "Deploy Android build")
        XCTAssertNil(catalog.linked(deploy.steps[4], from: deploy))
        XCTAssertNil(catalog.linked(deploy.steps[5], from: deploy))
        XCTAssertNil(catalog.linked(deploy.steps[6], from: deploy))
    }

    func testMissingOrMalformedStepsAreIgnored() {
        XCTAssertEqual(ShortcutEntry(json: ["file": "a.sh", "scope": "shortcut"]).steps, [])
        XCTAssertEqual(ShortcutEntry(json: ["file": "a.sh", "steps": "build"]).steps, [])
        XCTAssertEqual(ShortcutEntry(json: ["file": "a.sh", "steps": ["name": "x"]]).steps, [])
        XCTAssertEqual(ShortcutEntry(json: ["file": "a.sh", "steps": .null]).steps, [])
        let entry = ShortcutEntry(json: [
            "file": "a.sh",
            "steps": [
                3, true, .null, ["name": ""], ["name": "   "], ["name": 7], ["shortcut": 4], [:],
                ["name": "  Build\nand sign  ", "shortcut": 9],
                ["shortcut": " other.sh "],
                "Check the homepage",
            ],
        ])
        XCTAssertEqual(entry.steps, [
            ShortcutStep(number: 1, name: "Build and sign"),
            ShortcutStep(number: 2, name: "other.sh", shortcut: "other.sh"),
            ShortcutStep(number: 3, name: "Check the homepage"),
        ])
        XCTAssertEqual(entry.stepsText, "3 steps")
    }

    func testStepsStopAtThirtyTwo() {
        let many = (1...40).map { JSONValue.object(["name": .string("Step \($0)")]) }
        let entry = ShortcutEntry(json: ["file": "a.sh", "steps": .array(many)])
        XCTAssertEqual(entry.steps.count, ShortcutStep.maxSteps)
        XCTAssertEqual(entry.steps.last?.name, "Step 32")
    }

    func testCountAndVoiceOverText() {
        XCTAssertEqual(ShortcutStep.countText(1), "1 step")
        XCTAssertNil(ShortcutStep.countText(0))
        let step = ShortcutStep(number: 2, name: "Deploy Android build", shortcut: "deploy-build-android.sh")
        XCTAssertEqual(step.accessibilityLabel(of: 7, opens: true), "Step 2 of 7, Deploy Android build, opens shortcut")
        XCTAssertEqual(step.accessibilityLabel(of: 7, opens: false), "Step 2 of 7, Deploy Android build")
    }
}
