import FMPackages
import SwiftUI
import UniformTypeIdentifiers

enum SecureDriveTab: String, CaseIterable, Identifiable {
    case files, activity, devices

    var id: String { rawValue }

    var title: String {
        switch self {
        case .files: return "Files"
        case .activity: return "Activity"
        case .devices: return "Devices"
        }
    }
}

struct SecureDriveScreen: View {
    @State private var model: SecureDriveModel = SecureDriveFeature.model()
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        content
            .bareShellTop()
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                if model.phase == .unlocked {
                    if model.selected != nil {
                        ToolbarItem(placement: .topBarLeading) {
                            Button {
                                Task { await model.select(nil) }
                            } label: {
                                Label("Drives", systemImage: "chevron.backward")
                            }
                            .accessibilityIdentifier("secure-drive-back")
                        }
                    }
                    ToolbarItem(placement: .topBarTrailing) {
                        Button {
                            model.lock()
                        } label: {
                            Image(systemName: "lock")
                        }
                        .accessibilityLabel("Lock Secure Drive")
                        .accessibilityIdentifier("secure-drive-lock")
                    }
                }
            }
            .safeAreaInset(edge: .bottom, spacing: 0) { statusBar }
            .simultaneousGesture(TapGesture().onEnded { model.touch() })
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in model.scenePhaseChanged(phase) }
            .task {
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 30_000_000_000)
                    model.checkIdle()
                }
            }
            .task(id: model.selected) {
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 10_000_000_000)
                    if Task.isCancelled { break }
                    await model.poll()
                }
            }
    }

    private var title: String {
        if model.phase == .unlocked, let current = model.current { return current.label.isEmpty ? "Secure Drive" : current.label }
        return "Secure Drive"
    }

    @ViewBuilder private var content: some View {
        switch model.phase {
        case .loading:
            ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
        case .setup:
            SecureDriveSetupView(model: model)
        case .locked:
            SecureDriveUnlockView(model: model)
        case .unlocked:
            if let current = model.current {
                if model.snapshot.id == current.id ? model.snapshot.pending : current.pending {
                    SecureDrivePendingView(model: model)
                } else {
                    SecureDriveDriveView(model: model)
                }
            } else {
                SecureDriveListView(model: model)
            }
        }
    }

    @ViewBuilder private var statusBar: some View {
        if model.busy || !model.status.isEmpty {
            HStack(spacing: 8) {
                if model.busy { ProgressView().controlSize(.small) }
                Text(model.status)
                    .font(.footnote)
                    .foregroundStyle(model.statusIsError ? Color.red : Color.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .lineLimit(4)
                if !model.busy {
                    Button {
                        model.say("")
                    } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                    }
                    .accessibilityLabel("Dismiss")
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .background(.bar)
        }
    }
}

struct SecureDriveSetupView: View {
    let model: SecureDriveModel
    @State private var name = ""
    @State private var passphrase = ""
    @State private var again = ""

    var body: some View {
        Form {
            Section {
                Text("One encrypted file on your router's USB drive holds every document, version and log entry. The keys live only on the devices you approve.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
            Section {
                TextField("iPhone name, for example Front desk iPhone", text: $name)
                    .textInputAutocapitalization(.words)
                SecureField("Passphrase", text: $passphrase)
                    .textContentType(.newPassword)
                SecureField("Type it again", text: $again)
                    .textContentType(.newPassword)
            } header: {
                Text("Set up Secure Drive on this iPhone")
            } footer: {
                Text("At least 10 characters; a few unrelated words work well. Your passphrase opens this iPhone's keys. It is never stored or sent anywhere, and it cannot be recovered.")
            }
            Section {
                Button("Create this iPhone's keys") {
                    Task {
                        await model.setup(name: name, passphrase: passphrase, again: again)
                        passphrase = ""
                        again = ""
                    }
                }
                .disabled(model.busy || passphrase.isEmpty)
                .accessibilityIdentifier("secure-drive-setup")
            }
        }
    }
}

struct SecureDriveUnlockView: View {
    let model: SecureDriveModel
    @State private var passphrase = ""
    @FocusState private var focused: Bool

    var body: some View {
        Form {
            Section {
                SecureField("Passphrase", text: $passphrase)
                    .textContentType(.password)
                    .focused($focused)
                    .submitLabel(.go)
                    .onSubmit { unlock() }
            } header: {
                Text("Unlock Secure Drive")
            } footer: {
                Text("Your passphrase opens this iPhone's keys. It is never stored or sent anywhere.")
            }
            Section {
                Button("Unlock") { unlock() }
                    .disabled(model.busy || passphrase.isEmpty)
                    .accessibilityIdentifier("secure-drive-unlock")
            }
        }
        .onAppear { focused = true }
    }

    private func unlock() {
        let typed = passphrase
        passphrase = ""
        Task { await model.unlock(passphrase: typed) }
    }
}

struct SecureDriveListView: View {
    let model: SecureDriveModel
    @State private var naming = false
    @State private var label = ""
    @State private var moving: URL?
    @State private var movingShown = false
    @State private var picking = false
    @State private var joinFile: URL?
    @State private var code = ""
    @State private var changing = false

    var body: some View {
        List {
            if model.drives.isEmpty {
                Section {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("No drive yet").font(.headline)
                        Text("Create a drive and save its file on the router's shared USB drive, or join a drive another device already made with an invitation code.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.vertical, 4)
                }
            } else {
                Section("Drives") {
                    ForEach(model.drives) { (drive: SecureDriveSummary) in
                        Button {
                            Task { await model.select(drive.id) }
                        } label: {
                            HStack {
                                Image(systemName: drive.pending ? "hourglass" : "lock.shield")
                                    .foregroundStyle(Color.accentColor)
                                    .frame(width: 28)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(drive.label.isEmpty ? "Secure Drive" : drive.label)
                                        .foregroundStyle(.primary)
                                    Text(drive.pending ? "Waiting for approval" : URL(fileURLWithPath: drive.container).lastPathComponent)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                Image(systemName: "chevron.forward").font(.caption).foregroundStyle(.tertiary)
                            }
                        }
                    }
                }
            }
            Section {
                Button {
                    label = ""
                    naming = true
                } label: {
                    Label("Create a drive\u{2026}", systemImage: "plus")
                }
                .accessibilityIdentifier("secure-drive-create")
                Button {
                    picking = true
                } label: {
                    Label("Join a drive\u{2026}", systemImage: "person.badge.key")
                }
                .accessibilityIdentifier("secure-drive-join")
            }
            Section {
                Button("Change passphrase\u{2026}") { changing = true }
            }
        }
        .listStyle(.insetGrouped)
        .task { await model.reloadDrives() }
        .alert("Name the drive", isPresented: $naming) {
            TextField("Drive name", text: $label)
            Button("Cancel", role: .cancel) {}
            Button("Create") {
                Task {
                    moving = await model.prepareNewDrive(label: label)
                    movingShown = moving != nil
                }
            }
        } message: {
            Text("Shown only on approved devices; the drive file itself never holds it in the clear. Next, save the drive file on the router's USB drive, for example a shared folder in Files.")
        }
        .fileMover(isPresented: $movingShown, file: moving, onCompletion: { (result: Result<URL, Error>) in
            moving = nil
            Task { await model.finishNewDrive(result) }
        }, onCancellation: {
            moving = nil
            Task { await model.finishNewDrive(.failure(CocoaError(.userCancelled))) }
        })
        .fileImporter(isPresented: $picking, allowedContentTypes: [UTType.item]) { (result: Result<URL, Error>) in
            if case .success(let url) = result {
                code = ""
                joinFile = url
            }
        }
        .sheet(item: joinBinding) { (file: SecureDriveJoinFile) in
            NavigationStack {
                Form {
                    Section {
                        Text(file.url.lastPathComponent)
                        TextField("Invitation code", text: $code)
                            .textInputAutocapitalization(.characters)
                            .autocorrectionDisabled()
                            .font(.body.monospaced())
                    } header: {
                        Text("Join a drive")
                    } footer: {
                        Text("Type the invitation code from a device that is already approved. Then that device approves this iPhone after checking that both screens show the same safety number.")
                    }
                }
                .navigationTitle("Join a drive")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Cancel") { joinFile = nil }
                    }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Ask to Join") {
                            let typed = code
                            let url = file.url
                            joinFile = nil
                            Task { await model.join(url, code: typed) }
                        }
                        .disabled(SecureDriveCrypto.parseInviteCode(code) == nil)
                    }
                }
            }
        }
        .sheet(isPresented: $changing) {
            SecureDrivePassphraseSheet(model: model)
        }
    }

    private var joinBinding: Binding<SecureDriveJoinFile?> {
        Binding(get: { joinFile.map(SecureDriveJoinFile.init) }, set: { if $0 == nil { joinFile = nil } })
    }
}

