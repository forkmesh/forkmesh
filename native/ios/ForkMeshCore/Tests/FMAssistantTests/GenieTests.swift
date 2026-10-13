import FMApi
import FMBase
import FMLocalAI
import XCTest
@testable import FMAssistant

@MainActor
private final class RecordingActions: GenieActions {
    var me = "ada"
    var paletteContext = PaletteContext()
    var commands: [PaletteCommand] = []
    var started: [(prompt: String, repository: String?)] = []

    func run(_ command: PaletteCommand) {
        commands.append(command)
    }

    func startAgent(prompt: String, repository: String?) async -> String {
        started.append((prompt, repository))
        return "Started an agent."
    }
}

@MainActor
private final class ScriptedVoice: GenieVoice {
    var heard: [GenieHeard] = []
    var said: [String] = []

    func say(_ text: String) async {
        said.append(text)
    }

    func listen(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        guard !heard.isEmpty else { return .silence }
        let next = heard.removeFirst()
        if case .words(let words) = next { onWords(words) }
        return next
    }

    func stopListening() {}
    func stopSpeaking() {}
}

@MainActor
final class GenieTests: XCTestCase {
        private func toolCall(_ name: String, _ args: JSONObject) -> FakeLocalChatEngine.Step {
        .event(.toolCall(LocalToolCall(id: LocalToolCall.makeId(), name: name, arguments: args)))
    }

    private var finished: FakeLocalChatEngine.Step {
        .event(.finished(LocalGenerationStats(promptTokens: 1, generatedTokens: 1, tokensPerSecond: 1)))
    }

    private func makeBrain(_ engine: FakeLocalChatEngine?, actions: RecordingActions, store: KeyValueStore = InMemoryKeyValueStore()) -> GenieBrain {
        let toolbox = GenieToolbox(data: nil, actions: actions)
        let model = engine.map { engine in
            GenieModel(name: "Fake", engine: engine) {
                try await engine.load(modelDirectory: URL(fileURLWithPath: "/m"), model: LocalModelCatalog.gemma4E2B)
            }
        }
        return GenieBrain(toolbox: toolbox, memory: GenieMemory(store: store), model: model)
    }

    func testRouteNamesResolveToScreens() {
        XCTAssertEqual(GenieToolbox.route(named: "Inbox", me: "ada"), .inbox(.all))
        XCTAssertEqual(GenieToolbox.route(named: "my inbox please", me: "ada"), .inbox(.all))
        XCTAssertEqual(GenieToolbox.route(named: "forkmesh/forkmesh#12", me: "ada"), .issue(RepoRef(owner: "forkmesh", name: "forkmesh"), number: 12))
        XCTAssertEqual(GenieToolbox.route(named: "@bob", me: "ada"), .profile(name: "bob"))
        XCTAssertNotNil(GenieToolbox.route(named: "the settings screen", me: "ada"))
        XCTAssertNil(GenieToolbox.route(named: "zzqx", me: "ada"))
    }

    func testOpenScreenSelectsTabsAndOpensRoutes() async {
        let actions = RecordingActions()
        let toolbox = GenieToolbox(data: nil, actions: actions)
        let opened = await toolbox.invoke(LocalToolCall(id: "1", name: "open_screen", arguments: ["screen": "chat"]))
        XCTAssertEqual(opened.text, "Opening Chat.")
        XCTAssertEqual(actions.commands, [.selectTab(.chat)])
        XCTAssertTrue(toolbox.navigated)
        let missing = await toolbox.invoke(LocalToolCall(id: "2", name: "open_screen", arguments: ["screen": "zzqx"]))
        XCTAssertTrue(missing.text.hasPrefix("No screen matches"))
    }

    func testFindAndRunAppActions() async {
        let actions = RecordingActions()
        let toolbox = GenieToolbox(data: nil, actions: actions)
        let found = await toolbox.invoke(LocalToolCall(id: "1", name: "find_app_actions", arguments: ["query": "timer"]))
        XCTAssertTrue(found.text.contains("[timer]"), found.text)
        let ran = await toolbox.invoke(LocalToolCall(id: "2", name: "run_app_action", arguments: ["id": "timer"]))
        XCTAssertEqual(ran.text, "Running Start a timer.")
        XCTAssertEqual(actions.commands, [.open(.timers)])
    }

