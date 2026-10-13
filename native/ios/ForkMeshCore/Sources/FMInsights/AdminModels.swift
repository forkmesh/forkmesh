import FMBase
import Foundation
import Observation

public enum AdminSection: String, CaseIterable, Sendable {
    case overview, users, errors, database, usage, repos

    public var title: String {
        switch self {
        case .overview: return "Overview"
        case .users: return "Users"
        case .errors: return "Errors"
        case .database: return "Database"
        case .usage: return "Usage & jobs"
        case .repos: return "Repository purge"
        }
    }

    public var symbol: String {
        switch self {
        case .overview: return "gauge.with.dots.needle.33percent"
        case .users: return "person.2"
        case .errors: return "exclamationmark.octagon"
        case .database: return "cylinder.split.1x2"
        case .usage: return "clock.arrow.2.circlepath"
        case .repos: return "trash"
        }
    }

    public init(route: String?) {
        self = AdminSection(rawValue: (route ?? "").lowercased()) ?? .overview
    }
}

public struct AdminOverview: Equatable, Sendable {
    public var actor: String
    public var errors24h: Int?
    public var pendingVerifications: Int?
    public var relaysAwaitingApproval: Int?
    public var users: Int?
    public var quarantinedUsers: Int?

    public init(json: JSONObject) {
        actor = json.str("actor")
        errors24h = json["errors24h"]?.intValue
        pendingVerifications = json["pendingVerifications"]?.intValue
        relaysAwaitingApproval = json["relaysAwaitingApproval"]?.intValue
        users = json["users"]?.intValue
        quarantinedUsers = json["quarantinedUsers"]?.intValue
    }
}

public struct ErrorUser: Equatable, Sendable, Identifiable {
    public var name: String
    public var count: Int
    public var id: String { name }
}

public struct ErrorGroup: Equatable, Sendable, Identifiable {
    public var status: String
    public var method: String
    public var path: String
    public var message: String
    public var source: String
    public var count: Int
    public var firstSeen: Int64
    public var lastSeen: Int64
    public var hours: [Int]
    public var users: [ErrorUser]
    public var userCount: Int
    public var anonymous: Int

    public var id: String { "\(status) \(method) \(path) \(message)" }

    public init(json: JSONObject) {
        status = json.str("status")
        method = json.str("method")
        path = json.str("path")
        message = json.str("message")
        source = json.str("source")
        count = json.int("count")
        firstSeen = json.int64("firstSeen")
        lastSeen = json.int64("lastSeen")
        hours = json.list("hours").map { $0.int() }
        users = json.maps("users").map { ErrorUser(name: $0.str("name"), count: $0.int("count")) }
        userCount = json.int("userCount")
        anonymous = json.int("anonymous")
    }

    public var sourceLabel: String {
        switch source {
        case "desktop": return "Desktop"
        case "javascript": return "Browser"
        default: return "Server"
        }
    }

    public var title: String { message.isEmpty ? "HTTP \(status)" : message }

    public var requestLine: String { [method, path].filter { !$0.isEmpty }.joined(separator: " ") }

    public var affected: String {
        var parts: [String] = []
        if userCount > 0 { parts.append("\(userCount) user\(userCount == 1 ? "" : "s")") }
        if anonymous > 0 { parts.append("\(anonymous) anonymous") }
        return parts.joined(separator: " · ")
    }

    public func agentPrompt(host: String) -> String {
        var lines = [
            "Investigate this server error group from \(host.isEmpty ? "ForkMesh" : host) and fix its cause.",
            "",
            "Status: \(status)",
            "Request: \(requestLine)",
            "Source: \(sourceLabel)",
            "Occurrences in the last 24 hours: \(count)",
            "Message:",
            message,
        ]
        if userCount > 0 { lines.append("Accounts affected: \(userCount)") }
        return lines.joined(separator: "\n")
    }
}

public struct ErrorReport: Equatable, Sendable {
    public var total: Int
    public var hours: [Int]
    public var groupCount: Int
    public var usersHit: Int
    public var truncated: Bool
    public var groups: [ErrorGroup]

    public init(json: JSONObject) {
        total = json.int("total")
        hours = json.list("hours").map { $0.int() }
        groupCount = json.int("groupCount")
        usersHit = json.int("usersHit")
        truncated = json.bool("truncated")
        groups = json.maps("groups").map(ErrorGroup.init(json:))
    }

