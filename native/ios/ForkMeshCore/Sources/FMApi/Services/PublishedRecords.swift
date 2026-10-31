// The published record parsers: `issueFromRecordJson`, `issueFromMarkdown`,
// `issueEventFromMarkdown`, `pullFromMarkdown`, `pullEventFromMarkdown`, `extractPatch`,
// `discussionFromMarkdown`, `discussionEventFromMarkdown`, `frontMatter`, `firstHeading`,
// `bodyWithoutFrontMatter`, plus `splitLines` and `trimRight`.
// Pure functions, public so other layers and tests can use them.

import FMBase
import Foundation

public enum PublishedRecords {
    // MARK: Issues

    /// One `.forkmesh/issues/{open,closed}/<n>/issue-<n>.json` signed-event record -> Issue. The
    /// body and display author ride on the "open" event (after its latest signed edit).
    /// Nil when the text is not a JSON object.
    public static func issueFromRecordJson(_ number: String, _ text: String) -> Issue? {
        guard let decoded = (try? JSON.parse(text))?.objectValue else { return nil }
        let events = decoded["events"]?.arrayValue
        var open: JSONObject?
        for event in (events ?? []).compactMap(\.objectValue) where event["type"] == .string("open") {
            open = event
            break
        }
        var openBody = open?["body"]
        if let events, let open, !(open["id"].map { $0.isNull ? "" : $0.text } ?? "").isEmpty {
            for event in events.compactMap(\.objectValue) where event["type"] == .string("edit") && event["target"] == open["id"] {
                openBody = event["body"]
            }
        }
        var merged = decoded
        merged["number"] = ModelJSON.coalesce(decoded["number"]) ?? .int(Int64(DartCore.intTryParse(number) ?? 0))
        merged["title"] = ModelJSON.coalesce(decoded["title"], open?["title"]) ?? .string("Issue #\(number)")
        merged["body"] = ModelJSON.coalesce(decoded["body"], openBody) ?? .string("")
        merged["author"] = ModelJSON.coalesce(decoded["authorName"], open?["authorName"], decoded["author"]) ?? .string("")
        return Issue(json: merged)
    }

    /// A pre-JSON `issues/<n>/issue.md` record.
    public static func issueFromMarkdown(_ number: String, _ md: String, events: [IssueEvent] = []) -> Issue {
        let meta = frontMatter(md)
        func metaList(_ key: String) -> [String] {
            (meta[key] ?? "").replacingOccurrences(of: "[", with: "").replacingOccurrences(of: "]", with: "")
                .components(separatedBy: ",").map(\.dartTrimmed).filter { !$0.isEmpty }
        }
        let title = meta["title"] ?? firstHeading(md) ?? "Issue #\(number)"
        let status = meta["status"] ?? (md.contains("status: closed") ? "closed" : "open")
        return Issue(
            number: DartCore.intTryParse(number) ?? 0,
            title: title,
            status: status,
            body: bodyWithoutFrontMatter(md),
            author: meta["authorName"] ?? meta["author"] ?? "",
            labels: metaList("labels"),
            milestone: meta["milestone"] ?? "",
            priority: DartCore.intTryParse(meta["priority"] ?? "") ?? 0,
            assignees: metaList("assignees"),
            events: events,
            votes: events.filter { $0.type == "vote" }.count,
            bountyUsd: DartCore.doubleTryParse(meta["bountyUsd"] ?? "") ?? 0,
            bountyAddress: meta["bountyAddress"] ?? meta["address"] ?? "",
            bountyStatus: meta["bountyStatus"] ?? "",
            bountyRequiredLamports: DartCore.intTryParse(meta["bountyRequiredLamports"] ?? "") ?? 0,
            bountyReceivedLamports: DartCore.intTryParse(meta["bountyReceivedLamports"] ?? "") ?? 0,
            bountyAmountSol: DartCore.doubleTryParse(meta["bountyAmountSol"] ?? "") ?? 0,
            bountyPayUri: meta["bountyPayUri"] ?? meta["payUri"] ?? meta["uri"] ?? "",
            bountyPayee: meta["bountyPayee"] ?? meta["payee"] ?? "",
            bountyPayoutSig: meta["bountyPayoutSig"] ?? meta["payoutSig"] ?? "")
    }

