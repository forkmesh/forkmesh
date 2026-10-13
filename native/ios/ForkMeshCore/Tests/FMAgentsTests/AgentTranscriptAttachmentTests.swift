import FMAccount
import FMApi
import FMBase
import FMCode
import XCTest
@testable import FMAgents

private let jpeg = Data([0xFF, 0xD8, 0xFF, 0xE0] + [UInt8](repeating: 3, count: 50))
private let relayId = String(repeating: "d", count: 32)

private struct DesktopOutOfReach: Error {}

@MainActor
final class PhotoAgentsApi: AgentsApi {
    let inner = FakeAgentsApi()
    var steeredPhotos: [[PromptAttachment]] = []
    var launchedPhotos: [[PromptAttachment]] = []
    var failSteer: AgentLaunchException?
    var listsAttachments = true
    var steerGate: CheckedContinuation<Void, Never>?
    var holdSteer = false

    private func turn(_ text: String, _ photos: [PromptAttachment]) -> AgentLaunchMessage {
        AgentLaunchMessage(
            role: "user", author: "alice", text: text,
            attachments: listsAttachments
                ? photos.enumerated().map { index, photo in
                    AgentAttachmentMeta(
                        id: String(repeating: String(index + 1), count: 32), name: photo.name,
                        mediaType: photo.mediaType, size: photo.size)
                }
                : [])
    }

    func repositories() async throws -> [Repository] { try await inner.repositories() }
    func agentSessionPage(_ owner: String, _ name: String, ownerAccount: String) async throws -> AgentSessionPage {
        try await inner.agentSessionPage(owner, name, ownerAccount: ownerAccount)
    }
    func agentTranscript(_ owner: String, _ name: String, _ id: Int, ownerAccount: String) async throws -> AgentTranscript {
        AgentTranscript(status: "done", transcript: "Fix this\nAttached image: /home/f/agent-images/paste-1.png\nDone.")
    }
    func agentLaunchOptions(_ owner: String, _ repo: String) async throws -> AgentLaunchOptions {
        try await inner.agentLaunchOptions(owner, repo)
    }
    func publishedPulls(_ owner: String, _ name: String) async throws -> [PublishedPull] { [] }
    func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String, model: String,
        targetNode: String, title: String, createPr: Bool, issueNumber: Int, orgScope: Bool
    ) async throws -> AgentQueueResult {
        try await launchAgentSession(
            owner, repo, prompt: prompt, provider: provider, model: model, targetNode: targetNode, title: title,
            createPr: createPr, issueNumber: issueNumber, orgScope: orgScope, attachments: [])
    }
    func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String, model: String,
        targetNode: String, title: String, createPr: Bool, issueNumber: Int, orgScope: Bool,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult {
        launchedPhotos.append(attachments)
        let text = promptOrPhotoNote(prompt, attachments)
        return AgentQueueResult(
            queueState: "ready_for_claim", targetOnline: true, message: "Queued.",
            session: AgentLaunchSession(
                sessionId: "sess-new", owner: owner, repo: repo, status: "queued", history: [turn(text, attachments)]))
    }
    func steerAgentSession(_ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool) async throws -> AgentQueueResult {
        try await steerAgentSession(owner, repo, sessionId, prompt: prompt, orgScope: orgScope, attachments: [])
    }
    func steerAgentSession(
        _ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult {
        if holdSteer { await withCheckedContinuation { steerGate = $0 } }
        if let failSteer { throw failSteer }
        steeredPhotos.append(attachments)
        var options = inner.options["alice/mobile"] ?? AgentLaunchOptions()
        var session = options.sessions.first ?? AgentLaunchSession(sessionId: sessionId)
        session.history.append(turn(promptOrPhotoNote(prompt, attachments), attachments))
        options.sessions = [session]
        inner.options["alice/mobile"] = options
        return AgentQueueResult(message: "Follow-up sent.")
    }
    func retryAgentSession(_ owner: String, _ repo: String, _ session: AgentLaunchSession) async throws -> AgentRetryResult {
        try await inner.retryAgentSession(owner, repo, session)
    }
    func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws {}
}