    public func filtered(source: String?) -> [ErrorGroup] {
        guard let source, !source.isEmpty else { return groups }
        return groups.filter { $0.source == source }
    }
}

public struct PendingVerification: Equatable, Sendable, Identifiable {
    public var name: String
    public var joinedAt: Int64
    public var id: String { name }
}

public struct AdminRelay: Equatable, Sendable, Identifiable {
    public var label: String
    public var baseUrl: String
    public var status: String
    public var registeredAt: Int64
    public var approvedAt: Int64
    public var id: String { baseUrl + label }
}

public struct DatabaseColumn: Equatable, Sendable, Identifiable {
    public var name: String
    public var type: String
    public var id: String { name }
}

public struct DatabaseTable: Equatable, Sendable, Identifiable {
    public var name: String
    public var rows: Int
    public var restricted: Bool
    public var storage: String
    public var plaintextData: Bool
    public var gaps: [String]
    public var auditCategory: String
    public var auditNote: String
    public var columns: [DatabaseColumn]
    public var id: String { name }

    public init(json: JSONObject) {
        name = json.str("name")
        rows = json.int("rows")
        restricted = json.bool("restricted")
        storage = json.str("storage")
        plaintextData = json.bool("plaintextData")
        gaps = json.strings("gaps")
        auditCategory = json.str("auditCategory")
        auditNote = json.str("auditNote")
        columns = json.maps("columns").map { DatabaseColumn(name: $0.str("name"), type: $0.str("type")) }
    }
}

public struct DatabaseInventory: Equatable, Sendable {
    public var sampledAt: Int64
    public var tables: [DatabaseTable]

    public init(json: JSONObject) {
        sampledAt = json.int64("sampledAt")
        tables = json.maps("tables").map(DatabaseTable.init(json:))
    }

    public var totalRows: Int { tables.reduce(0) { $0 + $1.rows } }

    public func filtered(_ query: String) -> [DatabaseTable] {
        let needle = query.trimmingCharacters(in: .whitespaces).lowercased()
        let sorted = tables.sorted { $0.rows != $1.rows ? $0.rows > $1.rows : $0.name < $1.name }
        return needle.isEmpty ? sorted : sorted.filter { $0.name.lowercased().contains(needle) }
    }
}

public struct DatabaseCell: Equatable, Sendable, Identifiable {
    public var column: String
    public var value: String
    public var id: String { column }
}

public struct DatabaseRow: Equatable, Sendable, Identifiable {
    public var index: Int
    public var rowid: String
    public var digest: String
    public var cells: [DatabaseCell]
    public var dataMode: String
    public var data: String
    public var walletKeys: Bool

    public var id: String { rowid.isEmpty ? "\(index):\(digest)" : rowid }

    public static let cellLimit = 400

    public init(index: Int, json: JSONObject, columns: [String]) {
        self.index = index
        rowid = json["rowid"].flatMap { $0.isNull ? nil : $0.text } ?? ""
        digest = json.str("digest")
        let values = json.obj("values")
        cells = columns.map { column in
            let value = values[column]
            let text: String
            if let value, !value.isNull {
                let raw = value.stringValue ?? JSON.encode(value)
                text = String(raw.prefix(DatabaseRow.cellLimit))
            } else {
                text = column == "data" && json["data"] != nil ? "(decoded below)" : "NULL"
            }
            return DatabaseCell(column: column, value: text)
        }
        dataMode = json.str("dataMode")
        if let data = json["data"], !data.isNull {
            self.data = String(JSON.encodePretty(data).prefix(4000))
        } else {
            data = ""
        }
        walletKeys = json.bool("walletKeys")
    }

    public var summary: String {
        cells.prefix(3).map { "\($0.column): \($0.value)" }.joined(separator: " · ")
    }
}

public struct DatabasePage: Equatable, Sendable {
    public var table: String
    public var columns: [String]
    public var rows: [DatabaseRow]
    public var nextCursor: String

    public init(json: JSONObject) {
        table = json.str("table")
        let names = json.maps("columns").map { $0.str("name") }
        columns = names
        rows = json.maps("rows").enumerated().map { DatabaseRow(index: $0.offset, json: $0.element, columns: names) }
        nextCursor = json.str("nextCursor")
    }
}

public enum DatabaseGrid {
    public static let minChars = 3
    public static let maxChars = 24