    func testStartAgentNeedsApproval() async {
        let toolbox = GenieToolbox(data: nil, actions: RecordingActions())
        let summary = await toolbox.approval(for: LocalToolCall(id: "1", name: "start_agent", arguments: ["prompt": "Fix the build", "repository": "ada/site"]))
        XCTAssertEqual(summary, "Start an agent on ada/site: Fix the build")
        let none = await toolbox.approval(for: LocalToolCall(id: "2", name: "open_screen", arguments: ["screen": "inbox"]))
        XCTAssertNil(none)
    }

    func testBrainRunsToolsThenReplies() async {
        let engine = FakeLocalChatEngine()
        engine.enqueue([toolCall("open_screen", ["screen": "inbox"]), finished])
        engine.enqueueReply(["Opening your **Inbox**."])
        let actions = RecordingActions()
        let brain = makeBrain(engine, actions: actions)
        let outcome = await brain.begin("I'd like to see what came in")
        XCTAssertEqual(outcome, .reply("Opening your Inbox."))
        XCTAssertEqual(actions.commands, [.selectTab(.inbox)])
        XCTAssertEqual(brain.steps.map(\.tool), ["open_screen"])
        XCTAssertEqual(engine.requests.count, 2)
        XCTAssertEqual(engine.requests[1].messages.last?.role, .tool)
    }

    func testBrainAsksThenLearnsTheMeaning() async {
        let engine = FakeLocalChatEngine()
        engine.enqueue([toolCall(GenieToolbox.askUser, ["question": "Which board?"]), finished])
        engine.enqueueReply(["Here is the task board."])
        let store = InMemoryKeyValueStore()
        let brain = makeBrain(engine, actions: RecordingActions(), store: store)
        let first = await brain.begin("show the board")
        XCTAssertEqual(first, .clarify("Which board?"))
        XCTAssertTrue(brain.awaitingAnswer)
        let second = await brain.answer("the tasks one")
        XCTAssertEqual(second, .reply("Here is the task board."))
        XCTAssertEqual(GenieMemory(store: store).lessons, [GenieMemory.Lesson(request: "show the board", meaning: "the tasks one")])
        let third = makeBrain(engine, actions: RecordingActions(), store: store)
        engine.enqueueReply(["Ok."])
        _ = await third.begin("hello")
        XCTAssertTrue(engine.requests.last?.messages.first?.content.contains("\"show the board\" meant: the tasks one") ?? false)
    }

    func testSmallTalkGoesStraightToTheModelAndIsRemembered() async {
        let engine = FakeLocalChatEngine()
        engine.enqueueReply(["I'm doing great, thanks for asking! How about you?"])
        engine.enqueueReply(["Glad to hear it."])
        let brain = makeBrain(engine, actions: RecordingActions())
        let first = await brain.begin("how are you")
        XCTAssertEqual(first, .reply("I'm doing great, thanks for asking! How about you?"))
        XCTAssertEqual(engine.requests[0].messages.map(\.role), [.system, .user])
        XCTAssertEqual(engine.requests[0].messages.first?.content, GenieBrain.systemPrompt)
        _ = await brain.begin("pretty good")
        XCTAssertEqual(engine.requests[1].messages.map(\.content).dropFirst(), ["how are you", "I'm doing great, thanks for asking! How about you?", "pretty good"])
        brain.forgetConversation()
        engine.enqueueReply(["Hi."])
        _ = await brain.begin("hello")
        XCTAssertEqual(engine.requests[2].messages.count, 2)
    }

    func testBrainHandsOffWhenTheModelSaysSo() async {
        let engine = FakeLocalChatEngine()
        engine.enqueue([toolCall(GenieToolbox.handOff, ["reason": "No tool edits the theme"]), finished])
        let brain = makeBrain(engine, actions: RecordingActions())
        let outcome = await brain.begin("make the app purple")
        XCTAssertEqual(outcome, .handOff("No tool edits the theme"))
        let handoff = brain.handoff(reason: "No tool edits the theme", settings: "Opus · Auto")
        XCTAssertTrue(handoff.prompt.contains("Request: \"make the app purple\""))
        XCTAssertTrue(handoff.prompt.contains("The user's current agent settings: Opus · Auto"))
        XCTAssertTrue(handoff.prompt.contains("GenieToolbox.swift"))
    }

