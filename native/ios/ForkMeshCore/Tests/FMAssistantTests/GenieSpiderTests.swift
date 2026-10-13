import FMApi
import FMBase
import FMLocalAI
import XCTest
@testable import FMAssistant

@MainActor
private final class SpiderActions: GenieActions {
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
private final class SpiderVoice: GenieVoice {
    var heard: [GenieHeard] = []
    var said: [String] = []

    func say(_ text: String) async {
        said.append(text)
    }

    func listen(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        guard !heard.isEmpty else { return .silence }
        return heard.removeFirst()
    }

    func stopListening() {}
    func stopSpeaking() {}
}

@MainActor
private final class Counter: @unchecked Sendable {
    var count = 0
}

@MainActor
final class GenieSpiderTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_790_000_000)

    private var utc: Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        return calendar
    }

    private var finished: FakeLocalChatEngine.Step {
        .event(.finished(LocalGenerationStats(promptTokens: 1, generatedTokens: 1, tokensPerSecond: 1)))
    }

    private func loaded(_ availability: LocalEngineAvailability = .available) async -> FakeLocalChatEngine {
        let engine = FakeLocalChatEngine(availability: availability)
        try? await engine.load(modelDirectory: URL(fileURLWithPath: "/m"), model: LocalModelCatalog.gemma4E2B)
        return engine
    }

    func testVitalsFollowCodeAndMarketing() {
        let idle = GenieVitals(GenieVitalSigns(measuredAt: now), now: now)
        XCTAssertEqual(idle.health, 0)
        XCTAssertEqual(idle.level, .sick)
        XCTAssertEqual(idle.hearts, 0)
        let busy = GenieVitals(GenieVitalSigns(commits: 20, agentRuns: 6, merged: 3, emailsSent: 8, contactsTouched: 10, replies: 4, measuredAt: now), now: now)
        XCTAssertGreaterThan(busy.code, 0.9)
        XCTAssertGreaterThan(busy.marketing, 0.9)
        XCTAssertEqual(busy.level, .thriving)
        XCTAssertEqual(busy.hearts, GenieVitals.maxHearts)
        let lopsided = GenieVitals(GenieVitalSigns(commits: 40, agentRuns: 10, measuredAt: now), now: now)
        XCTAssertGreaterThan(lopsided.code, 0.95)
        XCTAssertEqual(lopsided.marketing, 0)
        XCTAssertLessThan(lopsided.health, 0.6)
        XCTAssertEqual(lopsided.weakest, "marketing")
        XCTAssertTrue(lopsided.summary.contains("Marketing is starving with no outreach this week."), lopsided.summary)
        XCTAssertTrue(lopsided.summary.contains("40 commits and 10 agent runs this week"), lopsided.summary)
        XCTAssertTrue(lopsided.summary.hasSuffix("Reach out to a contact to feed me."), lopsided.summary)
    }

    func testVitalsFadeWhenStale() {
        let signs = GenieVitalSigns(commits: 20, emailsSent: 8, measuredAt: now.addingTimeInterval(-5 * 24 * 3600))
        let fresh = GenieVitals(GenieVitalSigns(commits: 20, emailsSent: 8, measuredAt: now), now: now)
        let stale = GenieVitals(signs, now: now)
        XCTAssertLessThan(stale.health, fresh.health)
        XCTAssertGreaterThan(stale.health, 0)
    }

    func testVitalSignsRoundTripThroughTheStore() {
        let store = InMemoryKeyValueStore()
        let signs = GenieVitalSigns(commits: 3, agentRuns: 2, merged: 1, emailsSent: 4, contactsTouched: 5, marketingVisits: 6, replies: 7, bounced: 8, measuredAt: now)
        signs.save(store)
        XCTAssertEqual(GenieVitalSigns.load(store), signs)
        XCTAssertNil(GenieVitalSigns.load(InMemoryKeyValueStore()))
    }

    func testActivityLogKeepsOrderSkipsRepeatsAndPersists() {
        let store = InMemoryKeyValueStore()
        let log = GenieActivityLog(store: store)
        log.record(GenieActivity.tab(.inbox), at: now)
        log.record(GenieActivity.tab(.inbox), at: now.addingTimeInterval(5))
        log.record(GenieActivity.page(.crm), at: now.addingTimeInterval(10))
        XCTAssertEqual(log.entries.map(\.kind), ["tab.inbox", "page.crm"])
        XCTAssertEqual(log.entries.map(\.routeKind), [.inbox, .crm])
        XCTAssertEqual(log.marketingVisits(since: now), 1)
        XCTAssertEqual(GenieActivityLog(store: store).entries, log.entries)
        for index in 0..<(GenieActivityLog.most + 10) {
            log.record("page.notes.\(index % 2)", at: now.addingTimeInterval(Double(100 + index * 60)))
        }
        XCTAssertEqual(log.entries.count, GenieActivityLog.most)
    }

    func testHabitNextFollowsWhatUsuallyComesAfter() {
        var log: [GenieActivity] = []
        var at = now
        for _ in 0..<4 {
            log.append(GenieActivity(kind: GenieActivity.tab(.inbox), at: at))
            log.append(GenieActivity(kind: GenieActivity.tab(.chat), at: at.addingTimeInterval(60)))
            at = at.addingTimeInterval(3600)
        }
        log.append(GenieActivity(kind: GenieActivity.tab(.inbox), at: at))
        XCTAssertEqual(GenieHints.habitNext(log, now: at), .chat)
        let hints = GenieHints.suggest(log: log, vitals: nil, context: GenieHintContext(tab: .inbox), now: at, calendar: utc)
        XCTAssertEqual(hints.first?.text, "You usually open Chat next. Go there?")
        XCTAssertEqual(hints.first?.spot?.arguments, ["screen": "chat"])
        XCTAssertNil(GenieHints.habitNext(Array(log.prefix(3)), now: at))
    }

    func testHabitHourNeedsSeveralDaysAndSkipsWhatIsDone() {
        let day: TimeInterval = 24 * 3600
        let log = (1...3).map { GenieActivity(kind: GenieActivity.page(.calendar), at: now.addingTimeInterval(-Double($0) * day)) }
        XCTAssertEqual(GenieHints.habitHour(log, now: now, calendar: utc), .calendar)
        XCTAssertNil(GenieHints.habitHour(Array(log.prefix(2)), now: now, calendar: utc))
        XCTAssertNil(GenieHints.habitHour(log + [GenieActivity(kind: GenieActivity.page(.calendar), at: now.addingTimeInterval(-60))], now: now, calendar: utc))
    }

    func testHintsRankPingsFinishedAgentsAndHunger() {
        let hungry = GenieVitals(GenieVitalSigns(commits: 30, agentRuns: 6, measuredAt: now), now: now)
        let hints = GenieHints.suggest(
            log: [], vitals: hungry, context: GenieHintContext(unread: 3, running: 1, finished: 1, tab: .agents), now: now, calendar: utc)
        XCTAssertEqual(hints.map(\.id), ["unread", "finished", "feed.marketing"])
        XCTAssertEqual(hints[0].text, "You have 3 unread pings. Shall I read the newest?")
        XCTAssertEqual(hints[0].spot?.tool, "read_latest")
        XCTAssertEqual(hints[2].spot?.arguments, ["screen": "crm"])
        XCTAssertEqual(GenieHints.greeting(hints[0]), "You have 3 unread pings. Shall I read the newest? Or ask me anything.")
        XCTAssertEqual(GenieHints.greeting(nil), GenieSession.greeting)
        let calm = GenieHints.suggest(log: [], vitals: nil, context: GenieHintContext(running: 2), now: now, calendar: utc)
        XCTAssertEqual(calm.map(\.text), ["2 agents are at work. Check on them?"])
        XCTAssertTrue(GenieHints.suggest(log: [], vitals: nil, context: GenieHintContext(), now: now).isEmpty)
    }

    func testTapLinesLeadWithNewsAndAreFiveToTenWords() {
        let hungry = GenieVitals(GenieVitalSigns(commits: 30, agentRuns: 6, measuredAt: now), now: now)
        let lines = GenieHints.lines(
            log: [], vitals: hungry, context: GenieHintContext(unread: 3, running: 2, finished: 1, tab: .agents), now: now, calendar: utc)
        XCTAssertEqual(Array(lines.prefix(4)), [
            "You have 3 unread pings in your inbox.",
            "An agent just finished; review it in Agents.",
            "Marketing is hungry; reach out to a CRM contact.",
            "2 agents are at work right now.",
        ])
        XCTAssertEqual(Array(lines.suffix(GenieHints.general.count)), GenieHints.general)
        XCTAssertEqual(GenieHints.lines(log: [], vitals: nil, context: GenieHintContext(), now: now), GenieHints.general)
        let day: TimeInterval = 24 * 3600
        let log = (1...3).map { GenieActivity(kind: GenieActivity.page(.calendar), at: now.addingTimeInterval(-Double($0) * day)) }
        let busy = GenieHintContext(unread: 1, running: 1, finished: 4)
        let quiet = GenieVitals(GenieVitalSigns(measuredAt: now), now: now)
        for line in GenieHints.lines(log: log, vitals: quiet, context: busy, now: now, calendar: utc) + lines {
            XCTAssertTrue((5...10).contains(GenieHints.words(line)), line)
        }
    }

    func testEveryHintScreenOpensAScreen() async {
        for (kind, _) in GenieHints.screens {
            let actions = SpiderActions()
            let toolbox = GenieToolbox(data: nil, actions: actions)
            guard let spot = GenieHints.screenSpot(kind) else { return XCTFail("no spot for \(kind)") }
            let result = await toolbox.invoke(spot.call)
            XCTAssertTrue(result.text.hasPrefix("Opening"), "\(kind): \(result.text)")
            XCTAssertEqual(actions.commands.count, 1, "\(kind)")
        }
    }

    func testVitalsQuestionIsSpottedAndAnswered() async {
        XCTAssertEqual(GenieSpotter.spot("Genie, how are you feeling?")?.tool, GenieToolbox.vitalsTool)
        XCTAssertEqual(GenieSpotter.spot("are you hungry")?.tool, GenieToolbox.vitalsTool)
        XCTAssertEqual(GenieAliases.resolve(LocalToolCall(id: "1", name: "how_are_you", arguments: [:]), specs: GenieToolbox.specs).name, GenieToolbox.vitalsTool)
        let toolbox = GenieToolbox(data: nil, actions: SpiderActions())
        let without = await toolbox.invoke(LocalToolCall(id: "1", name: GenieToolbox.vitalsTool, arguments: [:]))
        XCTAssertEqual(without.text, "I can't read my health on this device yet.")
        let stamp = now
        toolbox.vitals = { GenieVitals(GenieVitalSigns(commits: 2, emailsSent: 1, measuredAt: stamp), now: stamp) }
        let brain = GenieBrain(toolbox: toolbox, memory: GenieMemory(store: InMemoryKeyValueStore()), model: nil)
        let outcome = await brain.begin("genie how are you feeling")
        guard case .reply(let text) = outcome else { return XCTFail("\(outcome)") }
        XCTAssertTrue(text.hasPrefix("I'm feeling"), text)
        XCTAssertTrue(GenieCapabilities.describe(topic: "health", toolNames: toolbox.toolNames).contains("code velocity"))
    }

    func testSessionGreetsWithAHintAndYesRunsIt() async {
        let voice = SpiderVoice()
        voice.heard = [.words("yes please")]
        let actions = SpiderActions()
        let brain = GenieBrain(toolbox: GenieToolbox(data: nil, actions: actions), memory: GenieMemory(store: InMemoryKeyValueStore()), model: nil)
        let session = GenieSession(brain: brain, voice: voice) { _ in "" }
        session.greeter = { GenieHint(id: "x", text: "Chat is busy. Open it?", spot: GenieHints.screenSpot(.chat), weight: 1) }
        session.open()
        await session.waitUntilIdle()
        XCTAssertEqual(voice.said, ["Chat is busy. Open it? Or ask me anything.", "Opening Chat."])
        XCTAssertEqual(actions.commands, [.selectTab(.chat)])
        XCTAssertNil(session.offer)
    }

    func testSessionHintNoDeclines() async {
        let voice = SpiderVoice()
        voice.heard = [.words("no thanks")]
        let actions = SpiderActions()
        let brain = GenieBrain(toolbox: GenieToolbox(data: nil, actions: actions), memory: GenieMemory(store: InMemoryKeyValueStore()), model: nil)
        let session = GenieSession(brain: brain, voice: voice) { _ in "" }
        session.greeter = { GenieHint(id: "x", text: "Open chat?", spot: GenieHints.screenSpot(.chat), weight: 1) }
        session.open()
        await session.waitUntilIdle()
        XCTAssertEqual(voice.said.last, "Okay.")
        XCTAssertTrue(actions.commands.isEmpty)
    }

    func testDuoUsesAppleForToolsAndGemmaForLongReplies() async throws {
        let apple = await loaded()
        let gemma = await loaded()
        let duo = GenieDuoEngine(quick: apple, deep: gemma)
        apple.enqueue([.event(.toolCall(LocalToolCall(id: "1", name: "open_screen", arguments: ["screen": "inbox"]))), finished])
        let first = try await duo.complete(messages: [.user("open inbox")])
        XCTAssertEqual(first.toolCalls.map(\.name), ["open_screen"])
        XCTAssertEqual(duo.lastVoice, .quick)
        XCTAssertTrue(gemma.requests.isEmpty)

        let long = String(repeating: "commit line ", count: 200)
        let history: [LocalChatMessage] = [.user("summarise my commits"), .tool(long, callId: "1", name: "read_latest")]
        apple.enqueueReply(["Several commits."])
        gemma.enqueueReply(["You landed the runner cache and the iOS ticker fix."])
        let second = try await duo.complete(messages: history)
        XCTAssertEqual(second.text, "You landed the runner cache and the iOS ticker fix.")
        XCTAssertEqual(duo.lastVoice, .deep)
        XCTAssertEqual(gemma.requests.count, 1)
    }

    func testDuoFallsBackBothWays() async throws {
        let apple = await loaded()
        let gemma = await loaded()
        let warmed = Counter()
        let duo = GenieDuoEngine(quick: apple, deep: gemma) { warmed.count += 1 }
        apple.enqueue([.fail(.generationFailed("context window exceeded"))])
        gemma.enqueueReply(["Gemma answers."])
        let rescued = try await duo.complete(messages: [.user("hello")])
        XCTAssertEqual(rescued.text, "Gemma answers.")
        XCTAssertEqual(warmed.count, 1)

        apple.enqueueReply([""])
        gemma.enqueue([.fail(.generationFailed("out of memory"))])
        let kept = try await duo.complete(messages: [.user("hello")])
        XCTAssertEqual(kept.text, "")
        XCTAssertEqual(duo.lastVoice, .quick)

        let appleOnly = GenieDuoEngine(quick: await loaded(), deep: FakeLocalChatEngine(availability: .unavailable(reason: "off")))
        XCTAssertTrue(appleOnly.availability.isAvailable)
        let neither = GenieDuoEngine(
            quick: FakeLocalChatEngine(availability: .unavailable(reason: "no Apple Intelligence")),
            deep: FakeLocalChatEngine(availability: .unavailable(reason: "off")))
        XCTAssertEqual(neither.availability.reason, "no Apple Intelligence")
    }
}
