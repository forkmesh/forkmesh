import FMBase
import Foundation

public struct ModelMention: Equatable, Sendable {
    public enum Intent: String, Sendable { case answer, work }

    public let alias: String
    public let model: String
    public let provider: String
    public let request: String
    public let intent: Intent
}

public enum IssueModelMentions {
    static let aliasTable: [(String, String)] = [
        ("opus", "claude-opus-5"), ("sonnet", "claude-sonnet-5"), ("haiku", "claude-haiku-4-5"),
        ("fable", "claude-fable-5"), ("claude", "claude-sonnet-5"), ("codex", "gpt-5.5"), ("gpt", "gpt-5.5"),
        ("sol", "gpt-5.6-sol"), ("luna", "gpt-5.6-luna"), ("terra", "gpt-5.6-terra"), ("spark", "gpt-5.3-codex-spark"),
    ]

    public static var aliases: [String] { aliasTable.map(\.0) }

    static func trimAlias(_ alias: String) -> String {
        var out = alias
        while out.hasSuffix(".") || out.hasSuffix("-") { out.removeLast() }
        return out
    }

    public static func model(forAlias alias: String) -> String {
        let name = trimAlias(alias.dartTrimmed.lowercased())
        if name.isEmpty { return "" }
        if let hit = aliasTable.first(where: { $0.0 == name }) { return hit.1 }
        if name.hasPrefix("claude-") && name.count > "claude-".count { return name }
        if name.hasPrefix("gpt-") && name.count > "gpt-".count { return name }
        return ""
    }

    public static func provider(forModel model: String) -> String {
        let id = model.dartTrimmed.lowercased()
        if id.hasPrefix("claude") { return "claude-code" }
        if id.hasPrefix("gpt") { return "codex" }
        return ""
    }

    static func regex(_ pattern: String, _ options: NSRegularExpression.Options = []) -> NSRegularExpression {
        try! NSRegularExpression(pattern: pattern, options: options)
    }

    static let mentionRe = regex("(?:^|[^A-Za-z0-9_@./-])@([A-Za-z][A-Za-z0-9.-]{0,63})")
    static let leadRe = regex(
        "^(?:hey|hi|hello|ok|okay|so|also|and|please|pls|plz|can\\s+you|could\\s+you|would\\s+you|will\\s+you|do\\s+you\\s+mind|i\\s+need\\s+you\\s+to|we\\s+need\\s+you\\s+to|let'?s|go\\s+ahead\\s+and|if\\s+you\\s+can|when\\s+you\\s+can)\\b[\\s,:;.!-]*",
        .caseInsensitive)
    static let explicitAgentRe = regex(
        "\\b(?:start|spawn|launch|kick\\s+off|fire\\s+up|run)\\s+(?:an?\\s+|the\\s+)?(?:coding\\s+|background\\s+|new\\s+)?agent\\b")
    static let workRe = regex(
        "\\b(?:fix|implement|patch|repair|resolve|solve|refactor|rewrite|migrate|unbreak|land|ship)\\b|\\b(?:open|send|raise|submit|make)\\s+(?:an?|the)\\s+(?:pr|pull\\s+request|patch|fix|change)\\b|\\bwork\\s+on\\s+(?:this|it|the\\s+issue)\\b|\\btake\\s+(?:this|it)\\s+on\\b|\\bwrite\\s+(?:the|a)\\s+(?:code|fix|patch)\\b|\\bdo\\s+(?:it|this|that)\\b")
    static let whQuestionRe = regex(
        "^(?:how|what|what's|whats|why|when|where|which|who|whose|whom|thoughts|explain|describe|tell|summari[sz]e|review|compare|analy[sz]e|opinion|any\\s+idea)\\b")
    static let auxQuestionRe = regex(
        "^(?:is|are|was|were|do|does|did|can|could|would|will|should|shall|may|might|have|has|had)\\b")

    static func matches(_ re: NSRegularExpression, _ text: String) -> Bool {
        re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
    }

    static func simplified(_ text: String) -> String {
        text.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    }

    static func stripPoliteLead(_ request: String) -> String {
        var text = request.dartTrimmed
        for _ in 0..<6 {
            guard let match = leadRe.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
                  match.range.length > 0, let range = Range(match.range, in: text) else { break }
            text = String(text[range.upperBound...]).dartTrimmed
        }
        return text
    }

    public static func requestsWork(_ request: String) -> Bool {
        let text = stripPoliteLead(simplified(request)).lowercased()
        if text.isEmpty { return false }
        if matches(explicitAgentRe, text) { return true }
        if !matches(workRe, text) { return false }
        if matches(whQuestionRe, text) { return false }
        if text.hasSuffix("?") && matches(auxQuestionRe, text) { return false }
        return true
    }

    static func trimRequest(_ raw: Substring) -> String {
        let start = raw.firstIndex { !($0.isWhitespace || $0 == ":" || $0 == "," || $0 == ";" || $0 == "." || $0 == "-") }
        return start.map { String(raw[$0...]).dartTrimmed } ?? ""
    }

    public static func parse(_ text: String) -> [ModelMention] {
        if text.dartTrimmed.isEmpty { return [] }
        let ns = text as NSString
        struct Hit { let start: Int; let end: Int; let alias: String; let model: String }
        var hits: [Hit] = []
        for match in mentionRe.matches(in: text, range: NSRange(location: 0, length: ns.length)) {
            let captured = ns.substring(with: match.range(at: 1))
            let model = model(forAlias: captured)
            if model.isEmpty { continue }
            hits.append(Hit(start: match.range(at: 1).location - 1, end: NSMaxRange(match.range(at: 1)),
                            alias: trimAlias(captured.lowercased()), model: model))
        }
        if hits.isEmpty { return [] }
        var stripped = ""
        var cursor = 0
        for hit in hits {
            stripped += ns.substring(with: NSRange(location: cursor, length: hit.start - cursor))
            cursor = hit.end
        }
        stripped = simplified(stripped + ns.substring(from: cursor))
        var out: [ModelMention] = []
        for (i, hit) in hits.enumerated() {
            let stop = i + 1 < hits.count ? hits[i + 1].start : ns.length
            var request = trimRequest(Substring(ns.substring(with: NSRange(location: hit.end, length: stop - hit.end))))
            if request.utf16.count < 2 { request = stripped }
            out.append(ModelMention(
                alias: hit.alias, model: hit.model, provider: provider(forModel: hit.model), request: request,
                intent: requestsWork(request) ? .work : .answer))
        }
        return out
    }

    public static func agentPrompt(issueNumber: Int, title: String, question: String) -> String {
        let ask = question.dartTrimmed.isEmpty ? "Summarize this issue and propose next steps." : question.dartTrimmed
        return "Issue #\(issueNumber): \(title)\n\n\(ask)\n\nReply on the issue thread; do not change code unless asked."
    }

    public static func reprioritizePrompt(openCount: Int, fromReadme: Bool) -> String {
        if fromReadme {
            return "Read the README and the \(openCount) open issues, then set each issue's priority (0-4) so the work the README describes as most important comes first. Only change priorities; explain the order in one comment on the highest-priority issue."
        }
        return "Review the \(openCount) open issues and set each one's priority (0-4) by user impact and effort. Only change priorities; explain the new order in one comment on the highest-priority issue."
    }
}
