import FMApi
import FMBase
import FMCrypto
import FMNet
import XCTest

@testable import FMCode

final class IssueTaskMoveTests: XCTestCase {
    let issue = Issue(number: 7, title: "Mirror roster goes stale", body: "Steps to reproduce")

    func testTaskBodyCarriesTheIssueAndItsSource() {
        let body = IssueTaskMove.taskBody(owner: "alice", name: "demo", issue: issue, mover: "mona")
        XCTAssertEqual(body.str("title"), "Mirror roster goes stale")
        XCTAssertEqual(body.str("details"), "Steps to reproduce\n\nMoved from issue alice/demo#7 by mona.")
        XCTAssertEqual(body.str("repository"), "alice/demo")
        XCTAssertEqual(body.str("department"), "general")
        XCTAssertEqual(body.str("assigneeKind"), "unassigned")
    }

    func testTaskBodyFitsTheTaskLimits() {
        let long = Issue(number: 1, title: String(repeating: "t", count: 300), body: String(repeating: "b", count: 9000))
        let body = IssueTaskMove.taskBody(owner: "a b", name: "demo", issue: long, mover: "")
        XCTAssertEqual(body.str("title").count, IssueTaskMove.maxTitle)
        XCTAssertEqual(body.str("details").count, IssueTaskMove.maxDetails)
        XCTAssertEqual(body.str("repository"), "")
        XCTAssertTrue(body.str("details").hasSuffix("Moved from issue demo#1 by a repository manager."))
    }

    @MainActor func testMoveCreatesTheTaskThenDeletesTheIssue() async throws {
        let transport = FakeCollabTransport()
        let signer = CollabSigner(identity: try Identity.fromSeed([UInt8](repeating: 3, count: 32)), clock: FixedClock(epochMillis: 4))
        let writer = CollabRecordWriter(transport: transport, signer: signer, owner: "alice", name: "demo")
        var calls: [(String, String)] = []
        let task = try await IssueTaskMove.move(issue, owner: "alice", name: "demo", mover: "mona", writer: writer) { method, suffix, _ in
            calls.append((method, suffix))
            XCTAssertTrue(transport.requests.isEmpty)
            return ["ok": true, "task": .object(["id": "t1"])]
        }
        XCTAssertEqual(task.str("id"), "t1")
        XCTAssertEqual(calls.map(\.0), ["POST"])
        XCTAssertEqual(transport.requests.count, 1)
        XCTAssertEqual(transport.requests[0].url.path, "/api/repo/alice/demo/issues")
        XCTAssertEqual(transport.requests[0].body.obj("event").str("type"), "delete")
        XCTAssertEqual(transport.requests[0].body.obj("event").str("target"), "self")
    }

    @MainActor func testAFailedTaskKeepsTheIssue() async throws {
        let transport = FakeCollabTransport()
        let signer = CollabSigner(identity: try Identity.fromSeed([UInt8](repeating: 3, count: 32)), clock: FixedClock(epochMillis: 4))
        let writer = CollabRecordWriter(transport: transport, signer: signer, owner: "alice", name: "demo")
        do {
            try await IssueTaskMove.move(issue, owner: "alice", name: "demo", mover: "mona", writer: writer) { _, _, _ in
                throw OrgApiException("org_member_required", "org member required")
            }
            XCTFail("expected an error")
        } catch let error as IssueTaskMoveError {
            guard case .taskNotCreated = error else { return XCTFail("\(error)") }
        }
        XCTAssertTrue(transport.requests.isEmpty)
    }

    @MainActor func testAFailedDeleteTrashesTheNewTask() async throws {
        let transport = FakeCollabTransport()
        transport.responder = { _, _, _ in HTTPResponse(status: 403, text: "{\"error\":\"not_authorized\"}") }
        let signer = CollabSigner(identity: try Identity.fromSeed([UInt8](repeating: 3, count: 32)), clock: FixedClock(epochMillis: 4))
        let writer = CollabRecordWriter(transport: transport, signer: signer, owner: "alice", name: "demo")
        var calls: [(String, String)] = []
        do {
            try await IssueTaskMove.move(issue, owner: "alice", name: "demo", mover: "mona", writer: writer) { method, suffix, _ in
                calls.append((method, suffix))
                return ["ok": true, "task": .object(["id": "t1"])]
            }
            XCTFail("expected an error")
        } catch let error as IssueTaskMoveError {
            guard case .issueKept(_, let trashed) = error else { return XCTFail("\(error)") }
            XCTAssertTrue(trashed)
        }
        XCTAssertEqual(calls.map(\.0), ["POST", "DELETE"])
        XCTAssertEqual(calls[1].1, "/t1")
    }
}