    public static func widths(columns: [String], rows: [DatabaseRow]) -> [Int] {
        columns.indices.map { index in
            let longest = rows.reduce(columns[index].count) { widest, row in
                guard row.cells.indices.contains(index) else { return widest }
                return max(widest, line(row.cells[index].value).count)
            }
            return min(max(longest, minChars), maxChars)
        }
    }

    public static func rowidWidth(_ rows: [DatabaseRow]) -> Int {
        min(max(rows.map(\.rowid.count).max() ?? 0, 1), maxChars)
    }

    public static func line(_ value: String) -> String {
        value.split(whereSeparator: \.isNewline).joined(separator: " ")
    }
}

public struct CronSummary: Equatable, Sendable {
    public struct Tick: Equatable, Sendable, Identifiable {
        public var ts: Int64
        public var ms: Int
        public var failures: Int
        public var ran: [String]
        public var failed: [String]
        public var id: Int64 { ts }
    }

    public var ticks: Int
    public var failedTicks: Int
    public var lastTickAt: Int64
    public var gapMs: Int64
    public var recent: [Tick]
    public var unavailable: String

    public init(json: JSONObject) {
        let summary = json.obj("summary")
        ticks = summary.int("ticks")
        failedTicks = summary.int("failedTicks")
        lastTickAt = summary.int64("lastTickAt")
        gapMs = summary.int64("gapMs")
        recent = json.maps("ticks").prefix(30).map {
            Tick(ts: $0.int64("ts"), ms: $0.int("ms"), failures: $0.int("failures"),
                 ran: $0.strings("ran"), failed: $0.strings("failed"))
        }
        unavailable = json.bool("ok") || json["ok"] == nil ? "" : json.str("error")
    }

    public var health: StatsTileStatus {
        if !unavailable.isEmpty { return .warning }
        if lastTickAt == 0 { return .none }
        if gapMs > 10 * 60 * 1000 { return .critical }
        if failedTicks > 0 { return .warning }
        return .good
    }
}

public struct StorageUsage: Equatable, Sendable {
    public struct Entry: Equatable, Sendable, Identifiable {
        public var label: String
        public var value: String
        public var id: String { label }
    }

    public var entries: [Entry]

    public init(json: JSONObject) {
        let storage = json.obj("storage")
        entries = storage.keys.sorted().compactMap { key in
            guard let value = storage[key], !value.isNull else { return nil }
            if let number = value.doubleValue {
                let lower = key.lowercased()
                let formatted = lower.contains("byte") || lower.hasSuffix("size")
                    ? StatsFormat.compactBytes(Int64(number)) : StatsFormat.compactNumber(number)
                return Entry(label: key, value: formatted)
            }
            if let text = value.stringValue { return Entry(label: key, value: text) }
            return nil
        }
    }
}

public struct AdminUser: Equatable, Sendable, Identifiable {
    public var name: String
    public var email: String
    public var emailVerified: Bool
    public var isAdmin: Bool
    public var createdAt: Int64
    public var status: String
    public var profilePrivate: Bool
    public var referralStatus: String
    public var signupCountry: String
    public var nodes: Int
    public var quarantined: Bool
    public var id: String { name }

    public init(json: JSONObject, quarantined: Bool = false) {
        name = json.str("name")
        email = json.str("email")
        emailVerified = json.bool("emailVerified")
        isAdmin = json.bool("isAdmin")
        let created = json.int64("createdAt")
        createdAt = created > 0 && created < 100_000_000_000 ? created * 1000 : created
        status = json.str("accountStatus").isEmpty ? json.str("status") : json.str("accountStatus")
        profilePrivate = json.bool("profilePrivate")
        referralStatus = json.str("referralStatus")
        signupCountry = json.str("signupCountry")
        nodes = json.list("nodes").count
        self.quarantined = quarantined
    }

    public var badges: [String] {
        var out: [String] = []
        if isAdmin { out.append("Admin") }
        if quarantined { out.append("Quarantined") }
        if !emailVerified { out.append("Unverified") }
        if profilePrivate { out.append("Private") }
        return out
    }
}

public struct AdminUsers: Equatable, Sendable {
    public var users: [AdminUser]
    public var quarantined: [AdminUser]

