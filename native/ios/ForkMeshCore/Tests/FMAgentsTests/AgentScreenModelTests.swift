// The agent screen's logic: the composer, the run, the
// pull-request offer, "reference a file", and the setup sheet.

import FMBase
import FMCode
import FMLocalAI
import FMNet
import XCTest
@testable import FMAgents

@MainActor
final class AgentScreenModelTests: XCTestCase {
    private var temp: TempDirectory!
    private var workspaces: [GitWorkspace] = []

    override func setUp() async throws {
        temp = TempDirectory()
    }

    override func tearDown() async throws {
        for workspace in workspaces { workspace.dispose() }
        workspaces.removeAll()
        temp.cleanUp()
    }

    private func configuredStore() -> AgentCredentialStore {
        let store = AgentCredentialStore(secureStore: InMemorySecureStore(), http: FakeHTTPClient())
        store.write(AgentCredentials(kind: .anthropic, apiKey: "sk-ant-test-key-000000000000", model: "claude-opus-5")) // forkmesh-secret-scan:ignore-line
        return store
    }

    private func model(
        _ workspace: AgentWorkspace? = nil,
        store: AgentCredentialStore? = nil,
        script: [AgentTurn] = [],
        initialPrompt: String = "",
        autoStart: Bool = false,
        git: GitWorkspace? = nil,
        record: LocalRepoRecord? = nil,
        pullRequests: PullRequestService? = nil
    ) -> AgentScreenModel {
        AgentScreenModel(
            repoName: "repo",
            workspace: workspace ?? MemoryWorkspace(files: ["a.txt": "one\n"]),
            gitWorkspace: git,
            record: record,
            pullRequests: pullRequests,
            sessions: AgentSessionService(prefs: InMemoryKeyValueStore()),
            store: store ?? AgentCredentialStore(secureStore: InMemorySecureStore(), http: FakeHTTPClient()),
            author: testAuthor,
            localModels: { nil },
            agentFactory: { toolset, _ in CodingAgent(provider: ScriptedProvider(script), toolset: toolset) },
            initialPrompt: initialPrompt,
            autoStart: autoStart
        )
    }

    func testWithNoKeyItAsksToConnectAModel() async {
        let screen = model()
        screen.load()
        XCTAssertNil(screen.credentials)
        XCTAssertEqual(screen.modelChipLabel, "Choose a model")
        screen.draft = "do it"
        let outcome = await screen.send()
        XCTAssertEqual(outcome, .needsSetup)
        // The prompt stays for after setup.
        XCTAssertEqual(screen.draft, "do it")
    }

    func testWithAKeyItNamesTheModel() async {
        let screen = model(store: configuredStore())
        screen.load()
        XCTAssertEqual(screen.modelChipLabel, "Claude Opus 5")
        XCTAssertEqual(screen.composerHint, "Ask anything…")
    }

    func testAPromptRunsAndTheTranscriptShowsTheWork() async {
        let screen = model(store: configuredStore(), script: [
            toolTurn("read_file", ["path": "a.txt"]),
            AgentTurn(text: "It says one.", toolCalls: []),
        ])
        screen.load()
        screen.draft = "what is in a.txt?"
        let outcome = await screen.send()
        XCTAssertEqual(outcome, .started)
        XCTAssertEqual(screen.draft, "")
        await screen.handle?.waitUntilDone()
        XCTAssertEqual(screen.events.compactMap(\.userText), ["what is in a.txt?"])
        XCTAssertEqual(screen.events.compactMap(\.assistantText), ["It says one."])
        XCTAssertEqual(screen.handle?.lastDone?.reason, .completed)
        XCTAssertFalse(screen.isRunning)
    }

