import AuthenticationServices
import FMApi
import FMBase
import FMVault
import Foundation
import UIKit

@MainActor
enum VaultCredentialExchange {
    static let plistActivityType = "ASCredentialExchangeActivityType"
    static let destinationLabel = "the app you chose"
    private static var waiting: VaultIncomingImport?

    static var supported: Bool {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) { return true }
        #endif
        return false
    }

    static var activityType: String {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) { return ASCredentialExchangeActivity }
        #endif
        return plistActivityType
    }

    static func receive(_ activity: NSUserActivity) {
        #if compiler(>=6.2)
        guard #available(iOS 26.0, *) else { return }
        guard activity.activityType == ASCredentialExchangeActivity,
              let token = activity.userInfo?[ASCredentialImportToken] as? UUID
        else { return }
        Task { @MainActor in
            do {
                let data = try await ASCredentialImportManager().importCredentials(token: token)
                let source = data.exporterDisplayName.trimmingCharacters(in: .whitespacesAndNewlines)
                deliver(VaultIncomingImport(source: source.isEmpty ? "another app" : source, logins: logins(from: data)))
            } catch {
                Log.warning("Vault", "a Credential Exchange import failed (\(error))")
            }
        }
        #endif
    }

    static func deliver(_ incoming: VaultIncomingImport) {
        guard !incoming.logins.isEmpty else { return }
        if let vault = Services.get(VaultService.self) {
            vault.receiveImport(incoming)
        } else {
            waiting = incoming
        }
        NotificationActions.open(route: .vault(section: nil), href: "")
    }

    static func handOver(to vault: VaultService) {
        if let incoming = waiting {
            waiting = nil
            vault.receiveImport(incoming)
        }
    }

    static func export(_ logins: [VaultTransferLogin]) async throws -> String? {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            guard let window = anchorWindow() else { throw VaultError("exchange_unavailable") }
            let manager = ASCredentialExportManager(presentationAnchor: window)
            do {
                let options = try await manager.requestExport(for: nil)
                let account = Services.get(VaultService.self)?.account ?? ""
                try await manager.exportCredentials(exportData(logins, account: account, formatVersion: options.formatVersion))
            } catch {
                if cancelled(error) { return nil }
                throw error
            }
            return destinationLabel
        }
        #endif
        throw VaultError("exchange_unavailable")
    }

    static func failureText(_ error: Error) -> String {
        if let error = error as? VaultError, error.code == "exchange_unavailable" {
            return VaultTransferModel.exchangeUnavailable
        }
        return "The logins were not sent. Check that the other app is installed and accepts imports, and that this iPhone has a passcode."
    }

    static func cancelled(_ error: Error) -> Bool {
        if error is CancellationError { return true }
        let ns = error as NSError
        if ns.domain == ASAuthorizationError.errorDomain && ns.code == ASAuthorizationError.Code.canceled.rawValue { return true }
        return ns.domain == ASExtensionErrorDomain && ns.code == ASExtensionError.Code.userCanceled.rawValue
    }

    static func anchorWindow() -> UIWindow? {
        let windows = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .filter { $0.activationState == .foregroundActive }
            .flatMap(\.windows)
        return windows.first(where: \.isKeyWindow) ?? windows.first
    }

    #if compiler(>=6.2)
    @available(iOS 26.0, *)
    static func logins(from data: ASExportedCredentialData) -> [VaultTransferLogin] {
        var out: [VaultTransferLogin] = []
        for account in data.accounts {
            for item in account.items {
                var basics: [ASImportableCredential.BasicAuthentication] = []
                var notes: [String] = []
                var otpauth = ""
                for credential in item.credentials {
                    switch credential {
                    case .basicAuthentication(let basic):
                        basics.append(basic)
                    case .note(let note):
                        notes.append(note.content.value)
                    case .totp(let totp):
                        if otpauth.isEmpty { otpauth = VaultOneTimeCode.uri(totp, fallbackLabel: item.title) }
                    default:
                        break
                    }
                }
                let url = item.scope?.urls.first?.absoluteString ?? ""
                for basic in basics {
                    let username = basic.userName?.value ?? ""
                    let password = basic.password?.value ?? ""
                    if username.isEmpty && password.isEmpty { continue }
                    out.append(VaultTransferLogin(
                        title: item.title, url: url.isEmpty ? (item.subtitle ?? "") : url, username: username,
                        password: password, notes: notes.joined(separator: "\n\n"), otpauth: otpauth))
                }
            }
        }
        return out
    }

    @available(iOS 26.0, *)
    static func exportData(
        _ logins: [VaultTransferLogin], account: String, formatVersion: ASExportedCredentialData.FormatVersion
    ) -> ASExportedCredentialData {
        let now = Date()
        let items = logins.map { (login: VaultTransferLogin) -> ASImportableItem in
            var credentials: [ASImportableCredential] = [
                .basicAuthentication(ASImportableCredential.BasicAuthentication(
                    userName: ASImportableEditableField(id: nil, fieldType: .string, value: login.username),
                    password: ASImportableEditableField(id: nil, fieldType: .concealedString, value: login.password))),
            ]
            if let totp = VaultOneTimeCode.credential(login.otpauth) {
                credentials.append(.totp(totp))
            }
            if !login.notes.isEmpty {
                credentials.append(.note(ASImportableCredential.Note(
                    content: ASImportableEditableField(id: nil, fieldType: .string, value: login.notes))))
            }
            let scope = URL(string: VaultCrypto.loginHref(login.url)).map {
                ASImportableCredentialScope(urls: [$0], androidApps: [])
            }
            return ASImportableItem(
                id: withUnsafeBytes(of: UUID().uuid) { Data($0) }, created: now, lastModified: now,
                title: login.displayTitle, subtitle: nil, favorite: false, scope: scope, credentials: credentials, tags: [])
        }
        let owner = ASImportableAccount(
            id: Data((account.isEmpty ? "forkmesh" : account).utf8), userName: account, email: "", fullName: nil,
            collections: [], items: items)
        return ASExportedCredentialData(
            accounts: [owner], formatVersion: formatVersion, exporterRelyingPartyIdentifier: "forkmesh.com",
            exporterDisplayName: "ForkMesh", timestamp: now)
    }
    #endif
}

