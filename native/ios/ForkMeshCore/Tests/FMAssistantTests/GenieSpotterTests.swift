import FMApi
import FMBase
import FMLocalAI
import XCTest
@testable import FMAssistant

@MainActor
private final class SpotActions: GenieActions {
    var me = "ada"
    var paletteContext = PaletteContext()
    var commands: [PaletteCommand] = []
    var started: [String] = []

    func run(_ command: PaletteCommand) {
        commands.append(command)
    }

    func startAgent(prompt: String, repository: String?) async -> String {
        started.append(prompt)
        return "Started an agent."
    }
}

@MainActor
private final class AmbientVoice: GenieVoice {
    var overheard: [GenieHeard] = []
    var heard: [GenieHeard] = []
    var said: [String] = []
    var ambientCalls = 0

    func say(_ text: String) async {
        said.append(text)
    }

    func listen(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        guard !heard.isEmpty else { return .silence }
        let next = heard.removeFirst()
        if case .words(let words) = next { onWords(words) }
        return next
    }

    func listenAmbient(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        ambientCalls += 1
        guard !overheard.isEmpty else {
            try? await Task.sleep(for: .milliseconds(20))
            return .silence
        }
        let next = overheard.removeFirst()
        if case .words(let words) = next { onWords(words) }
        return next
    }

    func stopListening() {}
    func stopSpeaking() {}
}

