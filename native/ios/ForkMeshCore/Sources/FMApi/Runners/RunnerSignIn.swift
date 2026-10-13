import FMBase
import Foundation

public enum RunnerAttentionState: String, Sendable, Hashable {
    case expired
    case expiring
}

public struct RunnerAttentionItem: Equatable, Hashable, Sendable, Identifiable {
    public var runner: String
    public var provider: String
    public var state: RunnerAttentionState
    public var message: String
    public var expiresAtMs: Int64
    public var openLoginId: String
    public var connectPath: String
    public var canConnect: Bool

    public init(
        runner: String, provider: String, state: RunnerAttentionState, message: String = "",
        expiresAtMs: Int64 = 0, openLoginId: String = "", connectPath: String = "", canConnect: Bool = true
    ) {
        self.runner = runner
        self.provider = provider
        self.state = state
        self.message = message
        self.expiresAtMs = expiresAtMs
        self.openLoginId = openLoginId
        self.connectPath = connectPath
        self.canConnect = canConnect
    }

    public var id: String { "\(runner):\(provider)" }

    public var isExpired: Bool { state == .expired }

    public var title: String {
        let short: String = RunnerSignIn.providerShort(provider)
        switch state {
        case .expired: return "\(runner) needs \(short) sign-in"
        case .expiring: return "\(runner): \(short) sign-in expires soon"
        }
    }

    public func detail(formatExpiry: (Int64) -> String = RunnerSignIn.expiryLabel) -> String {
        switch state {
        case .expired:
            return "Jobs for \(RunnerSignIn.providerLong(provider)) are not running on this runner."
        case .expiring:
            let base: String = "Sign in again before it expires."
            if expiresAtMs <= 0 { return base }
            return base + " Expires \(formatExpiry(expiresAtMs))."
        }
    }

    public var buttonLabel: String { openLoginId.isEmpty ? "Sign in" : "Open sign-in" }

    public var request: RunnerSignInRequest {
        RunnerSignInRequest(
            runner: runner, provider: provider, openLoginId: openLoginId,
            connectPath: connectPath.isEmpty ? RunnerSignIn.dashboardPath(runner, provider) : connectPath)
    }
}

public struct RunnerSignInRequest: Equatable, Hashable, Sendable, Identifiable {
    public var runner: String
    public var provider: String
    public var openLoginId: String
    public var connectPath: String

    public init(runner: String, provider: String, openLoginId: String = "", connectPath: String = "") {
        self.runner = runner
        self.provider = provider
        self.openLoginId = openLoginId
        self.connectPath = connectPath.isEmpty ? RunnerSignIn.dashboardPath(runner, provider) : connectPath
    }

    public var id: String { "\(runner):\(provider)" }

    public var title: String { "Sign in \(RunnerSignIn.providerLong(provider)) on \(runner)" }
}

public enum RunnerSignIn {
    public static let providers: [String] = ["claude-code", "codex"]
    public static let maxInputBytes: Int = 4096
    public static let pollSeconds: TimeInterval = 1
    public static let maxTextBytes: Int = 200_000
    public static let trimmedNotice: String = "\r\n[older sign-in output was trimmed]\r\n"