    public init(json: JSONObject) {
        users = json.maps("users").map { AdminUser(json: $0) }.filter { !$0.name.isEmpty }
        quarantined = json.maps("quarantinedUsers").map { AdminUser(json: $0, quarantined: true) }
            .filter { !$0.name.isEmpty }
    }

    public var hasAdminFields: Bool { users.contains { $0.isAdmin } || !quarantined.isEmpty }

    public func filtered(_ query: String, showQuarantined: Bool) -> [AdminUser] {
        let pool = showQuarantined ? quarantined : users
        let needle = query.trimmingCharacters(in: .whitespaces).lowercased()
        let sorted = pool.sorted { $0.createdAt != $1.createdAt ? $0.createdAt > $1.createdAt : $0.name < $1.name }
        if needle.isEmpty { return sorted }
        return sorted.filter { $0.name.lowercased().contains(needle) || $0.email.lowercased().contains(needle) }
    }
}

public enum AdminUserAction: String, Sendable {
    case quarantine, unquarantine, delete

    public var path: String {
        switch self {
        case .quarantine: return "/api/accounts/admin-quarantine"
        case .unquarantine: return "/api/accounts/admin-unquarantine"
        case .delete: return "/api/accounts/admin-delete"
        }
    }

    public var title: String {
        switch self {
        case .quarantine: return "Quarantine"
        case .unquarantine: return "Restore"
        case .delete: return "Delete"
        }
    }

    public var pastTense: String {
        switch self {
        case .quarantine: return "Quarantined"
        case .unquarantine: return "Restored"
        case .delete: return "Deleted"
        }
    }

    public var needsDeviceAuth: Bool { self == .delete || self == .quarantine }
}

@MainActor
public final class AdminClient {
    public static let maxBatch = 25

    private let transport: InsightsRequesting

    public init(transport: InsightsRequesting) {
        self.transport = transport
    }

    private func get(_ section: String, _ query: [(String, String)] = []) async throws -> JSONObject {
        try await transport.call("GET", InsightsQuery.path("/api/admin/phone/\(section)", query), nil)
    }

    public func overview() async throws -> AdminOverview { AdminOverview(json: try await get("overview")) }
    public func errors() async throws -> ErrorReport { ErrorReport(json: try await get("errors")) }
    public func cron() async throws -> CronSummary { CronSummary(json: try await get("cron")) }
    public func storage() async throws -> StorageUsage { StorageUsage(json: try await get("storage")) }

    public func pending() async throws -> [PendingVerification] {
        try await get("pending").maps("pending").map {
            PendingVerification(name: $0.str("name"), joinedAt: $0.int64("joinedAt"))
        }
    }

    public func relays() async throws -> [AdminRelay] {
        try await get("relays").maps("relays").map {
            AdminRelay(label: $0.str("label"), baseUrl: $0.str("baseUrl"), status: $0.str("status"),
                       registeredAt: $0.int64("registeredAt"), approvedAt: $0.int64("approvedAt"))
        }
    }

    public func inventory(refresh: Bool = false) async throws -> DatabaseInventory {
        DatabaseInventory(json: try await get("database", [("op", "inventory"), ("refresh", refresh ? "1" : "")]))
    }

    public func rows(table: String, cursor: String = "", limit: Int = 25) async throws -> DatabasePage {
        DatabasePage(json: try await get(
            "database", [("op", "rows"), ("table", table), ("cursor", cursor), ("limit", String(min(max(limit, 1), 50)))]))
    }

    public func users() async throws -> AdminUsers {
        AdminUsers(json: try await transport.call("GET", "/api/accounts/users", nil))
    }

    public func perform(_ action: AdminUserAction, target: String) async throws {
        let name = target.trimmingCharacters(in: .whitespaces).lowercased()
        guard AdminClient.validName(name) else { throw InsightsError("invalid_user") }
        _ = try await transport.call("POST", action.path, ["target": .string(name)])
    }

    public func perform(_ action: AdminUserAction, targets: [String]) async -> [String: InsightsError] {
        var failures: [String: InsightsError] = [:]
        for target in targets.prefix(AdminClient.maxBatch) {
            do {
                try await perform(action, target: target)
            } catch {
                failures[target] = InsightsError.from(error)
            }
        }
        return failures
    }

