import FMBase
import Foundation

public enum Lamports {
    public static let perSol: Int64 = 1_000_000_000

    public static func sol(_ lamports: Int64) -> String {
        let negative = lamports < 0
        let value = lamports.magnitude
        let whole = value / UInt64(perSol)
        var fraction = String(value % UInt64(perSol))
        fraction = String(repeating: "0", count: 9 - fraction.count) + fraction
        while fraction.hasSuffix("0") { fraction.removeLast() }
        let text = fraction.isEmpty ? "\(whole)" : "\(whole).\(fraction)"
        return (negative ? "-" : "") + text + " SOL"
    }
}

public struct LedgerEntry: Equatable, Sendable, Identifiable {
    public var id: String
    public var category: String
    public var title: String
    public var status: String
    public var direction: String
    public var currency: String
    public var amountLamports: Int64
    public var amountSol: String
    public var amountUsd: String
    public var amountKnown: Bool
    public var occurredAt: Int64
    public var transactionId: String
    public var explorerUrl: String
    public var note: String

    public init(json: JSONObject) {
        id = json.str("id")
        category = json.str("category", "record")
        title = json.str("title", "Financial record")
        status = json.str("status", "recorded")
        direction = json.str("direction", "record")
        currency = json.str("currency", "SOL")
        amountLamports = max(0, json.int64("amountLamports"))
        amountSol = json.str("amountSol")
        amountUsd = json.str("amountUsd")
        amountKnown = json.optBool("amountKnown") ?? true
        occurredAt = json.int64("occurredAt")
        transactionId = json.str("transactionId")
        explorerUrl = json.str("explorerUrl")
        note = json.str("note")
    }

    public var amountText: String {
        if !amountKnown { return "Amount not recorded" }
        if currency == "SOL" { return Lamports.sol(amountLamports) }
        return amountUsd.isEmpty ? currency : "$" + amountUsd
    }

    public var verified: Bool { !transactionId.isEmpty }
}

public struct LedgerCursor: Equatable, Sendable {
    public var beforeAt: Int64
    public var beforeId: String

    public var query: [(String, String)] { [("beforeAt", String(beforeAt)), ("beforeId", beforeId)] }
}

public struct LedgerPage: Equatable, Sendable {
    public var entries: [LedgerEntry]
    public var next: LedgerCursor?
    public var solUsd: String

    public init(json: JSONObject) {
        entries = json.maps("entries").map(LedgerEntry.init(json:)).filter { !$0.id.isEmpty }
        let page = json.obj("page")
        let cursor = page.obj("nextCursor")
        next = page.bool("hasMore") && !cursor.str("beforeId").isEmpty
            ? LedgerCursor(beforeAt: cursor.int64("beforeAt"), beforeId: cursor.str("beforeId")) : nil
        let quote = json.obj("solUsd")
        solUsd = quote.bool("available") ? quote.str("price") : ""
    }
}

public struct RewardEstimate: Equatable, Sendable, Identifiable {
    public var rewardId: String
    public var recipient: String
    public var recipientAddress: String
    public var repository: String
    public var sourceKind: String
    public var subjectNumber: Int
    public var subjectTitle: String
    public var amountLamports: Int64
    public var status: String
    public var transactionSignature: String
    public var createdAt: Int64

    public var id: String { rewardId }

    public init(json: JSONObject) {
        rewardId = json.str("rewardId")
        recipient = json.str("recipient")
        recipientAddress = json.str("recipientAddress")
        repository = json.str("repository")
        sourceKind = json.str("sourceKind")
        subjectNumber = json.int("subjectNumber")
        subjectTitle = json.str("subjectTitle")
        amountLamports = max(0, json.int64("amountLamports"))
        status = json.str("status")
        transactionSignature = json.str("transactionSignature")
        createdAt = json.int64("createdAt")
    }

    public var paid: Bool { status == "paid" || !transactionSignature.isEmpty }

    public var subjectLabel: String {
        let kind = sourceKind == "pull" ? "PR" : "Issue"
        return "\(repository) \(kind) #\(subjectNumber)"
    }
}

public struct PersonalRecord: Equatable, Sendable, Identifiable, Codable {
    public enum Kind: String, Codable, Sendable, CaseIterable {
        case expense
        case income
    }

    public var id: String
    public var kind: Kind
    public var title: String
    public var amountMinor: Int64
    public var currency: String
    public var occurredAt: Int64
    public var note: String
    public var settled: Bool

    public init(
        id: String = UUID().uuidString.lowercased(), kind: Kind, title: String, amountMinor: Int64, currency: String = "USD",
        occurredAt: Int64, note: String = "", settled: Bool = false
    ) {
        self.id = id
        self.kind = kind
        self.title = String(title.prefix(200))
        self.amountMinor = max(0, amountMinor)
        self.currency = currency
        self.occurredAt = occurredAt
        self.note = String(note.prefix(2000))
        self.settled = settled
    }

    public var amountText: String { MoneyText.cents(amountMinor, currency) }
}

@MainActor
public final class PersonalLedgerStore {
    public static let maxRecords = 500
    public static let key = "packages/ledger/personal-records"

    private let store: SecureStore

    public init(store: SecureStore) {
        self.store = store
    }

    public func load() -> [PersonalRecord] {
        guard let text = store.read(PersonalLedgerStore.key), let data = text.data(using: .utf8),
            let records = try? JSONDecoder().decode([PersonalRecord].self, from: data)
        else { return [] }
        return records.sorted { $0.occurredAt > $1.occurredAt }
    }

    @discardableResult
    public func save(_ records: [PersonalRecord]) -> [PersonalRecord] {
        let bounded = Array(records.sorted { $0.occurredAt > $1.occurredAt }.prefix(PersonalLedgerStore.maxRecords))
        if let data = try? JSONEncoder().encode(bounded), let text = String(data: data, encoding: .utf8) {
            store.write(PersonalLedgerStore.key, text)
        }
        return bounded
    }

    public func clear() { store.delete(PersonalLedgerStore.key) }

    public static func totals(_ records: [PersonalRecord]) -> [(currency: String, income: Int64, expense: Int64)] {
        var byCurrency: [String: (Int64, Int64)] = [:]
        for record in records {
            var entry = byCurrency[record.currency] ?? (0, 0)
            if record.kind == .income { entry.0 += record.amountMinor } else { entry.1 += record.amountMinor }
            byCurrency[record.currency] = entry
        }
        return byCurrency.keys.sorted().map { (currency: $0, income: byCurrency[$0]!.0, expense: byCurrency[$0]!.1) }
    }
}
