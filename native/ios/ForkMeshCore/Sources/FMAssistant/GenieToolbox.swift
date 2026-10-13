import FMApi
import FMBase
import FMLocalAI
import Foundation

@MainActor
public protocol GenieActions: AnyObject {
    var me: String { get }
    var paletteContext: PaletteContext { get }
    func run(_ command: PaletteCommand)
    func startAgent(prompt: String, repository: String?) async -> String
    var canTurnPages: Bool { get }
    func turnPage(_ turn: GeniePageTurn) -> String?
}

public extension GenieActions {
    var canTurnPages: Bool { false }

    func turnPage(_ turn: GeniePageTurn) -> String? { nil }
}

@MainActor
public final class GenieToolbox: OnDeviceChatToolbox {
    public nonisolated static let askUser = "ask_user"
    public nonisolated static let handOff = "hand_off_to_agent"
    public nonisolated static let say = "say"
    public nonisolated static let vitalsTool = "check_vitals"
    public nonisolated static let fillerWords: Set<String> = [
        "the", "my", "a", "an", "screen", "page", "tab", "view", "list", "open", "show", "me", "go", "to", "please", "up",
    ]

    public let data: OnDeviceChatToolbox?
    public var actions: GenieActions?
    public var vitals: (@MainActor () async -> GenieVitals?)?
    public var tricks: (@MainActor (GenieTrick?) async -> GenieImprovisation?)?
    public private(set) var navigated = false
    public private(set) var commands: [PaletteCommand] = []

    public init(data: OnDeviceChatToolbox?, actions: GenieActions?) {
        self.data = data
        self.actions = actions
    }

    public var tools: [LocalToolSpec] { GenieToolbox.specs + (data?.tools ?? []) }

    public var toolNames: [String] { tools.map(\.name) }

    public func briefing() async -> String {
        await data?.briefing() ?? ""
    }

    public func beginRequest() {
        navigated = false
    }

    public func resolve(_ call: LocalToolCall) -> LocalToolCall {
        GenieAliases.resolve(call, specs: tools)
    }

    public var canTurnPages: Bool { actions?.canTurnPages ?? false }

    public func spot(_ text: String) -> GenieSpot? {
        GenieSpotter.spot(text, me: actions?.me ?? "", paging: canTurnPages)
    }

    public func capabilities(_ topic: String?) -> String {
        GenieCapabilities.describe(topic: topic, toolNames: toolNames)
    }

    public func approval(for raw: LocalToolCall) async -> String? {
        let call = resolve(raw)
        switch call.name {
        case "start_agent":
            let prompt = AssistantText.argument(call.arguments, "prompt") ?? ""
            let repo = AssistantText.argument(call.arguments, "repository").map { " on \($0)" } ?? ""
            return "Start an agent\(repo): \(OnDeviceChatTools.clip(prompt, limit: 200))"
        case let name where GenieToolbox.appToolNames.contains(name):
            return nil
        default:
            return await data?.approval(for: call)
        }
    }

    public func invoke(_ raw: LocalToolCall) async -> OnDeviceToolResult {
        let call = resolve(raw)
        let args = call.arguments
        switch call.name {
        case "open_screen":
            return OnDeviceToolResult(openScreen(AssistantText.argument(args, "screen") ?? ""))
        case "find_app_actions":
            return OnDeviceToolResult(findActions(AssistantText.argument(args, "query") ?? ""))
        case "run_app_action":
            return OnDeviceToolResult(runAction(AssistantText.argument(args, "id") ?? ""))
        case "search_app":
            return OnDeviceToolResult(searchApp(AssistantText.argument(args, "query") ?? ""))
        case "open_search":
            return OnDeviceToolResult(searchApp(""))
        case "start_agent":
            return OnDeviceToolResult(await startAgent(args))
        case GenieCapabilities.tool:
            return OnDeviceToolResult(capabilities(AssistantText.argument(args, "topic")))
        case GenieToolbox.vitalsTool:
            guard let vitals, let now = await vitals() else { return OnDeviceToolResult("I can't read my health on this device yet.") }
            return OnDeviceToolResult(now.summary)
        case GenieTricks.tool:
            return OnDeviceToolResult(await performTrick(AssistantText.argument(args, "trick") ?? ""))
        case GeniePages.tool:
            return OnDeviceToolResult(turnPage(AssistantText.argument(args, "direction"), asked: raw.name))
        case GenieToolbox.askUser, GenieToolbox.handOff, GenieToolbox.say:
            return OnDeviceToolResult("\(call.name) is handled by the Genie itself.")
        default:
            guard let data, data.tools.contains(where: { $0.name == call.name }) else {
                return OnDeviceToolResult("There is no tool named \(call.name). Use one of: " + toolNames.joined(separator: ", ") + ".")
            }
            return await data.invoke(withScreenRepository(call))
        }
    }

