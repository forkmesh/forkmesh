// The issue models: Issue, IssueEvent, IssueMention and the relationship readers
// (issueRelationNumbers, issueBodyWithoutRelationLines,
// issueReferenceNumbers, subIssueNumbersOf, issueMentionsOf).

import Foundation
import FMBase

public struct Issue: Equatable, Hashable, Sendable, Identifiable {
    public var number: Int
    public var title: String
    /// "open" or "closed".
    public var status: String
    public var body: String
    public var author: String
    public var labels: [String]
    public var milestone: String
    public var priority: Int
    /// 0...100 percent complete.
    public var progress: Int
    public var assignees: [String]
    public var events: [IssueEvent]
    public var votes: Int
    public var bountyUsd: Double
    public var bountyAddress: String
    public var bountyStatus: String
    public var bountyRequiredLamports: Int
    public var bountyReceivedLamports: Int
    public var bountyAmountSol: Double
    public var bountyPayUri: String
    public var bountyPayee: String
    public var bountyPayoutSig: String

    public var id: Int { number }

    public init(
        number: Int, title: String, status: String = "open", body: String = "", author: String = "",
        labels: [String] = [], milestone: String = "", priority: Int = 0, progress: Int = 0,
        assignees: [String] = [], events: [IssueEvent] = [], votes: Int = 0, bountyUsd: Double = 0,
        bountyAddress: String = "", bountyStatus: String = "", bountyRequiredLamports: Int = 0,
        bountyReceivedLamports: Int = 0, bountyAmountSol: Double = 0, bountyPayUri: String = "",
        bountyPayee: String = "", bountyPayoutSig: String = ""
    ) {
        self.number = number
        self.title = title
        self.status = status
        self.body = body
        self.author = author
        self.labels = labels
        self.milestone = milestone
        self.priority = priority
        self.progress = progress
        self.assignees = assignees
        self.events = events
        self.votes = votes
        self.bountyUsd = bountyUsd
        self.bountyAddress = bountyAddress
        self.bountyStatus = bountyStatus
        self.bountyRequiredLamports = bountyRequiredLamports
        self.bountyReceivedLamports = bountyReceivedLamports
        self.bountyAmountSol = bountyAmountSol
        self.bountyPayUri = bountyPayUri
        self.bountyPayee = bountyPayee
        self.bountyPayoutSig = bountyPayoutSig
    }

    /// `labels` or `assignees` that are not a list read as empty.
    public init(json j: JSONObject) {
        let bountyData: JSONValue
        if let record = j["bounty"], record.objectValue != nil {
            bountyData = record
        } else {
            func pick(_ values: JSONValue?...) -> JSONValue {
                values.lazy.compactMap { $0 }.first { !$0.isNull } ?? .null
            }
            bountyData = .object([
                "address": pick(j["bountyAddress"], j["address"]),
                "status": pick(j["bountyStatus"]),
                "amountUsd": pick(j["bountyUsd"], j["amountUsd"]),
                "requiredLamports": pick(j["bountyRequiredLamports"], j["requiredLamports"]),
                "receivedLamports": pick(j["bountyReceivedLamports"], j["receivedLamports"]),
                "amountSol": pick(j["bountyAmountSol"], j["amountSol"]),
                "uri": pick(j["bountyPayUri"], j["payUri"], j["uri"]),
                "payee": pick(j["bountyPayee"], j["payee"]),
                "payoutSig": pick(j["bountyPayoutSig"], j["payoutSig"]),
            ])
        }
        let bounty = IssueBounty(json: bountyData)
        self.init(
            number: ModelJSON.int(j["number"]),
            title: j.str("title"),
            status: j.str("status", "open"),
            body: j.str("body"),
            author: j.str("author"),
            labels: ModelJSON.stringList(j["labels"]),
            milestone: j.str("milestone"),
            priority: ModelJSON.int(j["priority"]),
            progress: min(max(ModelJSON.int(j["progress"]), 0), 100),
            assignees: ModelJSON.stringList(j["assignees"]),
            events: j.maps("events").map(IssueEvent.init(json:)),
            votes: ModelJSON.int(j["votes"]),
            bountyUsd: j["bountyUsd"]?.doubleValue ?? bounty.amountUsd,
            bountyAddress: bounty.address,
            bountyStatus: bounty.status,
            bountyRequiredLamports: bounty.requiredLamports,
            bountyReceivedLamports: bounty.receivedLamports,
            bountyAmountSol: bounty.amountSol,
            bountyPayUri: bounty.payUri,
            bountyPayee: bounty.payee,
            bountyPayoutSig: bounty.payoutSig
        )
    }

