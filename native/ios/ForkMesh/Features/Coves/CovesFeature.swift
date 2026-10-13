import FMAccount
import FMApi
import FMBase
import FMChat
import FMCoves
import FMCrypto
import Foundation
import SwiftUI

@MainActor
enum CovesFeature {
    private static var models: [String: CovesModel] = [:]

    static var api: ApiService? { Services.get(ApiService.self) }
    static var account: String { Services.get(AuthService.self)?.session?.nodeName ?? "" }
    static var openerKey: String { Services.get(DeviceIdentity.self)?.publicKeyB64url ?? "" }

    static func model(for repo: RepoRef) -> CovesModel? {
        guard let api else { return nil }
        let key = repo.slug.lowercased()
        let current = account
        if let existing = models[key] {
            if CoveStore.normalizedAccount(existing.account) != CoveStore.normalizedAccount(current) {
                existing.forget()
                existing.account = current
            }
            existing.openerKey = openerKey
            return existing
        }
        let model = CovesModel(repo: repo, account: current, openerKey: openerKey, io: io(api: api))
        model.notifier = notifier
        models[key] = model
        return model
    }

    static let notifier: CoveNotifier = CoveNotifier(
        invite: { (notice: CoveInviteNotice) -> Bool in
            guard let relay: RelayService = Services.get(RelayService.self) else { return false }
            return relay.sendCoveInvite(
                inviteeAccount: notice.inviteeAccount, coveId: notice.coveId, coveName: notice.coveName,
                inviterName: notice.inviterName, shareUrl: notice.shareUrl, ts: notice.ts)
        },
        opened: { (notice: CoveOpenNotice) -> Bool in
            guard let relay: RelayService = Services.get(RelayService.self),
                  let identity: DeviceIdentity = Services.get(DeviceIdentity.self),
                  identity.publicKeyB64url == notice.openerKey
            else { return false }
            let signature: String = identity.sign(notice.canonical)
            return relay.sendCoveOpen(
                creatorKey: notice.creatorKey, coveId: notice.coveId, coveName: notice.coveName,
                openerKey: notice.openerKey, openerName: notice.openerName, ts: notice.ts, signature: signature)
        })

    static func lockEverything() {
        for model in models.values { model.lockAll() }
    }

    static func forgetEverything() {
        for model in models.values { model.forget() }
        models.removeAll()
    }

    private static func io(api: ApiService) -> CoveRepositoryIO {
        CoveRepositoryIO(
            listFiles: { repo in
                do {
                    let tree = try await api.tree(repo.owner, repo.name, path: CoveStore.covesDir)
                    return tree.entries.filter { $0.type == "file" }.map { CoveFileRef(path: $0.path, size: $0.size) }
                } catch {
                    let message = "\(error)"
                    if message.contains("404") || message.lowercased().contains("not found") { return [] }
                    throw error
                }
            },
            readFile: { repo, path, maxBytes in
                [UInt8](try await api.rawBytes(repo.owner, repo.name, path, maxBytes: maxBytes))
            },
            propose: { repo, title, head, patch, description in
                guard let inbox = Services.get(InboxService.self) else {
                    throw ApiException("This device has no signing identity, so it cannot propose changes.")
                }
                let branches = (try? await api.branches(repo.owner, repo.name)) ?? []
                let base = branches.first(where: \.isDefault)?.name ?? branches.first?.name ?? "main"
                try await inbox.submitNewPull(
                    repo.owner, repo.name, title: title, base: base, head: head, patch: patch,
                    description: description, authorName: CovesFeature.account)
            },
            checkAccount: { name in
                do {
                    let encoded = name.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? name
                    let record = try await api.accountApiCall("GET", "/api/accounts/\(encoded)")
                    let exists = record["exists"]?.boolValue ?? true
                    let status = record["status"]?.stringValue ?? "active"
                    return CoveAccountCheck(exists: exists, active: status == "active")
                } catch {
                    return CoveAccountCheck(exists: false, active: false)
                }
            })
    }

    static func authenticate(reason: String) async -> Bool {
        await AgentOpsFeature.authenticate(reason: reason)
    }
}
