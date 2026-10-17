// The beat half of the motion table: `WatchBeat`, `motionForTool`,
// `motionForCommand`, `beatsFromTranscript`, `plainProse` and `newBeats`. The
// on-device run's `beatsFromAgentEvents` needs FMAgents' event types, so it
// lives in FMAgents/AgentWatchTargets.swift.
//
// Lengths are counted in UTF-16 code units, so a beat's bytes (and so its
// note) are the same on every platform.

import FMBase
import Foundation

/// One thing an agent did, in the order it did it.
public struct WatchBeat: Hashable, Sendable, CustomStringConvertible {
    public let motion: AgentMotion

    /// What it was, in a few words: "Bash git status", "Edit main.dart". Shown
    /// on the list of sounds and on a tapped agent's card.
    public let text: String

    /// The whole reply, for a paragraph of prose: what the agent's display
    /// under its body scrolls.
    public let said: String

    private let explicitBytes: Int?

    public init(_ motion: AgentMotion, _ text: String, bytes: Int? = nil, said: String = "") {
        self.motion = motion
        self.text = text
        self.explicitBytes = bytes
        self.said = said
    }

    /// Roughly how much text it carried (the desktop's `payloadBytes`): louder
    /// notes and bigger marks for more data moved.
    public var bytes: Int { min(explicitBytes ?? text.utf16.count, kPayloadCap) }

    /// A stable hash of what it was about, never 0 (the desktop's
    /// `eventSeed`): the same command always sounds and looks the same, and no
    /// two different ones quite do.
    public var seed: Int {
        let hash = fnv1a("\(motion.name)|\(text)")
        return hash == 0 ? 1 : hash
    }

    // Two beats are the same beat when they are the same kind of work on the
    // same thing: that is how `newBeats` finds its place.
    public static func == (lhs: WatchBeat, rhs: WatchBeat) -> Bool {
        lhs.motion == rhs.motion && lhs.text == rhs.text
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(motion)
        hasher.combine(text)
    }

    public var description: String { "WatchBeat(\(motion.name), \(text))" }
}

// MARK: - Tools and commands

/// The kind of work a tool call is, from its name and, for a shell, from the
/// command it runs. Mirrors the desktop's `motionForTool`, order and all:
/// several checks exist only to win against a later one ("TodoWrite" is a
/// todo, not a write).
public func motionForTool(_ rawName: String, command: String = "") -> AgentMotion {
    let name = rawName.lowercased()
    func has(_ needle: String) -> Bool { name.contains(needle) }
    if name.hasPrefix("mcp__") { return .mcp }
    if has("askuser") || name.hasPrefix("ask") { return .ask }
    if has("skill") { return .skill }
    if has("todo") || name == "taskcreate" || name == "taskupdate" || name == "tasklist" || name == "taskget" {
        return .todo
    }
    if has("plan") { return .plan }
    if has("monitor") || has("bashoutput") || has("taskoutput") || has("sleep") || has("wait")
        || has("killshell") || has("taskstop") {
        return .wait
    }
    if has("web") || has("browser") || has("url") { return .web }
    if has("task") || has("agent") || has("workflow") { return .delegate }
    if has("image") || has("screenshot") { return .image }
    // The phone's own agent has git_status and git_commit tools.
    if has("git_") { return .git }
    if has("glob") || name == "ls" || has("list_dir") || has("listdir") || has("list_files") || has("tree") {
        return .scan
    }
    if has("grep") || has("search") || has("find") { return .search }
    if has("edit") || has("patch") || has("apply") || has("notebook") || has("replace") { return .write }
    if has("write") || has("create") { return .create }
    if has("read") || has("fetch") || has("view") || has("list") { return .read }
    return motionForCommand(command)
}

private let testCommands = [
    "pytest", "ctest", "cargo test", "go test", "npm test", "npm run test", "yarn test", "pnpm test",
    "flutter test", "dart test", "jest", "vitest", "playwright", "gradlew test", "-tests", "unittest",
]
private let installCommands = [
    "npm install", "npm i ", "npm ci", "pip install", "pip3 install", "uv add", "uv pip", "uv sync",
    "apt install", "apt-get install", "brew install", "cargo add", "cargo install", "yarn add",
    "yarn install", "pnpm add", "pnpm install", "pub get", "go get", "gem install", "poetry add",
    "poetry install",
]
private let buildCommands = [
    "ninja", "cmake", "make ", "cargo build", "go build", "npm run build", "gradlew", "flutter build",
    "xcodebuild", "webpack", "vite build", "g++ ", "gcc ", "clang",
]

