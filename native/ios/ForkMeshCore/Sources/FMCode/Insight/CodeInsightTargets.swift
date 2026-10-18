import FMApi
import FMBase
import FMCodeInsight
import Foundation

public enum CodeInsightHistory: String, CaseIterable, Sendable, Identifiable {
    case filesOnly = "files"
    case recent = "300"
    case extended = "2000"

    public static let preferenceKey = "codeInsight/historyDepth"
    public static let fallback = CodeInsightHistory.recent

    public var id: String { rawValue }

    public var commits: Int {
        switch self {
        case .filesOnly: return 1
        case .recent: return 300
        case .extended: return 2000
        }
    }

    public var label: String {
        switch self {
        case .filesOnly: return "Files only"
        case .recent: return "Last 300 commits"
        case .extended: return "Last 2,000 commits"
        }
    }

    public var shortLabel: String {
        switch self {
        case .filesOnly: return "files only"
        case .recent: return "300 commits"
        case .extended: return "2,000 commits"
        }
    }

    public var cloneDepth: Int { self == .filesOnly ? 1 : commits + 1 }

    public var options: InsightOptions {
        var options = InsightOptions()
        if self == .filesOnly {
            options.history = false
        } else {
            options.maxCommits = commits
        }
        return options
    }

    public static func from(_ id: String?) -> CodeInsightHistory {
        id.flatMap(CodeInsightHistory.init(rawValue:)) ?? fallback
    }

    public static func load(_ prefs: KeyValueStore?) -> CodeInsightHistory {
        from(prefs?.string(forKey: preferenceKey))
    }

    public func save(_ prefs: KeyValueStore?) {
        prefs?.set(rawValue, forKey: CodeInsightHistory.preferenceKey)
    }
}

public enum CodeInsightTarget: Hashable, Sendable, Identifiable {
    case local(name: String, publishedAs: String?)
    case account(owner: String, name: String)

    public static let lastKey = "codeInsight/lastTarget"

    public var id: String { key }

    public var key: String {
        switch self {
        case let .local(name, _): return "local:\(name)"
        case let .account(owner, name): return "repo:\(owner)/\(name)"
        }
    }

    public var title: String {
        switch self {
        case let .local(name, _): return name
        case let .account(_, name): return name
        }
    }

    public var subtitle: String {
        switch self {
        case let .local(_, publishedAs): return publishedAs.map { "On this phone · \($0)" } ?? "On this phone"
        case let .account(owner, name): return "\(owner)/\(name)"
        }
    }

    public var isLocal: Bool {
        if case .local = self { return true }
        return false
    }

    public var repoRef: RepoRef? {
        switch self {
        case let .local(_, publishedAs): return publishedAs.flatMap(RepoRef.init(slug:))
        case let .account(owner, name): return RepoRef(owner: owner, name: name)
        }
    }

    public static func account(_ ref: RepoRef) -> CodeInsightTarget {
        .account(owner: ref.owner, name: ref.name)
    }

    public static func local(_ record: LocalRepoRecord) -> CodeInsightTarget {
        .local(name: record.name, publishedAs: record.publishedAs.flatMap { $0.isEmpty ? nil : $0 })
    }

    public static func parse(_ key: String?) -> CodeInsightTarget? {
        guard let key else { return nil }
        if key.hasPrefix("local:") {
            let name = String(key.dropFirst("local:".count))
            return name.isEmpty ? nil : .local(name: name, publishedAs: nil)
        }
        if key.hasPrefix("repo:"), let ref = RepoRef(slug: String(key.dropFirst("repo:".count))) {
            return .account(ref)
        }
        return nil
    }

    public static func choices(local: [LocalRepoRecord], account: [Repository]) -> [CodeInsightTarget] {
        var seen = Set<String>()
        var result: [CodeInsightTarget] = []
        for record in local {
            let target = CodeInsightTarget.local(record)
            if seen.insert(target.key).inserted { result.append(target) }
        }
        for repository in account {
            let target = CodeInsightTarget.account(owner: repository.owner, name: repository.name)
            if seen.insert(target.key).inserted { result.append(target) }
        }
        return result
    }

    public static func initial(route: RepoRef?, preferred: CodeInsightTarget?, last: CodeInsightTarget?, local: [LocalRepoRecord]) -> CodeInsightTarget? {
        if let route { return .account(route) }
        if let preferred { return resolved(preferred, local: local) }
        guard let last else { return nil }
        if case let .local(name, _) = last {
            return local.first { $0.name == name }.map(CodeInsightTarget.local)
        }
        return last
    }

    static func resolved(_ target: CodeInsightTarget, local: [LocalRepoRecord]) -> CodeInsightTarget {
        if case let .local(name, _) = target, let record = local.first(where: { $0.name == name }) {
            return .local(record)
        }
        return target
    }
}

public enum CodeInsightLabels {
    public static let onThisPhone = "On this phone"

    public static func sourceLabel(_ target: CodeInsightTarget, _ history: CodeInsightHistory) -> String {
        target.isLocal ? onThisPhone : "Analysis copy · \(history.shortLabel)"
    }

    public static func fileCount(_ count: Int) -> String {
        let formatter = NumberFormatter()
        formatter.numberStyle = .decimal
        formatter.locale = Locale(identifier: "en_US")
        let text = formatter.string(from: NSNumber(value: count)) ?? String(count)
        return count == 1 ? "\(text) file" : "\(text) files"
    }

    public static func statusLine(source: String, files: Int, summary: String) -> String {
        [source, fileCount(files), summary].filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public static func copyFolder(owner: String, name: String) -> String? {
        guard isSafeSegment(owner), isSafeSegment(name) else { return nil }
        return "\(owner)/\(name)"
    }

    static func isSafeSegment(_ value: String) -> Bool {
        if value.isEmpty || value.count > 100 || value == "." || value == ".." || value.hasPrefix(".") { return false }
        return value.unicodeScalars.allSatisfy { scalar in
            (scalar.value < 128 && CharacterSet.alphanumerics.contains(scalar)) || scalar == "-" || scalar == "_" || scalar == "."
        }
    }
}