    func testADirtyRepositoryRefusesAndExplainsWithoutABranch() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        try await workspace.writeFile("a.txt", "mine\n")
        let screen = model(workspace, store: configuredStore())
        screen.load()
        screen.draft = "change it"
        let outcome = await screen.send()
        XCTAssertEqual(outcome, .refused)
        XCTAssertTrue(screen.error?.contains("uncommitted") ?? false)
        let branches = try await workspace.branches()
        XCTAssertEqual(branches.map(\.name), ["main"])
    }

    func testAnEmptyPromptDoesNothing() async {
        let screen = model(store: configuredStore())
        screen.load()
        screen.draft = "   "
        let outcome = await screen.send()
        XCTAssertEqual(outcome, .empty)
        XCTAssertNil(screen.handle)
    }

    func testAHandedOverPingWaitsInTheComposerUnsent() async {
        let screen = model(store: configuredStore(), initialPrompt: "Ping: the build broke")
        XCTAssertFalse(screen.load())
        XCTAssertEqual(screen.draft, "Ping: the build broke")
        XCTAssertNil(screen.handle)
    }

    func testAnAutoStartFiresOnce() async {
        let screen = model(store: configuredStore(), initialPrompt: "Fix it", autoStart: true)
        XCTAssertTrue(screen.load())
        XCTAssertFalse(screen.takeAutoStart())
    }

    func testWithoutPullRequestWiringNoOfferAppears() async {
        let screen = model(store: configuredStore(), script: [AgentTurn(text: "done", toolCalls: [])])
        screen.load()
        screen.draft = "anything"
        _ = await screen.send()
        await screen.handle?.waitUntilDone()
        XCTAssertNil(screen.prCandidate)
    }

    private func realRepo() async throws -> (GitWorkspace, GitAgentWorkspace, LocalRepoRecord) {
        let git = GitWorkspace(worktree: temp.url.appendingPathComponent("repo", isDirectory: true))
        workspaces.append(git)
        try await git.initialize(files: ["a.txt": "one\n"], commitMessage: "base\n", author: GitAuthor(name: testAuthor.name, email: testAuthor.email))
        let record = LocalRepoRecord(name: "repo", path: git.worktree.path, createdAt: Date(), publishedAs: "alice/repo")
        return (git, GitAgentWorkspace(git), record)
    }

    func testAFinishedRunOffersAPullRequestAndSubmitsIt() async throws {
        let (git, workspace, record) = try await realRepo()
        let pushed = Box<[String]>([])
        let submitted = Box<[(owner: String, title: String, head: String, patch: String)]>([])
        let service = PullRequestService(
            pushBranch: { _, _, _, branch in
                pushed.value.append(branch)
                return LocalPushResult(refs: [LocalPushRef(remoteRef: "refs/heads/\(branch)", newId: String(repeating: "a", count: 40), outcome: "updated")], packBytes: 1, objects: 1)
            },
            submitPull: { owner, _, title, _, head, patch in
                _ = head
                submitted.value.append((owner, title, head, patch))
            })
        let screen = model(workspace, store: configuredStore(), script: [
            toolTurn("write_file", ["path": "a.txt", "content": "two\n"], id: "1"),
            toolTurn("git_commit", ["message": "Update a.txt"], id: "2"),
            AgentTurn(text: "Done.", toolCalls: []),
        ], git: git, record: record, pullRequests: service)
        screen.load()
        screen.draft = "change a.txt"
        _ = await screen.send()
        XCTAssertNil(screen.prCandidate, "no offer while the run is going")
        await screen.handle?.waitUntilDone()

        let candidate = try XCTUnwrap(screen.prCandidate)
        XCTAssertTrue(candidate.branch.hasPrefix("agent/"))
        XCTAssertEqual(screen.suggestedPullTitle, "change a.txt")
        let prepared = await screen.preparePullRequest()
        let preparation = try XCTUnwrap(prepared)
        XCTAssertTrue(preparation.isReady, preparation.message)
        XCTAssertTrue(preparation.summary.contains("1 file"))

        let line = await screen.submitPullRequest(preparation, title: "change a.txt")
        XCTAssertEqual(line, "Pull request opened: \(preparation.summary)")
        XCTAssertEqual(pushed.value.count, 1)
        XCTAssertTrue(pushed.value.first?.hasPrefix("agent/") ?? false)
        XCTAssertEqual(submitted.value.first?.owner, "alice")
        XCTAssertEqual(submitted.value.first?.title, "change a.txt")
        XCTAssertTrue(submitted.value.first?.patch.contains("diff --git a/a.txt b/a.txt") ?? false)
        // The offer disappears once the pull request exists.
        XCTAssertNil(screen.prCandidate)
    }

    func testARunWithNothingToSendExplainsTheBlocker() async throws {
        let (git, workspace, record) = try await realRepo()
        let service = PullRequestService(
            pushBranch: { _, _, _, _ in throw PullRequestSubmitError("must not push") },
            submitPull: { _, _, _, _, _, _ in throw PullRequestSubmitError("must not submit") })
        let screen = model(workspace, store: configuredStore(), script: [AgentTurn(text: "Nothing to do.", toolCalls: [])],
                           git: git, record: record, pullRequests: service)
        screen.load()
        screen.draft = "do nothing"
        _ = await screen.send()
        await screen.handle?.waitUntilDone()
        let prepared = await screen.preparePullRequest()
        let preparation = try XCTUnwrap(prepared)
        XCTAssertFalse(preparation.isReady)
        XCTAssertTrue(preparation.message.contains("no commits"), preparation.message)
    }

    func testTheAddButtonNamesAWorkingTreeFileInThePrompt() async throws {
        XCTAssertEqual(AgentScreenModel.insertReference("a.txt", into: "").text, "`a.txt` ")
        XCTAssertEqual(AgentScreenModel.insertReference("a.txt", into: "look at").text, "look at `a.txt` ")
        let middle = AgentScreenModel.insertReference("b.md", into: "see  now", at: 4)
        XCTAssertEqual(middle.text, "see `b.md`  now")
        XCTAssertEqual(middle.caret, 11)

        let (git, _, _) = try await realRepo()
        try FileManager.default.createDirectory(at: git.worktree.appendingPathComponent("lib"), withIntermediateDirectories: true)
        try Data("x".utf8).write(to: git.worktree.appendingPathComponent("lib/b.dart"))
        let picker = AttachFileModel(root: git.worktree)
        await picker.scan()
        XCTAssertEqual(picker.paths, ["a.txt", "lib/b.dart"])
        picker.query = "LIB"
        XCTAssertEqual(picker.matches, ["lib/b.dart"])
        picker.query = "zzz"
        XCTAssertEqual(picker.emptyText, "No file matches that.")
    }

    func testAGemmaSessionNamesTheOnDeviceModel() async throws {
        let manager = try localModels(FakeLocalChatEngine(), in: temp)
        let store = AgentCredentialStore(secureStore: InMemorySecureStore(), http: FakeHTTPClient())
        store.write(.onDevice(model: tinyModel.id))
        let screen = AgentScreenModel(
            repoName: "repo", workspace: MemoryWorkspace(files: [:]), sessions: AgentSessionService(prefs: InMemoryKeyValueStore()),
            store: store, author: testAuthor, localModels: { manager })
        screen.load()
        XCTAssertEqual(screen.modelChipLabel, "Tiny")
        XCTAssertEqual(screen.gemmaReadiness?.isReady, true)
    }

    // MARK: - Setup sheet

    func testAKeyInTheWrongShapeIsCaughtBeforeAnyRequest() async {
        let http = FakeHTTPClient()
        let setup = AgentSetupModel(store: AgentCredentialStore(secureStore: InMemorySecureStore(), http: http), initial: nil, localModels: { nil })
        setup.key = "hello"
        let saved = await setup.save()
        XCTAssertNil(saved)
        XCTAssertEqual(setup.problem, "That does not look like a Claude key. Claude keys start with \"sk-ant-\".")
        XCTAssertTrue(http.requests.isEmpty)
    }

    func testSwitchingProviderSwapsTheModelList() async {
        let setup = AgentSetupModel(store: AgentCredentialStore(secureStore: InMemorySecureStore(), http: FakeHTTPClient()), initial: nil, localModels: { nil })
        XCTAssertEqual(setup.model, "claude-opus-5-5")
        setup.selectKind(.openai)
        XCTAssertEqual(setup.models.map(\.id), ["gpt-5.3-codex", "gpt-5.3"])
        XCTAssertEqual(setup.model, "gpt-5.3-codex")
        XCTAssertEqual(setup.keyFieldLabel, "OpenAI API key")
    }

    func testAnOpenAIKeyPastedIntoTheClaudeFieldIsRefused() async {
        let setup = AgentSetupModel(store: AgentCredentialStore(secureStore: InMemorySecureStore(), http: FakeHTTPClient()), initial: nil, localModels: { nil })
        setup.key = "sk-proj-abc123456789012345"
        let saved = await setup.save()
        XCTAssertNil(saved)
        XCTAssertTrue(setup.problem?.contains("\"sk-ant-\"") ?? false)
    }

    func testAVerifiedKeyIsStored() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["content": [], "stop_reason": "end_turn"])
        let secure = InMemorySecureStore()
        let store = AgentCredentialStore(secureStore: secure, http: http)
        let setup = AgentSetupModel(store: store, initial: nil, localModels: { nil })
        setup.key = "  sk-ant-api03-aaaaaaaaaaaaaaa  " // forkmesh-secret-scan:ignore-line
        let saved = await setup.save()
        XCTAssertEqual(saved?.apiKey, "sk-ant-api03-aaaaaaaaaaaaaaa") // forkmesh-secret-scan:ignore-line
        XCTAssertEqual(store.read(), saved)
    }

    func testGemmaNeedsNoKeyButADownloadedModel() async throws {
        let manager = try localModels(FakeLocalChatEngine(), in: temp, enabled: false)
        let store = AgentCredentialStore(secureStore: InMemorySecureStore(), http: FakeHTTPClient(), localModels: manager)
        let setup = AgentSetupModel(store: store, initial: nil, localModels: { manager })
        setup.selectKind(.onDevice)
        XCTAssertEqual(setup.models.map(\.id), [tinyModel.id])
        XCTAssertEqual(setup.model, tinyModel.id)
        XCTAssertEqual(setup.readiness?.canBeFixedInSettings, true)
        let refused = await setup.save()
        XCTAssertNil(refused)
        XCTAssertEqual(setup.problem, "On-device AI is off. Turn it on and download Tiny to run agents on this iPhone.")

        manager.setEnabled(true)
        let saved = await setup.save()
        XCTAssertEqual(saved, .onDevice(model: tinyModel.id))
        XCTAssertEqual(store.read()?.kind, .onDevice)
    }
}
