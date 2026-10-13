import FMDiff
import Foundation

public enum DiffLineFlag: String, CaseIterable, Sendable {
    case secret, tlsBypass, unsafeApi, permissive, insecureUrl, conflictMarker
    case todo, debug, broadCatch, longLine, trailingWhitespace, deepNesting, request

    public var label: String {
        switch self {
        case .secret: return "secret"
        case .tlsBypass: return "TLS bypass"
        case .unsafeApi: return "unsafe call"
        case .permissive: return "permissive"
        case .insecureUrl: return "plain HTTP"
        case .conflictMarker: return "conflict marker"
        case .todo: return "TODO"
        case .debug: return "debug"
        case .broadCatch: return "broad catch"
        case .longLine: return "long line"
        case .trailingWhitespace: return "trailing space"
        case .deepNesting: return "deep nesting"
        case .request: return "network call"
        }
    }

    public var severe: Bool {
        switch self {
        case .secret, .tlsBypass, .unsafeApi, .permissive, .insecureUrl, .conflictMarker: return true
        default: return false
        }
    }

    public var metric: String {
        switch self {
        case .secret, .tlsBypass, .unsafeApi, .permissive: return "security"
        case .insecureUrl, .request: return "network"
        case .conflictMarker, .trailingWhitespace: return "practices"
        case .todo, .debug, .broadCatch, .longLine: return "quality"
        case .deepNesting: return "complexity"
        }
    }
}

public struct DiffLineMark: Equatable, Hashable, Sendable {
    public let metric: String
    public let severe: Bool

    public init(metric: String, severe: Bool) {
        self.metric = metric
        self.severe = severe
    }

    public static let metricOrder = ["merge", "complexity", "quality", "security", "network", "practices"]

    public static func of(_ flags: [DiffLineFlag]) -> [DiffLineMark] {
        let marks = metricOrder.compactMap { (key: String) -> DiffLineMark? in
            let hits = flags.filter { $0.metric == key }
            return hits.isEmpty ? nil : DiffLineMark(metric: key, severe: hits.contains(where: \.severe))
        }
        return marks.filter(\.severe) + marks.filter { !$0.severe }
    }
}

public enum DiffHealthTone: String, Sendable {
    case green, orange, red

    public init(score: Int) {
        self = score >= 85 ? .green : (score >= 60 ? .orange : .red)
    }
}

public struct DiffHealthMetric: Equatable, Sendable, Identifiable {
    public let key: String
    public let label: String
    public let score: Int
    public let detail: String

    public var id: String { key }
    public var tone: DiffHealthTone { DiffHealthTone(score: score) }
    public var pill: String { "\(label) \(score)" }
    public var explanation: String {
        "\(label): \(score)/100 · \(tone.rawValue)\n\(detail)\n\nGreen 85–100 · orange 60–84 · red 0–59"
    }
    public var accessibility: String { "\(label) health score: \(score) out of 100, \(tone.rawValue)" }
}

public enum DiffHealthGit: Equatable, Sendable {
    case unknown
    case merged
    case state(behind: Int, ahead: Int, mergeConflict: Bool, worktreeConflict: Bool)
}

public struct DiffHealthLineStats: Equatable, Sendable {
    public var controlFlowLines = 0
    public var nestedControlFlowLines = 0
    public var longLines = 0
    public var todoLines = 0
    public var debugLines = 0
    public var broadCatchLines = 0
    public var unsafeApiLines = 0
    public var transportBypassLines = 0
    public var permissiveSecurityLines = 0
    public var requestLines = 0
    public var hardcodedUrls = 0
    public var insecureUrls = 0
    public var conflictMarkerLines = 0
    public var trailingWhitespaceLines = 0
    public var secretMatches = 0
    public var resilienceEvidence = false

    public init() {}
}