    private static let runnerNamePattern = try! NSRegularExpression(pattern: "^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
    private static let loginIdPattern = try! NSRegularExpression(pattern: "^[0-9a-f]{32}$")

    public static func isRunnerName(_ name: String) -> Bool { DartRegex.matches(runnerNamePattern, name) }

    public static func isLoginId(_ id: String) -> Bool { DartRegex.matches(loginIdPattern, id) }

    public static func isProvider(_ provider: String) -> Bool { providers.contains(provider) }

    public static func providerShort(_ provider: String) -> String { provider == "codex" ? "Codex" : "Claude" }

    public static func providerLong(_ provider: String) -> String { provider == "codex" ? "Codex" : "Claude Code" }

    public static func expiryLabel(_ ms: Int64) -> String {
        let formatter: DateFormatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter.string(from: Date(epochMillis: ms))
    }

    public static func loginStateOpen(_ state: String) -> Bool { state == "pending" || state == "running" }

    public static func stateText(_ state: String) -> String {
        switch state {
        case "pending": return "Waiting for the runner to pick this up. It checks in about every 20 seconds."
        case "running": return "The runner is running the sign-in. Open the link below, approve it, and paste the code."
        case "succeeded":
            return "Signed in. The credential stays on the runner, which adds it to its accounts and makes it the active one."
        case "failed": return "The sign-in did not finish. Read the terminal above, then try again."
        case "expired": return "The sign-in ran out of time (10 minutes). Start it again when you are ready."
        case "cancelled": return "The sign-in was cancelled."
        default: return state
        }
    }

    public static func summaryTitle(_ items: [RunnerAttentionItem]) -> String {
        if items.count == 1 { return items[0].title }
        return "\(items.count) runner sign-ins needed"
    }

    public static func anyExpired(_ items: [RunnerAttentionItem]) -> Bool { items.contains { $0.isExpired } }

    public static func attentionPath() -> String { "/api/runners/attention" }

    public static func runnersPath() -> String { "/api/runners" }

    public static func startPath(_ runner: String) -> String { "/api/runners/\(runner)/login" }

    public static func outputPath(_ runner: String, _ id: String) -> String { "/api/runners/\(runner)/login/\(id)/output" }

    public static func inputPath(_ runner: String, _ id: String) -> String { "/api/runners/\(runner)/login/\(id)/input" }

    public static func cancelPath(_ runner: String, _ id: String) -> String { "/api/runners/\(runner)/login/\(id)/cancel" }

    public static func resizePath(_ runner: String, _ id: String) -> String { "/api/runners/\(runner)/login/\(id)/resize" }

    public static func dashboardPath(_ runner: String, _ provider: String) -> String {
        "/dashboard/runners?runner=\(runner)&provider=\(provider)&connect=1"
    }

    public static func connectPath(_ raw: String) -> String {
        let trimmed: String = raw.dartTrimmed
        guard trimmed.hasPrefix("/dashboard/runners"), !trimmed.hasPrefix("//") else { return "" }
        let rest: Substring = trimmed.dropFirst("/dashboard/runners".count)
        guard rest.isEmpty || rest.hasPrefix("?") || rest.hasPrefix("/?") else { return "" }
        return trimmed
    }

    public static func request(fromHref href: String) -> RunnerSignInRequest? {
        let trimmed: String = href.dartTrimmed
        guard let components = URLComponents(string: trimmed) else { return nil }
        let path: String = components.path.hasSuffix("/") ? String(components.path.dropLast()) : components.path
        guard path == "/dashboard/runners" else { return nil }
        if let host = components.host, !host.isEmpty, components.scheme != "https" { return nil }
        var runner: String = ""
        var provider: String = ""
        for item in components.queryItems ?? [] {
            if item.name == "runner" { runner = (item.value ?? "").dartTrimmed.lowercased() }
            if item.name == "provider" { provider = (item.value ?? "").dartTrimmed.lowercased() }
        }
        guard isRunnerName(runner), isProvider(provider) else { return nil }
        return RunnerSignInRequest(runner: runner, provider: provider)
    }

    public static func isRunnerAlert(_ notification: ForkNotification) -> Bool {
        notification.kind == "operational_alert" && notification.href.contains("/dashboard/runners")
    }

    private static func endpointState(_ raw: String) -> RunnerAttentionState? {
        switch raw.dartTrimmed.lowercased() {
        case "", "ok", "available": return nil
        case "expiring": return .expiring
        default: return .expired
        }
    }

    private static func openLogin(_ runner: JSONObject, _ provider: String) -> String {
        for login in runner.maps("logins") where login.str("provider") == provider {
            let id: String = login.str("id").dartTrimmed
            if loginStateOpen(login.str("state")) && isLoginId(id) { return id }
        }
        return ""
    }

    public static func derive(runner row: JSONObject) -> [RunnerAttentionItem] {
        let name: String = row.str("name").dartTrimmed
        guard isRunnerName(name), row.str("status") == "active" else { return [] }
        let availability: JSONObject = row.obj("availability")
        var out: [RunnerAttentionItem] = []
        for provider in providers {
            guard let entry = availability[provider]?.objectValue, entry.bool("binaryFound") else { continue }
            let loginState: String = entry.str("loginState")
            let auth: JSONObject = entry.obj("auth")
            let authState: String = loginState == "expired" ? "expired" : auth.str("state")
            let state: RunnerAttentionState
            if authState == "expired" {
                state = .expired
            } else if loginState == "available" && authState == "expiring" {
                state = .expiring
            } else {
                continue
            }
            out.append(RunnerAttentionItem(
                runner: name, provider: provider, state: state, message: auth.str("message"),
                expiresAtMs: auth.int64("expiresAt"), openLoginId: openLogin(row, provider)))
        }
        return out
    }

    public static func derive(runners payload: JSONValue) -> [RunnerAttentionItem] {
        normalized(payload.list("runners").compactMap(\.objectValue).flatMap { derive(runner: $0) })
    }

    private static func flatItem(_ entry: JSONObject, runner: String, inherited: JSONObject) -> RunnerAttentionItem? {
        let provider: String = entry.str("provider").dartTrimmed.lowercased()
        guard isRunnerName(runner), isProvider(provider) else { return nil }
        var raw: String = entry.str("state")
        if raw.isEmpty { raw = entry.str("loginState") }
        if raw.isEmpty { raw = entry.obj("auth").str("state") }
        guard let state = endpointState(raw) else { return nil }
        var loginId: String = entry.str("loginId").dartTrimmed
        if loginId.isEmpty { loginId = entry.str("openLoginId").dartTrimmed }
        if loginId.isEmpty { loginId = entry.obj("login").str("id").dartTrimmed }
        if loginId.isEmpty { loginId = openLogin(inherited, provider) }
        var message: String = entry.str("message")
        if message.isEmpty { message = entry.obj("auth").str("message") }
        var expires: Int64 = entry.int64("expiresAt")
        if expires <= 0 { expires = entry.obj("auth").int64("expiresAt") }
        return RunnerAttentionItem(
            runner: runner, provider: provider, state: state, message: message, expiresAtMs: max(0, expires),
            openLoginId: isLoginId(loginId) ? loginId : "", connectPath: connectPath(entry.str("connectUrl")),
            canConnect: entry.optBool("canConnect") ?? true)
    }

    public static func parseAttention(_ payload: JSONValue) -> [RunnerAttentionItem] {
        var list: [JSONValue] = payload.arrayValue ?? []
        if list.isEmpty {
            for key in ["items", "attention", "runners"] {
                if let found = payload[key]?.arrayValue {
                    list = found
                    break
                }
            }
        }
        var out: [RunnerAttentionItem] = []
        for value in list {
            guard let entry = value.objectValue else { continue }
            if entry.contains("availability") {
                out.append(contentsOf: derive(runner: entry))
                continue
            }
            let runner: String = entry.firstStr("runner", "name").dartTrimmed.lowercased()
            if let grouped = entry["providers"]?.arrayValue {
                for provider in grouped {
                    if let object = provider.objectValue, let item = flatItem(object, runner: runner, inherited: entry) {
                        out.append(item)
                    }
                }
                continue
            }
            if let item = flatItem(entry, runner: runner, inherited: entry) { out.append(item) }
        }
        return normalized(out)
    }

    public static func normalized(_ items: [RunnerAttentionItem]) -> [RunnerAttentionItem] {
        var best: [String: RunnerAttentionItem] = [:]
        for item in items {
            guard let existing = best[item.id] else {
                best[item.id] = item
                continue
            }
            if item.isExpired && !existing.isExpired {
                best[item.id] = item
            } else if item.state == existing.state && existing.openLoginId.isEmpty && !item.openLoginId.isEmpty {
                best[item.id] = item
            }
        }
        return best.values.sorted { (a: RunnerAttentionItem, b: RunnerAttentionItem) -> Bool in
            if a.isExpired != b.isExpired { return a.isExpired }
            if a.runner != b.runner { return a.runner < b.runner }
            return a.provider < b.provider
        }
    }

    public static func terminalText(_ raw: String) -> String {
        var text: String = raw
        text = replacing(text, #"\x{1b}\][^\x{07}\x{1b}]*(?:\x{07}|\x{1b}\\)"#, "")
        text = replacing(text, #"\x{1b}\[[0-?]*[ -/]*[@-~]"#, "")
        text = replacing(text, #"\x{1b}[@-Z\\-_]"#, "")
        text = text.replacingOccurrences(of: "\r\n", with: "\n")
        let lines: [String] = text.components(separatedBy: "\n").map { (line: String) -> String in
            let parts: [String] = line.components(separatedBy: "\r")
            let visible: String = parts.reversed().first { !$0.isEmpty } ?? ""
            return String(String.UnicodeScalarView(visible.unicodeScalars.filter { (scalar: Unicode.Scalar) -> Bool in
                let value: UInt32 = scalar.value
                return !((value <= 0x08) || (value >= 0x0b && value <= 0x1f) || value == 0x7f)
            }))
        }
        return lines.joined(separator: "\n")
    }

    private static func replacing(_ text: String, _ pattern: String, _ with: String) -> String {
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return text }
        let range: NSRange = NSRange(text.startIndex..., in: text)
        return regex.stringByReplacingMatches(in: text, range: range, withTemplate: with)
    }

    public static func loginUrls(_ text: String) -> [URL] {
        guard let regex = try? NSRegularExpression(pattern: #"https://[^\s"'<>]+"#) else { return [] }
        let range: NSRange = NSRange(text.startIndex..., in: text)
        var seen: [URL] = []
        for match in regex.matches(in: text, range: range) {
            guard let found = Range(match.range, in: text) else { continue }
            var candidate: String = String(text[found])
            while let last = candidate.last, ".,;)".contains(last) { candidate.removeLast() }
            guard let url = URL(string: candidate), url.scheme == "https", let host = url.host, !host.isEmpty,
                  url.user == nil, url.password == nil else { continue }
            if !seen.contains(url) { seen.append(url) }
        }
        return seen
    }

    public static func inputValid(_ data: String) -> Bool {
        if data.isEmpty || data.utf8.count > maxInputBytes { return false }
        for scalar in data.unicodeScalars {
            if scalar == "\r" || scalar == "\n" || scalar == "\t" || scalar == " " { continue }
            switch scalar.properties.generalCategory {
            case .control, .format, .surrogate, .privateUse, .unassigned, .lineSeparator, .paragraphSeparator,
                 .spaceSeparator:
                return false
            default:
                continue
            }
        }
        return true
    }

    public static func terminalInputValid(_ data: String) -> Bool {
        !data.isEmpty && data.utf8.count <= maxInputBytes && !data.unicodeScalars.contains("\u{0}")
    }

    public static func codeSubmission(_ code: String) -> String {
        let clean: String = code.replacingOccurrences(of: "\r", with: "").replacingOccurrences(of: "\n", with: "")
            .trimmingCharacters(in: .whitespaces)
        return clean.isEmpty ? "" : clean + "\r"
    }
}

public final class RunnerLoginSession {
    public enum Start: Equatable, Sendable {
        case started
        case attached
        case failed
    }

    public let runner: String
    public let provider: String
    public let kind: String
    public private(set) var id: String = ""
    public private(set) var state: String = ""
    public private(set) var message: String = ""
    public private(set) var error: String = ""
    public private(set) var afterSeq: Int64 = 0
    public private(set) var more: Bool = false
    public private(set) var failed: Bool = false
    public private(set) var raw: [UInt8] = []
    public private(set) var exitCode: Int?

    public init(runner: String, provider: String, kind: String = "login") {
        self.runner = runner.dartTrimmed.lowercased()
        self.provider = provider.dartTrimmed.lowercased()
        self.kind = kind
    }

    public var open: Bool { !failed && !id.isEmpty && RunnerSignIn.loginStateOpen(state) }

    public var needsCancelOnClose: Bool { !id.isEmpty && RunnerSignIn.loginStateOpen(state) }

    public var text: String { RunnerSignIn.terminalText(Utf8.decodeLossy(raw)) }

    public var urls: [URL] { RunnerSignIn.loginUrls(text) }

    public var statusText: String {
        if failed { return error }
        let base: String = RunnerSignIn.stateText(state)
        return message.isEmpty ? base : base + " " + message
    }

    private func applyPublic(_ body: JSONObject) {
        let next: String = body.str("state")
        if !next.isEmpty { state = next }
        if body.contains("state") { message = body.str("message") }
        if let code = body["exitCode"]?.intValue { exitCode = code }
    }

    public func attach(_ loginId: String) {
        guard RunnerSignIn.isLoginId(loginId) else { return }
        id = loginId
        state = "pending"
        error = ""
        failed = false
    }

    public func markFailed(_ text: String) {
        failed = true
        error = text
    }

    @discardableResult
    public func applyStartReply(status: Int, body: JSONObject) -> Start {
        let loginId: String = body.str("id").dartTrimmed
        let session: JSONObject = body.obj("session")
        if (status == 200 || status == 201) && RunnerSignIn.isLoginId(loginId) {
            attach(loginId)
            applyPublic(session)
            return .started
        }
        if status == 409 && body.str("error") == "\(kind)_active" && RunnerSignIn.isLoginId(loginId) {
            attach(loginId)
            applyPublic(session)
            return .attached
        }
        var text: String = body.str("message").dartTrimmed
        let code: String = body.str("error").dartTrimmed
        if text.isEmpty {
            switch status {
            case 401: text = "Sign in to your ForkMesh account on this phone first."
            case 403: text = "Only an administrator, or the registrant of a personal runner, may sign its agent CLIs in."
            case 404: text = "That runner is not registered."
            default: text = code.isEmpty ? "The sign-in could not be started." : code
            }
        }
        markFailed(text)
        return .failed
    }

    @discardableResult
    public func applyOutputReply(_ body: JSONObject) -> [UInt8] {
        applyPublic(body)
        var ordered: [(Int64, [UInt8])] = []
        for chunk in body.maps("chunks") {
            let seq: Int64 = chunk.int64("seq")
            if seq <= afterSeq { continue }
            guard let decoded = Data(base64Encoded: chunk.str("data")) else { continue }
            ordered.append((seq, [UInt8](decoded)))
        }
        ordered.sort { $0.0 < $1.0 }
        var out: [UInt8] = []
        if body.bool("truncated") && !ordered.isEmpty { out += Array(RunnerSignIn.trimmedNotice.utf8) }
        for (seq, bytes) in ordered {
            out += bytes
            afterSeq = seq
        }
        let next: Int64 = body.int64("nextSeq")
        if next > afterSeq { afterSeq = next }
        more = body.bool("more")
        raw += out
        if raw.count > RunnerSignIn.maxTextBytes { raw.removeFirst(raw.count - RunnerSignIn.maxTextBytes) }
        return out
    }

    public func applyCancelReply(_ body: JSONObject) {
        applyPublic(body)
        if RunnerSignIn.loginStateOpen(state) { state = "cancelled" }
    }
}
