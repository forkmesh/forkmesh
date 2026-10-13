import FMBase
import Foundation
import Observation

public struct AccountExportFile: Equatable, Sendable {
    public let fileName: String
    public let json: String
    public let devices: Int
    public let sshKeys: Int
}

public enum AccountExport {
    public static let path = "/api/accounts/export"
    public static let schema = "forkmesh-account-export-v1"
    public static let secretFields: Set<String> = ["pass_hash", "pass_salt", "totp_secret", "avatar_png", "sessionToken", "seed"]

    public static func file(_ map: JSONObject, account: String, nowMillis: Int64) throws -> AccountExportFile {
        guard map.str("schema") == schema else { throw AccountEndpointError("unsupported_export") }
        var cleaned = map
        cleaned.removeValue(forKey: "ok")
        var record = map.obj("account")
        for field in secretFields { record.removeValue(forKey: field) }
        cleaned["account"] = .object(record)
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyy-MM-dd"
        let day = formatter.string(from: Date(timeIntervalSince1970: TimeInterval(nowMillis) / 1_000))
        let stem = account.lowercased().filter { $0.isLetter || $0.isNumber || $0 == "-" }
        return AccountExportFile(
            fileName: "forkmesh-\(stem.isEmpty ? "account" : stem)-export-\(day).json",
            json: JSON.encodePretty(.object(cleaned)),
            devices: map.list("devices").count,
            sshKeys: map.list("sshKeys").count)
    }
}

public struct SecurityReportDraft: Equatable, Sendable {
    public static let components: [(id: String, label: String)] = [
        ("identity", "Identity keys and signing"),
        ("relay", "Relay"),
        ("mirroring", "Mirroring"),
        ("chat", "Encrypted chat"),
        ("donations", "Donations"),
        ("website", "Website and dashboard"),
        ("client", "Apps (desktop, phone)"),
        ("other", "Something else"),
    ]
    public static let path = "/api/security/report"
    public static let maxTitle = 200
    public static let maxBody = 16_384
    public static let maxContact = 254

    public var title = ""
    public var body = ""
    public var component = "client"
    public var contact = ""

    public init(title: String = "", body: String = "", component: String = "client", contact: String = "") {
        self.title = title
        self.body = body
        self.component = component
        self.contact = contact
    }

    public static func == (lhs: SecurityReportDraft, rhs: SecurityReportDraft) -> Bool {
        lhs.title == rhs.title && lhs.body == rhs.body && lhs.component == rhs.component && lhs.contact == rhs.contact
    }

    public var problem: String? {
        if title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return "Add a short title." }
        if body.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return "Describe the vulnerability and how to reproduce it." }
        if !SecurityReportDraft.components.contains(where: { $0.id == component }) { return "Pick a component." }
        if body.count > SecurityReportDraft.maxBody { return "The description is too long." }
        return nil
    }

    public var payload: JSONObject {
        [
            "title": .string(String(title.trimmingCharacters(in: .whitespacesAndNewlines).prefix(SecurityReportDraft.maxTitle))),
            "body": .string(String(body.trimmingCharacters(in: .whitespacesAndNewlines).prefix(SecurityReportDraft.maxBody))),
            "component": .string(component),
            "contact": .string(String(contact.trimmingCharacters(in: .whitespacesAndNewlines).prefix(SecurityReportDraft.maxContact))),
        ]
    }
}

@MainActor
@Observable
public final class AccountDataModel {
    public private(set) var exportFile: AccountExportFile?
    public private(set) var busy = false
    public private(set) var error = ""
    public private(set) var reportSent = false
    public var report = SecurityReportDraft()

    @ObservationIgnored private let client: AccountEndpointCalling
    @ObservationIgnored private let clock: Clock

    public init(client: AccountEndpointCalling, clock: Clock = SystemClock()) {
        self.client = client
        self.clock = clock
    }

    @discardableResult
    public func prepareExport(account: String) async -> AccountExportFile? {
        busy = true
        defer { busy = false }
        do {
            let file = try AccountExport.file(
                try await client.accountCall("GET", AccountExport.path, nil), account: account, nowMillis: clock.nowMillis)
            exportFile = file
            error = ""
            return file
        } catch let failure as AccountEndpointError where failure.status == 404 || failure.status == 405 {
            error = "This server cannot export account data yet."
            return nil
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not export your account data.")
            return nil
        }
    }

    public func clearExport() {
        exportFile = nil
    }

    @discardableResult
    public func sendReport() async -> Bool {
        if let problem = report.problem {
            error = problem
            return false
        }
        busy = true
        defer { busy = false }
        do {
            _ = try await client.accountCall("POST", SecurityReportDraft.path, report.payload)
            report = SecurityReportDraft()
            reportSent = true
            error = ""
            return true
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not send the report. Email security@forkmesh.com instead.")
            return false
        }
    }
}