public enum DiffHealth {
    static let codeExtensions: Set<String> = [
        "c", "cc", "cpp", "cxx", "h", "hpp", "cs", "java", "kt", "go", "rs", "py", "rb", "js", "jsx", "ts", "tsx",
        "mjs", "cjs", "php", "swift", "scala", "sh", "bash", "sql", "dart", "ex", "exs", "vue", "svelte",
    ]
    static let manifests: Set<String> = [
        "package.json", "pyproject.toml", "requirements.txt", "cargo.toml", "go.mod", "pom.xml", "build.gradle",
        "build.gradle.kts", "gemfile", "composer.json", "pubspec.yaml", "mix.exs",
    ]
    static let lockfiles: Set<String> = [
        "package-lock.json", "pnpm-lock.yaml", "yarn.lock", "cargo.lock", "go.sum", "gemfile.lock", "composer.lock",
        "pubspec.lock", "pipfile.lock", "poetry.lock",
    ]
    static let oddExtensions: Set<String> = ["exe", "dll", "dylib", "so", "a", "o", "db", "sqlite", "zip", "tar", "gz", "7z"]

    private static func regex(_ pattern: String, caseless: Bool = true) -> NSRegularExpression {
        try! NSRegularExpression(pattern: pattern, options: caseless ? [.caseInsensitive] : [])
    }