    public func purgeRepository(owner: String, name: String) async throws {
        let owner = owner.trimmingCharacters(in: .whitespaces)
        let name = name.trimmingCharacters(in: .whitespaces)
        guard AdminClient.validSegment(owner), AdminClient.validSegment(name) else { throw InsightsError("invalid_repository") }
        _ = try await transport.call(
            "DELETE", InsightsQuery.path("/api/repositories", [("owner", owner), ("name", name)]), nil)
    }

    public nonisolated static func validName(_ name: String) -> Bool {
        guard !name.isEmpty, name.count <= 63 else { return false }
        return name.unicodeScalars.allSatisfy { CharacterSet.alphanumerics.contains($0) || $0 == "-" || $0 == "_" || $0 == "." }
            && name.unicodeScalars.allSatisfy(\.isASCII)
    }

    public nonisolated static func validSegment(_ segment: String) -> Bool {
        guard !segment.isEmpty, segment.count <= 100, segment != ".", segment != ".." else { return false }
        return segment.unicodeScalars.allSatisfy {
            $0.isASCII && (CharacterSet.alphanumerics.contains($0) || $0 == "-" || $0 == "_" || $0 == ".")
        }
    }

    public nonisolated static func purgeConfirmation(owner: String, name: String) -> String { "\(owner)/\(name)" }
}

@MainActor
@Observable
public final class AdminScreenModel {
    @ObservationIgnored public let client: AdminClient

    public var section: AdminSection
    public private(set) var overview: InsightsLoad<AdminOverview> = .idle
    public private(set) var errors: InsightsLoad<ErrorReport> = .idle
    public private(set) var users: InsightsLoad<AdminUsers> = .idle
    public private(set) var pending: InsightsLoad<[PendingVerification]> = .idle
    public private(set) var inventory: InsightsLoad<DatabaseInventory> = .idle
    public private(set) var cron: InsightsLoad<CronSummary> = .idle
    public private(set) var storage: InsightsLoad<StorageUsage> = .idle
    public private(set) var relays: InsightsLoad<[AdminRelay]> = .idle
    public private(set) var denied = false
    public private(set) var lastAction = ""
    public private(set) var actionError: InsightsError?
    public private(set) var busy: Set<String> = []

    public init(client: AdminClient, section: AdminSection = .overview) {
        self.client = client
        self.section = section
    }

    public convenience init(transport: InsightsRequesting, section: AdminSection = .overview) {
        self.init(client: AdminClient(transport: transport), section: section)
    }

    private func load<T: Equatable & Sendable>(_ current: InsightsLoad<T>, _ fetch: () async throws -> T) async
        -> InsightsLoad<T>
    {
        do {
            return .loaded(try await fetch())
        } catch {
            let failure = InsightsError.from(error)
            if failure.needsAdmin { denied = true }
            if let value = current.value { return .loaded(value) }
            return .failed(failure)
        }
    }

    public func reload() async {
        switch section {
        case .overview:
            if overview.value == nil { overview = .loading }
            overview = await load(overview) { try await client.overview() }
        case .users:
            if users.value == nil { users = .loading }
            users = await load(users) { try await client.users() }
            pending = await load(pending) { try await client.pending() }
        case .errors:
            if errors.value == nil { errors = .loading }
            errors = await load(errors) { try await client.errors() }
        case .database:
            if inventory.value == nil { inventory = .loading }
            inventory = await load(inventory) { try await client.inventory() }
        case .usage:
            if cron.value == nil { cron = .loading }
            cron = await load(cron) { try await client.cron() }
            storage = await load(storage) { try await client.storage() }
            relays = await load(relays) { try await client.relays() }
        case .repos:
            break
        }
    }

    public func perform(_ action: AdminUserAction, target: String) async -> Bool {
        busy.insert(target)
        defer { busy.remove(target) }
        do {
            try await client.perform(action, target: target)
            actionError = nil
            lastAction = "\(action.pastTense) \(target)"
            users = await load(users) { try await client.users() }
            return true
        } catch {
            actionError = InsightsError.from(error)
            return false
        }
    }

    public func purge(owner: String, name: String) async -> Bool {
        let key = "\(owner)/\(name)"
        busy.insert(key)
        defer { busy.remove(key) }
        do {
            try await client.purgeRepository(owner: owner, name: name)
            actionError = nil
            lastAction = "Purged \(key)"
            return true
        } catch {
            actionError = InsightsError.from(error)
            return false
        }
    }

    public func clearActionState() {
        actionError = nil
        lastAction = ""
    }
}