    func testBrainHandsOffAfterTooManyRounds() async {
        let engine = FakeLocalChatEngine()
        for round in 0..<GenieBrain.maxRounds {
            engine.enqueue([toolCall("search_app", ["query": .string("x\(round)")]), finished])
        }
        let outcome = await makeBrain(engine, actions: RecordingActions()).begin("loop forever")
        guard case .handOff = outcome else { return XCTFail("\(outcome)") }
    }

    func testSayRequests() {
        XCTAssertEqual(GenieWords.sayRequest("Can you say"), "")
        XCTAssertEqual(GenieWords.sayRequest("Genie, say \"good morning\" please"), "good morning")
        XCTAssertEqual(GenieWords.sayRequest("repeat after me: hello world"), "hello world")
        XCTAssertNil(GenieWords.sayRequest("can you say that again"))
        XCTAssertNil(GenieWords.sayRequest("what did you say"))
        XCTAssertNil(GenieWords.sayRequest("read the last task"))
    }

    func testCanYouSayAsksWhatToSayThenSaysIt() async {
        let engine = FakeLocalChatEngine()
        let brain = makeBrain(engine, actions: RecordingActions())
        let asked = await brain.begin("Can you say")
        XCTAssertEqual(asked, .clarify(GenieBrain.sayQuestion))
        XCTAssertTrue(brain.awaitingAnswer)
        let said = await brain.answer("good morning")
        XCTAssertEqual(said, .reply("good morning"))
        XCTAssertEqual(brain.steps.map(\.tool), [GenieToolbox.say])
        XCTAssertTrue(engine.requests.isEmpty)
    }

    func testModelSayToolSpeaksTheWords() async {
        let engine = FakeLocalChatEngine()
        engine.enqueue([toolCall(GenieToolbox.say, ["words": "Hello there"]), finished])
        let outcome = await makeBrain(engine, actions: RecordingActions()).begin("greet my friend")
        XCTAssertEqual(outcome, .reply("Hello there"))
        XCTAssertEqual(engine.requests.count, 1)
    }

    func testRepeatedToolCallAsksInsteadOfLooping() async {
        let engine = FakeLocalChatEngine()
        for _ in 0..<GenieBrain.maxRounds {
            engine.enqueue([toolCall("search_app", ["query": "x"]), finished])
        }
        let brain = makeBrain(engine, actions: RecordingActions())
        let outcome = await brain.begin("hmm")
        XCTAssertEqual(outcome, .clarify(GenieBrain.stuckQuestion))
        XCTAssertEqual(brain.steps.map(\.tool), ["search_app"])
        XCTAssertEqual(engine.requests.count, 2)
        XCTAssertTrue(brain.awaitingAnswer)
    }

    func testSessionAsksWhatToSayAndSaysIt() async {
        let voice = ScriptedVoice()
        voice.heard = [.words("Can you say"), .words("happy birthday")]
        let session = GenieSession(brain: makeBrain(nil, actions: RecordingActions()), voice: voice) { _ in "" }
        session.open()
        await session.waitUntilIdle()
        XCTAssertEqual(voice.said, [GenieSession.greeting, GenieBrain.sayQuestion, "happy birthday"])
    }

    func testWithoutAModelSimpleRequestsStillWork() async {
        let actions = RecordingActions()
        let brain = makeBrain(nil, actions: actions)
        let opened = await brain.begin("Open calendar")
        XCTAssertEqual(opened, .reply("Opening Calendar."))
        let searched = await brain.begin("search for release notes")
        XCTAssertEqual(searched, .reply("Searching ForkMesh for \"release notes\"."))
        let unknown = await brain.begin("make the app purple")
        guard case .handOff = unknown else { return XCTFail("\(unknown)") }
    }