@MainActor
final class GenieSpotterTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_790_000_000)

    private func phrase(_ text: String, _ spot: GenieSpot?) -> String {
        guard let spot else { return "" }
        return GenieSpotter.segments(text, highlight: spot.phrase).filter(\.actionable).map(\.text).joined()
    }

    func testSpotsReadingTheLatestThings() {
        let inbox = "Can you read the last message from the inbox?"
        let spot = GenieSpotter.spot(inbox)
        XCTAssertEqual(spot?.tool, "read_latest")
        XCTAssertEqual(spot?.arguments, ["what": "inbox"])
        XCTAssertEqual(spot?.page, "Inbox")
        XCTAssertEqual(spot?.route, .inbox(.all))
        XCTAssertEqual(spot?.addressed, true)
        XCTAssertEqual(phrase(inbox, spot), "last message from the inbox")

        let commit = "check what the last commit was"
        XCTAssertEqual(GenieSpotter.spot(commit)?.arguments, ["what": "commit"])
        XCTAssertEqual(phrase(commit, GenieSpotter.spot(commit)), "last commit")

        XCTAssertEqual(GenieSpotter.spot("read the last chats")?.arguments, ["what": "chat", "count": "3"])
        XCTAssertEqual(GenieSpotter.spot("what are my last two pull requests")?.arguments, ["what": "pull", "count": "2"])
        XCTAssertEqual(GenieSpotter.spot("what's my next meeting")?.arguments, ["what": "event"])
        XCTAssertEqual(GenieSpotter.spot("read my inbox")?.arguments, ["what": "inbox", "count": "3"])
        XCTAssertEqual(GenieSpotter.spot("any new pings")?.arguments, ["what": "inbox", "count": "3"])
    }

    func testSpotsSearchAgentsAndScreens() {
        let search = GenieSpotter.spot("open up a search prompt")
        XCTAssertEqual(search?.tool, "open_search")
        XCTAssertEqual(search?.symbol, "magnifyingglass")
        XCTAssertEqual(GenieSpotter.spot("search for release notes please")?.arguments, ["query": "release notes"])
        XCTAssertEqual(GenieSpotter.spot("look up gemma")?.arguments, ["query": "gemma"])

        let task = GenieSpotter.spot("Can you create a task for me to make the genie smarter")
        XCTAssertEqual(task?.tool, "start_agent")
        XCTAssertEqual(task?.arguments, ["prompt": "make the genie smarter"])
        XCTAssertEqual(task?.needsApproval, true)
        XCTAssertEqual(task?.runsAlone, false)
        XCTAssertEqual(GenieSpotter.spot("start an agent on forkmesh/site to fix the header")?.arguments["repository"], "forkmesh/site")
        XCTAssertNotEqual(GenieSpotter.spot("create a new task")?.tool, "read_latest")

        let calendar = GenieSpotter.spot("show me my calendar")
        XCTAssertEqual(calendar?.tool, "open_screen")
        XCTAssertEqual(calendar?.page, "Calendar")
        XCTAssertNil(GenieSpotter.spot("show me how you did it"))
        XCTAssertNil(GenieSpotter.spot("I had lunch with Bob"))
    }

    func testAddressingAndWakeWords() {
        XCTAssertEqual(GenieSpotter.spot("I saw the last commit was broken")?.addressed, false)
        XCTAssertEqual(GenieSpotter.spot("genie what was the last commit")?.addressed, true)
        XCTAssertTrue(GenieSpotter.calledByName("Hey Genie, play some music"))
        XCTAssertFalse(GenieSpotter.calledByName("General Kenobi"))
        XCTAssertEqual(GenieSpotter.withoutWakeWord("Hey genie, open my inbox"), "open my inbox")
        XCTAssertEqual(GenieSpotter.withoutWakeWord("open my inbox, genie"), "open my inbox")
        XCTAssertEqual(
            GenieSpotter.segments("so read the last commit now", highlight: 3..<23),
            [
                GenieSegment(text: "so ", actionable: false), GenieSegment(text: "read the last commit", actionable: true),
                GenieSegment(text: " now", actionable: false),
            ])
    }

    private func toolbox(_ actions: SpotActions) -> GenieToolbox {
        let sources = AssistantSources(
            account: { AssistantAccount(username: "ada") },
            repositories: { [Repository(owner: "ada", name: "site", updatedMs: 5)] },
            commits: { owner, name in
                XCTAssertEqual("\(owner)/\(name)", "ada/site")
                return [
                    ["sha": "aaaaaaa111", "subject": "Older work", "author": "bob", "timestamp": .int(1_700_000_000)],
                    ["sha": "bbbbbbb222", "subject": "Make the genie smarter\n\nbody", "author": .object(["name": "Ada"]), "timestamp": .int(1_790_000_000)],
                ]
            },
            chatMessages: {
                [
                    ChatMessage(id: "m1", conversation: "#general", senderId: "b", senderName: "bob", text: "hi", timestamp: self.now.addingTimeInterval(-60)),
                    ChatMessage(id: "m2", conversation: "#general", senderId: "b", senderName: "bob", text: "release is tomorrow", timestamp: self.now),
                ]
            },
            pings: { _ in
                NotificationPage(notifications: [
                    ForkNotification(id: "p2", kind: "review", title: "Review merged", ts: 1, readAt: 2),
                    ForkNotification(id: "p1", kind: "mention", title: "bob mentioned you", body: "look at this", actor: "bob", ts: 1_790_000_000_000),
                ], unread: 1)
            })
        let data = ForkMeshToolbox(sources: sources, timeZone: TimeZone(identifier: "UTC")!, now: { self.now })
        return GenieToolbox(data: data, actions: actions)
    }

    func testReadLatestReadsTheNewestItem() async {
        let genie = toolbox(SpotActions())
        let inbox = await genie.invoke(LocalToolCall(id: "1", name: "read_latest", arguments: ["what": "inbox"]))
        XCTAssertTrue(inbox.text.hasPrefix("The newest in your inbox: "), inbox.text)
        XCTAssertTrue(inbox.text.contains("from bob: bob mentioned you. look at this · unread"), inbox.text)
        XCTAssertTrue(inbox.text.hasSuffix("You have 1 unread."), inbox.text)

        let commit = await genie.invoke(LocalToolCall(id: "2", name: "read_latest", arguments: ["what": "commits"]))
        XCTAssertTrue(commit.text.hasPrefix("The last commit in ada/site: "), commit.text)
        XCTAssertTrue(commit.text.hasSuffix("by Ada: Make the genie smarter (bbbbbbb)"), commit.text)

        let chats = await genie.invoke(LocalToolCall(id: "3", name: "read_latest", arguments: ["what": "chat", "count": .int(2)]))
        XCTAssertTrue(chats.text.hasPrefix("The last 2 chat messages:\n- "), chats.text)
        XCTAssertTrue(chats.text.contains("bob said: release is tomorrow\n- "), chats.text)

        let wrong = await genie.invoke(LocalToolCall(id: "4", name: "read_latest", arguments: ["what": "weather"]))
        XCTAssertTrue(wrong.text.hasPrefix("read_latest needs what"), wrong.text)
    }

    func testReadLatestUsesTheRepositoryOnScreen() {
        let actions = SpotActions()
        actions.paletteContext = PaletteContext(target: ComposeTarget(repo: RepoRef(owner: "forkmesh", name: "core")))
        let genie = GenieToolbox(data: nil, actions: actions)
        let commit = genie.withScreenRepository(LocalToolCall(id: "1", name: "read_latest", arguments: ["what": "commit"]))
        XCTAssertEqual(commit.arguments["repository"], "forkmesh/core")
        let inbox = genie.withScreenRepository(LocalToolCall(id: "2", name: "read_latest", arguments: ["what": "inbox"]))
        XCTAssertNil(inbox.arguments["repository"])
        let named = genie.withScreenRepository(LocalToolCall(id: "3", name: "read_latest", arguments: ["what": "commit", "repository": "ada/site"]))
        XCTAssertEqual(named.arguments["repository"], "ada/site")
    }

    func testOpenSearchOpensAnEmptyPrompt() async {
        let actions = SpotActions()
        let result = await GenieToolbox(data: nil, actions: actions).invoke(LocalToolCall(id: "1", name: "open_search", arguments: [:]))
        XCTAssertEqual(result.text, "Opening search.")
        XCTAssertEqual(actions.commands, [.searchEverywhere("")])
    }

    func testBrainTakesTheFastPathEvenWithAModel() async {
        let engine = FakeLocalChatEngine()
        let actions = SpotActions()
        let brain = GenieBrain(
            toolbox: toolbox(actions), memory: GenieMemory(store: InMemoryKeyValueStore()),
            model: GenieModel(name: "Fake", engine: engine))
        let outcome = await brain.begin("Read the last message from my inbox")
        guard case .reply(let reply) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertTrue(reply.hasPrefix("The newest in your inbox:"), reply)
        XCTAssertTrue(engine.requests.isEmpty)
        XCTAssertEqual(brain.steps.map(\.tool), ["read_latest"])
    }

    func testBrainAsksBeforeStartingASpottedAgent() async {
        let actions = SpotActions()
        let brain = GenieBrain(toolbox: GenieToolbox(data: nil, actions: actions), memory: GenieMemory(store: nil), model: nil)
        var asked: [String] = []
        let declined = await brain.begin("create a task for me to make the genie smarter", confirm: { summary in
            asked.append(summary)
            return false
        })
        XCTAssertEqual(declined, .reply("Okay, I won't."))
        XCTAssertEqual(asked, ["Start an agent: make the genie smarter"])
        XCTAssertTrue(actions.started.isEmpty)
        let allowed = await brain.begin("create a task for me to make the genie smarter", confirm: { _ in true })
        XCTAssertEqual(allowed, .reply("Started an agent."))
        XCTAssertEqual(actions.started, ["make the genie smarter"])
    }

    private func wait(_ timeout: Double = 3, until done: @MainActor () -> Bool) async {
        let deadline = Date().addingTimeInterval(timeout)
        while !done() && Date() < deadline {
            try? await Task.sleep(for: .milliseconds(10))
        }
    }

    func testAlwaysListeningActsOnlyOnWhatIsMeant() async {
        let actions = SpotActions()
        let voice = AmbientVoice()
        voice.overheard = [
            .words("I had lunch with Bob"),
            .words("I think the last commit broke it"),
            .words("genie, read the last message from my inbox"),
        ]
        let brain = GenieBrain(toolbox: toolbox(actions), memory: GenieMemory(store: nil), model: nil)
        let session = GenieSession(brain: brain, voice: voice) { _ in "" }
        var spotted: [String] = []
        session.onSpot = { spot, runs in spotted.append("\(spot.tool):\(runs)") }
        session.setAlwaysListening(true)
        await wait { !voice.said.isEmpty }
        await session.waitUntilIdle()
        session.setAlwaysListening(false)
        XCTAssertEqual(spotted, ["read_latest:false", "read_latest:true"])
        XCTAssertEqual(voice.said.count, 1)
        XCTAssertTrue(voice.said[0].hasPrefix("The newest in your inbox:"), voice.said[0])
        XCTAssertEqual(session.lines.first?.text, "genie, read the last message from my inbox")
        XCTAssertFalse(session.alwaysListening)
        XCTAssertNil(session.overheard)
    }

    func testCallingTheGenieByNameAsksTheBrain() async {
        let voice = AmbientVoice()
        voice.overheard = [.words("hey genie make the app purple")]
        var launched: [String] = []
        let session = GenieSession(
            brain: GenieBrain(toolbox: GenieToolbox(data: nil, actions: SpotActions()), memory: GenieMemory(store: nil), model: nil),
            voice: voice
        ) { handoff in
            launched.append(handoff.request)
            return "An agent is on it."
        }
        session.setAlwaysListening(true)
        await wait { !launched.isEmpty }
        await session.waitUntilIdle()
        session.setAlwaysListening(false)
        XCTAssertEqual(launched, ["make the app purple"])
    }

    func testTappingASpotPerformsIt() async {
        let actions = SpotActions()
        let voice = AmbientVoice()
        let session = GenieSession(
            brain: GenieBrain(toolbox: toolbox(actions), memory: GenieMemory(store: nil), model: nil), voice: voice
        ) { _ in "" }
        let spot = GenieSpotter.spot("open up a search prompt")!
        session.perform(spot)
        await session.waitUntilIdle()
        XCTAssertEqual(actions.commands, [.searchEverywhere("")])
        XCTAssertEqual(voice.said, ["Opening search."])
        XCTAssertEqual(session.phase, .finished)
    }
}
