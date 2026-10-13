import FMApi
import FMBase
import XCTest

@testable import FMCode

final class CollabIssueBoardTests: XCTestCase {
    let columns = IssueBoard.defaultColumns

    func testIssuesLandInTheirStatusColumn() {
        XCTAssertEqual(IssueBoard.column(of: Issue(number: 1, title: "a"), columns: columns), "Backlog")
        XCTAssertEqual(IssueBoard.column(of: Issue(number: 2, title: "b", labels: ["bug", "Status:In Progress"]), columns: columns), "In Progress")
        XCTAssertEqual(IssueBoard.column(of: Issue(number: 3, title: "c", status: "closed", labels: ["status:todo"]), columns: columns), "Done")
        let layout = IssueBoard.layout([
            Issue(number: 1, title: "a"), Issue(number: 5, title: "e", priority: 3), Issue(number: 4, title: "d", labels: ["status:todo"]),
        ], columns: columns)
        XCTAssertEqual(layout.map(\.issues.count), [2, 1, 0, 0])
        XCTAssertEqual(layout[0].issues.map(\.number), [5, 1])
        XCTAssertTrue(layout[3].isDone)
    }

    func testMovingRewritesStatusLabelsAndStatus() {
        let issue = Issue(number: 7, title: "x", labels: ["bug", "status:todo"])
        let toProgress = IssueBoard.move(issue, to: "in progress", columns: columns)!
        XCTAssertEqual(toProgress.edits, [.labels(["bug", "status:in progress"])])
        XCTAssertEqual(toProgress.summary, "Moved #7 to In Progress.")
        let toDone = IssueBoard.move(issue, to: "Done", columns: columns)!
        XCTAssertEqual(toDone.edits, [.labels(["bug"]), .status(open: false)])
        let closed = Issue(number: 8, title: "y", status: "closed")
        XCTAssertEqual(IssueBoard.move(closed, to: "Backlog", columns: columns)?.edits, [.labels(["status:backlog"]), .status(open: true)])
        XCTAssertNil(IssueBoard.move(issue, to: "Todo", columns: columns))
        XCTAssertNil(IssueBoard.move(issue, to: "Nowhere", columns: columns))
        XCTAssertEqual(IssueBoard.neighbours(of: "Todo", columns: columns).left, "Backlog")
        XCTAssertNil(IssueBoard.neighbours(of: "Done", columns: columns).right)
    }

    func testColumnsAreCleanedStoredAndValidated() {
        XCTAssertEqual(IssueBoard.cleanColumns([" A ", "a", "", "B"]), ["A", "B"])
        XCTAssertEqual(IssueBoard.validate(["only"]), IssueBoard.tooFewColumns)
        XCTAssertNil(IssueBoard.validate(["a", "b"]))
        let store = InMemoryKeyValueStore()
        XCTAssertEqual(IssueBoard.load(store, owner: "o", name: "r"), IssueBoard.defaultColumns)
        IssueBoard.save(store, owner: "o", name: "r", columns: ["Now", "Next", "Shipped"])
        XCTAssertEqual(IssueBoard.load(store, owner: "o", name: "r"), ["Now", "Next", "Shipped"])
        XCTAssertEqual(IssueBoard.storageKey(owner: "o", name: "r"), "issueBoard/columns/o/r")
    }
}

final class CollabIssueEditTests: XCTestCase {
    func testDiffProducesOnlyChangedFields() {
        let issue = Issue(number: 1, title: "Old", labels: ["a"], priority: 1, assignees: ["mona"])
        let edits = IssueFields.diff(issue, title: "New", labels: ["a", "A", "b"], assignees: ["Mona"], priority: 9, progress: 0, open: false)
        XCTAssertEqual(edits.map(\.type), ["title", "labels", "priority", "status"])
        XCTAssertEqual(edits[1].fields.strings("labels"), ["a", "b"])
        XCTAssertEqual(edits[2].fields.int("priority"), 4)
        XCTAssertEqual(IssueFields.parseList("a, b\nc,,"), ["a", "b", "c"])
        XCTAssertEqual(IssueFields.priorityLabel(3), "High")
    }

