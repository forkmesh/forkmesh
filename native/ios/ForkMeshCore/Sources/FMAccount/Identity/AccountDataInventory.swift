import FMBase
import Foundation
import Observation

public enum AccountDataPaths {
    public static let inventory = "/api/accounts/data"
    public static let store = "/api/accounts/data/store"

    public static func inventory(refresh: Bool) -> String {
        refresh ? inventory + "?refresh=1" : inventory
    }

    public static func rows(subject: String, table: String, after: Int64) -> String {
        var path = "/api/accounts/data/rows?subject=\(encode(subject))&table=\(encode(table))"
        if after > 0 { path += "&after=\(after)" }
        return path
    }

    public static func row(table: String, rowid: Int64) -> String {
        "/api/accounts/data/rows/\(encode(table))/\(rowid)"
    }

    public static func storeItem(_ key: String) -> String {
        store + "/" + encode(key)
    }

    static func encode(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._"))) ?? ""
    }
}

public struct AccountDataSubject: Equatable, Sendable, Identifiable {
    public let id: String
    public let kind: String
    public let label: String
    public let role: String
    public let readable: Bool
    public let tables: Int
    public let rows: Int
    public let nodes: [String]
    public let repositories: Int

    public init(_ map: JSONObject) {
        id = map.str("id")
        kind = map.str("kind")
        label = map.str("label")
        role = map.str("role")
        readable = map["readable"]?.boolValue ?? true
        tables = map.int("tables")
        rows = map.int("rows")
        nodes = map.strings("nodes")
        repositories = map.int("repositories")
    }

    public var title: String {
        switch kind {
        case "organization": return "Organization \(label)"
        default: return label
        }
    }

    public var subtitle: String {
        let amount = rows == 1 ? "1 item" : "\(rows) items"
        let places = tables == 1 ? "1 place" : "\(tables) places"
        var parts = [amount + " in " + places]
        switch kind {
        case "account":
            if !nodes.isEmpty { parts.append("with nodes " + nodes.joined(separator: ", ")) }
        case "repositories":
            parts.append(repositories == 1 ? "1 repository" : "\(repositories) repositories")
        case "organization":
            parts.append("you are \(role.isEmpty ? "a member" : role == "owner" || role == "admin" ? "an \(role)" : "a \(role)")")
            if !readable { parts.append("owners and admins can open the contents") }
        default:
            break
        }
        return parts.joined(separator: " · ")
    }
}

public struct AccountDataGroup: Equatable, Sendable, Identifiable {
    public let subject: String
    public let table: String
    public let label: String
    public let category: String
    public let rows: Int
    public let truncated: Bool
    public let readable: Bool
    public let withheld: Bool
    public let deletable: Bool

    public var id: String { subject + "/" + table }

    public init(_ map: JSONObject) {
        subject = map.str("subject")
        table = map.str("table")
        label = map.str("label").isEmpty ? map.str("table") : map.str("label")
        category = map.str("category").isEmpty ? "Other" : map.str("category")
        rows = map.int("rows")
        truncated = map.bool("truncated")
        readable = map["readable"]?.boolValue ?? true
        withheld = map.bool("withheld")
        deletable = map.bool("deletable")
    }

    public var countText: String { truncated ? "\(rows)+" : "\(rows)" }

    public var note: String {
        if withheld { return "Security credentials: counted here, never shown or downloaded." }
        if !readable { return "Belongs to the organization. Its owners and admins can open it." }
        if deletable { return "You can remove these one at a time." }
        return ""
    }
}

public struct AccountStoreItem: Equatable, Sendable, Identifiable {
    public let key: String
    public let kind: String
    public let label: String
    public let device: String
    public let bytes: Int
    public let revision: Int
    public let updatedAt: Int64
    public let readable: Bool

    public var id: String { key }

    public init(_ map: JSONObject) {
        key = map.str("key")
        kind = map.str("kind")
        label = map.str("label")
        device = map.str("device")
        bytes = map.int("bytes")
        revision = map.int("revision", 1)
        updatedAt = map.int64("updatedAt")
        readable = map["readable"]?.boolValue ?? true
    }

    public var title: String { label.isEmpty ? key : label }
}

public struct AccountDataSection: Equatable, Sendable, Identifiable {
    public let category: String
    public let groups: [AccountDataGroup]

    public var id: String { category }
}

public struct AccountDataInventory: Equatable, Sendable {
    public let subjects: [AccountDataSubject]
    public let groups: [AccountDataGroup]
    public let store: [AccountStoreItem]
    public let generatedAt: Int64

    public init(subjects: [AccountDataSubject] = [], groups: [AccountDataGroup] = [], store: [AccountStoreItem] = [], generatedAt: Int64 = 0) {
        self.subjects = subjects
        self.groups = groups
        self.store = store
        self.generatedAt = generatedAt
    }