#if compiler(>=6.2)
@available(iOS 26.0, *)
enum VaultOneTimeCode {
    static func uri(_ totp: ASImportableCredential.TOTP, fallbackLabel: String) -> String {
        let algorithm: VaultOTPAuth.Algorithm
        switch totp.algorithm {
        case .sha256: algorithm = .sha256
        case .sha512: algorithm = .sha512
        default: algorithm = .sha1
        }
        guard !totp.secret.isEmpty else { return "" }
        let account = totp.userName ?? ""
        return VaultOTPAuth(
            secret: [UInt8](totp.secret), period: Int(totp.period), digits: Int(totp.digits), algorithm: algorithm,
            issuer: totp.issuer ?? "", account: account.isEmpty ? fallbackLabel : account
        ).uri
    }

    static func credential(_ uri: String) -> ASImportableCredential.TOTP? {
        guard !uri.isEmpty, let code = VaultOTPAuth(uri: uri) else { return nil }
        let algorithm: ASImportableCredential.TOTP.Algorithm
        switch code.algorithm {
        case .sha1: algorithm = .sha1
        case .sha256: algorithm = .sha256
        case .sha512: algorithm = .sha512
        }
        return ASImportableCredential.TOTP(
            secret: Data(code.secret), period: UInt16(clamping: code.period), digits: UInt16(clamping: code.digits),
            userName: code.account.isEmpty ? nil : code.account, algorithm: algorithm,
            issuer: code.issuer.isEmpty ? nil : code.issuer)
    }
}
#endif