    func testWords() {
        XCTAssertEqual(GenieWords.isYes("Yes please"), true)
        XCTAssertEqual(GenieWords.isYes("no, don't"), false)
        XCTAssertNil(GenieWords.isYes("maybe later"))
        XCTAssertEqual(GenieWords.spoken("## Title\n- one\n- **two**"), "Title one two")
    }

    func testCompactToolText() {
        let text = GenieToolText.compact([GenieToolbox.specs[0]] + [ForkMeshToolbox.specs[1]])
        XCTAssertTrue(text.contains("- open_screen(screen): Opens"))
        XCTAssertTrue(text.contains("filter=mine|open|done|all?"))
        let call = GenieToolText.call(tool: "open_screen", arguments: "{\"screen\":\"inbox\"}", known: GenieToolbox.specs)
        XCTAssertEqual(call?.arguments, ["screen": "inbox"])
        XCTAssertNil(GenieToolText.call(tool: "none", arguments: "{}", known: GenieToolbox.specs))
    }

    func testSessionGreetsListensActsAndFinishes() async {
        let engine = FakeLocalChatEngine()
        engine.enqueue([toolCall("open_screen", ["screen": "inbox"]), finished])
        engine.enqueueReply(["Opening Inbox."])
        let voice = ScriptedVoice()
        voice.heard = [.words("open my inbox")]
        let session = GenieSession(brain: makeBrain(engine, actions: RecordingActions()), voice: voice) { _ in "" }
        var finishedCount = 0
        session.onFinish = { finishedCount += 1 }
        session.open()
        await session.waitUntilIdle()
        XCTAssertEqual(voice.said, [GenieSession.greeting, "Opening Inbox."])
        XCTAssertEqual(session.lines.map(\.fromUser), [false, true, false])
        XCTAssertEqual(session.phase, .finished)
        XCTAssertEqual(finishedCount, 1)
    }

    func testSessionHandsOffWithTheAgentSettings() async {
        let voice = ScriptedVoice()
        voice.heard = [.words("make the app purple")]
        var launched: [GenieHandoff] = []
        let session = GenieSession(
            brain: makeBrain(nil, actions: RecordingActions()), voice: voice, settings: { "Opus · Auto" }
        ) { handoff in
            launched.append(handoff)
            return "An agent is on it."
        }
        session.open()
        await session.waitUntilIdle()
        XCTAssertEqual(launched.map(\.request), ["make the app purple"])
        XCTAssertEqual(launched.first?.settings, "Opus · Auto")
        XCTAssertEqual(voice.said, [GenieSession.greeting, GenieSession.handOffNotice, "An agent is on it."])
        XCTAssertEqual(session.phase, .idle)
    }

    func testSessionConfirmsByVoice() async {
        let engine = FakeLocalChatEngine()
        engine.enqueue([toolCall("start_agent", ["prompt": "Fix the build"]), finished])
        engine.enqueueReply(["Started."])
        let actions = RecordingActions()
        let voice = ScriptedVoice()
        voice.heard = [.words("get the build fixed"), .words("yes")]
        let session = GenieSession(brain: makeBrain(engine, actions: actions), voice: voice) { _ in "" }
        session.open()
        await session.waitUntilIdle()
        XCTAssertEqual(actions.started.map(\.prompt), ["Fix the build"])
        XCTAssertTrue(voice.said.contains("Start an agent: Fix the build. Shall I go ahead?"))
        XCTAssertEqual(voice.said.last, "Started.")
    }

    func testSessionClarifiesThenGivesUpToAnAgent() async {
        let engine = FakeLocalChatEngine()
        for _ in 0...GenieSession.maxClarifications {
            engine.enqueue([toolCall(GenieToolbox.askUser, ["question": "Which one?"]), finished])
        }
        let voice = ScriptedVoice()
        voice.heard = [.words("do the thing"), .words("that one"), .words("the other")]
        var launched = 0
        let session = GenieSession(brain: makeBrain(engine, actions: RecordingActions()), voice: voice) { _ in
            launched += 1
            return "Sent."
        }
        session.open()
        await session.waitUntilIdle()
        XCTAssertEqual(voice.said.filter { $0 == "Which one?" }.count, GenieSession.maxClarifications)
        XCTAssertEqual(launched, 1)
    }
}
