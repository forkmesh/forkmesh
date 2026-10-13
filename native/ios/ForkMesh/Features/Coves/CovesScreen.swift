import FMApi
import FMBase
import FMCoves
import SwiftUI
import UIKit

struct CovesRouteScreen: View {
    let repo: RepoRef?

    var body: some View {
        if let repo {
            CovesScreen(repo: repo)
        } else {
            CovesRepoPicker()
        }
    }
}

struct CoveRouteScreen: View {
    let id: String

    var body: some View {
        if let target = CoveFiles.parseRouteId(id) {
            CoveLinkScreen(repo: RepoRef(owner: target.owner, name: target.repository), coveId: target.coveId)
        } else {
            ContentUnavailableView {
                Label("Cove link not recognised", systemImage: "lock.trianglebadge.exclamationmark")
            } description: {
                Text("Open coves from their repository, or from a forkmesh://cove link.")
            } actions: {
                FmMessageTools("Cove link not recognised: \(id). Open coves from their repository, or from a forkmesh://cove link.")
            }
        }
    }
}

struct CovesRepoPicker: View {
    @State private var repos: [Repository] = []
    @State private var loading = true
    @State private var error: String?
    @State private var query = ""

    var body: some View {
        List {
            if let error {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            Section {
                ForEach(filtered, id: \.fullName) { repo in
                    NavigationLink {
                        CovesScreen(repo: RepoRef(owner: repo.owner, name: repo.name))
                            .shellPageClearance()
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(verbatim: "\(repo.owner)/\(repo.name)").font(.body)
                            if !repo.description.isEmpty {
                                Text(repo.description).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                        }
                    }
                }
            } footer: {
                Text("Coves are encrypted files inside a repository. Only people with the password, or accounts the creator invited, can read them.")
            }
        }
        .overlay {
            if loading && repos.isEmpty {
                ProgressView()
            } else if !loading && filtered.isEmpty {
                ContentUnavailableView("No repositories", systemImage: "shippingbox", description: Text("Coves live in a repository you can read."))
            }
        }
        .searchable(text: $query)
        .navigationTitle("Coves")
        .task { await load() }
        .fmRefreshable { await load() }
    }

    private var filtered: [Repository] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let me = CoveStore.normalizedAccount(CovesFeature.account)
        let mine: [Repository] = repos.sorted { (lhs: Repository, rhs: Repository) -> Bool in
            let lhsMine: Bool = lhs.owner.lowercased() == me
            let rhsMine: Bool = rhs.owner.lowercased() == me
            if lhsMine != rhsMine { return lhsMine }
            return lhs.fullName.lowercased() < rhs.fullName.lowercased()
        }
        if needle.isEmpty { return mine }
        return mine.filter { "\($0.owner)/\($0.name)".lowercased().contains(needle) }
    }

    private func load() async {
        guard let api = CovesFeature.api else {
            loading = false
            error = "Sign in to browse repositories."
            return
        }
        loading = true
        defer { loading = false }
        do {
            repos = try await api.repositories()
            error = nil
        } catch {
            self.error = "\(error)"
        }
    }
}

struct CovesScreen: View {
    let repo: RepoRef
    @State private var model: CovesModel?

    init(repo: RepoRef) {
        self.repo = repo
        _model = State(initialValue: CovesFeature.model(for: repo))
    }

    var body: some View {
        if let model {
            CovesList(model: model)
        } else {
            ContentUnavailableView("Sign in to open coves", systemImage: "lock", description: Text("Coves belong to your ForkMesh account."))
                .navigationTitle("Coves")
        }
    }
}

private struct CovesList: View {
    @Bindable var model: CovesModel
    @State private var creating = false
    @State private var unlocking: Cove?
    @State private var message: String?

