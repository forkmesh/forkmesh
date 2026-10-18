import FMFiles
import SwiftUI
import UniformTypeIdentifiers

struct FilesScreen: View {
    @State private var library = FilesLibrary.shared
    @State private var addingFolder = false
    @State private var addingShare = false
    @State private var editing: NetworkShare?
    @State private var problem = ""

    var body: some View {
        List {
            if !problem.isEmpty {
                Text(problem).foregroundStyle(.red).font(.callout)
            }
            Section("This iPhone") {
                NavigationLink {
                    FolderScreen(source: library.deviceSource())
                } label: {
                    Label("On this iPhone", systemImage: "iphone")
                }
                .accessibilityIdentifier("files-device")
                ForEach(library.folders) { folder in
                    NavigationLink {
                        SavedFolderScreen(folder: folder)
                    } label: {
                        Label(folder.name, systemImage: "folder")
                    }
                    .swipeActions {
                        Button("Remove", role: .destructive) { library.removeFolder(folder) }
                    }
                }
                Button {
                    addingFolder = true
                } label: {
                    Label("Add folder…", systemImage: "folder.badge.plus")
                }
                .accessibilityIdentifier("files-add-folder")
            }
            Section {
                ForEach(library.shares) { share in
                    NavigationLink {
                        NetworkShareEntryScreen(share: share, saved: true)
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Label(share.title, systemImage: "externaldrive.connected.to.line.below")
                            Text(share.guest ? "Guest" : share.user).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    .swipeActions {
                        Button("Remove", role: .destructive) { library.removeShare(share) }
                        Button("Edit") { editing = share }
                    }
                    .contextMenu {
                        Button("Edit") { editing = share }
                        Button("Remove", role: .destructive) { library.removeShare(share) }
                    }
                }
                ForEach(library.suggestions) { suggestion in
                    NavigationLink {
                        NetworkShareEntryScreen(share: suggestion.share, saved: false)
                    } label: {
                        Label(suggestion.title, systemImage: "wifi.router")
                    }
                }
                Button {
                    addingShare = true
                } label: {
                    Label("Add network share", systemImage: "plus")
                }
                .accessibilityIdentifier("files-add-share")
            } header: {
                Text("Network shares")
            } footer: {
                Text("Disks shared over SMB by your router, a computer or a NAS on your network.")
            }
        }
        .navigationTitle("Files")
        .onAppear { library.reload() }
        .fileImporter(isPresented: $addingFolder, allowedContentTypes: [.folder]) { result in
            switch result {
            case .success(let url):
                do {
                    try library.addFolder(url)
                    problem = ""
                } catch {
                    problem = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
                }
            case .failure(let error):
                problem = error.localizedDescription
            }
        }
        .sheet(isPresented: $addingShare) {
            NavigationStack { AddNetworkShareSheet(existing: nil) }
        }
        .sheet(item: $editing) { share in
            NavigationStack { AddNetworkShareSheet(existing: share) }
        }
    }
}

struct SavedFolderScreen: View {
    let folder: SavedFolder
    @State private var source: FileSource?
    @State private var problem = ""

    var body: some View {
        Group {
            if let source {
                FolderScreen(source: source)
            } else if !problem.isEmpty {
                ContentUnavailableView(folder.name, systemImage: "folder.badge.questionmark", description: Text(problem))
            } else {
                ProgressView()
            }
        }
        .task {
            do {
                source = try FilesLibrary.shared.folderSource(folder)
            } catch {
                problem = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
            }
        }
    }
}

struct NetworkShareEntryScreen: View {
    let share: NetworkShare
    var saved: Bool = false
    var path: String = ""
    @State private var source: FileSource?
    @State private var user = ""
    @State private var password = ""
    @State private var remember = true
    @State private var asking = false
    @State private var busy = false
    @State private var problem = ""

    var body: some View {
        Group {
            if let source {
                FolderScreen(source: source, path: path)
            } else if asking {
                Form {
                    Section {
                        LabeledContent("Share", value: share.title)
                        TextField("User", text: $user)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .textContentType(.username)
                        SecureField("Password", text: $password)
                            .textContentType(.password)
                        Toggle("Remember password", isOn: $remember)
                    } footer: {
                        Text(problem.isEmpty ? "Sign in with a user set up on the file server." : problem)
                            .foregroundStyle(problem.isEmpty ? Color.secondary : Color.red)
                    }
                    Button("Connect") { Task { await connect(asking: true) } }
                        .disabled(busy || user.trimmingCharacters(in: .whitespaces).isEmpty)
                        .accessibilityIdentifier("files-share-connect")
                }
                .overlay { if busy { ProgressView() } }
                .navigationTitle(share.share)
            } else if !problem.isEmpty {
                ContentUnavailableView {
                    Label(share.title, systemImage: "externaldrive.badge.exclamationmark")
                } description: {
                    Text(problem)
                } actions: {
                    Button("Try again") { Task { await connect(asking: false) } }
                    Button("Sign in as a user") { asking = true }
                }
            } else {
                ProgressView("Connecting to \(share.title)…")
            }
        }
        .task {
            guard source == nil else { return }
            user = share.user
            if FilesLibrary.shared.canConnectWithoutAsking(share) {
                await connect(asking: false)
            } else {
                asking = true
            }
        }
    }