    public static func issueEventFromMarkdown(_ md: String) -> IssueEvent {
        let meta = frontMatter(md)
        return IssueEvent(
            type: meta["type"] ?? "comment",
            body: bodyWithoutFrontMatter(md),
            author: meta["author"] ?? "",
            authorName: meta["authorName"] ?? "",
            status: meta["status"] ?? "",
            ts: DartCore.intTryParse(meta["ts"] ?? "") ?? 0)
    }

    // MARK: Pulls

    public static func pullFromMarkdown(_ number: String, _ md: String, events: [PullEvent] = []) -> PublishedPull {
        let meta = frontMatter(md)
        let body = bodyWithoutFrontMatter(md)
        return PublishedPull(
            number: DartCore.intTryParse(number) ?? 0,
            title: meta["title"] ?? firstHeading(md) ?? "Pull request #\(number)",
            status: meta["status"] ?? "open",
            body: body,
            base: meta["base"] ?? "",
            head: meta["head"] ?? "",
            branchBacked: meta["derive"] == "branch",
            patch: meta["patch"] ?? meta["diff"] ?? extractPatch(body),
            signed: !(meta["sig"] ?? "").isEmpty,
            events: events,
            author: meta["author"] ?? "",
            authorName: meta["authorName"] ?? "",
            createdMs: DartCore.intTryParse(meta["ts"] ?? meta["createdAt"] ?? "") ?? 0,
            mergedMs: DartCore.intTryParse(meta["mergedAt"] ?? "") ?? 0,
            mergedBy: meta["mergedBy"] ?? "")
    }

    public static func pullHeadFromMarkdown(_ number: String, _ md: String) -> PublishedPull {
        let meta = frontMatter(md)
        return PublishedPull(
            number: DartCore.intTryParse(number) ?? 0,
            title: meta["title"] ?? firstHeading(md) ?? "Pull request #\(number)",
            status: meta["status"] ?? "open",
            base: meta["base"] ?? "",
            head: meta["head"] ?? "",
            branchBacked: meta["derive"] == "branch",
            signed: !(meta["sig"] ?? "").isEmpty,
            author: meta["author"] ?? "",
            authorName: meta["authorName"] ?? "",
            createdMs: DartCore.intTryParse(meta["ts"] ?? meta["createdAt"] ?? "") ?? 0,
            mergedMs: DartCore.intTryParse(meta["mergedAt"] ?? "") ?? 0,
            mergedBy: meta["mergedBy"] ?? "")
    }

    public static func pullEventFromMarkdown(_ md: String) -> PullEvent {
        let meta = frontMatter(md)
        return PullEvent(
            type: meta["type"] ?? "comment",
            body: bodyWithoutFrontMatter(md),
            author: meta["author"] ?? "",
            authorName: meta["authorName"] ?? "",
            state: meta["state"] ?? "",
            ts: DartCore.intTryParse(meta["ts"] ?? "") ?? 0)
    }

    private static let fencedPatch = try! NSRegularExpression(pattern: "```(?:diff|patch)\\s*\\n([\\s\\S]*?)```", options: [.anchorsMatchLines])

    /// The patch in a pull body: a ```diff / ```patch fence, else everything from the first
    /// `diff --git ` or `--- ` line.
    public static func extractPatch(_ body: String) -> String {
        let range = NSRange(body.startIndex..., in: body)
        if let match = fencedPatch.firstMatch(in: body, range: range) {
            guard let group = Range(match.range(at: 1), in: body) else { return "" }
            return trimRight(String(body[group]))
        }
        let lines = splitLines(body)
        guard let start = lines.firstIndex(where: { $0.hasPrefix("diff --git ") || $0.hasPrefix("--- ") }) else { return "" }
        return trimRight(lines[start...].joined(separator: "\n"))
    }

    // MARK: Discussions