    var body: some View {
        List {
            if case .failed(let reason) = model.phase {
                Section {
                    Label("Couldn't load coves. \(reason)", systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools("Couldn't load coves. \(reason)")
                    Button("Retry") { Task { await model.load() } }
                }
            }
            if !model.unlockedCoves.isEmpty {
                Section("Open") {
                    ForEach(model.unlockedCoves) { cove in
                        NavigationLink {
                            CoveScreen(model: model, coveId: cove.id)
                                .shellPageClearance()
                        } label: {
                            CoveRow(cove: cove, pending: model.entry(cove.id)?.pendingReview ?? false)
                        }
                    }
                }
            }
            if !model.lockedCoves.isEmpty {
                Section {
                    ForEach(model.lockedCoves) { cove in
                        Button {
                            unlocking = cove
                        } label: {
                            CoveRow(cove: cove, pending: false)
                        }
                        .tint(.primary)
                    }
                } header: {
                    Text("Locked")
                } footer: {
                    Text("Locked coves show nothing about themselves: not a name, not who can open them.")
                }
            }
            if model.skipped > 0 {
                Section {
                    Text("\(model.skipped) cove file(s) could not be read.").font(.footnote).foregroundStyle(.secondary)
                }
            }
        }
        .overlay {
            if model.phase == .loading && model.entries.isEmpty {
                ProgressView()
            } else if model.phase == .loaded && model.entries.isEmpty {
                ContentUnavailableView {
                    Label("No coves yet", systemImage: "lock.shield")
                } description: {
                    Text("A cove is an encrypted folder of notes, sheets and pipelines inside \(model.repo.slug).")
                } actions: {
                    Button("New cove") { creating = true }.buttonStyle(.borderedProminent)
                }
            }
        }
        .navigationTitle("Coves")
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button {
                    creating = true
                } label: {
                    Label("New cove", systemImage: "plus")
                }
                .disabled(model.busy)
                Menu {
                    Button("Lock all", systemImage: "lock") { model.lockAll() }
                } label: {
                    Label("More", systemImage: "ellipsis.circle")
                }
            }
        }
        .task {
            if model.phase == .idle {
                await model.load()
            } else {
                await model.unlockAccountCoves()
            }
        }
        .fmRefreshable { await model.load() }
        .sheet(isPresented: $creating) {
            NewCoveSheet(model: model) { created in
                creating = false
                if let created { message = "Proposed \u{201C}\(created.name)\u{201D}. It appears for others once the repository owner merges it." }
            }
        }
        .sheet(item: $unlocking) { cove in
            UnlockCoveSheet(model: model, cove: cove) { unlocking = nil }
        }
        .alert("Cove", isPresented: Binding(get: { message != nil }, set: { if !$0 { message = nil } })) {
            Button("OK", role: .cancel) { message = nil }
            Button("Copy") { UIPasteboard.general.string = message ?? "" }
            Button("Send to agent") { FmMessageHandoff.send(message ?? "") }
        } message: {
            Text(message ?? "")
        }
    }
}

struct CoveRow: View {
    let cove: Cove
    let pending: Bool

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: cove.unlocked ? (cove.accountScoped ? "person.2.badge.key" : "lock.open") : "lock.fill")
                .foregroundStyle(cove.unlocked ? Color.accentColor : Color.secondary)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(cove.displayName).font(.body)
                Text(subtitle).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if pending {
                Text("Pending review")
                    .font(.caption2.weight(.semibold))
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(Color.orange.opacity(0.18)))
            }
        }
        .accessibilityElement(children: .combine)
    }

    private var subtitle: String {
        if !cove.unlocked { return "Encrypted \u{00B7} \(cove.slug.prefix(8))" }
        let docs = cove.documents.count == 1 ? "1 document" : "\(cove.documents.count) documents"
        if cove.accountScoped {
            let people = cove.members.count == 1 ? "1 person" : "\(cove.members.count) people"
            return "\(docs) \u{00B7} \(people)"
        }
        return "\(docs) \u{00B7} password"
    }
}

private struct UnlockCoveSheet: View {
    let model: CovesModel
    let cove: Cove
    let onDone: () -> Void
    @State private var password = ""
    @State private var working = false
    @State private var failed = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    SecureField("Password", text: $password)
                        .textContentType(.password)
                        .submitLabel(.go)
                        .onSubmit { Task { await unlock() } }
                } footer: {
                    if failed {
                        Text("That password didn't unlock this cove.").foregroundStyle(.red)
                            .fmMessageTools("That password didn't unlock this cove.")
                    } else {
                        Text("The password is shared by the cove's creator, out of band. It never leaves this phone.")
                    }
                }
            }
            .navigationTitle("Unlock cove")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: onDone) }
                ToolbarItem(placement: .confirmationAction) {
                    if working {
                        ProgressView()
                    } else {
                        Button("Unlock") { Task { await unlock() } }.disabled(password.isEmpty)
                    }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func unlock() async {
        working = true
        let ok = await model.unlock(cove.id, password: password)
        working = false
        if ok {
            password = ""
            onDone()
        } else {
            failed = true
        }
    }
}

