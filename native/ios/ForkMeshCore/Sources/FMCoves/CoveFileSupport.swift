import FMBase
import Foundation

public enum CoveEditorKind: String, Sendable {
    case text, spreadsheet, pipeline
}

public struct CoveFileTemplate: Equatable, Sendable, Identifiable {
    public let id: String
    public let title: String
    public let description: String
    public let defaultPath: String
    public let mime: String
    public let body: String
    public let editor: CoveEditorKind
    public let symbol: String
}

public enum CoveFiles {
    public static let pipelineMime = "application/vnd.forkmesh.pipeline+json"

    public static func isoNow(_ date: Date = Date()) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter.string(from: date)
    }

    public static func defaultPipelineObject(now: Date = Date()) -> JSONObject {
        let stages: [(String, String, String)] = [
            ("new", "New", "#58a6ff"), ("qualified", "Qualified", "#a371f7"), ("proposal", "Proposal", "#d29922"),
            ("won", "Won", "#3fb950"), ("lost", "Lost", "#f85149"),
        ]
        return [
            "createdAt": .string(isoNow(now)),
            "history": .array([]),
            "leads": .array([]),
            "name": .string("Lead pipeline"),
            "stages": .array(stages.map { .object(["color": .string($0.2), "id": .string($0.0), "name": .string($0.1)]) }),
            "version": .int(1),
        ]
    }

    public static func defaultPipelineJson(now: Date = Date()) -> String {
        JSON.encodePretty(.object(defaultPipelineObject(now: now)), indent: "    ") + "\n"
    }

    public static func templates(now: Date = Date()) -> [CoveFileTemplate] {
        [
            CoveFileTemplate(
                id: "blank-note", title: "Blank note", description: "A clean Markdown document for ideas and drafts.",
                defaultPath: "notes.md", mime: "text/markdown", body: "# Notes\n\nStart writing here.\n", editor: .text,
                symbol: "doc.text"),
            CoveFileTemplate(
                id: "spreadsheet", title: "Spreadsheet", description: "An editable CSV grid for lists, tracking, and analysis.",
                defaultPath: "spreadsheet.csv", mime: "text/csv",
                body: "Name,Status,Owner,Due date\r\nFirst item,Not started,,\r\n", editor: .spreadsheet,
                symbol: "tablecells"),
            CoveFileTemplate(
                id: "lead-pipeline", title: "Lead pipeline", description: "Stages, leads, movement history, and value reporting.",
                defaultPath: "leads.pipeline.json", mime: pipelineMime, body: defaultPipelineJson(now: now), editor: .pipeline,
                symbol: "chart.bar.doc.horizontal"),
            CoveFileTemplate(
                id: "project-brief", title: "Project brief",
                description: "Goals, audience, scope, milestones, and success measures.", defaultPath: "project-brief.md",
                mime: "text/markdown",
                body: "# Project brief\n\n## Goal\n\n## Audience\n\n## Scope\n\n### In scope\n\n### Out of scope\n\n## Milestones\n\n## Success measures\n",
                editor: .text, symbol: "doc.richtext"),
            CoveFileTemplate(
                id: "meeting-notes", title: "Meeting notes", description: "Agenda, attendees, decisions, and follow-up actions.",
                defaultPath: "meeting-notes.md", mime: "text/markdown",
                body: "# Meeting notes\n\n**Date:** \n**Attendees:** \n\n## Agenda\n\n## Notes\n\n## Decisions\n\n## Action items\n\n- [ ] Owner \u{2014} next step\n",
                editor: .text, symbol: "person.2"),
            CoveFileTemplate(
                id: "contacts", title: "Contact list", description: "A spreadsheet ready for people and account details.",
                defaultPath: "contacts.csv", mime: "text/csv", body: "Name,Company,Role,Email,Phone,Notes\r\n",
                editor: .spreadsheet, symbol: "person.crop.rectangle.stack"),
            CoveFileTemplate(
                id: "budget", title: "Budget tracker",
                description: "Plan and compare categories, estimates, and actual spend.", defaultPath: "budget.csv",
                mime: "text/csv", body: "Category,Item,Estimated,Actual,Owner,Notes\r\nOperations,Example,0,0,,\r\n",
                editor: .spreadsheet, symbol: "dollarsign.circle"),
            CoveFileTemplate(
                id: "checklist", title: "Checklist", description: "A lightweight Markdown checklist grouped by phase.",
                defaultPath: "checklist.md", mime: "text/markdown",
                body: "# Checklist\n\n## Prepare\n\n- [ ] First task\n\n## Review\n\n- [ ] Final review\n", editor: .text,
                symbol: "checklist"),
            CoveFileTemplate(
                id: "decision-log", title: "Decision log",
                description: "Record context, options, owners, and outcomes over time.", defaultPath: "decision-log.md",
                mime: "text/markdown",
                body: "# Decision log\n\n## Decision title\n\n**Date:** \n**Owner:** \n**Status:** Proposed\n\n### Context\n\n### Options considered\n\n### Decision\n\n### Consequences\n",
                editor: .text, symbol: "arrow.triangle.branch"),
            CoveFileTemplate(
                id: "structured-data", title: "Structured JSON",
                description: "A valid JSON starting point for structured private data.", defaultPath: "data.json",
                mime: "application/json", body: "{\n  \"items\": []\n}\n", editor: .text, symbol: "curlybraces"),
        ]
    }

    public static func template(_ id: String) -> CoveFileTemplate? { templates().first { $0.id == id } }

    public static func editorKind(path: String, mime: String) -> CoveEditorKind {
        let lowerPath = path.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let lowerMime = mime.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if lowerMime == pipelineMime || lowerPath.hasSuffix(".pipeline.json") { return .pipeline }
        if lowerMime == "text/csv" || lowerPath.hasSuffix(".csv") { return .spreadsheet }
        return .text
    }

    public static func uniqueName(_ wanted: String, existing: [String]) -> String {
        let taken = Set(existing.map { CoveStore.cleanPath($0).lowercased() })
        let clean = CoveStore.cleanPath(wanted.trimmingCharacters(in: .whitespacesAndNewlines))
        if !taken.contains(clean.lowercased()) { return clean }
        let lastSlash = clean.lastIndex(of: "/")
        let fileStart = lastSlash.map { clean.index(after: $0) } ?? clean.startIndex
        let file = String(clean[fileStart...])
        let folder = String(clean[..<fileStart])
        let dot = file.firstIndex(of: ".").map { file.distance(from: file.startIndex, to: $0) } ?? file.count
        let stem = String(file.prefix(dot))
        let ext = String(file.dropFirst(dot))
        var counter = 2
        while true {
            let candidate = "\(folder)\(stem)-\(counter)\(ext)"
            if !taken.contains(candidate.lowercased()) { return candidate }
            counter += 1
        }
    }

    public static func parseCsv(_ csv: String) -> [[String]] {
        var rows: [[String]] = []
        var row: [String] = []
        var field = ""
        var quoted = false
        let chars = Array(csv.unicodeScalars)
        var i = 0
        while i < chars.count {
            let ch = chars[i]
            if quoted {
                if ch == "\"" {
                    if i + 1 < chars.count && chars[i + 1] == "\"" {
                        field.unicodeScalars.append("\"")
                        i += 1
                    } else {
                        quoted = false
                    }
                } else {
                    field.unicodeScalars.append(ch)
                }
                i += 1
                continue
            }
            if ch == "\"" && field.isEmpty {
                quoted = true
            } else if ch == "," {
                row.append(field)
                field = ""
            } else if ch == "\r" || ch == "\n" {
                row.append(field)
                field = ""
                rows.append(row)
                row = []
                if ch == "\r" && i + 1 < chars.count && chars[i + 1] == "\n" { i += 1 }
            } else {
                field.unicodeScalars.append(ch)
            }
            i += 1
        }
        let last = chars.last
        let endedWithNewline = last == "\n" || last == "\r"
        if !endedWithNewline || !row.isEmpty || !field.isEmpty {
            row.append(field)
            rows.append(row)
        }
        if chars.isEmpty || rows.isEmpty { rows = [[""]] }
        return rows
    }

    public static func serializeCsv(_ rows: [[String]]) -> String {
        rows.map { row in
            row.map { field -> String in
                let needsQuotes = field.contains(",") || field.contains("\"") || field.unicodeScalars.contains("\r")
                    || field.unicodeScalars.contains("\n")
                if !needsQuotes { return field }
                return "\"" + field.replacingOccurrences(of: "\"", with: "\"\"") + "\""
            }.joined(separator: ",")
        }.joined(separator: "\r\n") + "\r\n"
    }

    public static func shareUrl(owner: String, repository: String, coveId: String) -> String {
        let parts = [owner, repository, coveId].map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        if parts.contains(where: \.isEmpty) { return "" }
        var allowed = CharacterSet.urlPathAllowed
        allowed.remove(charactersIn: "/?#")
        let encoded = parts.map { $0.addingPercentEncoding(withAllowedCharacters: allowed) ?? $0 }
        return "forkmesh://cove/" + encoded.joined(separator: "/")
    }

    public static func parseShareUrl(_ raw: String) -> (owner: String, repository: String, coveId: String)? {
        guard let components = URLComponents(string: raw.trimmingCharacters(in: .whitespacesAndNewlines)),
            components.scheme == "forkmesh", components.host == "cove"
        else { return nil }
        let parts = components.path.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
        guard parts.count == 3 else { return nil }
        let trimmed = parts.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        if trimmed.contains(where: \.isEmpty) { return nil }
        return (parts[0], parts[1], parts[2])
    }

    public static func routeId(owner: String, repository: String, coveId: String) -> String {
        "\(owner)/\(repository)/\(coveId)"
    }

    public static func parseRouteId(_ id: String) -> (owner: String, repository: String, coveId: String)? {
        if let link = parseShareUrl(id) { return link }
        let parts = id.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 3, !parts.contains(where: \.isEmpty) else { return nil }
        return (parts[0], parts[1], parts[2])
    }
}