    public func openScreen(_ name: String) -> String {
        guard !name.isEmpty else { return "open_screen needs the name of a screen." }
        guard let route = GenieToolbox.route(named: name, me: actions?.me ?? "") else {
            let known = GenieToolbox.destinations.map(\.title)
            return "No screen matches \"\(name)\". Screens include: " + known.joined(separator: ", ") + "."
        }
        if let tab = AppTab.barOrder.first(where: { $0.root == route }) {
            perform(.selectTab(tab))
        } else {
            perform(.open(route))
        }
        return "Opening \(route.title)."
    }

    public func findActions(_ query: String) -> String {
        let context = actions?.paletteContext ?? PaletteContext()
        let entries = GenieToolbox.entries(query: query, context: context)
        guard !entries.isEmpty else { return "No app action matches \"\(query)\"." }
        return AssistantText.list("App actions matching \"\(query)\":", entries.map { entry in
            "[\(entry.id)] \(entry.title)\(entry.subtitle.isEmpty ? "" : " · \(entry.subtitle)")"
        }, limit: 15)
    }

    public func runAction(_ id: String) -> String {
        let context = actions?.paletteContext ?? PaletteContext()
        let all = GenieToolbox.entries(query: "", context: context) + CommandPalette.destinationEntries()
        guard let entry = all.first(where: { $0.id == id }) ?? all.first(where: { $0.title.lowercased() == id.lowercased() }) else {
            return "No app action has the id \(id). Call find_app_actions first."
        }
        perform(entry.command)
        return "Running \(entry.title)."
    }

    public func searchApp(_ query: String) -> String {
        let words = query.trimmingCharacters(in: .whitespacesAndNewlines)
        perform(.searchEverywhere(words))
        return words.isEmpty ? "Opening search." : "Searching ForkMesh for \"\(words)\"."
    }

    public func performTrick(_ asked: String) async -> String {
        guard let tricks else { return "I can only do tricks while I'm on screen." }
        let wanted = GenieTricks.find(asked)
        guard let done = await tricks(wanted) else { return "I can't do tricks right now." }
        let words = asked.trimmingCharacters(in: .whitespacesAndNewlines)
        if wanted == nil, !GenieTricks.isSurprise(words) {
            return "I don't know \(words) yet, so here's my \(done.trick.title.lowercased()). \(done.line)"
        }
        return done.line
    }

    public func turnPage(_ direction: String?, asked name: String = GeniePages.tool) -> String {
        let wanted: GeniePageTurn?
        if let direction {
            wanted = GeniePageTurn.parse(direction)
        } else {
            wanted = GeniePageTurn.parse(name) ?? .next
        }
        guard let turn = wanted else {
            return "turn_page needs a direction: " + GeniePageTurn.allCases.map(\.rawValue).joined(separator: ", ") + "."
        }
        guard canTurnPages, let done = actions?.turnPage(turn) else { return GeniePages.nothingOpen }
        navigated = true
        return done
    }

    public nonisolated static let repositoryTools: Set<String> = ["read_latest", "list_issues", "list_pull_requests"]

    public func withScreenRepository(_ call: LocalToolCall) -> LocalToolCall {
        guard GenieToolbox.repositoryTools.contains(call.name), AssistantText.argument(call.arguments, "repository") == nil,
              let repo = actions?.paletteContext.target.repo else { return call }
        if call.name == "read_latest", !["commit", "issue", "pull"].contains(ForkMeshToolbox.latestKind(AssistantText.argument(call.arguments, "what") ?? "") ?? "") {
            return call
        }
        var next = call
        next.arguments["repository"] = .string("\(repo.owner)/\(repo.name)")
        return next
    }

    private func startAgent(_ args: JSONObject) async -> String {
        guard let prompt = AssistantText.argument(args, "prompt") else { return "start_agent needs a prompt." }
        guard let actions else { return "Agents cannot be started from here." }
        return await actions.startAgent(prompt: prompt, repository: AssistantText.argument(args, "repository"))
    }

    private func perform(_ command: PaletteCommand) {
        navigated = true
        commands.append(command)
        actions?.run(command)
    }

    public static var destinations: [Route] {
        var seen = Set<Route>()
        let installed = PackageGate.installed
        return (AppTab.barOrder.map(\.root) + DestinationsSearchSource.defaultRoutes).filter {
            PackageGate.shows($0, installed: installed) && seen.insert($0).inserted
        }
    }

