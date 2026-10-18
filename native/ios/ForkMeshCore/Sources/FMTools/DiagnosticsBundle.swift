import Foundation

public enum DiagnosticsBundle {
    public static let repository: String = "forkmesh/forkmesh"
    public static let playbook: String = "docs/native/PERFORMANCE_CHECKS.md"
    public static let logSections: [String] = ["## App log", "## Busiest log lines", "## stalls.log tail"]

    static let rules: [(NSRegularExpression, String)] = [
        (#"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]+"#, "Bearer [redacted]"),
        (#"(?i)\b(authorization|bearer|token|access_token|refresh_token|secret|password|passwd|api[_-]?key|cookie|session(?:_?id)?|signature|sig)\b(["']?\s*[:=]\s*["']?)[^\s"',;&]+"#, "$1$2[redacted]"),
        (#"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]*"#, "[token]"),
        (#"(?i)"(text|body|message|content|prompt|title|note|subject)"\s*:\s*"(?:[^"\\]|\\.)*""#, #""$1":"[redacted]""#),
        (#"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}"#, "[email]"),
        (#"(https?://[^\s?#"]+)\?[^\s"]*"#, "$1?[query]"),
        (#"\+\d[\d ()-]{8,}\d"#, "[number]"),
        (#"\b[A-Za-z0-9_-]{32,}\b"#, "[id]"),
        (#"/(Users|home|var/mobile/Containers/Data/Application)/[^/\s]+"#, "/$1/~"),
    ].map { (pattern: String, template: String) -> (NSRegularExpression, String) in
        (try! NSRegularExpression(pattern: pattern), template)
    }

    public static func scrub(_ text: String) -> String {
        var out: String = text
        for (expression, template) in rules {
            let range: NSRange = NSRange(out.startIndex..<out.endIndex, in: out)
            out = expression.stringByReplacingMatches(in: out, options: [], range: range, withTemplate: template)
        }
        return out
    }

    public static func dropSections(_ text: String, titles: [String]) -> String {
        var kept: [String] = []
        var skipping: Bool = false
        for line in text.components(separatedBy: "\n") {
            if line.hasPrefix("## ") {
                skipping = titles.contains { (title: String) -> Bool in line.hasPrefix(title) }
            }
            if !skipping { kept.append(line) }
        }
        return kept.joined(separator: "\n")
    }

    public static func lead(reason: String) -> String {
        let why: String = reason.isEmpty ? "The user asked for a performance check" : "The app detected a performance issue: \(reason)"
        return """
            \(why). Diagnose what makes the ForkMesh native iOS app (native/ios in \(repository)) slow on this iPhone, \
            using the diagnostics bundle below. Fix the cause on a branch with tests, and explain what you found. \
            The bundle was scrubbed of tokens, emails and message contents on the phone. \
            First read \(playbook): how to read each section, and what earlier checks fixed and ruled out. \
            Before committing, add this check to it (evidence, cause, fix, ruled out) and correct what it got wrong; \
            if the bundle lacked evidence you needed, make the app collect it.
            """
    }

    public static func prompt(reason: String, report: String, includeLogs: Bool, limit: Int) -> String {
        prompt(lead: lead(reason: reason), report: report, includeLogs: includeLogs, limit: limit)
    }

    public static func prompt(lead: String, report: String, includeLogs: Bool, limit: Int) -> String {
        let body: String = scrub(includeLogs ? report : dropSections(report, titles: logSections))
        let head: String = lead + "\n\n# Diagnostics bundle\n"
        let room: Int = max(0, limit - head.count)
        let clipped: String = body.count <= room ? body : String(body.prefix(max(0, room - 24))) + "\n[cut to fit the prompt]"
        return head + clipped
    }
}