    public static func parse(_ map: JSONObject) -> AccountDataInventory {
        AccountDataInventory(
            subjects: map.list("subjects").compactMap { $0.objectValue.map(AccountDataSubject.init) },
            groups: map.list("groups").compactMap { $0.objectValue.map(AccountDataGroup.init) },
            store: map.list("store").compactMap { $0.objectValue.map(AccountStoreItem.init) },
            generatedAt: map.int64("generatedAt"))
    }

    public var totalRows: Int { groups.reduce(0) { $0 + $1.rows } + store.count }

    public func groups(for subject: String) -> [AccountDataSection] {
        var order: [String] = []
        var buckets: [String: [AccountDataGroup]] = [:]
        for group in groups where group.subject == subject {
            if buckets[group.category] == nil { order.append(group.category) }
            buckets[group.category, default: []].append(group)
        }
        let sorted = order.sorted { lhs, rhs in
            if lhs == "Account" { return rhs != "Account" }
            if rhs == "Account" { return false }
            if lhs == "Other" { return false }
            if rhs == "Other" { return true }
            return lhs < rhs
        }
        return sorted.map { AccountDataSection(category: $0, groups: (buckets[$0] ?? []).sorted { $0.label.localizedCaseInsensitiveCompare($1.label) == .orderedAscending }) }
    }

    public func removing(group: String, rows removed: Int) -> AccountDataInventory {
        let updated = groups.compactMap { item -> AccountDataGroup? in
            guard item.id == group else { return item }
            let left = max(0, item.rows - removed)
            guard left > 0 || item.truncated else { return nil }
            return AccountDataGroup([
                "subject": .string(item.subject), "table": .string(item.table), "label": .string(item.label),
                "category": .string(item.category), "rows": .int(Int64(left)), "truncated": .bool(item.truncated),
                "readable": .bool(item.readable), "withheld": .bool(item.withheld), "deletable": .bool(item.deletable),
            ])
        }
        return AccountDataInventory(subjects: subjects, groups: updated, store: store, generatedAt: generatedAt)
    }

    public func removingStore(_ key: String?) -> AccountDataInventory {
        AccountDataInventory(subjects: subjects, groups: groups, store: key.map { k in store.filter { $0.key != k } } ?? [], generatedAt: generatedAt)
    }
}

public struct AccountDataField: Equatable, Sendable, Identifiable {
    public let name: String
    public let value: JSONValue

    public var id: String { name }

    public var label: String {
        let words = name.replacingOccurrences(of: "_", with: " ")
        return words.prefix(1).uppercased() + words.dropFirst()
    }

    public var text: String {
        switch value {
        case .null: return "empty"
        case .bool(let flag): return flag ? "yes" : "no"
        case .int(let number):
            if AccountDataField.looksLikeTime(name), number > 1_000_000_000_000, number < 10_000_000_000_000 {
                return AccountDataField.date(number)
            }
            return String(number)
        case .double(let number): return JSON.formatDouble(number)
        case .string(let text): return text.isEmpty ? "empty" : text
        case .array, .object: return JSON.encodePretty(value)
        }
    }

    public var isStructured: Bool {
        switch value {
        case .array, .object: return true
        default: return false
        }
    }

    static func looksLikeTime(_ name: String) -> Bool {
        name.hasSuffix("_at") || name.hasSuffix("At") || name == "ts" || name.hasSuffix("_ts") || name == "last_seen" || name.hasSuffix("_ms")
    }

    static func date(_ millis: Int64) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyy-MM-dd HH:mm 'UTC'"
        return formatter.string(from: Date(timeIntervalSince1970: TimeInterval(millis) / 1_000))
    }
}

public struct AccountDataRow: Equatable, Sendable, Identifiable {
    public let id: String
    public let rowid: Int64
    public let deletable: Bool
    public let fields: [AccountDataField]

    public init(_ map: JSONObject, index: Int) {
        rowid = map.int64("rowid")
        id = rowid > 0 ? String(rowid) : "i\(index)"
        deletable = map.bool("deletable") && rowid > 0
        let raw = map.obj("fields")
        fields = raw.keys.map { AccountDataField(name: $0, value: raw[$0] ?? .null) }
    }

    public static let titleFields = ["label", "title", "name", "device_label", "username", "kind", "package_id", "repo", "account_name", "key_type", "fingerprint"]

    public var title: String {
        for name in AccountDataRow.titleFields {
            if let field = fields.first(where: { $0.name == name }), case .string(let text) = field.value, !text.isEmpty, !text.hasPrefix("(") {
                return text
            }
        }
        for field in fields {
            if case .object(let inner) = field.value {
                for name in AccountDataRow.titleFields where !inner.str(name).isEmpty && !inner.str(name).hasPrefix("(") {
                    return inner.str(name)
                }
            }
        }
        if let time = fields.first(where: { AccountDataField.looksLikeTime($0.name) && $0.value.int64Value ?? 0 > 0 }) {
            return time.text
        }
        return rowid > 0 ? "Item \(rowid)" : "Item"
    }