@MainActor
final class AgentTranscriptAttachmentTests: XCTestCase {
    private let repo = Repository(owner: "alice", name: "mobile")
    private let photo = PromptAttachment(id: "local-1", name: "photo-1.jpg", bytes: jpeg)

    private func claimed() -> AgentLaunchSession {
        AgentLaunchSession(
            sessionId: "sess-9", owner: "alice", repo: "mobile", status: "running",
            history: [AgentLaunchMessage(role: "user", text: "Start")], localAgentId: 3)
    }

    private func screen(_ api: PhotoAgentsApi, store: AgentAttachmentStore, sent: AgentSentPhotos) -> LaunchedAgentSessionModel {
        api.inner.options["alice/mobile"] = AgentLaunchOptions(sessions: [claimed()])
        return LaunchedAgentSessionModel(
            api: api, repo: repo, session: claimed(), attachmentLoader: AgentAttachmentLoader(store: store),
            sentPhotos: sent)
    }

    func testAFollowUpsPhotosShowAtOnceThenUnderTheRelaysIds() async throws {
        let api = PhotoAgentsApi()
        api.holdSteer = true
        let store = AgentAttachmentStore()
        let model = screen(api, store: store, sent: AgentSentPhotos())
        model.followUp = "Match this"
        model.followUpAttachments = [photo]
        let sending = Task { await model.send() }
        while api.steerGate == nil { await Task.yield() }

        XCTAssertEqual(model.displayHistory.count, 2)
        XCTAssertTrue(model.isPending(1))
        XCTAssertEqual(model.displayHistory[1].state, "sending")
        XCTAssertEqual(model.attachments(at: 1).map(\.origin), [.local])
        XCTAssertEqual(model.followUp, "")
        XCTAssertTrue(model.followUpAttachments.isEmpty)
        let local = try await model.attachmentLoader.load(model.attachments(at: 1)[0])
        XCTAssertEqual(local, jpeg)

        api.steerGate?.resume()
        await sending.value
        XCTAssertNil(model.pending)
        XCTAssertEqual(api.steeredPhotos.first?.map(\.id), ["local-1"])
        let refs = model.attachments(at: 1)
        XCTAssertEqual(refs.map(\.meta.id), [String(repeating: "1", count: 32)])
        XCTAssertEqual(refs[0].origin, .relay(owner: "alice", repo: "mobile", sessionId: "sess-9", orgScope: false))
        let cached = await store.cached(refs[0].cacheKey)
        XCTAssertEqual(cached, jpeg)
    }

    func testARelayThatDoesNotListPhotosStillShowsTheSenderTheirs() async throws {
        let api = PhotoAgentsApi()
        api.listsAttachments = false
        let sent = AgentSentPhotos()
        let model = screen(api, store: AgentAttachmentStore(), sent: sent)
        model.followUp = ""
        model.followUpAttachments = [photo]
        await model.send()
        XCTAssertEqual(model.session.history.last?.text, "See the attached photos.")
        XCTAssertEqual(model.attachments(at: 1).map(\.origin), [.local])
        XCTAssertTrue(model.attachments(at: 0).isEmpty)
    }

    func testAFailedSendPutsTheDraftBack() async {
        let api = PhotoAgentsApi()
        api.failSteer = AgentLaunchException("The relay is busy.")
        let model = screen(api, store: AgentAttachmentStore(), sent: AgentSentPhotos())
        model.followUp = "Match this"
        model.followUpAttachments = [photo]
        await model.send()
        XCTAssertNil(model.pending)
        XCTAssertEqual(model.displayHistory.count, 1)
        XCTAssertEqual(model.followUp, "Match this")
        XCTAssertEqual(model.followUpAttachments.map(\.id), ["local-1"])
        XCTAssertEqual(model.error, "The relay is busy.")
    }

