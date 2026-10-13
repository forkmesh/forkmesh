import Foundation

public struct SugarContractLine: Equatable, Identifiable, Sendable {
    public let id: Int
    public let name: String
    public let caption: String
}

public struct SugarContract: Hashable, Sendable {
    public static let title = "No Sugar Contract"
    public static let brand = "SUGAR BREAK"
    public static let signerRange = 1...12
    public static let defaultSigners = 2
    public static let maxNameLength = 40

    public var startDay: String
    public var endDay: String
    public private(set) var names: [String]

    public init(startDay: String, endDay: String, signers: Int = SugarContract.defaultSigners) {
        self.startDay = startDay
        self.endDay = endDay < startDay ? startDay : endDay
        names = Array(repeating: "", count: Self.clamp(signers))
    }

    public static func clamp(_ signers: Int) -> Int {
        min(signerRange.upperBound, max(signerRange.lowerBound, signers))
    }

    public var signers: Int {
        get { names.count }
        set {
            let count = Self.clamp(newValue)
            if count < names.count {
                names.removeLast(names.count - count)
            } else if count > names.count {
                names.append(contentsOf: Array(repeating: "", count: count - names.count))
            }
        }
    }

    public mutating func setName(_ index: Int, _ name: String) {
        guard names.indices.contains(index) else { return }
        names[index] = String(name.prefix(Self.maxNameLength))
    }

    public mutating func setStart(_ day: String) {
        startDay = day
        if endDay < day { endDay = day }
    }

    public mutating func setEnd(_ day: String) {
        endDay = day < startDay ? startDay : day
    }

    public var isOneDay: Bool { startDay == endDay }

    public var days: Int {
        guard let start = CaloriesText.tryParseDay(startDay, timeZone: Self.utc),
            let end = CaloriesText.tryParseDay(endDay, timeZone: Self.utc)
        else { return 1 }
        return max(1, Int(((end.timeIntervalSince(start)) / 86_400).rounded()) + 1)
    }

    public var dateLine: String {
        if isOneDay { return SugarContractText.full(startDay) }
        if SugarContractText.year(startDay) == SugarContractText.year(endDay) {
            return "\(SugarBreakText.monthDay(startDay)) – \(SugarContractText.full(endDay))"
        }
        return "\(SugarContractText.full(startDay)) – \(SugarContractText.full(endDay))"
    }

    public var lengthLine: String { SugarBreakText.days(days) }

    public var terms: String {
        let who = signers == 1 ? "I, the undersigned, promise" : "We, the undersigned, promise"
        if isOneDay { return "\(who) to eat no added sugar on \(SugarContractText.full(startDay))." }
        return "\(who) to eat no added sugar from \(SugarBreakText.monthDay(startDay)) through \(SugarContractText.full(endDay))."
    }

    public static let fineprint = "No sweets · No soda · No desserts"

    public var lines: [SugarContractLine] {
        names.enumerated().map { index, raw in
            let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            return SugarContractLine(id: index, name: name, caption: name.isEmpty ? "Signature" : name)
        }
    }

    public var message: String {
        let when = isOneDay ? "on \(dateLine)" : "for \(dateLine) (\(lengthLine))"
        let ask = signers == 1 ? "Hold me to it." : "Sign the contract and go sugar-free with me."
        return "No sugar \(when). \(ask)"
    }

    public var fileName: String { "no-sugar-contract-\(startDay).png" }

    static let utc = TimeZone(identifier: "UTC") ?? .current
}

public enum SugarContractText {
    public static func year(_ day: String) -> Int {
        Int(day.split(separator: "-").first ?? "") ?? 0
    }

    public static func full(_ day: String) -> String {
        let year = year(day)
        let label = SugarBreakText.monthDay(day)
        return year > 0 && label != day ? "\(label), \(year)" : day
    }
}

extension SugarBreakModel {
    public func contract(signers: Int = SugarContract.defaultSigners) -> SugarContract {
        if isActive, !endDay.isEmpty, endDay >= today {
            return SugarContract(startDay: max(startDay, today), endDay: endDay, signers: signers)
        }
        let length = min(365, max(1, selectedLength))
        return SugarContract(
            startDay: today, endDay: CaloriesText.addDays(today, length - 1, timeZone: timeZone, now: Date()),
            signers: signers)
    }
}
