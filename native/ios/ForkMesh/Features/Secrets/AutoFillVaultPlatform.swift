import AuthenticationServices
import FMBase
import FMVault
import Foundation
import LocalAuthentication
import UIKit

@MainActor
final class AutoFillVaultPlatform: VaultPlatform {
    func autofillStatus() async -> VaultAutofillStatus {
        let state = await ASCredentialIdentityStore.shared.state()
        let context = LAContext()
        var error: NSError?
        let secure = context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error)
        return VaultAutofillStatus(
            supported: true, enabled: state.isEnabled, deviceSecure: secure, logins: VaultAutofillShared.loginCount)
    }

    func openAutofillSettings() async {
        if #available(iOS 18.0, *) {
            if await ASSettingsHelper.requestToTurnOnCredentialProviderExtension() { return }
        }
        do {
            try await ASSettingsHelper.openCredentialProviderAppSettings()
        } catch {
            if let url = URL(string: UIApplication.openSettingsURLString) {
                await UIApplication.shared.open(url)
            }
        }
    }

    @discardableResult
    func syncAutofill(account: String, logins: [VaultAutofillLogin]) async -> Bool {
        let context = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else {
            await clearAutofill()
            return false
        }
        let usable = VaultAutofillCache.usable(logins)
        do {
            try VaultAutofillShared.vault().store(account: account, logins: usable)
        } catch {
            Log.warning("Vault", "the AutoFill cache could not be written (\(error))")
            await clearAutofill()
            return false
        }
        VaultAutofillShared.loginCount = usable.count
        let linkStore = VaultAutofillShared.links()
        var links = linkStore.readLinks()
        let stored = links
        links.keep(Set(usable.map(\.id)))
        if links != stored { linkStore.writeLinks(links) }
        let identities = VaultAutofillServices.identities(VaultAutofillCache.identities(usable, links: links))
        let store = ASCredentialIdentityStore.shared
        let state = await store.state()
        if state.isEnabled {
            do {
                _ = try await store.replaceCredentialIdentities(with: identities)
            } catch {
                Log.warning("Vault", "AutoFill suggestions could not be updated (\(error))")
            }
        }
        return true
    }

    func clearAutofill() async {
        VaultAutofillShared.vault().clear()
        VaultAutofillShared.links().writeLinks(VaultAutofillLinks())
        VaultAutofillShared.loginCount = 0
        let store = ASCredentialIdentityStore.shared
        let state = await store.state()
        if state.isEnabled {
            _ = try? await store.removeAllCredentialIdentities()
        }
    }

    func takePendingSaves() async -> [VaultPendingSave] { [] }
}
