import FMVault
import LocalAuthentication
import Observation
import SwiftUI

@MainActor
@Observable
final class AutoFillListModel {
    let serviceIdentifiers: [String]
    let siteLabel: String
    let configurationOnly: Bool
    let inserting: Bool
    var logins: [VaultAutofillLogin] = []
    var links = VaultAutofillLinks()
    var unlocking = false
    var failure: String?
    var query = ""
    var choosing: VaultAutofillLogin?

    init(serviceIdentifiers: [String], siteLabel: String = "", configurationOnly: Bool = false, inserting: Bool = false) {
        self.serviceIdentifiers = serviceIdentifiers
        self.siteLabel = siteLabel
        self.configurationOnly = configurationOnly
        self.inserting = inserting
    }

    var ranked: (suggested: [VaultAutofillLogin], others: [VaultAutofillLogin]) {
        VaultAutofillCache.ranked(logins, serviceIdentifiers: serviceIdentifiers, query: query, links: links)
    }

    func unlock(_ load: @MainActor (LAContext) throws -> [VaultAutofillLogin]) async {
        unlocking = true
        defer { unlocking = false }
        let context = LAContext()
        context.localizedReason = "Fill a password from your ForkMesh vault"
        do {
            let ok = try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: "Fill a password from your ForkMesh vault")
            guard ok else {
                failure = "Authentication was cancelled."
                return
            }
            logins = try load(context)
            links = VaultAutofillShared.links().readLinks()
            failure = nil
        } catch VaultAutofillCacheError.noCache {
            failure = "Turn on AutoFill in ForkMesh \u{2192} Vault \u{2192} Settings, then unlock your vault once."
        } catch VaultAutofillCacheError.noKey {
            failure = "Unlock your vault in ForkMesh once to refresh AutoFill."
        } catch let error as LAError {
            failure = error.code == .passcodeNotSet
                ? "Set a device passcode to use AutoFill."
                : "Authentication was cancelled."
        } catch {
            failure = "Couldn't read your saved logins. Open ForkMesh and unlock your vault."
        }
    }

    func forget() {
        logins = []
        choosing = nil
    }
}

struct AutoFillListView: View {
    @Bindable var model: AutoFillListModel
    let onPick: (VaultAutofillLogin) -> Void
    let onInsert: (String) -> Void
    let onCancel: () -> Void

    var body: some View {
        NavigationStack {
            content
                .navigationTitle("ForkMesh Vault")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button(model.configurationOnly ? "Done" : "Cancel") {
                            model.forget()
                            onCancel()
                        }
                    }
                }
                .confirmationDialog(
                    model.choosing.map { $0.title.isEmpty ? $0.host : $0.title } ?? "",
                    isPresented: choosingShown, titleVisibility: .visible
                ) {
                    if let login = model.choosing {
                        if !login.username.isEmpty {
                            Button("Insert username") { insert(login.username) }
                        }
                        Button("Insert password") { insert(login.password) }
                        Button("Cancel", role: .cancel) { model.choosing = nil }
                    }
                }
        }
    }

    private var choosingShown: Binding<Bool> {
        Binding(get: { model.choosing != nil }, set: { shown in
            if !shown { model.choosing = nil }
        })
    }

    private func insert(_ text: String) {
        model.forget()
        onInsert(text)
    }

    @ViewBuilder private var content: some View {
        if model.configurationOnly {
            ContentUnavailableView {
                Label("AutoFill is on", systemImage: "key.fill")
            } description: {
                Text("Logins from your ForkMesh vault are offered in Safari and other apps after Face ID or your passcode. Open ForkMesh and unlock your vault to keep the list current.")
            }
        } else if model.unlocking && model.logins.isEmpty {
            ProgressView("Unlocking\u{2026}")
        } else if let failure = model.failure, model.logins.isEmpty {
            ContentUnavailableView("Nothing to fill", systemImage: "lock.trianglebadge.exclamationmark", description: Text(failure))
        } else {
            let ranked = model.ranked
            List {
                if !ranked.suggested.isEmpty {
                    Section(model.siteLabel.isEmpty ? "Suggested" : "For \(model.siteLabel)") {
                        ForEach(ranked.suggested, id: \.id) { row($0) }
                    }
                }
                if !ranked.others.isEmpty {
                    Section(ranked.suggested.isEmpty && !model.siteLabel.isEmpty ? "Pick a login for \(model.siteLabel)" : "All logins") {
                        ForEach(ranked.others, id: \.id) { row($0) }
                    }
                }
            }
            .searchable(text: $model.query, prompt: "Search titles, usernames, and sites")
            .overlay {
                if ranked.suggested.isEmpty && ranked.others.isEmpty {
                    ContentUnavailableView.search(text: model.query)
                }
            }
        }
    }

    private func row(_ login: VaultAutofillLogin) -> some View {
        Button {
            if model.inserting {
                model.choosing = login
            } else {
                onPick(login)
            }
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                Text(login.title.isEmpty ? login.host : login.title).foregroundStyle(.primary)
                Text(login.username.isEmpty ? login.host : "\(login.username) \u{00B7} \(login.host)")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
            .contentShape(Rectangle())
        }
        .accessibilityLabel("\(login.title), \(login.username), \(login.host)")
    }
}