    func testOnlyTheCreatorMaySoftDelete() {
        let issue = Issue(number: 1, title: "x", author: "K", events: [IssueEvent(type: "open", author: "K")])
        XCTAssertTrue(IssueFields.canDeleteOwn(issue, author: "K"))
        XCTAssertFalse(IssueFields.canDeleteOwn(issue, author: "Z"))
        XCTAssertEqual(IssueEdit.deleteOwnIssue().fields.str("target"), "self")
        XCTAssertEqual(IssueEdit.react(target: "e", reaction: "rocket").type, "reaction")
        XCTAssertEqual(IssueEdit.retract(eventId: "e2").fields.str("target"), "e2")
    }

    func testChecklistParsesAndToggles() {
        let body = "Intro\n- [ ] one\n  * [x] two\n```\n- [ ] code\n```\n- [] not\n+ [X]"
        let items = IssueChecklist.items(body)
        XCTAssertEqual(items.map(\.text), ["one", "two", ""])
        XCTAssertEqual(items.map(\.done), [false, true, true])
        XCTAssertEqual(IssueChecklist.progress(body).done, 2)
        let toggled = IssueChecklist.toggled(body, lineIndex: 1)!
        XCTAssertTrue(toggled.contains("- [x] one"))
        XCTAssertTrue(IssueChecklist.toggled(toggled, lineIndex: 2)!.contains("  * [ ] two"))
        XCTAssertNil(IssueChecklist.toggled(body, lineIndex: 4))
        XCTAssertNil(IssueChecklist.toggled(body, lineIndex: 0))
    }

    func testClosingKeywordsLinkIssues() {
        XCTAssertEqual(IssueLinks.linkedIssueNumbers("Fixes #12, closes #3.\nresolves: #12"), [12, 3])
        XCTAssertEqual(IssueLinks.bodyLinking("Body", issue: 4), "Body\n\nCloses #4")
        XCTAssertEqual(IssueLinks.bodyLinking("Closes #4", issue: 4), "Closes #4")
        XCTAssertEqual(IssueLinks.bodyLinking("", issue: 4), "Closes #4")
    }

    func testModelMentionsFollowTheDesktopRules() {
        let mentions = IssueModelMentions.parse("@opus please fix the crash, and @codex what do you think?")
        XCTAssertEqual(mentions.map(\.model), ["claude-opus-5", "gpt-5.5"])
        XCTAssertEqual(mentions[0].provider, "claude-code")
        XCTAssertEqual(mentions[0].intent, .work)
        XCTAssertEqual(mentions[1].intent, .answer)
        XCTAssertEqual(mentions[1].request, "what do you think?")
        XCTAssertTrue(IssueModelMentions.parse("mail me@opus.dev").isEmpty)
        XCTAssertEqual(IssueModelMentions.parse("@claude-opus-9.").first?.model, "claude-opus-9")
        XCTAssertEqual(IssueModelMentions.parse("@sonnet").first?.request, "")
        XCTAssertFalse(IssueModelMentions.requestsWork("how would you fix this"))
        XCTAssertFalse(IssueModelMentions.requestsWork("does this need a fix?"))
        XCTAssertTrue(IssueModelMentions.requestsWork("hey, can you start an agent"))
        XCTAssertTrue(IssueModelMentions.requestsWork("do it"))
        XCTAssertTrue(IssueModelMentions.agentPrompt(issueNumber: 3, title: "T", question: "").contains("Issue #3: T"))
    }

    func testThreadCopyText() {
        let issue = Issue(number: 2, title: "Crash", body: "Steps", events: [
            IssueEvent(type: "comment", body: "me too", authorName: "bea"), IssueEvent(type: "status", body: ""),
        ])
        XCTAssertEqual(IssueThreadText.copyText(repo: "o/r", issue: issue, includeThread: false), "o/r#2: Crash\n\nSteps")
        XCTAssertTrue(IssueThreadText.copyText(repo: "o/r", issue: issue, includeThread: true).hasSuffix("— bea:\nme too"))
    }
}
