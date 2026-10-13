import FMBase
import Foundation

public struct VaultTransferLogin: Equatable, Sendable {
    public var title: String
    public var url: String
    public var username: String
    public var password: String
    public var notes: String
    public var otpauth: String

    public init(title: String = "", url: String = "", username: String = "", password: String = "", notes: String = "", otpauth: String = "") {
        self.title = title
        self.url = url
        self.username = username
        self.password = password
        self.notes = notes
        self.otpauth = otpauth
    }

    public var host: String { VaultCrypto.loginHost(url) }

    public var displayTitle: String {
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty { return trimmed }
        if !host.isEmpty { return host }
        let user = username.trimmingCharacters(in: .whitespacesAndNewlines)
        return user.isEmpty ? VaultTransfer.untitled : user
    }

    public var fields: JSONObject {
        var body = notes.trimmingCharacters(in: .whitespacesAndNewlines)
        let code = otpauth.trimmingCharacters(in: .whitespacesAndNewlines)
        if !code.isEmpty && !body.contains(code) {
            body = body.isEmpty ? code : body + "\n\n" + code
        }
        return [
            "title": .string(displayTitle),
            "url": .string(url.trimmingCharacters(in: .whitespacesAndNewlines)),
            "username": .string(username),
            "password": .string(password),
            "notes": .string(body),
        ]
    }

    public init(item: VaultItem) {
        var noteLines: [String] = []
        var code = ""
        for line in item.notes.components(separatedBy: "\n") {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if code.isEmpty && trimmed.lowercased().hasPrefix("otpauth://") {
                code = trimmed
            } else {
                noteLines.append(line)
            }
        }
        self.init(
            title: item.title, url: item.url, username: item.username, password: item.password,
            notes: noteLines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines), otpauth: code)
    }

    var identity: String {
        let user = username.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let place = host.isEmpty ? displayTitle.lowercased() : host
        return place + "\u{0}" + user + "\u{0}" + password
    }
}

public enum VaultTransferSource: String, CaseIterable, Sendable {
    case applePasswords
    case duckDuckGo
    case chrome
    case firefox
    case bitwarden
    case onePassword
    case other

    public var label: String {
        switch self {
        case .applePasswords: return "Apple Passwords"
        case .duckDuckGo: return "DuckDuckGo"
        case .chrome: return "Chrome"
        case .firefox: return "Firefox"
        case .bitwarden: return "Bitwarden"
        case .onePassword: return "1Password"
        case .other: return "a password manager"
        }
    }
}

public struct VaultCSVImport: Equatable, Sendable {
    public let source: VaultTransferSource
    public let logins: [VaultTransferLogin]
    public let skippedRows: Int
}

public struct VaultImportPlan: Equatable, Sendable {
    public let fresh: [VaultTransferLogin]
    public let duplicates: Int
}

public struct VaultImportReport: Equatable, Sendable {
    public let added: Int
    public let duplicates: Int
    public let remaining: Int
    public let stoppedBy: String

    public init(added: Int, duplicates: Int, remaining: Int = 0, stoppedBy: String = "") {
        self.added = added
        self.duplicates = duplicates
        self.remaining = remaining
        self.stoppedBy = stoppedBy
    }
}

public enum VaultTransfer {
    public static let untitled = "Imported login"
    public static let exportFileName = "ForkMesh Passwords.csv"
    public static let exportHeader = ["Title", "URL", "Username", "Password", "Notes", "OTPAuth"]
    public static let maxFileBytes = 16 * 1024 * 1024

    static let columns: [(field: String, names: [String])] = [
        ("title", ["title", "name", "account", "item name", "login name", "display name"]),
        ("url", ["url", "login_uri", "uri", "website", "web site", "web address", "site", "hostname", "login url", "urls"]),
        ("username", ["username", "login_username", "user name", "user", "login", "email", "email address", "user id"]),
        ("password", ["password", "login_password", "pass"]),
        ("notes", ["notes", "note", "comments", "comment", "extra"]),
        ("otpauth", ["otpauth", "login_totp", "totp", "otp", "one-time password", "otp auth", "otp url"]),
    ]

    public static func parseCSV(_ text: String) -> [[String]] {
        var rows: [[String]] = []
        var row: [String] = []
        var field = ""
        var quoted = false
        var atFieldStart = true
        var scalars = Array(text.unicodeScalars)
        if scalars.first == "\u{FEFF}" { scalars.removeFirst() }
        var i = 0
        func endField() {
            row.append(field)
            field = ""
            atFieldStart = true
        }
        func endRow() {
            endField()
            if !(row.count == 1 && row[0].isEmpty) { rows.append(row) }
            row = []
        }
        while i < scalars.count {
            let c = scalars[i]
            if quoted {
                if c == "\"" {
                    if i + 1 < scalars.count && scalars[i + 1] == "\"" {
                        field.unicodeScalars.append("\"")
                        i += 1
                    } else {
                        quoted = false
                    }
                } else {
                    field.unicodeScalars.append(c)
                }
            } else if c == "\"" && atFieldStart {
                quoted = true
                atFieldStart = false
            } else if c == "," {
                endField()
            } else if c == "\r" {
                if i + 1 < scalars.count && scalars[i + 1] == "\n" { i += 1 }
                endRow()
            } else if c == "\n" {
                endRow()
            } else {
                field.unicodeScalars.append(c)
                atFieldStart = false
            }
            i += 1
        }
        if !field.isEmpty || !row.isEmpty || quoted { endRow() }
        return rows
    }

