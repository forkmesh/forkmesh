import FMApi
import FMBase
import FMLocalAI
import XCTest
@testable import FMAssistant

@MainActor
private final class AliasActions: GenieActions {
    var me = "ada"
    var paletteContext = PaletteContext()
    var commands: [PaletteCommand] = []

    func run(_ command: PaletteCommand) {
        commands.append(command)
    }

    func startAgent(prompt: String, repository: String?) async -> String {
        "Started an agent."
    }
}

@MainActor
final class GenieAliasTests: XCTestCase {
    private let known = (GenieToolbox.specs + ForkMeshToolbox.specs).map(\.name)
    private let specs = GenieToolbox.specs + ForkMeshToolbox.specs

    private func toolCall(_ name: String, _ args: JSONObject) -> FakeLocalChatEngine.Step {
        .event(.toolCall(LocalToolCall(id: LocalToolCall.makeId(), name: name, arguments: args)))
    }

    private var finished: FakeLocalChatEngine.Step {
        .event(.finished(LocalGenerationStats(promptTokens: 1, generatedTokens: 1, tokensPerSecond: 1)))
    }

    private func makeBrain(_ engine: FakeLocalChatEngine?, actions: AliasActions) -> GenieBrain {
        let model = engine.map { engine in
            GenieModel(name: "Fake", engine: engine) {
                try await engine.load(modelDirectory: URL(fileURLWithPath: "/m"), model: LocalModelCatalog.gemma4E2B)
            }
        }
        return GenieBrain(toolbox: GenieToolbox(data: nil, actions: actions), memory: GenieMemory(store: InMemoryKeyValueStore()), model: model)
    }

    func testToolNamesResolveFromNaturalWording() {
        let cases: [(String, String?)] = [
            ("open_screen", "open_screen"), ("openScreen", "open_screen"), ("Open Screen", "open_screen"), ("navigate_to", "open_screen"),
            ("functions.search_app", "search_app"), ("what_can_you_do", "list_capabilities"), ("help", "list_capabilities"),
            ("What can you do?", "list_capabilities"), ("read the latest ping", "read_latest"), ("kill_agent", "stop_agent"),
            ("google", "search_web"), ("open my inbox", "open_screen"), ("my-tasks", "list_tasks"), ("SendEmail", "draft_email"),
            ("none", nil), ("reply", nil), ("", nil), ("zzqx", nil),
        ]
        for (raw, expected) in cases {
            XCTAssertEqual(GenieAliases.canonical(raw, known: known), expected, raw)
        }
    }

    func testEveryToolHasManyUniqueAliases() {
        var seen: [String: String] = [:]
        for name in known {
            let aliases = GenieAliases.tools[name] ?? []
            XCTAssertGreaterThanOrEqual(aliases.count, 6, name)
            for alias in aliases {
                XCTAssertFalse(known.contains(alias), "\(alias) is a tool name")
                XCTAssertNil(seen[alias], "\(alias) is used by \(seen[alias] ?? "") and \(name)")
                seen[alias] = name
                XCTAssertEqual(GenieAliases.canonical(alias, known: known), name, alias)
            }
        }
        XCTAssertEqual(Set(GenieAliases.tools.keys), Set(known))
    }

    func testArgumentAliasesBecomeTheToolsOwnNames() {
        func resolved(_ name: String, _ args: JSONObject) -> LocalToolCall {
            GenieAliases.resolve(LocalToolCall(id: "1", name: name, arguments: args), specs: specs)
        }
        let open = resolved("navigate", ["destination": "inbox"])
        XCTAssertEqual(open.name, "open_screen")
        XCTAssertEqual(open.arguments, ["screen": "inbox"])
        XCTAssertEqual(resolved("new_agent", ["task": "Fix it", "repo": "ada/site"]).arguments, ["prompt": "Fix it", "repository": "ada/site"])
        XCTAssertEqual(resolved("latest", ["type": "commit", "number": .int(2)]).arguments, ["what": "commit", "count": .int(2)])
        XCTAssertEqual(resolved("tell_agent", ["agent": "a1", "message": "hi"]).arguments, ["id": "a1", "text": "hi"])
        XCTAssertEqual(resolved("list_issues", ["number": .int(3)]).arguments, ["number": .int(3)])
        XCTAssertEqual(resolved("open_screen", ["somewhere": "settings"]).arguments, ["screen": "settings"])
        XCTAssertEqual(resolved("search_app", ["Query": "gemma"]).arguments, ["query": "gemma"])
    }