    public var isOpen: Bool { status != "closed" }

    public var bounty: IssueBounty {
        IssueBounty(
            address: bountyAddress,
            status: bountyStatus.isEmpty ? "open" : bountyStatus,
            amountUsd: bountyUsd,
            requiredLamports: bountyRequiredLamports,
            receivedLamports: bountyReceivedLamports,
            amountSol: bountyAmountSol,
            payUri: bountyPayUri,
            payee: bountyPayee,
            payoutSig: bountyPayoutSig
        )
    }

    public var bountyStatusLabel: String { bounty.statusLabel }
    public var bountyProgressLabel: String { bounty.progressLabel }

    public var hasBountyFunding: Bool {
        bountyUsd > 0 || !bountyAddress.isEmpty || bountyRequiredLamports > 0 || bountyReceivedLamports > 0
            || !bountyPayoutSig.isEmpty
    }

    public func copyWithBounty(_ bounty: IssueBounty) -> Issue {
        var copy = self
        copy.bountyUsd = bounty.amountUsd > 0 ? bounty.amountUsd : bountyUsd
        copy.bountyAddress = bounty.address
        copy.bountyStatus = bounty.statusLabel
        copy.bountyRequiredLamports = bounty.requiredLamports
        copy.bountyReceivedLamports = bounty.receivedLamports
        copy.bountyAmountSol = bounty.amountSol
        copy.bountyPayUri = bounty.payUri
        copy.bountyPayee = bounty.payee
        copy.bountyPayoutSig = bounty.payoutSig
        return copy
    }
}

public struct IssueEvent: Equatable, Hashable, Sendable {
    public var type: String
    public var id: String
    /// edit/delete/reaction: the event id acted on.
    public var target: String
    public var body: String
    public var author: String
    public var authorName: String
    public var status: String
    public var ts: Int

    public init(
        type: String, id: String = "", target: String = "", body: String = "", author: String = "",
        authorName: String = "", status: String = "", ts: Int = 0
    ) {
        self.type = type
        self.id = id
        self.target = target
        self.body = body
        self.author = author
        self.authorName = authorName
        self.status = status
        self.ts = ts
    }

    public init(json j: JSONObject) {
        self.init(
            type: j.str("type"),
            id: j.str("id"),
            target: j.str("target"),
            body: j.str("body"),
            author: j.str("author"),
            authorName: j.str("authorName"),
            status: j.str("status"),
            ts: ModelJSON.int(j["ts"])
        )
    }

    public var displayAuthor: String { authorName.isEmpty ? author : authorName }
}

// Sub-issues keep their relationship in the signed opening Markdown, the same
// way the desktop and the website read it: "Parent issue: #N" or
// "Depends on: #A, #B" names the issue's parents. Planning templates write the
// line as a list item ("- Depends on: #481") or in bold, so both are allowed.
private let issueRelationLine = try! NSRegularExpression(
    pattern: "^[ \\t]*(?:[-*+][ \\t]+)?(?:\\*\\*)?(?:parent\\s+issues?|depends?\\s+on)(?:\\*\\*)?[ \\t]*:(?:\\*\\*)?[ \\t]*(.+)$",
    options: [.caseInsensitive, .anchorsMatchLines])
// `\b` and `\d` spelled out as ASCII classes: ICU's are Unicode-aware.
private let relationReference = try! NSRegularExpression(pattern: "#([1-9][0-9]*)(?![A-Za-z0-9_])")
private let relationAnyNumber = try! NSRegularExpression(pattern: "#[1-9]")
private let threeNewlines = try! NSRegularExpression(pattern: "\\n{3,}")
private let bareReference = try! NSRegularExpression(
    pattern: "(?<![A-Za-z0-9_&#/])#([1-9][0-9]{0,8})(?![A-Za-z0-9_])")
private let inlineCode = try! NSRegularExpression(pattern: "`+[^`]*`+")
private let subIssueRow = try! NSRegularExpression(
    pattern: "\\(sub-issue\\s+#([1-9][0-9]*)\\)", options: [.caseInsensitive])