struct SecureDriveJoinFile: Identifiable {
    let url: URL
    var id: String { url.absoluteString }
}

struct SecureDrivePassphraseSheet: View {
    let model: SecureDriveModel
    @Environment(\.dismiss) private var dismiss
    @State private var current = ""
    @State private var next = ""
    @State private var again = ""

    var body: some View {
        NavigationStack {
            Form {
                Section("Current passphrase") {
                    SecureField("Current passphrase", text: $current)
                }
                Section {
                    SecureField("New passphrase", text: $next)
                    SecureField("Type it again", text: $again)
                } header: {
                    Text("New passphrase")
                } footer: {
                    Text("At least 10 characters")
                }
            }
            .navigationTitle("Change passphrase")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Change") {
                        Task {
                            if await model.changePassphrase(current: current, next: next, again: again) { dismiss() }
                        }
                    }
                    .disabled(current.isEmpty || next.isEmpty || model.busy)
                }
            }
        }
    }
}

struct SecureDrivePendingView: View {
    let model: SecureDriveModel
    @State private var confirming = false

    var body: some View {
        List {
            Section {
                if model.snapshot.joinRequestMissing {
                    Text("The request is no longer on the drive: it was turned down, or someone deleted it. Cancel it and ask for a new invitation code.")
                        .foregroundStyle(.red)
                } else {
                    Text("This iPhone asked to join the drive \(URL(fileURLWithPath: model.current?.container ?? "").lastPathComponent). On a device that is already approved, open Secure Drive, then Devices, and approve this one only if it shows this safety number:")
                    if !model.snapshot.joinSafety.isEmpty {
                        Text(model.snapshot.joinSafety)
                            .font(.title2.monospacedDigit().weight(.semibold))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .center)
                            .padding(.vertical, 8)
                            .accessibilityIdentifier("secure-drive-pending-safety")
                    }
                }
            } header: {
                Text("Waiting for approval")
            }
            Section {
                Button("Check now") { Task { await model.refresh(force: true) } }
                    .disabled(model.busy)
                Button("Cancel request", role: .destructive) { confirming = true }
                    .disabled(model.busy)
            }
        }
        .listStyle(.insetGrouped)
        .task { await model.refresh(force: true) }
        .confirmationDialog("Cancel the request?", isPresented: $confirming, titleVisibility: .visible) {
            Button("Cancel Request", role: .destructive) { Task { await model.cancelJoin() } }
            Button("Keep Waiting", role: .cancel) {}
        } message: {
            Text("The request is removed from the drive. You can ask again with a new code.")
        }
    }
}

