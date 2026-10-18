import Foundation

public enum StrataSeverity: Int, Sendable, Equatable, CaseIterable, Comparable {
    case critical = 0, high, medium, low

    public var name: String {
        switch self {
        case .critical: return "Critical"
        case .high: return "High"
        case .medium: return "Medium"
        case .low: return "Low"
        }
    }

    public static func < (a: StrataSeverity, b: StrataSeverity) -> Bool { a.rawValue < b.rawValue }

    public static func at(_ index: Int) -> StrataSeverity { StrataSeverity(rawValue: max(0, min(3, index))) ?? .low }
}

public enum StrataCategory: Int, Sendable, Equatable, CaseIterable {
    case architecture = 0, design, codeSmells, maintainability

    public var name: String {
        switch self {
        case .architecture: return "Architecture"
        case .design: return "Design"
        case .codeSmells: return "Code smells"
        case .maintainability: return "Maintainability"
        }
    }
}

public enum StrataGateStatus: Int, Sendable, Equatable, CaseIterable {
    case pass = 0, warn, fail

    public var name: String {
        switch self {
        case .pass: return "Pass"
        case .warn: return "Warn"
        case .fail: return "Fail"
        }
    }
}

public struct StrataViolation: Sendable, Equatable {
    public var file = -1
    public var module = -1
    public var line = 0
    public var severity: StrataSeverity = .medium
    public var message = ""
    public var debt: Double = 0
    public var peerModule = -1

    public init(file: Int = -1, module: Int = -1, line: Int = 0, severity: StrataSeverity = .medium, message: String = "", debt: Double = 0, peerModule: Int = -1) {
        self.file = file
        self.module = module
        self.line = line
        self.severity = severity
        self.message = message
        self.debt = debt
        self.peerModule = peerModule
    }
}

public struct StrataRule: Sendable, Equatable {
    public var id = ""
    public var title = ""
    public var category: StrataCategory = .codeSmells
    public var severity: StrataSeverity = .medium
    public var description = ""
    public var howToFix = ""
    public var violations: [StrataViolation] = []
    public var debt: Double = 0
    public var moduleScope = false

    public init() {}

    public var worst: Int {
        var best = -1
        for violation in violations {
            let index = violation.severity.rawValue
            if best < 0 || index < best { best = index }
        }
        return best
    }
}

public struct StrataGate: Sendable, Equatable {
    public var name = ""
    public var status: StrataGateStatus = .pass
    public var measured = ""
    public var detail = ""

    public init() {}
}

public struct StrataScore: Sendable, Equatable {
    public var debt: Double = 0
    public var effort: Double = 0
    public var ratio: Double = 0
    public var rating = 0
    public var interest: Double = 0
    public var violations = 0
    public var worst = -1

    public init() {}
}

public struct StrataMetrics: Sendable, Equatable {
    public var codeLines = 0
    public var commentLines = 0
    public var files = 0
    public var modules = 0
    public var types = 0
    public var functions = 0
    public var averageComplexity: Double = 0
    public var maxComplexity = 0
    public var maxComplexityFunction = ""
    public var maxComplexityFile = -1
    public var commentPercent: Double = 0
    public var duplicatedPercent: Double = 0
    public var testFiles = 0

    public init() {}
}

public struct StrataViolationRef: Sendable, Hashable {
    public var rule: Int
    public var index: Int

    public init(rule: Int, index: Int) {
        self.rule = rule
        self.index = index
    }
}

public struct StrataReport: Sendable, Equatable {
    public var graph = InsightModuleGraph()
    public var rules: [StrataRule] = []
    public var files: [StrataScore] = []
    public var modules: [StrataScore] = []
    public var fileViolations: [[StrataViolationRef]] = []
    public var moduleViolations: [[StrataViolationRef]] = []
    public var total = StrataScore()
    public var gates: [StrataGate] = []
    public var metrics = StrataMetrics()
    public var bySeverity: [Int] = [0, 0, 0, 0]
    public var history = false

    public init() {}

    public func ruleIndex(_ id: String) -> Int {
        rules.firstIndex { $0.id == id } ?? -1
    }

    public func gatesWith(_ status: StrataGateStatus) -> Int {
        gates.filter { $0.status == status }.count
    }

    public func violation(_ ref: StrataViolationRef) -> StrataViolation {
        rules[ref.rule].violations[ref.index]
    }

    public func sortedBySeverity(_ refs: [StrataViolationRef]) -> [StrataViolationRef] {
        Strata.stableSorted(refs) { a, b in
            let va = violation(a)
            let vb = violation(b)
            if va.severity != vb.severity { return va.severity < vb.severity }
            return va.debt > vb.debt
        }
    }