    public static func route(named raw: String, me: String) -> Route? {
        let name = raw.trimmingCharacters(in: .whitespacesAndNewlines.union(CharacterSet(charactersIn: ".?!")))
        guard !name.isEmpty else { return nil }
        if let url = URL(string: name), url.scheme != nil, let link = RouteParser.parse(url, me: me), let route = link.destination {
            return route
        }
        if name.hasPrefix("/") {
            let segments = name.split(separator: "/").map(String.init)
            if let route = RouteParser.routes(segments: segments, me: me)?.last { return route }
        }
        if let entry = CommandPalette.directEntries(name).first, case let .open(route) = entry.command {
            return route
        }
        let candidates = destinations
        func best(_ query: String) -> Route? {
            SearchMatch.rank(candidates, query) { [$0.title, RouteCatalog.title($0.kind)] }.first
        }
        if let hit = best(name) { return hit }
        let words = name.lowercased().split(whereSeparator: { !$0.isLetter && !$0.isNumber }).map(String.init)
        let kept = words.filter { !fillerWords.contains($0) }
        if !kept.isEmpty, let hit = best(kept.joined(separator: " ")) { return hit }
        for word in kept.sorted(by: { $0.count > $1.count }) where word.count >= 3 {
            if let hit = best(word) { return hit }
            if word.hasSuffix("s"), let hit = best(String(word.dropLast())) { return hit }
        }
        return nil
    }

    public static func entries(query: String, context: PaletteContext) -> [PaletteEntry] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let sections = CommandPalette.sections(query: trimmed, context: context)
        var seen = Set<String>()
        var out = sections.flatMap(\.entries)
        if trimmed.isEmpty { out += CommandPalette.destinationEntries() }
        let installed = PackageGate.installed
        return out.filter { PackageGate.shows($0, installed: installed) && seen.insert($0.id).inserted }
    }

    public nonisolated static let appToolNames: Set<String> = Set(specs.map(\.name))

    public nonisolated static let specs: [LocalToolSpec] = [
        ForkMeshToolbox.spec(
            "open_screen",
            "Opens a screen of the ForkMesh app: a name such as Inbox, Settings, Calendar or Running agents, a repository as owner/name, an issue as owner/name#12, a person as @name, or a forkmesh.com link.",
            ["screen": ForkMeshToolbox.text("The screen, repository, issue, person or link")], required: ["screen"]),
        ForkMeshToolbox.spec(
            "find_app_actions",
            "Lists the app's commands and screens matching some words, each with an id for run_app_action.",
            ["query": ForkMeshToolbox.text("Words describing what the user wants to do")]),
        ForkMeshToolbox.spec(
            "run_app_action",
            "Runs one app command by the id find_app_actions gave.",
            ["id": ForkMeshToolbox.text("The id in brackets")], required: ["id"]),
        ForkMeshToolbox.spec(
            "search_app",
            "Opens ForkMesh search for these words across repositories, code, issues, people, notes and chat.",
            ["query": ForkMeshToolbox.text("What to search for")], required: ["query"]),
        ForkMeshToolbox.spec(
            "open_search",
            "Opens the empty search prompt so the user can type or say what to find.",
            [:]),
        ForkMeshToolbox.spec(
            "start_agent",
            "Starts a coding agent with the user's current agent settings, for work the user wants an agent to do.",
            [
                "prompt": ForkMeshToolbox.text("The task for the agent, in the user's words"),
                "repository": ForkMeshToolbox.text("owner/name, when the user named one"),
            ], required: ["prompt"]),
        ForkMeshToolbox.spec(
            GenieCapabilities.tool,
            "Says what the Genie can do, with examples. Use it when asked for help or what it can do.",
            ["topic": ForkMeshToolbox.text("An area such as agents, search, tasks, CRM or screens")]),
        ForkMeshToolbox.spec(
            vitalsTool,
            "Says how the Genie feels: its health, fed by this week's code velocity (commits, agent runs, merges) and marketing (CRM outreach).",
            [:]),
        ForkMeshToolbox.spec(
            GenieTricks.tool,
            "Makes the Genie spider do a trick on screen, such as dance, backflip, moonwalk, air guitar, levitate, juggle, play dead or web swing. Use surprise to let it choose.",
            ["trick": ForkMeshToolbox.text("The trick, or surprise")]),
        ForkMeshToolbox.spec(
            GeniePages.tool,
            "Turns the page of the diff on screen while the user reviews changes: the next or previous page, or the next or previous file.",
            ["direction": ForkMeshToolbox.choice("Which way to turn", GeniePageTurn.allCases.map(\.rawValue))]),
        ForkMeshToolbox.spec(
            say,
            "Says these words aloud, word for word, when the user asks you to say or repeat something.",
            ["words": ForkMeshToolbox.text("The words to say")], required: ["words"]),
        ForkMeshToolbox.spec(
            askUser,
            "Asks the user one short spoken question when the request is unclear. Use it instead of guessing.",
            ["question": ForkMeshToolbox.text("The question")], required: ["question"]),
        ForkMeshToolbox.spec(
            handOff,
            "When no tool can do what the user asked: hands the request to a coding agent that does it and teaches the Genie, so it works next time.",
            ["reason": ForkMeshToolbox.text("What is missing")], required: ["reason"]),
    ]
}
