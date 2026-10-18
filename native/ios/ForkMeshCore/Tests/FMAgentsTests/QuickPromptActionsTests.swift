import FMApi
import FMBase
import XCTest
@testable import FMAgents

@MainActor
final class QuickPromptActionsTests: XCTestCase {
    private func api(_ targets: [AgentLaunchTarget], canQueue: Bool = true) -> FakeAgentsApi {
        let api = FakeAgentsApi()
        let options = AgentLaunchOptions(canQueue: canQueue, targets: targets)
        api.options["alice/mobile"] = options
        api.options["bob/worker"] = options
        api.queueResult = AgentQueueResult(
            queueState: "ready_for_claim", targetOnline: true, message: "Queued.",
            session: AgentLaunchSession(sessionId: "sess-1", owner: "alice", repo: "mobile", status: "queued"))
        return api
    }

    private func launcher(
        _ api: FakeAgentsApi, prefs: InMemoryKeyValueStore = InMemoryKeyValueStore(), signedIn: Bool = true,
        actions: FakePromptActions? = nil, bare: Bool = false
    ) -> QuickAgentLauncher {
        let quick = QuickAgentLauncher(form: AgentStartModel(api: api, records: nil, signedIn: { signedIn }), prefs: prefs)
        quick.actions = bare ? nil : actions ?? FakePromptActions()
        return quick
    }

