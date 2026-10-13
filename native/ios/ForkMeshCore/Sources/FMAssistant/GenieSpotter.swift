import FMApi
import FMBase
import FMLocalAI
import Foundation

public struct GenieSpot: Equatable, Sendable {
    public var tool: String
    public var arguments: [String: String]
    public var title: String
    public var page: String
    public var symbol: String
    public var route: Route?
    public var phrase: Range<Int>
    public var addressed: Bool

    public init(
        tool: String, arguments: [String: String] = [:], title: String, page: String, symbol: String, route: Route? = nil,
        phrase: Range<Int>, addressed: Bool = false
    ) {
        self.tool = tool
        self.arguments = arguments
        self.title = title
        self.page = page
        self.symbol = symbol
        self.route = route
        self.phrase = phrase
        self.addressed = addressed
    }

    public var needsApproval: Bool { tool == "start_agent" }

    public var runsAlone: Bool { addressed && !needsApproval }

    public var call: LocalToolCall {
        var args = JSONObject()
        for (key, value) in arguments {
            if let number = Int(value), key == "count" {
                args[key] = .int(Int64(number))
            } else {
                args[key] = .string(value)
            }
        }
        return LocalToolCall(id: LocalToolCall.makeId(), name: tool, arguments: args)
    }
}

public struct GenieSegment: Equatable, Sendable {
    public var text: String
    public var actionable: Bool

    public init(text: String, actionable: Bool) {
        self.text = text
        self.actionable = actionable
    }
}

public enum GenieSpotter {
    struct Token {
        let word: String
        let start: Int
        let end: Int
    }