    public func worstForModule(_ module: Int, limit: Int) -> [StrataViolationRef] {
        guard module >= 0 && module < graph.nodes.count else { return [] }
        var refs: [StrataViolationRef] = []
        if module < moduleViolations.count { refs += moduleViolations[module] }
        for file in graph.nodes[module].files where file >= 0 && file < fileViolations.count {
            refs += fileViolations[file]
        }
        refs = sortedBySeverity(refs)
        if limit > 0 && refs.count > limit { refs = Array(refs.prefix(limit)) }
        return refs
    }
}

public enum Strata {
    public static func stableSorted<T>(_ items: [T], by less: (T, T) -> Bool) -> [T] {
        items.enumerated().sorted { x, y in
            if less(x.element, y.element) { return true }
            if less(y.element, x.element) { return false }
            return x.offset < y.offset
        }.map(\.element)
    }

    public static func effortMinutes(_ codeLines: Int) -> Double {
        Double(max(0, codeLines)) * 18.0 * 8.0 * 60.0 / 1000.0
    }

    public static func ratingFor(_ ratio: Double) -> Int {
        if ratio <= 0.05 { return 0 }
        if ratio <= 0.10 { return 1 }
        if ratio <= 0.20 { return 2 }
        if ratio <= 0.50 { return 3 }
        return 4
    }

    public static func ratingLetter(_ rating: Int) -> String {
        ["A", "B", "C", "D", "E"][max(0, min(4, rating))]
    }

    public static func grouped(_ value: Int) -> String {
        let digits = String(value.magnitude)
        var out = ""
        for (i, ch) in digits.enumerated() {
            if i > 0 && (digits.count - i) % 3 == 0 { out.append(",") }
            out.append(ch)
        }
        return value < 0 ? "-" + out : out
    }

    public static func fixed(_ value: Double, _ decimals: Int) -> String {
        fixedParts(value, decimals).joined()
    }

    public static func localeFixed(_ value: Double, _ decimals: Int) -> String {
        let parts = fixedParts(value, decimals)
        let whole = Int(parts[1]) ?? 0
        return parts[0] + grouped(whole) + parts[2]
    }

    static func fixedParts(_ value: Double, _ decimals: Int) -> [String] {
        guard value.isFinite else { return ["", value.isNaN ? "nan" : "inf", ""] }
        let negative = value < 0
        let exact = String(format: "%.60f", abs(value))
        let pieces = exact.split(separator: ".", omittingEmptySubsequences: false)
        var whole = Array(pieces[0].utf8).map { Int($0) - 48 }
        let fraction = pieces.count > 1 ? Array(pieces[1].utf8).map { Int($0) - 48 } : []
        var kept = Array(fraction.prefix(decimals))
        while kept.count < decimals { kept.append(0) }
        let next = decimals < fraction.count ? fraction[decimals] : 0
        if next >= 5 {
            var carry = 1
            var i = kept.count - 1
            while carry > 0 && i >= 0 {
                kept[i] += 1
                if kept[i] == 10 { kept[i] = 0 } else { carry = 0 }
                i -= 1
            }
            var j = whole.count - 1
            while carry > 0 && j >= 0 {
                whole[j] += 1
                if whole[j] == 10 { whole[j] = 0 } else { carry = 0 }
                j -= 1
            }
            if carry > 0 { whole.insert(1, at: 0) }
        }
        let wholeText = String(whole.map { Character(Unicode.Scalar(UInt8($0 + 48))) })
        let fractionText = kept.isEmpty ? "" : "." + String(kept.map { Character(Unicode.Scalar(UInt8($0 + 48))) })
        let isZero = whole.allSatisfy { $0 == 0 } && kept.allSatisfy { $0 == 0 }
        return [negative && !isZero ? "-" : "", wholeText, fractionText]
    }

    public static func formatDebt(_ minutes: Double) -> String {
        let total = max(Int64(0), Int64(minutes.rounded()))
        if total < 60 { return "\(total) min" }
        let hours = total / 60
        let rest = total % 60
        if hours < 8 { return rest != 0 ? "\(hours) h \(rest) min" : "\(hours) h" }
        let days = hours / 8
        let hourRest = hours % 8
        let dayText = grouped(Int(days))
        if days >= 100 || hourRest == 0 { return "\(dayText) d" }
        return "\(dayText) d \(hourRest) h"
    }

    public static func formatPercent(_ ratio: Double) -> String {
        let percent = ratio * 100.0
        if percent > 0 && percent < 0.1 { return "<0.1 %" }
        return localeFixed(percent, percent >= 10 ? 0 : 1) + " %"
    }

    public static func fileName(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return path }
        return String(path[path.index(after: slash)...])
    }

    public static func htmlEscaped(_ text: String) -> String {
        var out = ""
        out.reserveCapacity(text.count)
        for ch in text.unicodeScalars {
            switch ch {
            case "<": out += "&lt;"
            case ">": out += "&gt;"
            case "&": out += "&amp;"
            case "\"": out += "&quot;"
            default: out.unicodeScalars.append(ch)
            }
        }
        return out
    }
}