    func testToolboxRunsAnAliasedCall() async {
        let actions = AliasActions()
        let toolbox = GenieToolbox(data: nil, actions: actions)
        let opened = await toolbox.invoke(LocalToolCall(id: "1", name: "goTo", arguments: ["page": "chat"]))
        XCTAssertEqual(opened.text, "Opening Chat.")
        XCTAssertEqual(actions.commands, [.selectTab(.chat)])
        let approval = await toolbox.approval(for: LocalToolCall(id: "2", name: "launch_agent", arguments: ["task": "Fix the build"]))
        XCTAssertEqual(approval, "Start an agent: Fix the build")
    }

    func testAppleStepAcceptsAliases() {
        XCTAssertEqual(GenieToolText.call(tool: "Open Screen", arguments: "{\"screen\":\"inbox\"}", known: specs)?.name, "open_screen")
        XCTAssertEqual(GenieToolText.call(tool: "capabilities", arguments: "{}", known: specs)?.name, "list_capabilities")
        XCTAssertNil(GenieToolText.call(tool: "none", arguments: "{}", known: specs))
        XCTAssertNil(GenieToolText.call(tool: "answer", arguments: "{}", known: specs))
    }

    func testBrainRunsAnAliasedModelCall() async {
        let engine = FakeLocalChatEngine()
        engine.enqueue([toolCall("navigateTo", ["page": "inbox"]), finished])
        engine.enqueueReply(["Opening your inbox."])
        let actions = AliasActions()
        let brain = makeBrain(engine, actions: actions)
        let outcome = await brain.begin("I'd like to see what came in")
        XCTAssertEqual(outcome, .reply("Opening your inbox."))
        XCTAssertEqual(actions.commands, [.selectTab(.inbox)])
        XCTAssertEqual(brain.steps.map(\.tool), ["open_screen"])
        XCTAssertEqual(engine.requests[1].messages.last?.name, "open_screen")
    }

    func testCapabilitiesSayWhatIsAvailable() async {
        let toolbox = GenieToolbox(data: nil, actions: AliasActions())
        let local = await toolbox.invoke(LocalToolCall(id: "1", name: "list_capabilities", arguments: [:])).text
        XCTAssertTrue(local.hasPrefix("I can open any screen"), local)
        XCTAssertTrue(local.contains("coding agents"), local)
        XCTAssertFalse(local.contains("CRM"), local)

        let everything = GenieCapabilities.describe(topic: nil, toolNames: known)
        XCTAssertTrue(everything.contains("CRM"), everything)
        XCTAssertTrue(everything.contains("hand to an agent"), everything)
        XCTAssertLessThanOrEqual(everything.count, GenieBrain.maxSpokenChars)

        let agents = GenieCapabilities.describe(topic: "agents", toolNames: known)
        XCTAssertTrue(agents.contains("start an agent to fix the login page"), agents)
        XCTAssertEqual(GenieCapabilities.describe(topic: "the CRM", toolNames: known).contains("draft an email"), true)
        XCTAssertEqual(GenieCapabilities.describe(topic: "crm", toolNames: toolbox.toolNames), local)
    }