    private var online: [AgentLaunchTarget] {
        [AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)]
    }

    func testTheActionsAreOfferedBeforeTheModelsOnlyWhenTheAppProvidesThem() {
        let quick = launcher(api(online))
        XCTAssertEqual(
            Array(quick.modelChoices.prefix(5).map(\.label)),
            ["Bug report", "Task · assign", "Manual · create issue", "ActivityPub · post", "Take a note"])
        XCTAssertEqual(quick.modelChoices.dropFirst(5).first, AgentModelCatalog.defaultNodeModel)
        let bare = launcher(api(online), bare: true)
        XCTAssertNil(bare.modelChoices.first { QuickPromptAction(choice: $0) != nil })
    }

    func testWithNoAgentAnywhereTheChipDefaultsToABugReport() async {
        let quick = launcher(api([], canQueue: false))
        await quick.prepare()
        XCTAssertFalse(quick.hasAgent)
        XCTAssertEqual(quick.action, .bugReport)
        XCTAssertEqual(quick.chip.label, "Bug report")
        XCTAssertEqual(quick.contextLine, "Bug report · ForkMesh team")
        XCTAssertFalse(quick.takesRunOptions)
    }

    func testSignedOutPeopleCanStillSendABugReport() async {
        let actions = FakePromptActions()
        let quick = launcher(api(online), signedIn: false, actions: actions)
        await quick.prepare()
        XCTAssertEqual(quick.action, .bugReport)
        let photo = PromptAttachment(id: "a", name: "shot.png", bytes: Data([1]), mediaType: "image/png")
        let outcome = await quick.send(prompt: "The calendar said Invalid json.", images: [photo])
        XCTAssertEqual(outcome, .done("Sent to the ForkMesh team as a bug report."))
        XCTAssertEqual(actions.bugs.map(\.0), ["The calendar said Invalid json."])
        XCTAssertEqual(actions.bugs.first?.1, [photo])
    }

    func testWithAnAgentTheModelStaysTheDefault() async {
        let quick = launcher(api(online))
        await quick.prepare()
        XCTAssertTrue(quick.hasAgent)
        XCTAssertNil(quick.action)
        XCTAssertEqual(quick.chip, AgentModelCatalog.defaultNodeModel)
    }

    func testPickingAModelOverridesTheBugReportDefaultAndIsRemembered() async {
        let prefs = InMemoryKeyValueStore()
        let quick = launcher(api([], canQueue: false), prefs: prefs)
        await quick.prepare()
        quick.choose(model: AgentModelCatalog.defaultNodeModel)
        XCTAssertNil(quick.action)
        let again = launcher(api([], canQueue: false), prefs: prefs)
        await again.prepare()
        XCTAssertNil(again.action)
    }

    func testAChosenActionIsRememberedAndBlocksNoAgentRun() async {
        let prefs = InMemoryKeyValueStore()
        let quick = launcher(api(online), prefs: prefs)
        await quick.prepare()
        quick.choose(model: QuickPromptAction.note.choice)
        XCTAssertEqual(quick.chip.label, "Take a note")
        let again = launcher(api(online), prefs: prefs)
        XCTAssertEqual(again.action, .note)
        XCTAssertEqual(again.model, AgentModelCatalog.defaultNodeModel)
    }

    func testATaskGoesToThePickedMemberWithTheFirstLineAsItsTitle() async {
        let actions = FakePromptActions()
        actions.people = QuickTaskPeople(actor: "alice", members: ["bob", "alice", "carol"])
        let prefs = InMemoryKeyValueStore()
        let fake = api(online)
        let quick = launcher(fake, prefs: prefs, actions: actions)
        await quick.prepare()
        quick.choose(action: .task)
        await quick.loadTaskPeople()
        XCTAssertEqual(quick.taskAssignees, ["", "alice", "bob", "carol"])
        XCTAssertEqual(quick.taskAssigneeLabel("alice"), "@alice (you)")
        XCTAssertEqual(quick.taskAssigneeLabel(""), "Unassigned")
        quick.choose(assignee: "bob")
        XCTAssertEqual(quick.contextLine, "Task for @bob")
        let outcome = await quick.send(prompt: "Ship the calendar fix\nIt fails on save.", images: [])
        XCTAssertEqual(outcome, .done("Created a task for @bob."))
        XCTAssertEqual(actions.tasks.first, ["Ship the calendar fix", "Ship the calendar fix\nIt fails on save.", "bob", "alice/mobile"])
        XCTAssertTrue(fake.launches.isEmpty)
        XCTAssertEqual(launcher(api(online), prefs: prefs).taskAssignee, "bob")
    }

    func testAnUnassignedTaskSaysSo() async {
        let actions = FakePromptActions()
        let quick = launcher(api(online), actions: actions)
        quick.choose(action: .task)
        let outcome = await quick.send(prompt: "Plan the offsite", images: [])
        XCTAssertEqual(outcome, .done("Created an unassigned task on General."))
        XCTAssertEqual(actions.tasks.first?[2], "")
    }

    func testIssuesAndPostsGoToThePublishedRepository() async {
        let actions = FakePromptActions()
        let quick = launcher(api(online), actions: actions)
        await quick.prepare()
        quick.choose(action: .issue)
        XCTAssertEqual(quick.contextLine, "alice/mobile")
        var outcome = await quick.send(prompt: "Calendar save fails\nSteps: open, save.", images: [])
        XCTAssertEqual(outcome, .done("Filed an issue on alice/mobile."))
        XCTAssertEqual(actions.issues.first, ["alice", "mobile", "Calendar save fails", "Steps: open, save."])
        quick.choose(action: .activityPub)
        outcome = await quick.send(prompt: "Calendars work again", images: [])
        XCTAssertEqual(outcome, .done("Published to ActivityPub for alice/mobile."))
        XCTAssertEqual(actions.posts.first, ["alice", "mobile", "Calendars work again", ""])
    }

    func testANoteKeepsTheRestAsItsBody() async {
        let actions = FakePromptActions()
        let quick = launcher(api(online), actions: actions)
        quick.choose(action: .note)
        let outcome = await quick.send(prompt: "Groceries\n- milk\n- eggs", images: [])
        XCTAssertEqual(outcome, .done("Saved as a note."))
        XCTAssertEqual(actions.notes.first, ["Groceries", "- milk\n- eggs"])
    }

    func testOnlyBugReportsTakePhotosAndAccountActionsNeedAnAccount() async {
        let photo = PromptAttachment(id: "a", name: "shot.png", bytes: Data([1]), mediaType: "image/png")
        let quick = launcher(api(online))
        quick.choose(action: .note)
        let refused = await quick.send(prompt: "Hi", images: [photo])
        XCTAssertEqual(refused, .failed("Take a note cannot include photos yet. Remove them, or send a bug report."))
        let signedOut = launcher(api(online), signedIn: false)
        signedOut.choose(action: .task)
        guard case .needsFix(.signInAccount, _) = await signedOut.send(prompt: "Hi", images: []) else {
            return XCTFail("a task needs an account")
        }
    }

    func testARefusalIsShownAndTheTextIsKept() async {
        let actions = FakePromptActions()
        actions.error = QuickActionError("Only the assignee can do that.")
        let quick = launcher(api(online), actions: actions)
        quick.choose(action: .task)
        let outcome = await quick.send(prompt: "Do it", images: [])
        XCTAssertEqual(outcome, .failed("Only the assignee can do that."))
    }

    func testASpokenRepositoryStillStartsAnAgentWhateverTheChipSays() async {
        let api = api(online)
        let actions = FakePromptActions()
        let quick = launcher(api, actions: actions)
        quick.choose(action: .bugReport)
        let outcome = await quick.send(prompt: "Fix the crash", repository: "alice/mobile", onDevice: false)
        guard case .queued = outcome else { return XCTFail("\(outcome)") }
        XCTAssertTrue(actions.bugs.isEmpty)
    }

    func testTitlesAreTheFirstLineCappedLikeTheDesktop() {
        XCTAssertEqual(QuickPromptAction.titleAndBody("  One   two \n three ").title, "One two")
        XCTAssertEqual(QuickPromptAction.titleAndBody("  One   two \n three ").body, "three")
        XCTAssertEqual(QuickPromptAction.titleAndBody(String(repeating: "a", count: 200)).title.count, 160)
        XCTAssertEqual(QuickPromptAction.titleAndBody("\n\nbody only").title, "body only")
    }
}

@MainActor
final class FakePromptActions: QuickPromptActions {
    var error: Error?
    var people = QuickTaskPeople(actor: "alice", members: [])
    var bugs: [(String, [PromptAttachment])] = []
    var tasks: [[String]] = []
    var issues: [[String]] = []
    var posts: [[String]] = []
    var notes: [[String]] = []

    func reportBug(_ text: String, photos: [PromptAttachment]) async throws {
        if let error { throw error }
        bugs.append((text, photos))
    }

    func taskPeople() async throws -> QuickTaskPeople {
        if let error { throw error }
        return people
    }

    func createTask(title: String, details: String, assignee: String, repository: String) async throws {
        if let error { throw error }
        tasks.append([title, details, assignee, repository])
    }

    func createIssue(owner: String, name: String, title: String, body: String) async throws {
        if let error { throw error }
        issues.append([owner, name, title, body])
    }

    func postToActivityPub(owner: String, name: String, title: String, body: String) async throws {
        if let error { throw error }
        posts.append([owner, name, title, body])
    }

    func takeNote(title: String, markdown: String) async throws {
        if let error { throw error }
        notes.append([title, markdown])
    }
}