    static func normalized(_ header: String) -> String {
        header.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    public static func source(forHeader header: [String]) -> VaultTransferSource {
        let names = Set(header.map(normalized))
        if names.contains("login_uri") || names.contains("login_password") { return .bitwarden }
        if names.contains("httprealm") || names.contains("formactionorigin") || names.contains("guid") { return .firefox }
        if names.contains("otpauth") { return .applePasswords }
        if names.isSuperset(of: ["name", "url", "username", "password", "note"]) { return .chrome }
        if names.contains("archived") || names.contains("one-time password") { return .onePassword }
        if names.isSuperset(of: ["title", "url", "username", "password", "notes"]) { return .duckDuckGo }
        return .other
    }

    public static func readCSV(_ data: Data) throws -> VaultCSVImport {
        if data.count > maxFileBytes { throw VaultError("csv_too_large") }
        if VaultZip.isZip(data) { return try readCSV(VaultZip.firstCSV(in: data, maximumBytes: maxFileBytes)) }
        guard let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) else {
            throw VaultError("csv_unreadable")
        }
        return try readCSV(text)
    }

    public static func readCSV(_ text: String) throws -> VaultCSVImport {
        let rows = parseCSV(text)
        guard let header = rows.first else { throw VaultError("csv_empty") }
        let names = header.map(normalized)
        var index: [String: Int] = [:]
        for (field, aliases) in columns {
            for alias in aliases {
                if let at = names.firstIndex(of: alias) {
                    index[field] = at
                    break
                }
            }
        }
        guard index["password"] != nil else { throw VaultError("csv_unknown_columns") }
        var logins: [VaultTransferLogin] = []
        var skipped = 0
        for row in rows.dropFirst() {
            func value(_ field: String) -> String {
                guard let at = index[field], at < row.count else { return "" }
                return row[at]
            }
            let login = VaultTransferLogin(
                title: value("title").trimmingCharacters(in: .whitespacesAndNewlines),
                url: value("url").trimmingCharacters(in: .whitespacesAndNewlines),
                username: value("username"),
                password: value("password"),
                notes: value("notes"),
                otpauth: value("otpauth").trimmingCharacters(in: .whitespacesAndNewlines))
            if login.password.isEmpty && login.username.trimmingCharacters(in: .whitespaces).isEmpty {
                if row.contains(where: { !$0.trimmingCharacters(in: .whitespaces).isEmpty }) { skipped += 1 }
                continue
            }
            logins.append(login)
        }
        if logins.isEmpty { throw VaultError("csv_no_logins") }
        return VaultCSVImport(source: source(forHeader: header), logins: logins, skippedRows: skipped)
    }

    public static func writeCSV(_ logins: [VaultTransferLogin]) -> String {
        var lines = [exportHeader.map(quote).joined(separator: ",")]
        for login in logins {
            lines.append([login.title, login.url, login.username, login.password, login.notes, login.otpauth]
                .map(quote).joined(separator: ","))
        }
        return lines.joined(separator: "\r\n") + "\r\n"
    }

    static func quote(_ value: String) -> String {
        let needsQuotes = value.unicodeScalars.contains { $0 == "," || $0 == "\"" || $0 == "\n" || $0 == "\r" }
            || value.hasPrefix(" ") || value.hasSuffix(" ")
        return needsQuotes ? "\"" + value.replacingOccurrences(of: "\"", with: "\"\"") + "\"" : value
    }

    public static func logins(from items: [VaultItem]) -> [VaultTransferLogin] {
        items.filter { $0.readable && !($0.password.isEmpty && $0.username.isEmpty) }
            .map(VaultTransferLogin.init(item:))
            .sorted { ($0.displayTitle.lowercased(), $0.username.lowercased()) < ($1.displayTitle.lowercased(), $1.username.lowercased()) }
    }

    public static func plan(_ incoming: [VaultTransferLogin], existing: [VaultItem]) -> VaultImportPlan {
        var seen = Set(existing.filter(\.readable).map { VaultTransferLogin(item: $0).identity })
        var fresh: [VaultTransferLogin] = []
        var duplicates = 0
        for login in incoming {
            let key = login.identity
            if seen.contains(key) {
                duplicates += 1
            } else {
                seen.insert(key)
                fresh.append(login)
            }
        }
        return VaultImportPlan(fresh: fresh, duplicates: duplicates)
    }
}