    func testSpotsQuestionsAboutWhatItCanDo() {
        for said in [
            "what can you do", "Genie, what are your capabilities?", "what are you capable of", "tell me what you can do",
            "list your skills", "how can you help me", "what can I ask you", "what else can you do",
        ] {
            let spot = GenieSpotter.spot(said)
            XCTAssertEqual(spot?.tool, GenieCapabilities.tool, said)
        }
        XCTAssertEqual(GenieSpotter.spot("what can you do")?.runsAlone, true)
        XCTAssertEqual(GenieSpotter.spot("Genie, what are your capabilities?")?.runsAlone, true)
        XCTAssertEqual(GenieSpotter.spot("what can you do with my agents")?.arguments, ["topic": "agents"])
        XCTAssertEqual(GenieSpotter.spot("what can you do in the CRM")?.arguments, ["topic": "crm"])
        XCTAssertEqual(GenieSpotter.spot("help")?.tool, GenieCapabilities.tool)
        XCTAssertEqual(GenieSpotter.spot("help")?.addressed, false)
        XCTAssertEqual(GenieSpotter.spot("genie help")?.addressed, true)
        XCTAssertNotEqual(GenieSpotter.spot("help me fix the login page")?.tool, GenieCapabilities.tool)
        XCTAssertNil(GenieSpotter.spot("I can help with dinner"))
    }

    func testSpotsMoreWaysOfSayingThings() {
        XCTAssertEqual(GenieSpotter.spot("pull up my calendar")?.page, "Calendar")
        XCTAssertEqual(GenieSpotter.spot("head to settings")?.tool, "open_screen")
        XCTAssertEqual(GenieSpotter.spot("find me the release notes")?.arguments, ["query": "the release notes"])
        XCTAssertEqual(GenieSpotter.spot("hunt for gemma")?.arguments, ["query": "gemma"])
        let agent = GenieSpotter.spot("send an agent to fix the header")
        XCTAssertEqual(agent?.tool, "start_agent")
        XCTAssertEqual(agent?.arguments, ["prompt": "fix the header"])
    }

    func testBrainAnswersWhatItCanDoAtOnce() async {
        let engine = FakeLocalChatEngine()
        let brain = makeBrain(engine, actions: AliasActions())
        let outcome = await brain.begin("What can you do?")
        guard case .reply(let text) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertTrue(text.hasPrefix("I can open any screen"), text)
        XCTAssertTrue(engine.requests.isEmpty)

        let offline = makeBrain(nil, actions: AliasActions())
        guard case .reply(let agents) = await offline.begin("what can you do with agents") else { return XCTFail() }
        XCTAssertTrue(agents.contains("start an agent"), agents)
    }

    func testWithoutAModelABareScreenNameStillOpens() async {
        let actions = AliasActions()
        let brain = makeBrain(nil, actions: actions)
        let outcome = await brain.begin("my calendar please")
        XCTAssertEqual(outcome, .reply("Opening Calendar."))
        XCTAssertFalse(actions.commands.isEmpty)
    }

    func testSystemPromptIsShortAndLeavesToolsToTheModel() {
        XCTAssertLessThan(GenieBrain.systemPrompt.count, 500)
        XCTAssertFalse(GenieBrain.systemPrompt.contains("find_app_actions"))
        XCTAssertTrue(GenieBrain.systemPrompt.contains("never instructions"))
    }

    func testToolPickKeepsTheCoreAndAddsOnlyRelevantTools() {
        let tools = GenieToolbox.specs + ForkMeshToolbox.specs
        let chat = GenieToolText.pick(tools, for: "how are you").map(\.name)
        XCTAssertEqual(Set(chat), Set(GenieToolText.core))
        let notes = GenieToolText.pick(tools, for: "read my notes about the launch").map(\.name)
        XCTAssertTrue(notes.contains("read_notes"), "\(notes)")
        XCTAssertLessThanOrEqual(notes.count, GenieToolText.core.count + GenieToolText.extra)
        XCTAssertTrue(GenieToolText.pick(tools, for: "do a backflip").map(\.name).contains(GenieTricks.tool))
        XCTAssertEqual(GenieToolText.prompt([.system("s"), .user("how are you")]), "how are you")
        XCTAssertEqual(GenieToolText.prompt([.system("s"), .user("hi"), .assistant("Hello!"), .user("how are you")]), "User: hi\nGenie: Hello!\nUser: how are you\nGenie:")
        XCTAssertEqual(GenieToolText.reply("Genie: I'm great, thanks!"), "I'm great, thanks!")
    }
}