    func testAQuickLaunchsPhotosAreThereWhenItsSessionOpens() async throws {
        let api = PhotoAgentsApi()
        api.inner.options["alice/mobile"] = AgentLaunchOptions(
            canQueue: true, targets: [AgentLaunchTarget(node: "mirror35", providers: ["claude-code"], online: true)])
        let store = AgentAttachmentStore()
        let sent = AgentSentPhotos()
        let quick = QuickAgentLauncher(
            form: AgentStartModel(api: api, records: nil, signedIn: { true }), prefs: InMemoryKeyValueStore())
        quick.attachmentStore = store
        quick.sentPhotos = sent
        await quick.prepare(currentRepo: "alice/mobile")
        let outcome = await quick.send(prompt: "What is wrong here?", images: [photo])
        guard case .queued(let opened, let session?, _) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertEqual(api.launchedPhotos.first?.map(\.id), ["local-1"])
        let model = LaunchedAgentSessionModel(
            api: api, repo: opened, session: session, attachmentLoader: AgentAttachmentLoader(store: store), sentPhotos: sent)
        let refs = model.attachments(at: 0)
        XCTAssertEqual(refs.count, 1)
        let cached = await store.cached(refs[0].cacheKey)
        XCTAssertEqual(cached, jpeg)
    }

    func testADesktopTranscriptNamesItsFilesAsRows() async {
        let model = AgentTranscriptModel(
            api: PhotoAgentsApi(), repo: repo, session: AgentSession(id: 7, status: "done"), ownerAccount: "alice")
        await model.load()
        XCTAssertEqual(model.attachments.map(\.name), ["paste-1.png"])
        XCTAssertFalse(model.attachments[0].canFetch)
    }

    func testADesktopSessionShowsThePicturesTheDesktopLists() async {
        let listed = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: "a1", name: "paste-1.png", mediaType: "image/png", size: 54),
            origin: .desktop(sessionId: 7))
        let model = AgentTranscriptModel(
            api: PhotoAgentsApi(), repo: repo, session: AgentSession(id: 7, status: "done"), ownerAccount: "alice",
            desktopAttachments: { [listed] })
        await model.load()
        XCTAssertEqual(model.attachments, [listed])
        XCTAssertTrue(model.attachments[0].showsThumbnail)
        await model.load()
        XCTAssertEqual(model.attachments, [listed])
    }

    func testADesktopOutOfReachLeavesTheNamedFiles() async {
        let model = AgentTranscriptModel(
            api: PhotoAgentsApi(), repo: repo, session: AgentSession(id: 7, status: "done"), ownerAccount: "alice",
            desktopAttachments: { throw DesktopOutOfReach() })
        await model.load()
        XCTAssertEqual(model.attachments.map(\.name), ["paste-1.png"])
    }

    func testThePhonesOwnRunShowsThePhotosOnItsPrompt() async throws {
        let workspace = MemoryWorkspace(files: ["a.txt": "one\n"])
        let service = AgentSessionService(prefs: InMemoryKeyValueStore())
        let provider = ScriptedProvider([AgentTurn(text: "It is a cat.", toolCalls: [])])
        let task = try XCTUnwrap(service.submit(repoName: "repo", prompt: "What is this?", images: [photo]))
        let handle = try await service.start(
            task: task, workspace: workspace,
            buildAgent: { toolset in CodingAgent(provider: provider, toolset: toolset) }, author: testAuthor)
        await handle.waitUntilDone()
        let index = try XCTUnwrap(handle.events.firstIndex { $0.userText != nil })
        XCTAssertEqual(handle.attachments(at: index).map(\.meta.id), ["local-1"])
        XCTAssertTrue(handle.attachments(at: index + 1).isEmpty)
        let bytes = try await AgentAttachmentLoader().load(handle.attachments(at: index)[0])
        XCTAssertEqual(bytes, jpeg)
    }
}