    public var subtitle: String {
        fields.filter { !$0.isStructured && $0.text != "empty" && !$0.text.hasPrefix("(") && $0.text != title }
            .prefix(2).map { "\($0.label): \($0.text)" }.joined(separator: " · ")
    }
}

public struct AccountDataPage: Equatable, Sendable {
    public let rows: [AccountDataRow]
    public let next: Int64
    public let readable: Bool
    public let reason: String

    public init(rows: [AccountDataRow] = [], next: Int64 = 0, readable: Bool = true, reason: String = "") {
        self.rows = rows
        self.next = next
        self.readable = readable
        self.reason = reason
    }

    public static func parse(_ map: JSONObject, offset: Int = 0) -> AccountDataPage {
        let rows = map.list("rows").enumerated().compactMap { index, value in
            value.objectValue.map { AccountDataRow($0, index: offset + index) }
        }
        return AccountDataPage(rows: rows, next: map.int64("next"), readable: map["readable"]?.boolValue ?? true, reason: map.str("reason"))
    }

    public func appending(_ more: AccountDataPage) -> AccountDataPage {
        AccountDataPage(rows: rows + more.rows, next: more.next, readable: more.readable, reason: more.reason)
    }

    public func removing(rowid: Int64) -> AccountDataPage {
        AccountDataPage(rows: rows.filter { $0.rowid != rowid }, next: next, readable: readable, reason: reason)
    }

    public var reasonText: String {
        switch reason {
        case "organization_managers_only": return "This belongs to the organization. Ask one of its owners or admins for a copy."
        case "security_credential": return "These are security credentials, such as keys and sign-in secrets. ForkMesh counts them here but never shows or downloads them."
        default: return ""
        }
    }
}

@MainActor
@Observable
public final class AccountDataBrowser {
    public private(set) var inventory: AccountDataInventory?
    public private(set) var pages: [String: AccountDataPage] = [:]
    public private(set) var loading = false
    public private(set) var loadingGroup = ""
    public private(set) var error = ""
    public private(set) var rowsError = ""

    @ObservationIgnored private let client: AccountEndpointCalling

    public init(client: AccountEndpointCalling) {
        self.client = client
    }

    public func load(refresh: Bool = false) async {
        loading = true
        defer { loading = false }
        do {
            inventory = AccountDataInventory.parse(try await client.accountCall("GET", AccountDataPaths.inventory(refresh: refresh), nil))
            if refresh { pages = [:] }
            error = ""
        } catch let failure as AccountEndpointError where failure.status == 404 || failure.status == 405 {
            error = "This server cannot list your data yet."
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not load your data.")
        }
    }

    public func page(_ group: AccountDataGroup) -> AccountDataPage? { pages[group.id] }

    public func loadRows(_ group: AccountDataGroup, more: Bool = false) async {
        let current = pages[group.id]
        if more, (current?.next ?? 0) == 0 { return }
        let after = more ? current?.next ?? 0 : 0
        loadingGroup = group.id
        defer { loadingGroup = "" }
        do {
            let map = try await client.accountCall("GET", AccountDataPaths.rows(subject: group.subject, table: group.table, after: after), nil)
            let page = AccountDataPage.parse(map, offset: more ? current?.rows.count ?? 0 : 0)
            pages[group.id] = more ? (current ?? AccountDataPage()).appending(page) : page
            rowsError = ""
        } catch {
            rowsError = AccountErrorText.text(error, fallback: "Could not load these items.")
        }
    }

    @discardableResult
    public func delete(_ row: AccountDataRow, in group: AccountDataGroup) async -> Bool {
        guard row.deletable else { return false }
        do {
            _ = try await client.accountCall("DELETE", AccountDataPaths.row(table: group.table, rowid: row.rowid), nil)
            pages[group.id] = pages[group.id]?.removing(rowid: row.rowid)
            inventory = inventory?.removing(group: group.id, rows: 1)
            rowsError = ""
            return true
        } catch {
            rowsError = AccountErrorText.text(error, fallback: "Could not remove that item.")
            return false
        }
    }

    @discardableResult
    public func deleteStore(_ key: String?) async -> Bool {
        do {
            _ = try await client.accountCall("DELETE", key.map { AccountDataPaths.storeItem($0) } ?? AccountDataPaths.store, nil)
            inventory = inventory?.removingStore(key)
            error = ""
            return true
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not remove the saved data.")
            return false
        }
    }
}