// Built once; the patterns are constants.
// swiftlint:disable force_try
private let gitCommand = try! NSRegularExpression(pattern: #"(^|[;&|(]\s*)(git|gh)\s"#)
private let sleepCommand = try! NSRegularExpression(pattern: #"(^|[;&|]\s*)sleep\s"#)
private let tscCommand = try! NSRegularExpression(pattern: #"\btsc\b"#)
// swiftlint:enable force_try

private func matches(_ regex: NSRegularExpression, _ text: String) -> Bool {
    regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
}

/// A shell command's kind: tests, installs, builds and git each have their
/// own sound; anything else is a plain command.
public func motionForCommand(_ command: String, background: Bool = false) -> AgentMotion {
    if background { return .spawn }
    let c = command.lowercased()
    if testCommands.contains(where: { c.contains($0) }) { return .test }
    if installCommands.contains(where: { c.contains($0) }) { return .install }
    if buildCommands.contains(where: { c.contains($0) }) || dartTrim(c) == "make" || matches(tscCommand, c) {
        return .build
    }
    let trimmed = dartTrim(c)
    if matches(gitCommand, trimmed) { return .git }
    if matches(sleepCommand, trimmed) { return .wait }
    return .run
}

// MARK: - Transcripts

/// The beats in a desktop agent transcript (the `transcript.txt` a desktop
/// pushes to the relay and serves over the local network).
///
/// The format is line based: `==> ` status lines, `> ` prompts, `[Tool args]`
/// calls (a long call spans lines until one ends in `]`), and prose between
/// them. Each call is a beat; each paragraph of prose is one.
///
/// Only whole lines are read, and a beat is decided by the line it starts on,
/// so the beats of a transcript are a prefix of the beats of the same
/// transcript read again later with more appended, which is how a watcher
/// knows which beats are new.
public func beatsFromTranscript(_ transcript: String) -> [WatchBeat] {
    var beats: [BeatDraft] = []
    var text = transcript.replacingOccurrences(of: "\r\n", with: "\n")
    // A line still being written is left for the next read.
    if !text.isEmpty && !text.hasSuffix("\n") {
        if let cut = text.range(of: "\n", options: .backwards) {
            text = String(text[..<cut.upperBound])
        } else {
            text = ""
        }
    }
    var inTool = false
    var inPrompt = false
    var inProse = false
    var sawStart = false
    for line in text.components(separatedBy: "\n") {
        let length = line.utf16.count
        if inTool {
            beats[beats.count - 1].bytes += length + 1
            if dartTrimRight(line).hasSuffix("]") { inTool = false }
            continue
        }
        if dartTrim(line).isEmpty {
            inProse = false
            inPrompt = false
            continue
        }
        if line.hasPrefix("==>") {
            inProse = false
            inPrompt = false
            let body = dartTrim(String(line.dropFirst(3)))
            let lower = body.lowercased()
            let motion: AgentMotion
            if lower.contains("fail") || lower.contains("error") || lower.contains("refused")
                || lower.contains("denied") {
                motion = .notice
            } else if lower.contains("stop requested") || lower.hasPrefix("stopping") {
                motion = .notice
            } else if !sawStart && (lower.hasPrefix("started") || lower.hasPrefix("running")) {
                sawStart = true
                motion = .start
            } else if lower.contains("compact") {
                motion = .compact
            } else if lower.contains("retry") || lower.contains("retrying") {
                motion = .retry
            } else if lower.hasPrefix("hook") {
                motion = .hook
            } else {
                motion = .system
            }
            beats.append(BeatDraft(motion, clip(body), body.utf16.count))
            continue
        }
        if line.hasPrefix("> ") {
            inProse = false
            if !inPrompt {
                beats.append(BeatDraft(.prompt, clip(String(line.dropFirst(2))), length))
            } else {
                beats[beats.count - 1].bytes += length + 1
            }
            inPrompt = true
            continue
        }
        if let name = toolName(line) {
            inProse = false
            inPrompt = false
            var args = dartTrim(String(line.dropFirst(1 + name.count)))
            let closed = dartTrimRight(line).hasSuffix("]")
            if closed && args.hasSuffix("]") { args = String(args.dropLast()) }
            if !closed { inTool = true }
            let background = args.contains(#""run_in_background":true"#)
            let motion: AgentMotion = name.lowercased() == "bash" && background
                ? .spawn : motionForTool(name, command: args)
            beats.append(BeatDraft(motion, clip("\(name) \(args)"), length))
            continue
        }
        if inPrompt {
            beats[beats.count - 1].bytes += length + 1
            continue
        }
        if !inProse {
            let lower = line.lowercased()
            let motion: AgentMotion = lower.hasPrefix("thinking") || lower.hasPrefix("(thinking") ? .think : .speak
            beats.append(BeatDraft(motion, clip(line), 0))
            inProse = true
        }
        let last = beats.count - 1
        beats[last].bytes += length + 1
        beats[last].said += beats[last].said.isEmpty ? line : "\n\(line)"
    }
    return beats.map { $0.build() }
}

/// A beat still being read: a tool call's arguments and a paragraph's lines
/// add to it until the next beat starts.
private struct BeatDraft {
    let motion: AgentMotion
    let text: String
    var bytes: Int
    var said = ""

    init(_ motion: AgentMotion, _ text: String, _ bytes: Int) {
        self.motion = motion
        self.text = text
        self.bytes = bytes
    }

    func build() -> WatchBeat {
        WatchBeat(motion, text, bytes: bytes, said: motion == .speak ? plainProse(said) : "")
    }
}

/// `^\[([A-Za-z][A-Za-z0-9_\-]*)(?:\s|\]|$)`: "[Bash git status]",
/// "[mcp__forkmesh__search_issues {...}]", "[Read]".
private func toolName(_ line: String) -> String? {
    let units = Array(line.unicodeScalars)
    guard units.count >= 2, units[0] == "[", units[1].isASCIILetter else { return nil }
    var end = 2
    while end < units.count, units[end].isASCIILetter || units[end].isASCIIDigit
        || units[end] == "_" || units[end] == "-" {
        end += 1
    }
    if end < units.count {
        let next = units[end]
        guard next == "]" || next.properties.isWhitespace else { return nil }
    }
    var name = String.UnicodeScalarView()
    name.append(contentsOf: units[1..<end])
    return String(name)
}

extension Unicode.Scalar {
    fileprivate var isASCIILetter: Bool { (value >= 65 && value <= 90) || (value >= 97 && value <= 122) }
    fileprivate var isASCIIDigit: Bool { value >= 48 && value <= 57 }
}

/// Whitespace runs collapsed to one space, trimmed, and cut to `limit` UTF-16
/// units with an ellipsis.
func clip(_ text: String, _ limit: Int = 90) -> String {
    let line = collapseWhitespace(text)
    let units = Array(line.utf16)
    if units.count <= limit { return line }
    return String(decoding: units[0..<(limit - 1)], as: UTF16.self) + "\u{2026}"
}

/// Collapses whitespace runs to one space and trims.
func collapseWhitespace(_ text: String) -> String {
    var out = String.UnicodeScalarView()
    var pendingSpace = false
    for scalar in text.unicodeScalars {
        if scalar.properties.isWhitespace {
            pendingSpace = !out.isEmpty
        } else {
            if pendingSpace { out.append(" ") }
            pendingSpace = false
            out.append(scalar)
        }
    }
    return String(out)
}

/// Whitespace and line breaks trimmed from both ends.
func dartTrim(_ text: String) -> String {
    text.trimmingCharacters(in: .whitespacesAndNewlines)
}

/// Whitespace trimmed from the end.
func dartTrimRight(_ text: String) -> String {
    var scalars = Substring(text).unicodeScalars
    while let last = scalars.last, last.properties.isWhitespace { scalars.removeLast() }
    return String(scalars)
}

/// Prose as one plain line with its markdown taken off: the desktop's
/// `plainProse`, what an agent's display shows.
public func plainProse(_ text: String, limit: Int = 240) -> String {
    var kept: [String] = []
    var fenced = false
    for raw in text.components(separatedBy: "\n") {
        var line = dartTrim(raw)
        // A code block is not prose: the words round it are.
        if line.hasPrefix("```") {
            fenced.toggle()
            continue
        }
        if fenced || line.isEmpty { continue }
        while line.hasPrefix("#") || line.hasPrefix(">") {
            line = dartTrim(String(line.dropFirst()))
        }
        if line.hasPrefix("- ") || line.hasPrefix("* ") {
            line = String(line.dropFirst(2))
        }
        kept.append(line)
    }
    var plain = kept.joined(separator: " ")
    for mark in ["**", "__", "`"] {
        plain = plain.replacingOccurrences(of: mark, with: "")
    }
    plain = collapseWhitespace(plain)
    let units = Array(plain.utf16)
    if units.count > limit {
        plain = dartTrim(String(decoding: units[0..<(limit - 1)], as: UTF16.self)) + "\u{2026}"
    }
    return plain
}

/// The beats in `next` that `previous` had not seen, when both are readings
/// of the same growing log. Usually a plain suffix; when the log was trimmed
/// from the front in between (the local network serves only its tail), the
/// last beat already heard is looked for and everything after it is new. When
/// it cannot be found at all, only the last few are taken, rather than
/// replaying a whole run as if it had just happened.
public func newBeats(_ previous: [WatchBeat], _ next: [WatchBeat], fallback: Int = 4) -> [WatchBeat] {
    guard let last = previous.last else { return next }
    if next.count >= previous.count && next[previous.count - 1] == last {
        return Array(next[previous.count...])
    }
    var i = next.count - 1
    while i >= 0 {
        if next[i] == last { return Array(next[(i + 1)...]) }
        i -= 1
    }
    return next.count <= fallback ? next : Array(next[(next.count - fallback)...])
}