    static let controlFlow = regex(#"\b(?:if|elif|else\s+if|for|foreach|while|switch|case|catch|except|match|when)\b|&&|\|\|"#)
    static let todoMarker = regex(#"\b(?:TODO|FIXME|HACK|XXX)\b"#, caseless: false)
    static let debugCall = regex(#"\b(?:console\.(?:log|debug|trace)|debugger\s*;|qDebug\s*\(|System\.out\.print|var_dump\s*\(|dd\s*\()"#)
    static let broadCatch = regex(#"(?:except\s*(?::|Exception\b)|catch\s*\([^)]*(?:Exception|Throwable|\.\.\.)[^)]*\))"#)
    static let unsafeApi = regex(#"\b(?:eval|Function)\s*\(|\bexec\s*\(|\bsystem\s*\(|child_process\s*\.\s*exec|shell\s*=\s*True|dangerouslySetInnerHTML|\.innerHTML\s*="#)
    static let transportBypass = regex(#"verify\s*=\s*(?:false|0)|rejectUnauthorized\s*:\s*false|NODE_TLS_REJECT_UNAUTHORIZED\s*=\s*['"]?0|CURLOPT_SSL_VERIFYPEER\s*,\s*(?:0|false)|danger_accept_invalid_certs\s*\(\s*true"#)
    static let permissiveSecurity = regex(#"Access-Control-Allow-Origin[^\n]*['"]\*['"]|\bcors\s*\(\s*\)|\bchmod\s+(?:-R\s+)?777\b"#)
    static let requestSite = regex(#"\bfetch\s*\(|\baxios\s*(?:\.|\[)|\brequests\s*\.\s*(?:get|post|put|patch|delete|request)\s*\(|\b(?:http|https)\s*\.\s*(?:get|request)\s*\(|\bQNetwork(?:Request|AccessManager)\b|\bURLSession\b|\bWebSocket\s*\(|\bcurl_easy_perform\b|\bHttpClient\b"#)
    static let resilience = regex(#"\b(?:timeout|AbortController|CancellationToken|retry|retries|backoff|cancell?ation)\b"#)
    static let url = regex(#"https?://[^\s'"<>]+"#)

    static let secretPatterns: [(name: String, regex: NSRegularExpression)] = [
        ("GitHub token", regex(#"\bgh[pousr]_[A-Za-z0-9_]{36,}\b"#, caseless: false)),
        ("GitHub fine-grained PAT", regex(#"\bgithub_pat_[A-Za-z0-9_]{22,}\b"#, caseless: false)),
        ("AWS access key", regex(#"\bAKIA[0-9A-Z]{16,}\b"#, caseless: false)),
        ("AWS temporary access key", regex(#"\bASIA[0-9A-Z]{16,}\b"#, caseless: false)),
        ("Slack token", regex(#"\bxox[baprs]-[A-Za-z0-9-]{20,}\b"#, caseless: false)),
        ("OpenAI API key", regex(#"\bsk-proj-[A-Za-z0-9_-]{20,}\b|\bsk-[A-Za-z0-9]{48,}\b"#, caseless: false)),
        ("Anthropic API key", regex(#"\bsk-ant-[A-Za-z0-9_-]{20,}\b"#, caseless: false)),
        ("Stripe secret key", regex(#"\bsk_(live|test)_[A-Za-z0-9]{24,}\b"#, caseless: false)),
        ("Stripe restricted key", regex(#"\brk_(live|test)_[A-Za-z0-9]{24,}\b"#, caseless: false)),
        ("Google API key", regex(#"\bAIza[A-Za-z0-9_-]{35,}\b"#, caseless: false)),
        ("Google OAuth token", regex(#"\bya29\.[A-Za-z0-9_-]{20,}\b"#, caseless: false)),
        ("Google OAuth client secret", regex(#"\bGOCSPX-[A-Za-z0-9_-]{28,}\b"#, caseless: false)),
        ("SendGrid API key", regex(#"\bSG\.[A-Za-z0-9_-]{22,}\.[A-Za-z0-9_-]{43,}\b"#, caseless: false)),
        ("Twilio auth token", regex(#"\bSK[a-f0-9]{32,}\b"#, caseless: false)),
        ("npm access token", regex(#"\bnpm_[A-Za-z0-9]{36,}\b"#, caseless: false)),
        ("HashiCorp Vault token", regex(#"\bhv[sbr]\.[A-Za-z0-9]{24,}\b"#, caseless: false)),
        ("Cloudflare API token", regex(#"(?:CLOUDFLARE_API_TOKEN|CF_API_TOKEN)\s*=\s*['"]?([A-Za-z0-9_-]{40,})['"]?"#, caseless: false)),
        ("PEM private key", regex(#"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |)PRIVATE KEY(?:-----| BLOCK-----)"#, caseless: false)),
        ("Secret/token assignment", regex(#"(?:password|passwd|api[_\-]?(?:key|secret|token)|auth[_\-]?token|secret[_\-]?key|access[_\-]?token|private[_\-]?key)\s*[:=]\s*['"]([^'"\s]{20,})['"]"#)),
    ]
    static let secretFragments: [String] = [
        "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "akia", "asia", "xoxb-", "xoxa-", "xoxp-", "xoxr-",
        "xoxs-", "sk-proj-", "sk-", "sk_live_", "sk_test_", "rk_live_", "rk_test_", "aiza", "ya29.", "gocspx-", "sg.",
        "npm_", "hvs.", "hvb.", "hvr.", "cloudflare_api_token", "cf_api_token", "-----begin ", "password", "passwd",
        "api_key", "api-key", "apikey", "api_secret", "api-secret", "apisecret", "api_token", "api-token", "apitoken",
        "auth_token", "auth-token", "authtoken", "secret_key", "secret-key", "secretkey", "access_token",
        "access-token", "accesstoken", "private_key", "private-key", "privatekey",
    ]
    static let placeholderMarkers: [String] = [
        "correct-horse-battery-staple", "changeme", "change-me", "change_me", "placeholder", "example", "sample",
        "dummy", "redacted", "notreal", "not-a-real", "not_a_real", "fake", "your-", "your_", "test-password",
        "test_password", "testpassword", "${", "{{", "<",
    ]
    static let indirection = regex(#"^(?:\$\w+|\$\{[^}]*\}|\$\([^)]*\)|%\w+%)$"#, caseless: false)
    static let ignoreMarker = "forkmesh-secret-scan:ignore-line"

    static func fileName(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return path }
        return String(path[path.index(after: slash)...])
    }

    static func suffix(_ path: String) -> String {
        let name = fileName(path)
        guard let dot = name.lastIndex(of: ".") else { return "" }
        return String(name[name.index(after: dot)...]).lowercased()
    }

    public static func isCode(_ path: String) -> Bool { codeExtensions.contains(suffix(path)) }

    public static func isTest(_ path: String) -> Bool {
        let lower = path.lowercased()
        if lower.hasPrefix("test/") || lower.hasPrefix("tests/") || lower.contains("/test/") || lower.contains("/tests/")
            || lower.contains("/__tests__/") || lower.contains("/spec/") {
            return true
        }
        let base = fileName(lower)
        return base.hasPrefix("test_") || base.contains("_test.") || base.contains(".test.") || base.contains(".spec.")
            || base.contains("_spec.")
    }

    public static func isGenerated(_ path: String) -> Bool {
        let lower = "/" + path.lowercased()
        return lower.contains("/node_modules/") || lower.contains("/vendor/") || lower.contains("/dist/")
            || lower.contains("/generated/") || lower.contains("/build/") || lower.hasSuffix(".min.js")
            || lower.hasSuffix(".min.css") || lower.contains(".generated.")
    }

    static func isSensitive(_ file: DiffFile) -> Bool {
        guard file.status == .added else { return false }
        let base = fileName(file.path.lowercased())
        let ext = suffix(file.path)
        return base == ".env" || base == "id_rsa" || base == "id_ed25519" || ["pem", "p12", "pfx", "key"].contains(ext)
    }

    static func isOddArtifact(_ file: DiffFile) -> Bool {
        file.status == .added && oddExtensions.contains(suffix(file.path))
    }

    static func matches(_ regex: NSRegularExpression, _ text: String) -> Bool {
        regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
    }

    static func mayContainSecret(_ text: String) -> Bool {
        if text.contains("SK") { return true }
        let lower = text.lowercased()
        return secretFragments.contains { lower.contains($0) }
    }

    static func looksLikePlaceholder(_ value: String) -> Bool {
        let lower = value.lowercased()
        if placeholderMarkers.contains(where: { lower.contains($0) }) { return true }
        if matches(indirection, value) { return true }
        guard let first = lower.first else { return false }
        return lower.allSatisfy { $0 == first }
    }

    public static func secretMatches(_ text: String) -> [String] {
        if text.contains(ignoreMarker) || !mayContainSecret(text) { return [] }
        var found: [String] = []
        let range = NSRange(text.startIndex..., in: text)
        for pattern in secretPatterns {
            for match in pattern.regex.matches(in: text, range: range) {
                if pattern.name == "Secret/token assignment", match.numberOfRanges > 1,
                   let value = Range(match.range(at: 1), in: text), looksLikePlaceholder(String(text[value])) {
                    continue
                }
                found.append(pattern.name)
            }
        }
        return found
    }

    public static func urls(_ text: String) -> (all: Int, insecure: Int) {
        var all = 0
        var insecure = 0
        for match in url.matches(in: text, range: NSRange(text.startIndex..., in: text)) {
            guard let range = Range(match.range, in: text) else { continue }
            let found = text[range].lowercased()
            all += 1
            if !found.hasPrefix("http://") { continue }
            if found.hasPrefix("http://localhost") || found.hasPrefix("http://127.0.0.1") || found.hasPrefix("http://0.0.0.0")
                || found.hasPrefix("http://[::1]") || found.contains("example.") || found.contains(".invalid") {
                continue
            }
            insecure += 1
        }
        return (all, insecure)
    }

    public static func flags(path: String, text added: String) -> [DiffLineFlag] {
        var out: [DiffLineFlag] = []
        if !secretMatches(added).isEmpty { out.append(.secret) }
        if matches(transportBypass, added) { out.append(.tlsBypass) }
        if matches(unsafeApi, added) { out.append(.unsafeApi) }
        if matches(permissiveSecurity, added) { out.append(.permissive) }
        if urls(added).insecure > 0 { out.append(.insecureUrl) }
        if isConflictMarker(added) { out.append(.conflictMarker) }
        if matches(todoMarker, added) { out.append(.todo) }
        if matches(debugCall, added) { out.append(.debug) }
        if matches(broadCatch, added) { out.append(.broadCatch) }
        if isCode(path) {
            if added.utf16.count > 140 { out.append(.longLine) }
            if added.hasSuffix(" ") || added.hasSuffix("\t") { out.append(.trailingWhitespace) }
            if matches(controlFlow, added) && indent(added) >= 12 { out.append(.deepNesting) }
        }
        if matches(requestSite, added) { out.append(.request) }
        return out
    }

    static func isConflictMarker(_ text: String) -> Bool {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.hasPrefix("<<<<<<<") || trimmed.hasPrefix(">>>>>>>") || trimmed == "======="
    }

    static func indent(_ text: String) -> Int {
        var count = 0
        for ch in text {
            if ch.isWhitespace { count += 1 } else { break }
        }
        return count
    }

    public static func lineStats(_ files: [DiffFile]) -> DiffHealthLineStats {
        var stats = DiffHealthLineStats()
        for file in files {
            let path = file.status == .deleted ? "" : file.newPath
            let code = isCode(path)
            for hunk in file.hunks {
                for line in hunk.lines where line.kind == .added {
                    let added = line.text
                    if code {
                        if matches(controlFlow, added) {
                            stats.controlFlowLines += 1
                            if indent(added) >= 12 { stats.nestedControlFlowLines += 1 }
                        }
                        if added.utf16.count > 140 { stats.longLines += 1 }
                        if added.hasSuffix(" ") || added.hasSuffix("\t") { stats.trailingWhitespaceLines += 1 }
                    }
                    if matches(todoMarker, added) { stats.todoLines += 1 }
                    if matches(debugCall, added) { stats.debugLines += 1 }
                    if matches(broadCatch, added) { stats.broadCatchLines += 1 }
                    if matches(unsafeApi, added) { stats.unsafeApiLines += 1 }
                    if matches(transportBypass, added) { stats.transportBypassLines += 1 }
                    if matches(permissiveSecurity, added) { stats.permissiveSecurityLines += 1 }
                    if matches(requestSite, added) { stats.requestLines += 1 }
                    if !stats.resilienceEvidence && matches(resilience, added) { stats.resilienceEvidence = true }
                    if isConflictMarker(added) { stats.conflictMarkerLines += 1 }
                    let found = urls(added)
                    stats.hardcodedUrls += found.all
                    stats.insecureUrls += found.insecure
                    stats.secretMatches += secretMatches(added).count
                }
            }
        }
        return stats
    }

    static func evidence(_ parts: [String], _ fallback: String, _ caveat: String) -> String {
        var result = parts.isEmpty ? fallback : parts.joined(separator: "; ")
        if !result.hasSuffix(".") { result += "." }
        return result + " " + caveat
    }

    static func bounded(_ score: Int) -> Int { min(max(score, 0), 100) }

    public static func report(_ files: [DiffFile], git: DiffHealthGit = .unknown) -> [DiffHealthMetric] {
        var totalAdds = 0, totalDels = 0, maxFileChurn = 0, sourceFiles = 0, testFiles = 0, testAdds = 0, testDels = 0
        var binaryFiles = 0, generatedFiles = 0, sensitiveFiles = 0, oddArtifacts = 0
        var maxChurnPath = ""
        var lockfileChanged = false, manifestChanged = false
        for file in files {
            let adds = file.additions
            let dels = file.deletions
            totalAdds += adds
            totalDels += dels
            if adds + dels > maxFileChurn {
                maxFileChurn = adds + dels
                maxChurnPath = file.path
            }
            let test = isTest(file.path)
            if isCode(file.path) && !test { sourceFiles += 1 }
            if test {
                testFiles += 1
                testAdds += adds
                testDels += dels
            }
            if file.binary { binaryFiles += 1 }
            if isGenerated(file.path) { generatedFiles += 1 }
            if isSensitive(file) { sensitiveFiles += 1 }
            if isOddArtifact(file) { oddArtifacts += 1 }
            let base = fileName(file.path).lowercased()
            lockfileChanged = lockfileChanged || lockfiles.contains(base)
            manifestChanged = manifestChanged || manifests.contains(base)
        }
        let s = lineStats(files)
        let churn = totalAdds + totalDels
        let fileCount = files.count

        var complexity = 100
        if churn > 80 { complexity -= 4 }
        if churn > 250 { complexity -= 8 }
        if churn > 600 { complexity -= 14 }
        if churn > 1500 { complexity -= 20 }
        if maxFileChurn > 250 { complexity -= 8 }
        if maxFileChurn > 750 { complexity -= 10 }
        if s.controlFlowLines > 20 { complexity -= 6 }
        if s.controlFlowLines > 50 { complexity -= 10 }
        if s.controlFlowLines > 120 { complexity -= 15 }
        if s.nestedControlFlowLines > 8 { complexity -= 8 }
        if s.nestedControlFlowLines > 30 { complexity -= 12 }
        complexity = bounded(complexity)
        var complexityBits = ["\(churn) changed lines across \(fileCount) files"]
        if s.controlFlowLines > 0 { complexityBits.append("\(s.controlFlowLines) added control-flow lines") }
        if s.nestedControlFlowLines > 0 { complexityBits.append("\(s.nestedControlFlowLines) deeply indented branches") }
        if maxFileChurn > 0 && !maxChurnPath.isEmpty { complexityBits.append("largest file is \(maxFileChurn) lines in \(maxChurnPath)") }

        var quality = 100
        var qualityBits: [String] = []
        if sourceFiles > 0 && testFiles == 0 {
            quality -= 18
            qualityBits.append("\(sourceFiles) source files changed with no test file in the diff")
            if totalAdds > 250 { quality -= 10 }
        } else if testFiles > 0 {
            qualityBits.append("\(testFiles) test files changed")
        } else {
            qualityBits.append("no source-code files changed")
        }
        if testFiles > 0 && testAdds == 0 && testDels > 0 {
            quality -= 20
            qualityBits.append("tests were only removed")
        }
        quality -= min(25, s.todoLines * 5)
        quality -= min(15, s.debugLines * 5)
        quality -= min(15, s.broadCatchLines * 5)
        quality -= min(15, s.longLines * 2)
        if s.todoLines > 0 { qualityBits.append("\(s.todoLines) new TODO/FIXME markers") }
        if s.debugLines > 0 { qualityBits.append("\(s.debugLines) debug statements") }
        if s.broadCatchLines > 0 { qualityBits.append("\(s.broadCatchLines) broad exception handlers") }
        if s.longLines > 0 { qualityBits.append("\(s.longLines) code lines over 140 characters") }
        quality = bounded(quality)

        var security = 100
        var securityBits: [String] = []
        if s.secretMatches > 0 {
            security -= 70 + min(20, (s.secretMatches - 1) * 10)
            securityBits.append("\(s.secretMatches) high-confidence secret matches")
        }
        security -= min(70, s.transportBypassLines * 35)
        security -= min(50, s.unsafeApiLines * 20)
        security -= min(30, s.permissiveSecurityLines * 15)
        security -= min(70, sensitiveFiles * 55)
        if s.transportBypassLines > 0 { securityBits.append("\(s.transportBypassLines) TLS/certificate bypasses") }
        if s.unsafeApiLines > 0 { securityBits.append("\(s.unsafeApiLines) dynamic execution or unsafe DOM calls") }
        if s.permissiveSecurityLines > 0 { securityBits.append("\(s.permissiveSecurityLines) permissive security settings") }
        if sensitiveFiles > 0 { securityBits.append("\(sensitiveFiles) newly tracked key or environment files") }
        security = bounded(security)

        var network = 100
        var networkBits: [String] = []
        if s.requestLines > 0 {
            network -= min(20, s.requestLines * 4)
            networkBits.append("\(s.requestLines) added network request sites")
            if !s.resilienceEvidence {
                network -= 18
                networkBits.append("no added timeout, cancellation, retry, or backoff evidence")
            } else {
                networkBits.append("timeout/retry/cancellation evidence present")
            }
        }
        if s.hardcodedUrls > 0 {
            network -= min(10, s.hardcodedUrls * 2)
            networkBits.append("\(s.hardcodedUrls) hard-coded URLs")
        }
        if s.insecureUrls > 0 {
            network -= min(80, s.insecureUrls * 40)
            networkBits.append("\(s.insecureUrls) non-local plain-HTTP endpoints")
        }
        network = bounded(network)

        let lockfileWithoutManifest = lockfileChanged && !manifestChanged
        var practices = 100
        var practicesBits: [String] = []
        if s.conflictMarkerLines > 0 {
            practices = 10
            practicesBits.append("\(s.conflictMarkerLines) unresolved conflict-marker lines")
        }
        practices -= min(30, generatedFiles * 12)
        practices -= min(20, binaryFiles * 8)
        practices -= min(50, oddArtifacts * 25)
        practices -= min(15, s.trailingWhitespaceLines * 2)
        practices -= min(15, (s.todoLines + s.debugLines) * 3)
        if lockfileWithoutManifest { practices -= 8 }
        if churn > 1000 { practices -= 10 }
        if generatedFiles > 0 { practicesBits.append("\(generatedFiles) generated/vendor paths changed") }
        if binaryFiles > 0 { practicesBits.append("\(binaryFiles) binary files changed") }
        if oddArtifacts > 0 { practicesBits.append("\(oddArtifacts) unusual compiled/archive artifacts added") }
        if s.trailingWhitespaceLines > 0 { practicesBits.append("\(s.trailingWhitespaceLines) added code lines have trailing whitespace") }
        if lockfileWithoutManifest { practicesBits.append("a lockfile changed without its manifest") }
        practices = bounded(practices)

        let weighted = (complexity * 25 + quality * 20 + security * 25 + network * 10 + practices * 20 + 50) / 100
        var merge = weighted
        var mergeBits: [String] = []
        switch git {
        case .unknown:
            merge = min(merge, 84)
            mergeBits.append("this device has no Git base or conflict check for the branch")
        case .merged:
            mergeBits.append("already merged into the base")
        case let .state(behind, ahead, mergeConflict, worktreeConflict):
            if mergeConflict {
                merge = min(merge, 15)
                mergeBits.append("Git predicts content conflicts with the base")
            } else if worktreeConflict {
                merge = min(merge, 35)
                mergeBits.append("the branch worktree has unresolved local conflicts")
            } else {
                if behind > 0 {
                    merge -= min(24, 4 + behind * 2)
                    mergeBits.append("\(behind) commits behind the base")
                } else {
                    mergeBits.append("up to date with the base")
                }
                if ahead >= 0 { mergeBits.append("\(ahead) commits ahead") }
            }
        }
        if fileCount > 25 { merge -= 5 }
        if fileCount > 75 { merge -= 10 }
        if security < 60 || network < 40 || practices < 30 { merge = min(merge, 55) }
        merge = bounded(merge)
        mergeBits.append("weighted static diff health \(weighted)/100")

        return [
            DiffHealthMetric(
                key: "merge", label: "Merge", score: merge,
                detail: evidence(mergeBits, "No merge signals available",
                                 "Combines Git divergence/conflict state with the five static diff scores.")),
            DiffHealthMetric(
                key: "complexity", label: "Complexity", score: complexity,
                detail: evidence(complexityBits, "No changed lines",
                                 "Estimated from churn, control flow, and concentration; no code was executed.")),
            DiffHealthMetric(
                key: "quality", label: "Quality", score: quality,
                detail: evidence(qualityBits, "No quality findings", "Tests are inferred from paths and were not executed.")),
            DiffHealthMetric(
                key: "security", label: "Security", score: security,
                detail: evidence(securityBits, "No high-confidence secrets or risky added-line patterns found",
                                 "Static heuristic only; it does not replace dependency or SAST scanning.")),
            DiffHealthMetric(
                key: "network", label: "Network", score: network,
                detail: evidence(networkBits, "No added request sites or hard-coded URLs detected",
                                 "This is syntax analysis; ForkMesh sent no traffic for this check.")),
            DiffHealthMetric(
                key: "practices", label: "Practices", score: practices,
                detail: evidence(practicesBits, "No unusual artifacts, conflict markers, or diff-hygiene issues found",
                                 "Checks only the files and added lines in this diff.")),
        ]
    }
}