    public static func discussionFromMarkdown(_ number: String, _ md: String, events: [DiscussionEvent] = []) -> RepoDiscussion {
        let meta = frontMatter(md)
        return RepoDiscussion(
            number: DartCore.intTryParse(number) ?? 0,
            title: meta["title"] ?? firstHeading(md) ?? "Discussion #\(number)",
            body: bodyWithoutFrontMatter(md),
            author: meta["authorName"] ?? meta["author"] ?? "",
            category: meta["category"] ?? "general",
            updatedMs: DartCore.intTryParse(meta["updatedAt"] ?? meta["ts"] ?? "") ?? 0,
            events: events.filter { $0.type != "answer" },
            authorKey: meta["author"] ?? "",
            answer: events.last { $0.type == "answer" })
    }

    public static func discussionEventFromMarkdown(_ md: String) -> DiscussionEvent {
        let meta = frontMatter(md)
        return DiscussionEvent(
            type: meta["type"] ?? "comment",
            body: bodyWithoutFrontMatter(md),
            author: meta["author"] ?? "",
            authorName: meta["authorName"] ?? "",
            ts: DartCore.intTryParse(meta["ts"] ?? "") ?? 0,
            id: meta["id"] ?? "",
            target: meta["target"] ?? "")
    }

    // MARK: Markdown helpers

    /// `key: value` lines between a leading `---` and the next `---`. Later keys win.
    public static func frontMatter(_ md: String) -> [String: String] {
        var out: [String: String] = [:]
        var first = true
        eachLine(md) { line in
            if first {
                first = false
                return line.dartTrimmed == "---"
            }
            if line.dartTrimmed == "---" { return false }
            if let colon = line.firstIndex(of: ":"), colon > line.startIndex {
                out[String(line[..<colon]).dartTrimmed] = String(line[line.index(after: colon)...]).dartTrimmed
            }
            return true
        }
        return out
    }

    private static let headingPrefix = try! NSRegularExpression(pattern: "^#+\\s*")

    /// The first `#` heading, or the first plain line that is not front matter.
    public static func firstHeading(_ md: String) -> String? {
        var found: String?
        eachLine(md) { line in
            let clean = line.dartTrimmed
            if clean.hasPrefix("#") {
                let range = NSRange(clean.startIndex..., in: clean)
                found = headingPrefix.stringByReplacingMatches(in: clean, range: range, withTemplate: "").dartTrimmed
                return false
            }
            if !clean.isEmpty && clean != "---" && !clean.contains(":") {
                found = clean
                return false
            }
            return true
        }
        return found
    }

    public static func bodyWithoutFrontMatter(_ md: String) -> String {
        let lines = splitLines(md)
        guard let first = lines.first, first.dartTrimmed == "---" else { return md.dartTrimmed }
        guard let end = lines.indices.dropFirst().first(where: { lines[$0].dartTrimmed == "---" }) else {
            return md.dartTrimmed
        }
        return lines[(end + 1)...].joined(separator: "\n").dartTrimmed
    }

    /// Splits at `\n`, `\r\n` and `\r`; a final terminator adds no empty line.
    public static func splitLines(_ text: String) -> [String] {
        var lines: [String] = []
        eachLine(text) { line in
            lines.append(line)
            return true
        }
        return lines
    }

    public static func eachLine(_ text: String, _ visit: (String) -> Bool) {
        let scalars = text.unicodeScalars
        var start = scalars.startIndex
        var index = start
        while index < scalars.endIndex {
            let scalar = scalars[index]
            guard scalar == "\n" || scalar == "\r" else {
                index = scalars.index(after: index)
                continue
            }
            if !visit(String(scalars[start..<index])) { return }
            index = scalars.index(after: index)
            if scalar == "\r", index < scalars.endIndex, scalars[index] == "\n" {
                index = scalars.index(after: index)
            }
            start = index
        }
        if start < scalars.endIndex { _ = visit(String(scalars[start..<scalars.endIndex])) }
    }

    /// Strips trailing white space (`isDartWhitespace`).
    public static func trimRight(_ text: String) -> String {
        var scalars = Array(text.unicodeScalars)
        while let last = scalars.last, isDartWhitespace(last) { scalars.removeLast() }
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars)
        return String(view)
    }

    static func isDartWhitespace(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.value {
        case 0x09...0x0D, 0x20, 0x85, 0xA0, 0x1680, 0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF:
            return true
        default:
            return false
        }
    }
}