private struct NewCoveSheet: View {
    let model: CovesModel
    let onDone: (Cove?) -> Void
    @State private var name = ""
    @State private var usePassword = false
    @State private var password = ""
    @State private var confirm = ""
    @State private var notify = true
    @State private var working = false
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Name, e.g. Launch plans", text: $name)
                }
                Section {
                    Picker("Who can open it", selection: $usePassword) {
                        Text("Accounts I invite").tag(false)
                        Text("Anyone with a password").tag(true)
                    }
                    .pickerStyle(.inline)
                    .labelsHidden()
                    if usePassword {
                        SecureField("Password", text: $password).textContentType(.newPassword)
                        SecureField("Type it again", text: $confirm).textContentType(.newPassword)
                        Toggle("Tell me when someone opens it", isOn: $notify)
                    }
                } footer: {
                    Text(usePassword
                        ? "Share the password with your team out of band. ForkMesh never sees it."
                        : "Each invited account gets its own encrypted key grant. You can invite people after creating it.")
                }
                if let error {
                    Section { Text(error).foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
                }
                Section {
                    Text("The cove is proposed to \(model.repo.slug) as an encrypted file. The repository owner merges it like any change.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("New cove")
            .navigationBarTitleDisplayMode(.inline)
            .interactiveDismissDisabled(working)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { onDone(nil) }.disabled(working) }
                ToolbarItem(placement: .confirmationAction) {
                    if working {
                        ProgressView()
                    } else {
                        Button("Create") { Task { await create() } }.disabled(!valid)
                    }
                }
            }
        }
    }

    private var valid: Bool {
        let hasName = !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        if !usePassword { return hasName }
        return hasName && !password.isEmpty && password == confirm
    }

    private func create() async {
        working = true
        defer { working = false }
        do {
            let cove: Cove
            if usePassword {
                cove = try await model.createPasswordCove(name: name, password: password, notifyOnOpen: notify)
            } else {
                cove = try await model.createAccountCove(name: name)
            }
            FmHaptics.commit()
            onDone(cove)
        } catch {
            self.error = "\(error)"
        }
    }
}

struct CoveLinkScreen: View {
    let repo: RepoRef
    let coveId: String
    @State private var model: CovesModel?

    init(repo: RepoRef, coveId: String) {
        self.repo = repo
        self.coveId = coveId
        _model = State(initialValue: CovesFeature.model(for: repo))
    }

    var body: some View {
        if let model {
            CoveLinkBody(model: model, coveId: coveId)
        } else {
            ContentUnavailableView("Sign in to open coves", systemImage: "lock")
        }
    }
}

private struct CoveLinkBody: View {
    @Bindable var model: CovesModel
    let coveId: String
    @State private var unlocking = false

    var body: some View {
        Group {
            if let cove = model.cove(coveId), cove.unlocked {
                CoveScreen(model: model, coveId: coveId)
            } else if model.phase == .loading || model.phase == .idle {
                ProgressView().task { if model.phase == .idle { await model.load() } }
            } else if let cove = model.cove(coveId) {
                ContentUnavailableView {
                    Label("This cove is locked", systemImage: "lock.fill")
                } description: {
                    Text(cove.accountScoped || cove.accessMode.isEmpty
                        ? "Your account has no key grant for it, or it needs its password."
                        : "Enter the cove's password to open it.")
                } actions: {
                    FmMessageTools(cove.accountScoped || cove.accessMode.isEmpty
                        ? "This cove is locked. Your account has no key grant for it, or it needs its password."
                        : "This cove is locked. Enter the cove's password to open it.")
                    Button("Enter password") { unlocking = true }.buttonStyle(.borderedProminent)
                }
                .sheet(isPresented: $unlocking) {
                    UnlockCoveSheet(model: model, cove: cove) { unlocking = false }
                }
            } else {
                ContentUnavailableView {
                    Label("Cove not found", systemImage: "questionmark.folder")
                } description: {
                    Text("It is not in \(model.repo.slug), or its change has not been merged yet.")
                } actions: {
                    FmMessageTools("Cove not found. It is not in \(model.repo.slug), or its change has not been merged yet.")
                }
            }
        }
        .navigationTitle("Cove")
    }
}