/// Parent issue numbers named on relationship lines, first-seen order (at most 25).
public func issueRelationNumbers(_ body: String) -> [Int] {
    var numbers: [Int] = []
    for line in DartRegex.allMatches(issueRelationLine, body) {
        let named = DartRegex.group(line, 1, body)
        for ref in DartRegex.allMatches(relationReference, named) {
            guard let number = Int(DartRegex.group(ref, 1, named)) else { continue }
            if !numbers.contains(number) { numbers.append(number) }
            if numbers.count == 25 { return numbers }
        }
    }
    return numbers
}

/// The description without its relationship lines, which render as links.
/// "Depends on: None" names nothing, so it stays readable prose.
public func issueBodyWithoutRelationLines(_ body: String) -> String {
    let stripped = DartRegex.replaceAllMapped(issueRelationLine, body) { match in
        let named = DartRegex.group(match, 1, body)
        return DartRegex.contains(relationAnyNumber, named) ? "" : DartRegex.group(match, 0, body)
    }
    return DartRegex.replaceAll(threeNewlines, stripped, with: "\n\n").dartTrimmed
}

/// Every bare #N in Markdown prose, skipping fenced and inline code and any
/// #N glued to a word, path, or HTML entity.
public func issueReferenceNumbers(_ body: String) -> [Int] {
    var numbers: [Int] = []
    var fence = ""
    for rawLine in body.components(separatedBy: "\n") {
        let trimmed = rawLine.dartTrimmed
        let fenceLine = trimmed.hasPrefix("```") || trimmed.hasPrefix("~~~")
        if !fence.isEmpty {
            if fenceLine && trimmed.hasPrefix(fence) { fence = "" }
            continue
        }
        if fenceLine {
            fence = String(trimmed.prefix(3))
            continue
        }
        let line = DartRegex.replaceAll(inlineCode, rawLine, with: " ")
        for match in DartRegex.allMatches(bareReference, line) {
            guard let number = Int(DartRegex.group(match, 1, line)) else { continue }
            if !numbers.contains(number) { numbers.append(number) }
        }
    }
    return numbers
}

/// Children of `parent`: issues naming it on a relationship line, plus rows
/// its own checklist linked with "(sub-issue #N)". Ascending by number.
public func subIssueNumbersOf(_ issues: [Issue], _ parent: Issue) -> [Int] {
    var numbers = Set<Int>()
    for issue in issues where issue.number != parent.number {
        if issueRelationNumbers(issue.body).contains(parent.number) { numbers.insert(issue.number) }
    }
    for match in DartRegex.allMatches(subIssueRow, parent.body) {
        if let number = Int(DartRegex.group(match, 1, parent.body)), number != parent.number {
            numbers.insert(number)
        }
    }
    return numbers.sorted()
}

/// Another issue referencing this one in its description or a live comment.
public struct IssueMention: Equatable, Hashable, Sendable {
    public var sourceNumber: Int
    public var sourceTitle: String
    public var sourceStatus: String
    public var authorName: String
    public var ts: Int

    public init(sourceNumber: Int, sourceTitle: String, sourceStatus: String, authorName: String, ts: Int) {
        self.sourceNumber = sourceNumber
        self.sourceTitle = sourceTitle
        self.sourceStatus = sourceStatus
        self.authorName = authorName
        self.ts = ts
    }
}

/// Mentions of issue `number` across `issues`, one per source issue and
/// author, oldest first. Derived from the signed text, so existing mentions
/// show without rewriting any issue history.
public func issueMentionsOf(_ issues: [Issue], _ number: Int) -> [IssueMention] {
    var mentions: [IssueMention] = []
    for source in issues where source.number != number {
        var edits: [String: String] = [:]
        var deleted = Set<String>()
        for event in source.events {
            if event.type == "edit" && !event.target.isEmpty {
                edits[event.target] = event.body
            } else if event.type == "delete" && !event.target.isEmpty {
                deleted.insert(event.target)
            }
        }
        var credited = Set<String>()
        for event in source.events {
            let opening = event.type == "open"
            if !opening && event.type != "comment" { continue }
            if !event.id.isEmpty && deleted.contains(event.id) { continue }
            let body = edits[event.id] ?? event.body
            if !issueReferenceNumbers(body).contains(number) { continue }
            let who = opening && !source.author.isEmpty ? source.author : event.displayAuthor
            if !credited.insert(who).inserted { continue }
            mentions.append(IssueMention(
                sourceNumber: source.number,
                sourceTitle: source.title,
                sourceStatus: source.status,
                authorName: who,
                ts: event.ts
            ))
        }
    }
    // Swift's sort is stable, so equal timestamps keep their discovery order.
    return mentions.sorted { $0.ts < $1.ts }
}