    private func connect(asking typed: Bool) async {
        busy = true
        defer { busy = false }
        var target = share
        var secret: String?
        if typed {
            target.user = user.trimmingCharacters(in: .whitespaces)
            target.guest = false
            target.rememberPassword = remember
            secret = password
        }
        do {
            source = try await FilesLibrary.shared.connect(target, password: secret, save: saved || (typed && remember))
            problem = ""
            asking = false
        } catch {
            problem = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
            if let smb = error as? SmbError, smb.status == SmbStatus.logonFailure || smb.status == SmbStatus.accessDenied {
                asking = true
            }
        }
    }
}

struct AddNetworkShareSheet: View {
    let existing: NetworkShare?
    @Environment(\.dismiss) private var dismiss
    @State private var address = ""
    @State private var share = ""
    @State private var user = ""
    @State private var password = ""
    @State private var guest = false
    @State private var remember = true
    @State private var found: [String] = []
    @State private var busy = false
    @State private var problem = ""

    var body: some View {
        Form {
            Section {
                TextField("Address", text: $address, prompt: Text("192.168.1.1 or nas.local"))
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                    .accessibilityIdentifier("files-share-address")
                HStack {
                    TextField("Share", text: $share, prompt: Text("usb"))
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .accessibilityIdentifier("files-share-name")
                    Button("Find shares") { Task { await findShares() } }
                        .buttonStyle(.borderless)
                        .disabled(busy || address.trimmingCharacters(in: .whitespaces).isEmpty)
                }
                if !found.isEmpty {
                    Picker("Shares on \(address)", selection: $share) {
                        ForEach(found, id: \.self) { Text($0).tag($0) }
                    }
                }
            }
            Section {
                Toggle("Sign in as guest", isOn: $guest)
                if !guest {
                    TextField("User", text: $user)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .textContentType(.username)
                    SecureField("Password", text: $password)
                        .textContentType(.password)
                    Toggle("Remember password", isOn: $remember)
                }
            } footer: {
                Text(problem.isEmpty ? "Only file servers on your own network can be opened. Passwords are kept in the iPhone's keychain." : problem)
                    .foregroundStyle(problem.isEmpty ? Color.secondary : Color.red)
            }
        }
        .navigationTitle(existing == nil ? "Add network share" : "Edit network share")
        .navigationBarTitleDisplayMode(.inline)
        .disabled(busy)
        .overlay { if busy { ProgressView() } }
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Connect") { Task { await save() } }
                    .accessibilityIdentifier("files-share-save")
            }
        }
        .onAppear {
            guard let existing else { return }
            address = existing.address
            share = existing.share
            user = existing.user
            guest = existing.guest
            remember = existing.rememberPassword
            password = FilesLibrary.shared.savedPassword(existing) ?? ""
        }
        .onChange(of: address) { (_: String, typed: String) in
            if let parsed = SmbAddress.parse(typed).address, !parsed.share.isEmpty {
                share = parsed.share
                if !parsed.user.isEmpty { user = parsed.user }
                address = parsed.display
            }
        }
    }

    private var draft: NetworkShare {
        NetworkShare(
            id: existing?.id ?? UUID().uuidString, address: address.trimmingCharacters(in: .whitespaces),
            share: share.trimmingCharacters(in: .whitespaces), user: guest ? "" : user.trimmingCharacters(in: .whitespaces),
            guest: guest, rememberPassword: !guest && remember)
    }

    private func findShares() async {
        let parsed = SmbAddress.parse(address)
        if parsed.address == nil {
            problem = parsed.error
            return
        }
        busy = true
        defer { busy = false }
        do {
            found = try await FilesLibrary.shared.findShares(draft, password: password)
            problem = found.isEmpty ? "No shared folders were listed. Type the share name." : ""
            if let first = found.first, share.isEmpty { share = first }
        } catch {
            problem = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
        }
    }

    private func save() async {
        let candidate = draft
        if let issue = NetworkShare.problem(address: candidate.address, share: candidate.share, user: candidate.user, guest: candidate.guest) {
            problem = issue
            return
        }
        busy = true
        defer { busy = false }
        do {
            if let existing { FilesLibrary.shared.forget("smb:" + existing.id) }
            _ = try await FilesLibrary.shared.connect(candidate, password: guest ? "" : password, save: true)
            dismiss()
        } catch {
            problem = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
        }
    }
}
