import FMBase
import XCTest
@testable import FMControl

final class AgentPermissionModeTests: XCTestCase {
    func testLabelsMatchTheDesktopModes() {
        XCTAssertEqual(AgentPermissionMode.allCases.map(\.rawValue), ["Auto", "Ask", "Plan", "Edit"])
        XCTAssertEqual(AgentPermissionMode(label: " plan "), .plan)
        XCTAssertEqual(AgentPermissionMode(label: "Auto mode"), .auto)
        XCTAssertNil(AgentPermissionMode(label: "bypassPermissions"))
    }

    func testResolveTakesTheFirstKnownLabel() {
        XCTAssertEqual(AgentPermissionMode.resolve("", "Edit"), .edit)
        XCTAssertEqual(AgentPermissionMode.resolve("Plan", "Edit"), .plan)
        XCTAssertEqual(AgentPermissionMode.resolve("", ""), .auto)
    }

    func testOnlyCliProvidersTakeAMode() {
        XCTAssertTrue(AgentPermissionMode.providerTakesMode("claude-code"))
        XCTAssertTrue(AgentPermissionMode.providerTakesMode("codex"))
        XCTAssertTrue(AgentPermissionMode.providerTakesMode(""))
        XCTAssertFalse(AgentPermissionMode.providerTakesMode("local-llama"))
    }

    func testTheChoiceIsRemembered() {
        let store = InMemoryKeyValueStore()
        XCTAssertEqual(AgentPermissionMode.read(store), .auto)
        AgentPermissionMode.plan.write(to: store)
        XCTAssertEqual(AgentPermissionMode.read(store), .plan)
    }
}
