// "Just send it to the agent": the composer's Send starts the run with the
// remembered repository, target and model, and only asks when nothing can run.

import FMApi
import FMBase
import FMChat
import FMCode
import FMControl
import FMLocalAI
import XCTest
@testable import FMAgents

@MainActor
final class QuickAgentLaunchTests: XCTestCase {
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

    private func launcher(_ api: FakeAgentsApi, prefs: InMemoryKeyValueStore, signedIn: Bool = true) -> QuickAgentLauncher {
        QuickAgentLauncher(form: AgentStartModel(api: api, records: nil, signedIn: { signedIn }), prefs: prefs)
    }

    func testSendQueuesOnTheFirstOnlineNodeForTheProviderAndOpensTheSession() async {
        let api = api([
            AgentLaunchTarget(node: "desk-off", providers: ["claude-code"], online: false),
            AgentLaunchTarget(node: "mirror-codex", providers: ["codex"], online: true, runtimeMode: "headless"),
            AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true, runtimeMode: "headless"),
        ])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare()
        XCTAssertEqual(quick.contextLine, "alice/mobile · mirror35")
        let outcome = await quick.send(prompt: "Fix the crash", images: [])
        guard case .queued(let repo, let session, _) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertEqual(repo.fullName.lowercased(), "alice/mobile")
        XCTAssertEqual(session?.sessionId, "sess-1")
        XCTAssertEqual(api.launches.last?.node, "mirror35")
        XCTAssertEqual(api.launches.last?.provider, "auto")
    }

    private struct Offline: Error {}

    func testAFailedFirstRepositoryReadIsRetriedOnTheNextOpen() async {
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        api.repositoriesError = Offline()
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare()
        XCTAssertEqual(quick.form.targets?.count, 0)
        XCTAssertEqual(quick.contextLine, "Choose a repository")
        XCTAssertTrue(quick.form.targetsNeedLoading)

        api.repositoriesError = nil
        await quick.prepare()
        XCTAssertEqual(quick.form.targets?.count, 2)
        XCTAssertEqual(quick.contextLine, "alice/mobile · mirror35")
        XCTAssertFalse(quick.form.targetsNeedLoading)

        await quick.prepare()
        XCTAssertEqual(api.repositoriesCalls, 2)
    }

    func testASendAfterAFailedFirstReadLoadsTheRepositoriesAndQueues() async {
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        api.repositoriesError = Offline()
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare()
        api.repositoriesError = nil
        let outcome = await quick.send(prompt: "Fix the crash", images: [])
        guard case .queued = outcome else { return XCTFail("\(outcome)") }
        XCTAssertEqual(api.launches.last?.node, "mirror35")
    }

    func testRemembersRepositoryAndTargetPerRepository() async {
        let prefs = InMemoryKeyValueStore()
        let targets = [
            AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true),
            AgentLaunchTarget(node: "mirror36", providers: ["claude-code"], online: true),
        ]
        let first = launcher(api(targets), prefs: prefs)
        await first.prepare()
        let worker = first.form.targets!.first { $0.name == "worker" }!
        await first.choose(worker)
        first.choose(.node("mirror36"))

        let again = launcher(api(targets), prefs: prefs)
        await again.prepare()
        XCTAssertEqual(again.contextLine, "bob/worker · mirror36")
        // The current context wins over the last repository.
        await again.prepare(currentRepo: "alice/mobile")
        XCTAssertEqual(again.contextLine, "alice/mobile · mirror35")
    }

    func testWithNothingRememberedTheAccountsMostRecentRepositoryIsUsed() async {
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        api.repos = [
            Repository(owner: "alice", name: "mobile", updatedMs: 300),
            Repository(owner: "bob", name: "old", updatedMs: 100),
            Repository(owner: "bob", name: "worker", updatedMs: 200),
        ]
        let mine = QuickAgentLauncher(
            form: AgentStartModel(api: api, records: nil, signedIn: { true }), prefs: InMemoryKeyValueStore(),
            account: { "Bob" })
        await mine.prepare()
        XCTAssertEqual(mine.repository?.label, "bob/worker")

        let anyone = launcher(api, prefs: InMemoryKeyValueStore())
        await anyone.prepare()
        XCTAssertEqual(anyone.repository?.label, "alice/mobile")
    }

    func testNoUsableNodeOffersTheProviderSignInAndKeepsNothingSent() async {
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["codex"], online: true)])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare()
        let outcome = await quick.send(prompt: "Fix it", images: [])
        XCTAssertEqual(
            outcome, .needsFix(.signInProvider(node: "mirror35", provider: "claude-code"), message: "mirror35 has no Claude Code signed in."))
        XCTAssertEqual(QuickLaunchFix.signInProvider(node: "mirror35", provider: "claude-code").action, "Sign in to Claude Code on mirror35")
        XCTAssertTrue(api.launches.isEmpty)
    }

    func testAnExpiredRunnerOffersTheRunnerSignInNotTheNodeTerminal() async {
        let api = api([AgentLaunchTarget(
            node: "fmnext-n1", providers: [], online: false, runtimeMode: "runner-pool", runner: "fmnext-r1",
            needsLogin: ["claude-code"])])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare()
        let outcome = await quick.send(prompt: "Fix it", images: [])
        XCTAssertEqual(
            outcome,
            .needsFix(.signInRunner(runner: "fmnext-r1", provider: "claude-code"), message: "fmnext-r1 has no Claude Code signed in."))
        XCTAssertEqual(QuickLaunchFix.signInRunner(runner: "fmnext-r1", provider: "claude-code").action, "Sign in to Claude Code on fmnext-r1")
        XCTAssertTrue(api.launches.isEmpty)
    }

    func testARunnerSignedInAfterTheOptionsLoadedTakesTheSendWithoutAFix() async {
        let api = api([AgentLaunchTarget(
            node: "fmnext-n1", providers: [], online: true, runtimeMode: "runner-pool", runner: "fmnext-r1",
            needsLogin: ["claude-code"], queueNode: "fmnext-n1")])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare()
        XCTAssertEqual(quick.destination, .node("fmnext-n1"))
        api.options["alice/mobile"] = AgentLaunchOptions(canQueue: true, targets: [AgentLaunchTarget(
            node: "fmnext-r1", providers: ["claude-code"], online: true, runtimeMode: "runner-pool", runner: "fmnext-r1",
            queueNode: "fmnext-n1")])
        let outcome = await quick.send(prompt: "Fix it", images: [])
        guard case .queued = outcome else { return XCTFail("\(outcome)") }
        XCTAssertEqual(api.launches.last?.node, "fmnext-r1")
    }

    func testOpeningTheComposerAgainRereadsOptionsOnceTheyAreOld() async {
        let api = api([AgentLaunchTarget(
            node: "fmnext-n1", providers: [], online: true, runtimeMode: "runner-pool", runner: "fmnext-r1",
            needsLogin: ["claude-code"], queueNode: "fmnext-n1")])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare()
        api.options["alice/mobile"] = AgentLaunchOptions(canQueue: true, targets: [AgentLaunchTarget(
            node: "fmnext-r1", providers: ["claude-code"], online: true, runtimeMode: "runner-pool", runner: "fmnext-r1",
            queueNode: "fmnext-n1")])
        await quick.prepare()
        XCTAssertEqual(quick.form.plan?.options?.targets.first?.node, "fmnext-n1")
        XCTAssertFalse(quick.form.optionsOlder(than: QuickAgentLauncher.optionsFreshFor))
        XCTAssertTrue(quick.form.optionsOlder(than: QuickAgentLauncher.optionsFreshFor, now: Date().addingTimeInterval(16)))
        await quick.refresh()
        XCTAssertEqual(quick.form.plan?.options?.targetNamed("fmnext-n1")?.providers, ["claude-code"])
        XCTAssertEqual(quick.detail(.node("fmnext-n1")), "Runner · online")
    }

    func testTheCoreNameFindsThePoolTargetNotAPersonalRunner() {
        let options = AgentLaunchOptions(canQueue: true, targets: [
            AgentLaunchTarget(node: "my-box", runtimeMode: "runner-pool", runner: "my-box", queueNode: "fmnext-n1", runnerPin: "my-box"),
            AgentLaunchTarget(node: "fmnext-r1", runtimeMode: "runner-pool", runner: "fmnext-r1", queueNode: "fmnext-n1"),
        ])
        XCTAssertEqual(options.targetNamed("fmnext-n1")?.node, "fmnext-r1")
        XCTAssertEqual(options.targetNamed("my-box")?.node, "my-box")
        XCTAssertNil(options.targetNamed(""))
    }

    func testRunnerTargetsReadTheRunnerAndItsExpiredLogins() {
        let target = AgentLaunchTarget(json: [
            "node": .string("fmnext-n1"), "runtimeMode": .string("runner-pool"), "runner": .string("fmnext-r1"),
            "providers": .array([]), "needsLogin": .object(["claude-code": .array([.string("fmnext-r1")])]),
        ])
        XCTAssertTrue(target.isRunner)
        XCTAssertEqual(target.signInNode, "fmnext-r1")
        XCTAssertEqual(target.needsLogin, ["claude-code"])
        XCTAssertEqual(QuickLaunchFix.signIn(target, provider: "claude-code"), .signInRunner(runner: "fmnext-r1", provider: "claude-code"))
        let mirror = AgentLaunchTarget(node: "mirror35", providers: ["codex"], runtimeMode: "headless", runner: "ignored")
        XCTAssertEqual(mirror.signInNode, "mirror35")
        XCTAssertEqual(QuickLaunchFix.signIn(mirror, provider: "claude-code"), .signInProvider(node: "mirror35", provider: "claude-code"))
    }

    func testSignedOutAsksForAnAccount() async {
        let quick = launcher(api([AgentLaunchTarget(node: "m", providers: ["claude-code"], online: true)]), prefs: InMemoryKeyValueStore(), signedIn: false)
        let outcome = await quick.send(prompt: "Go", images: [])
        guard case .needsFix(.signInAccount, _) = outcome else { return XCTFail("\(outcome)") }
    }

    func testASpokenRepositoryTakesTheRunAndTheComposerKeepsItsOwn() async {
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare(currentRepo: "alice/mobile")
        let outcome = await quick.send(prompt: "Fix the crash", repository: "Worker", onDevice: false)
        guard case .queued = outcome else { return XCTFail("\(outcome)") }
        XCTAssertEqual(api.launches.last?.owner, "bob")
        XCTAssertEqual(api.launches.last?.repo, "worker")
        XCTAssertEqual(quick.repository?.key, "alice/mobile")

        _ = await quick.send(prompt: "Again", repository: "alice/mobile", onDevice: false)
        XCTAssertEqual(api.launches.last?.repo, "mobile")
        _ = await quick.send(prompt: "Last used", repository: nil, onDevice: false)
        XCTAssertEqual(api.launches.count, 3)
    }

    func testAnUnknownSpokenRepositoryStartsNothing() async {
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        let outcome = await quick.send(prompt: "Go", repository: "nowhere", onDevice: false)
        guard case .needsFix(.chooseRepository, let message) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertTrue(message.contains("nowhere"))
        XCTAssertTrue(api.launches.isEmpty)
        let empty = await quick.send(prompt: "  ", repository: nil, onDevice: false)
        XCTAssertEqual(empty, .ignored)
    }

    func testTheChipModelIsSentAndRemembered() async {
        let prefs = InMemoryKeyValueStore()
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code", "codex"], online: true)])
        let quick = launcher(api, prefs: prefs)
        await quick.prepare()
        XCTAssertEqual(quick.model.label, "Opus 5.5")
        quick.choose(model: AgentModelCatalog.nodeModels.first { $0.alias == "terra" }!)
        _ = await quick.send(prompt: "Go", images: [])
        XCTAssertEqual(api.launches.last?.provider, "codex")
        XCTAssertEqual(NodeModelPreference.read(prefs).alias, "terra")
    }

    func testAGemmaChipQueuesOnTheNodeThatServesGemma() async {
        let api = api([
            AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true, runtimeMode: "headless"),
            AgentLaunchTarget(node: "mirror-gemma", providers: ["codex", "local-llama"], online: true, runtimeMode: "headless"),
        ])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        await quick.prepare()
        XCTAssertEqual(quick.destination, .node("mirror35"))
        quick.choose(model: AgentModelCatalog.nodeModels.first { $0.alias == "gemma4:12b" }!)
        XCTAssertEqual(quick.destination, .node("mirror-gemma"))
        _ = await quick.send(prompt: "Go", images: [])
        XCTAssertEqual(api.launches.last?.node, "mirror-gemma")
        XCTAssertEqual(api.launches.last?.provider, "local-llama")
    }

    private func gemmaLauncher(
        _ api: FakeAgentsApi, prefs: InMemoryKeyValueStore, manager: LocalModelManager
    ) -> QuickAgentLauncher {
        let record = LocalRepoRecord(name: "mobile", path: "/tmp/mobile", createdAt: Date(), publishedAs: "alice/mobile")
        let form = AgentStartModel(api: api, records: { [record] }, signedIn: { true }, localModels: { manager })
        return QuickAgentLauncher(form: form, prefs: prefs)
    }

    func testOnDeviceGemmaIsOfferedOnlyWhereThePhoneCanRunIt() async throws {
        let temp = TempDirectory()
        defer { temp.cleanUp() }
        let manager = try localModels(FakeLocalChatEngine(), in: temp)
        let targets = [AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)]
        let phone = gemmaLauncher(api(targets), prefs: InMemoryKeyValueStore(), manager: manager)
        await phone.prepare()
        XCTAssertEqual(phone.modelChoices.last, AgentModelCatalog.onDeviceGemma)

        let noStore = launcher(api(targets), prefs: InMemoryKeyValueStore())
        await noStore.prepare()
        XCTAssertFalse(noStore.modelChoices.contains(AgentModelCatalog.onDeviceGemma))
    }

    func testTheGemmaChipRunsOnThisIPhoneWithGemma() async throws {
        let temp = TempDirectory()
        defer { temp.cleanUp() }
        let manager = try localModels(FakeLocalChatEngine(), in: temp)
        let prefs = InMemoryKeyValueStore()
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        let quick = gemmaLauncher(api, prefs: prefs, manager: manager)
        await quick.prepare()
        XCTAssertEqual(quick.destination, .node("mirror35"))

        quick.choose(model: AgentModelCatalog.onDeviceGemma)
        XCTAssertEqual(quick.destination, .device)
        XCTAssertEqual(quick.destinations, [.device])
        XCTAssertEqual(quick.detail(.device), "Tiny · on this iPhone")
        XCTAssertEqual(NodeModelPreference.read(prefs), AgentModelCatalog.onDeviceGemma)

        let outcome = await quick.send(prompt: "Fix it", images: [])
        guard case .device(let record, let prompt, _, let credentials) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertEqual(record.name, "mobile")
        XCTAssertEqual(prompt, "Fix it")
        XCTAssertEqual(credentials, .onDevice(model: tinyModel.id))
        XCTAssertTrue(api.launches.isEmpty)

        quick.choose(model: AgentModelCatalog.defaultNodeModel)
        XCTAssertEqual(quick.destination, .node("mirror35"))
    }

    func testARunMeantForThisIPhoneIsRefusedWhereNoScreenCanShowIt() async throws {
        let temp = TempDirectory()
        defer { temp.cleanUp() }
        let manager = try localModels(FakeLocalChatEngine(), in: temp)
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        let quick = gemmaLauncher(api, prefs: InMemoryKeyValueStore(), manager: manager)
        await quick.prepare()
        quick.choose(model: AgentModelCatalog.onDeviceGemma)
        let outcome = await quick.send(prompt: "Fix it", repository: nil, onDevice: false)
        guard case .failed(let message) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertTrue(message.contains("run on your iPhone"))
        XCTAssertTrue(api.launches.isEmpty)
        XCTAssertEqual(quick.destination, .device)
    }

    func testTheGemmaChipAsksForSetupWhenGemmaIsOff() async throws {
        let temp = TempDirectory()
        defer { temp.cleanUp() }
        let manager = try localModels(FakeLocalChatEngine(), in: temp, enabled: false)
        let api = api([AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        let quick = gemmaLauncher(api, prefs: InMemoryKeyValueStore(), manager: manager)
        await quick.prepare()
        quick.choose(model: AgentModelCatalog.onDeviceGemma)
        let outcome = await quick.send(prompt: "Fix it", images: [])
        guard case .needsFix(.setUpGemma, _) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertEqual(QuickLaunchFix.setUpGemma.action, "Set up Gemma")
        XCTAssertTrue(api.launches.isEmpty)
    }

    func testALinkedDesktopIsOfferedAndTakesThePromptDirectly() async {
        let api = api([AgentLaunchTarget(node: "fmnext-r1", providers: ["claude-code"], online: true, runtimeMode: "runner-pool")])
        let prefs = InMemoryKeyValueStore()
        let quick = launcher(api, prefs: prefs)
        await quick.prepare()
        XCTAssertFalse(quick.destinations.contains(.desktop("studio")))
        let desk = FakeQuickDesktop(name: "studio")
        quick.desktop = desk
        XCTAssertTrue(quick.destinations.contains(.desktop("studio")))
        XCTAssertEqual(quick.detail(.desktop("studio")), "Your desktop, directly")
        quick.choose(.desktop("studio"))
        XCTAssertEqual(quick.contextLine, "alice/mobile · studio (desktop)")
        let outcome = await quick.send(prompt: " Fix the build ", images: [])
        XCTAssertEqual(outcome, .desktop(name: "studio", agentId: 7, message: "Started agent #7 on studio."))
        XCTAssertEqual(desk.starts, [["alice", "mobile", "Fix the build", "claude-code", quick.model.modelId, "Auto"]])
        XCTAssertTrue(api.launches.isEmpty)
        XCTAssertTrue(quick.takesMode)
        quick.choose(mode: .plan)
        _ = await quick.send(prompt: "Plan the fix", images: [])
        XCTAssertEqual(desk.starts.last?[5], "Plan")
        XCTAssertEqual(AgentPermissionMode.read(prefs), .plan)

        let again = launcher(api, prefs: prefs)
        again.desktop = desk
        await again.prepare()
        XCTAssertEqual(again.destination, .desktop("studio"))
    }

    func testYoloAndNoWorktreeGoToTheDesktopOnly() async {
        let api = api([AgentLaunchTarget(node: "fmnext-r1", providers: ["claude-code"], online: true)])
        let prefs = InMemoryKeyValueStore()
        let quick = launcher(api, prefs: prefs)
        let desk = FakeQuickDesktop(name: "studio")
        quick.desktop = desk
        await quick.prepare()
        quick.choose(.node("fmnext-r1"))
        XCTAssertFalse(quick.takesRunOptions)
        quick.choose(.desktop("studio"))
        XCTAssertTrue(quick.takesRunOptions)
        XCTAssertTrue(quick.takesNoWorktree)

        _ = await quick.send(prompt: "Plain", images: [])
        XCTAssertEqual(desk.startOptions.last, QuickLaunchRunOptions(yolo: false, worktree: true))

        quick.toggleYolo()
        _ = await quick.send(prompt: "Ship it", images: [])
        XCTAssertEqual(desk.startOptions.last, QuickLaunchRunOptions(yolo: true, worktree: true))
        XCTAssertTrue(QuickLaunchRunOptions.read(prefs).yolo)

        quick.toggleWorktree()
        XCTAssertFalse(quick.sendsYolo)
        _ = await quick.send(prompt: "In the folder", images: [])
        XCTAssertEqual(desk.startOptions.last, QuickLaunchRunOptions(yolo: false, worktree: false))
        XCTAssertTrue(quick.runOptions.worktree)
        XCTAssertTrue(quick.runOptions.yolo)

        let again = launcher(api, prefs: prefs)
        XCTAssertEqual(again.runOptions, QuickLaunchRunOptions(yolo: true, worktree: true))
    }

    func testNoWorktreeIsNotOfferedForAProviderThatAlwaysCutsOne() async {
        let api = api([AgentLaunchTarget(node: "fmnext-r1", providers: ["claude-code", "local-llama"], online: true)])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        let desk = FakeQuickDesktop(name: "studio")
        quick.desktop = desk
        await quick.prepare()
        quick.choose(.desktop("studio"))
        quick.choose(model: AgentModelCatalog.nodeModels.first { $0.provider == "local-llama" }!)
        XCTAssertTrue(quick.takesRunOptions)
        XCTAssertFalse(quick.takesNoWorktree)
        quick.toggleYolo()
        quick.toggleWorktree()
        _ = await quick.send(prompt: "Go", images: [])
        XCTAssertEqual(desk.startOptions.last, QuickLaunchRunOptions(yolo: true, worktree: true))
    }

    func testADesktopRefusalIsExplained() async {
        let api = api([AgentLaunchTarget(node: "fmnext-r1", providers: ["claude-code"], online: true)])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        let desk = FakeQuickDesktop(name: "studio")
        desk.error = LocalControlException("no_checkout")
        quick.desktop = desk
        await quick.prepare()
        quick.choose(.desktop("studio"))
        let outcome = await quick.send(prompt: "Go", images: [])
        XCTAssertEqual(outcome, .failed("The desktop has no local checkout of that repository."))
    }

    func testPhotosGoStraightToTheDesktop() async {
        let api = api([AgentLaunchTarget(node: "fmnext-r1", providers: ["claude-code"], online: true)])
        let quick = launcher(api, prefs: InMemoryKeyValueStore())
        let desk = FakeQuickDesktop(name: "studio")
        quick.desktop = desk
        await quick.prepare()
        quick.choose(.desktop("studio"))
        let photo = PromptAttachment(id: "a", name: "shot.png", bytes: Data([0x89, 0x50, 0x4E, 0x47]), mediaType: "image/png")
        let outcome = await quick.send(prompt: "", images: [photo])
        XCTAssertEqual(outcome, .desktop(name: "studio", agentId: 7, message: "Started agent #7 on studio."))
        XCTAssertEqual(desk.starts.first?[2], "See the attached photos.")
        XCTAssertEqual(desk.startImages, [[photo]])
        XCTAssertTrue(api.launches.isEmpty)
    }
}

@MainActor
private final class FakeQuickDesktop: QuickLaunchDesktop {
    let desktopName: String
    var desktopDetail = ""
    var error: Error?
    var starts: [[String]] = []
    var startOptions: [QuickLaunchRunOptions] = []
    var startImages: [[PromptAttachment]] = []

    init(name: String) { desktopName = name }

    func start(
        owner: String, name: String, task: String, provider: String, model: String, mode: String,
        options: QuickLaunchRunOptions, images: [PromptAttachment]
    ) async throws -> Int {
        starts.append([owner, name, task, provider, model, mode])
        startOptions.append(options)
        startImages.append(images)
        if let error { throw error }
        return 7
    }
}
