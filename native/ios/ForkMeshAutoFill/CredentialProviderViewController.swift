import AuthenticationServices
import FMVault
import LocalAuthentication
import SwiftUI
import UIKit

final class CredentialProviderViewController: ASCredentialProviderViewController {
    private var host: UIHostingController<AutoFillListView>?
    private var serviceIdentifiers: [String] = []

    override func prepareCredentialList(for serviceIdentifiers: [ASCredentialServiceIdentifier]) {
        let identifiers = serviceIdentifiers.map(VaultAutofillServices.encode)
        self.serviceIdentifiers = identifiers
        let model = AutoFillListModel(serviceIdentifiers: identifiers, siteLabel: VaultAutofillServices.label(serviceIdentifiers))
        show(model)
        Task { @MainActor in
            await model.unlock { [weak self] context in try self?.loadLogins(context: context) ?? [] }
        }
    }

    override func provideCredentialWithoutUserInteraction(for credentialIdentity: ASPasswordCredentialIdentity) {
        cancel(.userInteractionRequired)
    }

    override func prepareInterfaceToProvideCredential(for credentialIdentity: ASPasswordCredentialIdentity) {
        let wanted = credentialIdentity.recordIdentifier ?? ""
        let service = credentialIdentity.serviceIdentifier
        serviceIdentifiers = [VaultAutofillServices.encode(service)]
        let model = AutoFillListModel(serviceIdentifiers: serviceIdentifiers, siteLabel: VaultAutofillServices.label([service]))
        show(model)
        Task { @MainActor in
            await model.unlock { [weak self] context in try self?.loadLogins(context: context) ?? [] }
            if let login = model.logins.first(where: { $0.id == wanted }) {
                self.complete(with: login)
            } else if model.failure == nil {
                model.failure = "That login is no longer in your vault. Open ForkMesh to refresh AutoFill."
            }
        }
    }

    @available(iOS 18.0, *)
    override func prepareInterfaceForUserChoosingTextToInsert() {
        serviceIdentifiers = []
        let model = AutoFillListModel(serviceIdentifiers: [], inserting: true)
        show(model)
        Task { @MainActor in
            await model.unlock { [weak self] context in try self?.loadLogins(context: context) ?? [] }
        }
    }

    override func prepareInterfaceForExtensionConfiguration() {
        show(AutoFillListModel(serviceIdentifiers: [], configurationOnly: true))
    }

    private func loadLogins(context: LAContext) throws -> [VaultAutofillLogin] {
        try VaultAutofillShared.vault(context: context).load().logins
    }

    private func show(_ model: AutoFillListModel) {
        let view = AutoFillListView(
            model: model,
            onPick: { [weak self] login in self?.complete(with: login) },
            onInsert: { [weak self] text in self?.insert(text) },
            onCancel: { [weak self] in
                guard let self else { return }
                if model.configurationOnly {
                    self.extensionContext.completeExtensionConfigurationRequest()
                } else {
                    self.cancel(.userCanceled)
                }
            })
        if let host {
            host.rootView = view
            return
        }
        let controller = UIHostingController(rootView: view)
        addChild(controller)
        controller.view.translatesAutoresizingMaskIntoConstraints = false
        self.view.addSubview(controller.view)
        NSLayoutConstraint.activate([
            controller.view.leadingAnchor.constraint(equalTo: self.view.leadingAnchor),
            controller.view.trailingAnchor.constraint(equalTo: self.view.trailingAnchor),
            controller.view.topAnchor.constraint(equalTo: self.view.topAnchor),
            controller.view.bottomAnchor.constraint(equalTo: self.view.bottomAnchor),
        ])
        controller.didMove(toParent: self)
        host = controller
    }

    private func complete(with login: VaultAutofillLogin) {
        let credential = ASPasswordCredential(user: login.username, password: login.password)
        remember(login) { [weak self] in
            self?.extensionContext.completeRequest(withSelectedCredential: credential, completionHandler: nil)
        }
    }

    private func insert(_ text: String) {
        if #available(iOS 18.0, *) {
            extensionContext.completeRequest(withTextToInsert: text, completionHandler: nil)
        } else {
            cancel(.userCanceled)
        }
    }

    private func remember(_ login: VaultAutofillLogin, then finish: @escaping @MainActor () -> Void) {
        let store = VaultAutofillShared.links()
        var links = store.readLinks()
        let added = links.link(login, serviceIdentifiers: serviceIdentifiers)
        guard !added.isEmpty else {
            finish()
            return
        }
        store.writeLinks(links)
        let user = login.username.isEmpty ? login.title : login.username
        let identities = VaultAutofillServices.identities(added.map {
            VaultAutofillIdentity(recordIdentifier: login.id, host: $0, user: user)
        })
        guard !identities.isEmpty else {
            finish()
            return
        }
        Task { @MainActor in
            let identityStore = ASCredentialIdentityStore.shared
            if await identityStore.state().isEnabled {
                _ = try? await identityStore.saveCredentialIdentities(identities)
            }
            finish()
        }
    }

    private func cancel(_ code: ASExtensionError.Code) {
        extensionContext.cancelRequest(withError: NSError(domain: ASExtensionErrorDomain, code: code.rawValue))
    }
}