struct SecureDriveDriveView: View {
    let model: SecureDriveModel
    @State private var tab: SecureDriveTab = .files
    @State private var forgetting = false

    var body: some View {
        VStack(spacing: 0) {
            Picker("Section", selection: $tab) {
                ForEach(SecureDriveTab.allCases) { (choice: SecureDriveTab) in
                    Text(choice.title).tag(choice)
                }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            switch tab {
            case .files: SecureDriveFilesView(model: model, banner: AnyView(banner))
            case .activity: SecureDriveActivityView(model: model)
            case .devices: SecureDriveDevicesView(model: model)
            }
        }
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button {
                        Task { await model.refresh(force: true) }
                    } label: {
                        Label("Check the drive now", systemImage: "arrow.clockwise")
                    }
                    Button(role: .destructive) {
                        forgetting = true
                    } label: {
                        Label("Forget this drive on this iPhone\u{2026}", systemImage: "minus.circle")
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityLabel("More drive actions")
            }
        }
        .fullScreenCover(item: editorBinding) { (target: SecureDriveEditorTarget) in
            SecureDriveEditorScreen(model: model, target: target)
        }
        .sheet(item: shareBinding) { (target: SecureDriveShareTarget) in
            SecureDriveShareSheet(items: [target.url])
        }
        .confirmationDialog("Forget \(model.snapshot.label)?", isPresented: $forgetting, titleVisibility: .visible) {
            Button("Forget This Drive", role: .destructive) { Task { await model.forget() } }
        } message: {
            Text("This iPhone deletes its keys to \(model.snapshot.label). The files on the drive stay where they are for the other devices. To come back, this iPhone needs a new invitation. To also lock it out for good, remove it under Devices from another device.")
        }
    }

    private var editorBinding: Binding<SecureDriveEditorTarget?> {
        Binding(get: { model.editor }, set: { model.editor = $0 })
    }

    private var shareBinding: Binding<SecureDriveShareTarget?> {
        Binding(get: { model.share }, set: { model.share = $0 })
    }

    private var banner: some View {
        let shot = model.snapshot
        let problems = !shot.issues.isEmpty
        return VStack(alignment: .leading, spacing: 4) {
            if problems {
                Label("Problems found on the drive", systemImage: "exclamationmark.triangle.fill")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.red)
                ForEach(Array(shot.issues.enumerated()), id: \.offset) { (item: (offset: Int, element: String)) in
                    Text("\u{2022} " + item.element).font(.footnote)
                }
            } else if shot.scanned {
                Label {
                    Text("Verified: \(shot.records.count) signed activity records from \(SecureDriveEditorText.plural(shot.members, "approved device")), none missing or altered. The drive file is \(Self.size(shot.containerBytes)), \(Self.size(shot.usedBytes)) in use.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                } icon: {
                    Image(systemName: "checkmark.shield").foregroundStyle(.secondary)
                }
            }
            if model.openCopies > 0 {
                Text("Decrypted copies open in other apps: \(model.openCopies). They are deleted when you lock.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .accessibilityIdentifier("secure-drive-banner")
    }

    static func size(_ bytes: Int64) -> String { ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file) }
}