    public static let wakeWords: Set<String> = ["genie", "jeannie", "jeanie", "genii", "djinn", "jinni", "jinny"]
    public static let openers: [[String]] = [
        ["can", "you"], ["could", "you"], ["would", "you"], ["will", "you"], ["i", "want", "you", "to"], ["i'd", "like", "you", "to"],
        ["i", "need", "you", "to"], ["go", "ahead", "and"], ["please"], ["hey"], ["ok"], ["okay"], ["now"], ["just"], ["and"], ["so"],
    ]
    public static let commandVerbs: Set<String> = [
        "read", "open", "show", "go", "take", "search", "find", "look", "check", "start", "launch", "create", "make", "run", "ask",
        "tell", "what's", "whats", "what", "who", "when", "any", "bring", "switch", "jump", "navigate", "pull", "get", "have", "spin",
        "kick", "head", "display", "view", "visit", "list", "how", "which", "where", "locate", "hunt", "send", "spawn",
        "dispatch", "draft", "write", "add", "stop", "message",
    ]
    static let latestTriggers: Set<String> = ["last", "latest", "newest", "recent", "new", "next", "previous"]
    static let numbers: [String: Int] = ["one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "1": 1, "2": 2, "3": 3, "4": 4, "5": 5]
    static let agentVerbs: Set<String> = [
        "start", "launch", "create", "make", "run", "spin", "kick", "ask", "have", "get", "fire", "send", "spawn", "dispatch", "begin",
    ]
    static let agentNouns: Set<String> = ["agent", "task", "job", "bot"]
    static let searchNouns: Set<String> = ["search", "searching"]
    static let openVerbs: Set<String> = ["open", "show", "bring", "pull", "launch", "start", "get", "give", "display", "need", "want"]
    static let screenPhrases: [[String]] = [
        ["take", "me", "to"], ["go", "to"], ["show", "me"], ["open", "up"], ["switch", "to"], ["bring", "up"], ["jump", "to"],
        ["navigate", "to"], ["head", "to"], ["pull", "up"], ["get", "me", "to"], ["let", "me", "see"], ["go", "back", "to"],
        ["return", "to"], ["flip", "to"], ["open"], ["show"], ["display"], ["visit"], ["view"],
    ]
    static let abilityNouns: Set<String> = [
        "capabilities", "capability", "abilities", "ability", "skills", "skill", "features", "powers", "commands", "tools",
        "functions", "options", "talents", "tricks", "limits", "limitations",
    ]
    static let abilityPhrases: [[String]] = [
        ["what", "can", "you", "do"], ["what", "else", "can", "you", "do"], ["what", "can", "you", "help"], ["what", "are", "you", "able"],
        ["what", "are", "you", "capable"], ["what", "do", "you", "do"], ["how", "can", "you", "help"],
        ["how", "do", "you", "work"], ["how", "do", "i", "use", "you"], ["what", "can", "i", "ask"], ["what", "can", "i", "say"],
        ["what", "can", "i", "tell"], ["what", "should", "i", "ask"], ["what", "should", "i", "say"], ["what", "you", "can", "do"],
        ["what", "are", "you", "good", "at"], ["what", "tools"], ["which", "tools"], ["what", "commands"], ["which", "commands"],
        ["what", "can", "you", "handle"], ["what", "do", "you", "support"], ["are", "you", "able", "to", "do"], ["what", "is", "possible"],
        ["what's", "possible"], ["who", "are", "you"],
    ]
    static let helpAlone: [[String]] = [["help"], ["help", "me"], ["help", "please"], ["i", "need", "help"], ["need", "help"], ["menu"]]
    static let tailFillers: Set<String> = ["please", "now", "thanks", "thank", "you", "for", "me"]

    @MainActor public static func spot(_ text: String, me: String = "", paging: Bool = false) -> GenieSpot? {
        let tokens = tokenize(text)
        guard !tokens.isEmpty else { return nil }
        let start = clauseStart(tokens)
        let addressed = isAddressed(tokens)
        let found = (paging ? GeniePages.spot(tokens, from: start) : nil) ?? vitals(tokens) ?? trick(tokens, from: start) ?? capabilities(tokens, from: start) ?? agent(tokens, text: text, from: start) ?? openSearch(tokens) ?? latest(tokens) ?? search(tokens, text: text, from: start)
            ?? screen(tokens, text: text, me: me)
        guard var spot = found else { return nil }
        spot.addressed = addressed || spot.tool == GeniePages.tool
        return spot
    }

    public static func addressed(_ text: String) -> Bool {
        isAddressed(tokenize(text))
    }

    public static func calledByName(_ text: String) -> Bool {
        tokenize(text).contains { wakeWords.contains($0.word) }
    }

    public static func withoutWakeWord(_ text: String) -> String {
        let tokens = tokenize(text)
        guard let wake = tokens.firstIndex(where: { wakeWords.contains($0.word) }) else { return text.dartTrimmed }
        let chars = Array(text)
        let before = String(chars[0..<tokens[wake].start])
        let after = String(chars[tokens[wake].end...])
        let leading = before.lowercased().trimmingCharacters(in: .whitespacesAndNewlines.union(.punctuationCharacters))
        let kept = ["", "hey", "ok", "okay", "hi", "oh"].contains(leading) ? after : before + after
        return kept.trimmingCharacters(in: .whitespacesAndNewlines.union(CharacterSet(charactersIn: ",.!?"))).dartTrimmed
    }

    public static func segments(_ text: String, highlight: Range<Int>?) -> [GenieSegment] {
        let chars = Array(text)
        guard let range = highlight, range.lowerBound >= 0, range.upperBound <= chars.count, !range.isEmpty else {
            return text.isEmpty ? [] : [GenieSegment(text: text, actionable: false)]
        }
        var out: [GenieSegment] = []
        if range.lowerBound > 0 { out.append(GenieSegment(text: String(chars[0..<range.lowerBound]), actionable: false)) }
        out.append(GenieSegment(text: String(chars[range]), actionable: true))
        if range.upperBound < chars.count { out.append(GenieSegment(text: String(chars[range.upperBound...]), actionable: false)) }
        return out
    }

    static func tokenize(_ text: String) -> [Token] {
        var tokens: [Token] = []
        var current = ""
        var begin = 0
        for (index, char) in text.enumerated() {
            if char.isLetter || char.isNumber || char == "'" || char == "’" || char == "/" || char == "#" || char == "@" {
                if current.isEmpty { begin = index }
                current.append(char == "’" ? "'" : char)
            } else if !current.isEmpty {
                tokens.append(Token(word: current.lowercased(), start: begin, end: index))
                current = ""
            }
        }
        if !current.isEmpty { tokens.append(Token(word: current.lowercased(), start: begin, end: text.count)) }
        return tokens
    }

    static func clauseStart(_ tokens: [Token]) -> Int {
        var index = 0
        var moved = true
        while moved && index < tokens.count {
            moved = false
            if wakeWords.contains(tokens[index].word) {
                index += 1
                moved = true
                continue
            }
            for opener in openers where index + opener.count <= tokens.count {
                if zip(opener, tokens[index..<(index + opener.count)]).allSatisfy({ $0 == $1.word }) {
                    index += opener.count
                    moved = true
                    break
                }
            }
        }
        return min(index, tokens.count)
    }

    static func isAddressed(_ tokens: [Token]) -> Bool {
        if tokens.contains(where: { wakeWords.contains($0.word) }) { return true }
        let start = clauseStart(tokens)
        guard start < tokens.count else { return false }
        return commandVerbs.contains(tokens[start].word)
    }

    static func range(_ tokens: [Token], _ from: Int, _ through: Int) -> Range<Int> {
        tokens[from].start..<tokens[through].end
    }

    static func rest(_ text: String, after token: Token) -> String {
        let chars = Array(text)
        guard token.end < chars.count else { return "" }
        var words = String(chars[token.end...]).trimmingCharacters(in: .whitespacesAndNewlines.union(CharacterSet(charactersIn: ",.!?")))
        var trimmed = true
        while trimmed {
            trimmed = false
            for filler in [" please", " for me", " now", " thanks", " thank you"] where words.lowercased().hasSuffix(filler) {
                words = String(words.dropLast(filler.count)).trimmingCharacters(in: .whitespacesAndNewlines.union(CharacterSet(charactersIn: ",.!?")))
                trimmed = true
            }
        }
        return words
    }

    static func latestPage(_ kind: String) -> (page: String, symbol: String, route: Route?) {
        switch kind {
        case "inbox": return ("Inbox", "tray.full", .inbox(.all))
        case "commit": return ("Commits", RouteCatalog.symbol(.commits), nil)
        case "chat": return ("Chat", AppTab.chat.symbol, .chat)
        case "agent": return ("Agents", AppTab.agents.symbol, .agents(.all))
        case "task": return ("Tasks", RouteCatalog.symbol(.tasks), .tasks(filter: nil))
        case "note": return ("Notes", RouteCatalog.symbol(.notes), .notes)
        case "issue": return ("Issues", RouteCatalog.symbol(.issue), nil)
        case "pull": return ("Pull requests", RouteCatalog.symbol(.pull), nil)
        default: return ("Calendar", RouteCatalog.symbol(.calendar), .calendar(date: nil, mode: .agenda))
        }
    }

    static let latestTitles: [String: String] = [
        "inbox": "Read the newest in your inbox", "commit": "Read the last commit", "chat": "Read the last chats",
        "agent": "Check your latest agent", "task": "Read the newest task", "note": "Read your last note",
        "issue": "Read the newest issue", "pull": "Read the newest pull request", "event": "Check your next event",
    ]

    static func noun(_ tokens: [Token], at index: Int) -> (kind: String, last: Int, plural: Bool)? {
        let word = tokens[index].word
        if index + 1 < tokens.count, ["pull", "merge"].contains(word), tokens[index + 1].word.hasPrefix("request") {
            return ("pull", index + 1, tokens[index + 1].word.hasSuffix("s"))
        }
        if word == "inbox" { return ("inbox", index, false) }
        guard let kind = ForkMeshToolbox.latestKind(word) else { return nil }
        if word == "run" || word == "calendar" { return nil }
        return (kind, index, word.hasSuffix("s") && !["news", "inbox"].contains(word))
    }

    static func latest(_ tokens: [Token]) -> GenieSpot? {
        for (index, token) in tokens.enumerated() where latestTriggers.contains(token.word) {
            if let prior = tokens[..<index].last(where: { !["a", "an", "the", "my", "most"].contains($0.word) }), ["create", "make", "start", "launch", "spin", "kick", "add", "write"].contains(prior.word) {
                continue
            }
            var first = index
            if index > 0, tokens[index - 1].word == "most" { first = index - 1 }
            let upper = min(tokens.count, index + 6)
            for probe in (index + 1)..<max(upper, index + 1) {
                guard var hit = noun(tokens, at: probe) else { continue }
                if token.word == "next" && hit.kind != "event" { break }
                if ["previous", "new"].contains(token.word) && hit.kind == "event" { break }
                var last = hit.last
                if hit.kind == "chat" {
                    let tail = min(tokens.count, last + 5)
                    if let inbox = ((last + 1)..<max(tail, last + 1)).first(where: { tokens[$0].word == "inbox" }) {
                        hit.kind = "inbox"
                        last = inbox
                    } else if ((last + 1)..<max(tail, last + 1)).contains(where: { ["pings", "notifications"].contains(tokens[$0].word) }) {
                        hit.kind = "inbox"
                    }
                }
                var count = hit.plural ? 3 : 1
                for between in (index + 1)..<probe {
                    if let number = numbers[tokens[between].word] { count = number }
                }
                if index > 0, let number = numbers[tokens[index - 1].word] { count = number }
                return latestSpot(hit.kind, count: count, phrase: range(tokens, first, last))
            }
        }
        for (index, token) in tokens.enumerated() where ["read", "check"].contains(token.word) {
            let upper = min(tokens.count, index + 4)
            for probe in (index + 1)..<max(upper, index + 1) where tokens[probe].word == "inbox" {
                return latestSpot("inbox", count: 3, phrase: range(tokens, index, probe))
            }
        }
        return nil
    }

    static func latestSpot(_ kind: String, count: Int, phrase: Range<Int>) -> GenieSpot {
        let page = latestPage(kind)
        var args = ["what": kind]
        if count > 1 { args["count"] = String(min(count, 5)) }
        return GenieSpot(
            tool: "read_latest", arguments: args, title: latestTitles[kind] ?? "Read the latest", page: page.page,
            symbol: page.symbol, route: page.route, phrase: phrase)
    }

    static func openSearch(_ tokens: [Token]) -> GenieSpot? {
        for (index, token) in tokens.enumerated() where searchNouns.contains(token.word) {
            let lower = max(0, index - 3)
            guard let verb = (lower..<index).last(where: { openVerbs.contains(tokens[$0].word) }) else { continue }
            if index + 1 < tokens.count, ["for", "about"].contains(tokens[index + 1].word) { continue }
            var last = index
            if index + 1 < tokens.count, ["prompt", "bar", "box", "screen", "page", "field", "window"].contains(tokens[index + 1].word) {
                last = index + 1
            }
            return GenieSpot(
                tool: "open_search", title: "Open search", page: "Search", symbol: RouteCatalog.symbol(.search),
                route: nil, phrase: range(tokens, verb, last))
        }
        return nil
    }

    static func search(_ tokens: [Token], text: String, from start: Int) -> GenieSpot? {
        guard start < tokens.count else { return nil }
        var verbEnd = start
        let word = tokens[start].word
        if word == "search" {
            if start + 1 < tokens.count, ["for", "about"].contains(tokens[start + 1].word) { verbEnd = start + 1 }
        } else if ["look", "hunt", "dig", "track"].contains(word), start + 1 < tokens.count, ["up", "for", "down"].contains(tokens[start + 1].word) {
            verbEnd = start + 1
        } else if word == "find", start + 1 < tokens.count, tokens[start + 1].word == "me" {
            verbEnd = start + 1
        } else if word != "find" && word != "locate" {
            return nil
        }
        let query = rest(text, after: tokens[verbEnd])
        guard !query.isEmpty else { return nil }
        return GenieSpot(
            tool: "search_app", arguments: ["query": query], title: "Search for \"\(query)\"", page: "Search",
            symbol: RouteCatalog.symbol(.search), route: nil, phrase: tokens[start].start..<text.count)
    }

    static let vitalsPhrases: [[String]] = [
        ["how", "are", "you", "feeling"], ["how", "do", "you", "feel"], ["how", "healthy", "are", "you"], ["are", "you", "hungry"],
        ["are", "you", "healthy"], ["are", "you", "sick"], ["your", "health"], ["your", "vitals"], ["your", "mood"],
        ["how", "is", "your", "health"], ["how's", "your", "health"], ["check", "your", "health"], ["feed", "you"],
    ]

    static func vitals(_ tokens: [Token]) -> GenieSpot? {
        let words = tokens.map(\.word)
        for phrase in vitalsPhrases {
            guard let at = find(phrase, in: words) else { continue }
            return GenieSpot(
                tool: GenieToolbox.vitalsTool, title: "How I'm feeling", page: "Health", symbol: "heart.fill", route: nil,
                phrase: range(tokens, at, at + phrase.count - 1))
        }
        return nil
    }

    static let trickVerbs: Set<String> = ["do", "show", "perform", "try", "give", "pull", "bust", "play"]
    static let trickFillers: Set<String> = [
        "me", "us", "a", "an", "the", "your", "some", "another", "little", "quick", "cool", "nice", "fun", "new", "best", "favourite",
        "favorite", "off",
    ]
    static let trickNouns: Set<String> = ["trick", "tricks", "stunt", "stunts"]
    static let trickTail: Set<String> = [
        "please", "now", "thanks", "thank", "you", "for", "me", "us", "again", "genie", "spider", "buddy", "little", "guy", "once", "more",
    ]

    static func trick(_ tokens: [Token], from start: Int) -> GenieSpot? {
        let words = tokens.map(\.word)
        guard start < words.count else { return nil }
        var at = start
        var asked = false
        while at < words.count, trickVerbs.contains(words[at]) || (asked && trickFillers.contains(words[at])) {
            asked = asked || trickVerbs.contains(words[at])
            at += 1
        }
        guard at < words.count else { return nil }
        let tailFrom: Int
        let chosen: GenieTrick?
        if trickNouns.contains(words[at]) || (words[at] == "surprise" && at + 1 < words.count && ["me", "us"].contains(words[at + 1])) {
            guard asked || words[at] == "surprise" else { return nil }
            chosen = nil
            tailFrom = words[at] == "surprise" ? at + 2 : at + 1
        } else if let hit = GenieTricks.match(words, at: at) {
            chosen = hit.trick
            tailFrom = at + hit.count
        } else {
            return nil
        }
        let rest = words[min(tailFrom, words.count)...]
        guard asked || rest.allSatisfy({ trickTail.contains($0) }) else { return nil }
        guard !rest.contains(where: { ["to", "into", "up", "on", "with", "agent", "about"].contains($0) }) || rest.allSatisfy({ trickTail.contains($0) }) else {
            return nil
        }
        let last = max(min(tailFrom, tokens.count) - 1, start)
        let phrase = range(tokens, start, last)
        guard let chosen else {
            return GenieSpot(
                tool: GenieTricks.tool, arguments: ["trick": "surprise"], title: "A surprise trick", page: GenieTricks.page,
                symbol: "wand.and.stars", route: nil, phrase: phrase)
        }
        return GenieSpot(
            tool: GenieTricks.tool, arguments: ["trick": chosen.id], title: chosen.title, page: GenieTricks.page, symbol: chosen.symbol,
            route: nil, phrase: phrase)
    }

    static func capabilities(_ tokens: [Token], from start: Int) -> GenieSpot? {
        let words = tokens.map(\.word)
        var match: (first: Int, last: Int)?
        for phrase in abilityPhrases {
            guard let at = find(phrase, in: words) else { continue }
            match = (at, at + phrase.count - 1)
            break
        }
        if match == nil, let at = words.indices.first(where: { $0 + 1 < words.count && words[$0] == "your" && abilityNouns.contains(words[$0 + 1]) }) {
            match = (at, at + 1)
        }
        if match == nil, start < words.count {
            let rest = Array(words[start...]).filter { !tailFillers.contains($0) || $0 == "me" }
            if helpAlone.contains(rest) { match = (start, tokens.count - 1) }
        }
        guard let match else { return nil }
        var args: [String: String] = [:]
        if match.last + 1 < tokens.count {
            let after = words[(match.last + 1)...].filter { !["do", "help", "you", "me", "for", "with", "about", "can", "i"].contains($0) }
            if let ability = after.lazy.compactMap({ GenieCapabilities.ability($0) }).first { args["topic"] = ability.topic }
        }
        return GenieSpot(
            tool: GenieCapabilities.tool, arguments: args, title: "What I can do", page: GenieCapabilities.page,
            symbol: GenieCapabilities.symbol, route: nil, phrase: range(tokens, match.first, match.last))
    }

    static func find(_ phrase: [String], in words: [String]) -> Int? {
        guard phrase.count <= words.count else { return nil }
        return (0...(words.count - phrase.count)).first { at in
            zip(phrase, words[at..<(at + phrase.count)]).allSatisfy { $0 == $1 }
        }
    }

    static func agent(_ tokens: [Token], text: String, from start: Int) -> GenieSpot? {
        for (index, token) in tokens.enumerated() where agentVerbs.contains(token.word) {
            let upper = min(tokens.count, index + 5)
            guard let nounAt = ((index + 1)..<max(upper, index + 1)).first(where: { agentNouns.contains(tokens[$0].word) }) else { continue }
            let between = tokens[(index + 1)..<nounAt].map(\.word)
            guard between.allSatisfy({ ["a", "an", "new", "the", "up", "off", "coding", "another", "me"].contains($0) }) else { continue }
            if ["have", "get", "ask"].contains(token.word), tokens[nounAt].word != "agent" { continue }
            var cut = nounAt
            while cut + 1 < tokens.count {
                let next = tokens[cut + 1].word
                if ["on", "in", "for"].contains(next), cut + 2 < tokens.count, RepoRef(slug: tokens[cut + 2].word) != nil {
                    cut += 2
                } else if ["for", "me", "to", "that", "which", "and", "who", "can", "will", "should", "please", "it"].contains(next) {
                    cut += 1
                } else {
                    break
                }
            }
            let prompt = rest(text, after: tokens[cut])
            guard prompt.split(separator: " ").count >= 2 else { continue }
            var args = ["prompt": prompt]
            if let repo = tokens.first(where: { $0.word.contains("/") && RepoRef(slug: $0.word) != nil }) { args["repository"] = repo.word }
            return GenieSpot(
                tool: "start_agent", arguments: args, title: "Start an agent: \(OnDeviceChatTools.clip(prompt, limit: 60))",
                page: "Agents", symbol: AppTab.agents.symbol, route: nil, phrase: tokens[index].start..<text.count)
        }
        return nil
    }

    @MainActor static func screen(_ tokens: [Token], text: String, me: String) -> GenieSpot? {
        for index in tokens.indices {
            for phrase in screenPhrases where index + phrase.count < tokens.count {
                guard zip(phrase, tokens[index..<(index + phrase.count)]).allSatisfy({ $0 == $1.word }) else { continue }
                let afterVerb = index + phrase.count
                let upper = min(tokens.count, afterVerb + 5)
                let candidates = tokens[afterVerb..<upper]
                var names: [(String, Int)] = []
                var words: [String] = []
                for (offset, candidate) in candidates.enumerated() {
                    words.append(candidate.word)
                    names.append((words.joined(separator: " "), afterVerb + offset))
                }
                for (name, last) in names.reversed() {
                    guard let route = GenieToolbox.route(named: name, me: me), confirms(route, name: name) else { continue }
                    return GenieSpot(
                        tool: "open_screen", arguments: ["screen": name], title: "Open \(route.title)", page: route.title,
                        symbol: AppTab.barOrder.first(where: { $0.root == route })?.symbol ?? RouteCatalog.symbol(route.kind),
                        route: route, phrase: range(tokens, index, last))
                }
            }
        }
        return nil
    }

    static func confirms(_ route: Route, name: String) -> Bool {
        if name.contains("/") || name.hasPrefix("@") || name.contains("#") { return true }
        let said = Set(name.split(separator: " ").map(String.init).filter { !GenieToolbox.fillerWords.contains($0) })
        guard !said.isEmpty else { return false }
        let titles = [route.title, RouteCatalog.title(route.kind)].joined(separator: " ").lowercased()
        let known = Set(titles.split(whereSeparator: { !$0.isLetter && !$0.isNumber }).map(String.init))
        return said.allSatisfy { word in
            known.contains(word) || known.contains(String(word.dropLast())) || known.contains(word + "s")
        }
    }
}
