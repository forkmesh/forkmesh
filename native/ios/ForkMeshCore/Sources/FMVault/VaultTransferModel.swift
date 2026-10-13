import FMBase
import Foundation
import Observation

public struct VaultExportFile: Equatable, Sendable {
    public let text: String
    public let count: Int
}

public struct VaultImportReview: Equatable, Sendable {
    public let source: String
    public let logins: [VaultTransferLogin]
    public let fresh: Int
    public let duplicates: Int
    public let skippedRows: Int

    public var title: String { "Import from \(source)?" }

    public var message: String {
        var parts = ["\(fresh) new login\(fresh == 1 ? "" : "s") will be encrypted into your vault."]
        if duplicates > 0 {
            parts.append("\(duplicates) already in the vault will be skipped.")
        }
        if skippedRows > 0 {
            parts.append("\(skippedRows) row\(skippedRows == 1 ? " has" : "s have") no username or password and will be left out.")
        }
        return parts.joined(separator: " ")
    }

    public var canImport: Bool { fresh > 0 }
}

@Observable
@MainActor
public final class VaultTransferModel {
    public static let header = "Move passwords"
    public static let explanation =
        "Bring logins in from Apple Passwords, DuckDuckGo, or another password manager, or move them out again. Duplicates are skipped."
    public static let importFileButton = "Import from a file"
    public static let exportFileButton = "Export to a file"
    public static let exchangeExportButton = "Send to another app"
    public static let exchangeNote =
        "On iOS 26, Apple Passwords, DuckDuckGo and other apps that support Credential Exchange take logins straight from ForkMesh. To bring logins in, open Passwords, tap \u{2026} \u{2192} Export Data to Another App and pick ForkMesh."
    public static let fileHelp =
        "Apple Passwords on iPhone: Settings \u{2192} Apps \u{2192} Safari \u{2192} Export, then import the ZIP file here. DuckDuckGo exports a CSV file from its Passwords settings on Mac and Windows. To move logins out, export a file and import it in Settings \u{2192} Apps \u{2192} Safari \u{2192} Import, or in DuckDuckGo's Import Passwords."
    public static let importButton = "Import"
    public static let exportConfirmTitle = "Export every login unencrypted?"
    public static let exportConfirmMessage =
        "The file holds your passwords in plain text. Save it only where the app you are moving to can import it, and delete it afterwards. Apple Passwords and DuckDuckGo can both import it."
    public static let exportConfirmButton = "Export"
    public static let exchangeUnavailable = "Sending to another app needs iOS 26 or newer. Export to a file instead."
    public static let nothingToExport = "There are no logins to export yet."
    public static let fileUnreadable = "That file could not be opened."

    @ObservationIgnored public let vault: VaultService

    public private(set) var busy = false
    public private(set) var message = ""
    public var review: VaultImportReview?
    public var confirmingExport = false
    public private(set) var exportFile: VaultExportFile?

    public init(vault: VaultService) {
        self.vault = vault
    }

    public var available: Bool { vault.phase == .unlocked }

    public func prepare(source: String, logins: [VaultTransferLogin], skippedRows: Int = 0) {
        message = ""
        vault.touch()
        let plan = VaultTransfer.plan(logins, existing: vault.items)
        review = VaultImportReview(
            source: source, logins: logins, fresh: plan.fresh.count, duplicates: plan.duplicates, skippedRows: skippedRows)
        if !plan.fresh.isEmpty { return }
        review = nil
        message = plan.duplicates > 0
            ? "Every login from \(source) is already in your vault."
            : "\(source.prefix(1).uppercased() + source.dropFirst()) had no logins to import."
    }

    public func prepareFile(_ data: Data) {
        do {
            let read = try VaultTransfer.readCSV(data)
            prepare(source: read.source.label, logins: read.logins, skippedRows: read.skippedRows)
        } catch {
            review = nil
            message = Self.errorText(error)
        }
    }

    public func fileFailed(_ error: Error) {
        review = nil
        if let error = error as? VaultError, Self.words[error.code] != nil {
            message = Self.errorText(error)
        } else {
            message = Self.fileUnreadable
        }
    }

    public func takeIncoming() {
        guard available, let incoming = vault.takeIncomingImport() else { return }
        prepare(source: incoming.source, logins: incoming.logins)
    }

    public func cancelImport() {
        review = nil
    }

    public func confirmImport() async {
        guard let review, review.canImport else { return }
        self.review = nil
        busy = true
        defer { busy = false }
        do {
            let report = try await vault.importLogins(review.logins)
            message = Self.reportText(report, source: review.source)
        } catch {
            message = VaultCopy.errorText(error)
        }
    }

    public func exportLogins() -> [VaultTransferLogin]? {
        message = ""
        do {
            let logins = try vault.exportLogins()
            if logins.isEmpty {
                message = Self.nothingToExport
                return nil
            }
            return logins
        } catch {
            message = VaultCopy.errorText(error)
            return nil
        }
    }

    public func exportCSV() -> String? {
        exportLogins().map(VaultTransfer.writeCSV)
    }

    public func requestExport() {
        message = ""
        confirmingExport = true
    }

    public func confirmExport() {
        confirmingExport = false
        guard let logins = exportLogins() else { return }
        exportFile = VaultExportFile(text: VaultTransfer.writeCSV(logins), count: logins.count)
    }

    public func finishExport(savedAs name: String?, failure: String = "") {
        let count = exportFile?.count ?? 0
        exportFile = nil
        if let name {
            message = "\(count) login\(count == 1 ? "" : "s") saved to \(name). Delete the file once the other app has imported it."
        } else if !failure.isEmpty {
            message = "The file was not saved (\(failure))."
        }
    }

    public func exported(_ count: Int, to destination: String) {
        message = "\(count) login\(count == 1 ? "" : "s") sent to \(destination)."
    }

    public func exportFailed(_ text: String) {
        message = text
    }

    public static func reportText(_ report: VaultImportReport, source: String) -> String {
        var parts = ["Imported \(report.added) login\(report.added == 1 ? "" : "s") from \(source)."]
        if report.duplicates > 0 {
            parts.append("Skipped \(report.duplicates) already in the vault.")
        }
        if !report.stoppedBy.isEmpty {
            parts.append("Stopped with \(report.remaining) left: \(report.stoppedBy)")
        }
        return parts.joined(separator: " ")
    }

    static let words: [String: String] = [
        "csv_too_large": "That file is too large to be a password export.",
        "csv_unreadable": "That file is not a text CSV file.",
        "csv_empty": "That file is empty.",
        "csv_unknown_columns": "That file has no Password column. Export a CSV file from your password manager and try again.",
        "csv_no_logins": "That file has no logins in it.",
        "zip_unreadable": "That ZIP file is damaged or uses a format this app cannot open.",
        "zip_encrypted": "That ZIP file is password-protected. Export it again without a password.",
        "zip_no_csv": "That ZIP file has no CSV file of passwords in it.",
    ]

    public static func errorText(_ error: Error) -> String {
        if let error = error as? VaultError, let text = words[error.code] { return text }
        return VaultCopy.errorText(error)
    }
}
