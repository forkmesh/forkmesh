import FMAccount
import FMApi
import FMBase
import FMCrypto
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct JSONTextDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.json] }

    var text: String

    init(text: String) {
        self.text = text
    }

    init(configuration: ReadConfiguration) throws {
        guard let data = configuration.file.regularFileContents, data.count <= 256 * 1024,
              let text = String(data: data, encoding: .utf8)
        else {
            throw CocoaError(.fileReadCorruptFile)
        }
        self.text = text
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data(text.utf8))
    }
}

@MainActor
struct IdentityScreen: View {
    @Environment(\.fmPalette) var fm
    @State var backup: IdentityBackupState
    @State var showKitSheet: Bool = false
    @State var showVerifySheet: Bool = false
    @State var showScanner: Bool = false
    @State var showTrustList: Bool = false
    @State var linkText: String = ""
    @State var link: NodeLinkModel?
    @State var recoveryNotice: FmNotice?
    @State var linkNotice: FmNotice?

    private let identity: DeviceIdentity?
    private let settings: SettingsService?
    private let auth: AuthService?
    private let store: IdentityBackupStore?

    init() {
        let settings: SettingsService? = Services.get(SettingsService.self)
        let auth: AuthService? = Services.get(AuthService.self)
        let api: ApiService? = Services.get(ApiService.self)
        self.identity = Services.get(DeviceIdentity.self)
        self.settings = settings
        self.auth = auth
        let store: IdentityBackupStore? = settings.map { IdentityBackupStore(prefs: $0.prefs) }
        self.store = store
        _backup = State(initialValue: store?.state ?? IdentityBackupState(backedUpKey: "", backedUpAt: 0, snoozedUntil: 0))
        _link = State(initialValue: api.map { (client: ApiService) in
            NodeLinkModel(client: client, user: { Services.get(AuthService.self)?.session?.nodeName ?? "" })
        })
    }

    private var publicKey: String { identity?.publicKeyB64url ?? "" }
    private var account: String { auth?.session?.nodeName ?? "" }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                SettingsSection(title: "Identity key") {
                    SettingsCardBox { keyCard }
                }
                SettingsSection(title: "Deploy approval key") {
                    SettingsCardBox { deployApprovalCard }
                }
                SettingsSection(title: "Recovery kit") {
                    SettingsCardBox { recoveryCard }
                }
                SettingsSection(title: "Link a node to your account") {
                    SettingsCardBox { linkCard }
                }
                SettingsSection(title: "Trust lists", last: true) {
                    SettingsCardBox(padded: false) {
                        FmListRow(
                            title: "Check a trust list",
                            subtitle: "Verify a signed list, revocation or appeal",
                            leading: FmIcon("verified_user_outlined", size: 22, color: fm.textSecondary).fmAny,
                            onTap: { showTrustList = true })
                    }
                }
            }
            .padding(FmSpace.x4)
        }
        .scrollDismissesKeyboard(.interactively)
        .background(fm.bgBase.ignoresSafeArea())
        .navigationTitle("Identity and recovery")
        .navigationBarTitleDisplayMode(.inline)
        .sheet(isPresented: $showKitSheet) {
            RecoveryKitSheet(identity: identity, account: account, onSaved: {
                store?.markBackedUp(publicKey: publicKey, at: SystemClock().nowMillis)
                backup = store?.state ?? backup
                recoveryNotice = FmNotice("Recovery kit saved")
            })
        }
        .sheet(isPresented: $showVerifySheet) {
            VerifyKitSheet(currentPublicKey: publicKey)
        }
        .sheet(isPresented: $showScanner) {
            QrScannerSheet(title: "Scan the desktop's code", onScanned: { (value: String) in
                linkText = value
                link?.review(value)
            })
        }
        .sheet(isPresented: $showTrustList) {
            TrustListSheet()
        }
    }

    private var keyCard: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            CopyableValueRow(label: "Node id (public key)", value: publicKey)
            if !publicKey.isEmpty {
                HStack {
                    Spacer()
                    QrCodeImage(text: "forkmesh:identity/\(account.lowercased())/\(publicKey)", size: 160)
                    Spacer()
                }
            }
            SettingsNote(text: "The private half never leaves this phone. Other devices and the relay only ever see this public key.")
        }
    }

    private var deployApprovalCard: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            CopyableValueRow(label: "Deploy approval key", value: publicKey)
            SettingsNote(text: "Merging a pull request or an agent branch from this phone signs a deploy approval with this key. Add it to approver_keys under [self_deploy] in the cores' fleet.toml so they deploy merges you approve here.")
        }
    }

    @ViewBuilder private var recoveryCard: some View {
        let now: Int64 = SystemClock().nowMillis
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            if backup.needsBackup(currentKey: publicKey) {
                HStack(alignment: .top, spacing: FmSpace.x3) {
                    FmIcon("warning_amber_outlined", size: 22, color: fm.warning)
                    SettingsNote(text: "This key has no recovery kit. If you lose this phone, the key and anything only it can open are gone.")
                        .fmMessageTools("This key has no recovery kit. If you lose this phone, the key and anything only it can open are gone.")
                }
            } else {
                HStack(alignment: .top, spacing: FmSpace.x3) {
                    FmIcon("verified_outlined", size: 22, color: fm.success)
                    SettingsNote(text: "Kit saved \(AccountTime.label(backup.backedUpAt, nowMillis: now)). Save a new one after you rotate this key.")
                }
            }
            SettingsNote(text: "The kit is encrypted on this phone with a passphrase only you know, in the same format the desktop and the website use. Keep two copies in separate places.")
            FmNoticeLine($recoveryNotice)
            HStack(spacing: FmSpace.x2) {
                FmFilledButton("Save a kit", icon: "save_outlined", onPressed: identity == nil ? nil : { showKitSheet = true })
                FmOutlinedButton("Test a kit", icon: "task_alt", onPressed: { showVerifySheet = true })
            }
            if backup.showsNag(currentKey: publicKey, nowMillis: now) {
                FmTextButton("Remind me next week", onPressed: {
                    store?.snooze(nowMillis: now)
                    backup = store?.state ?? backup
                })
            }
        }
    }

    @ViewBuilder private var linkCard: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            SettingsNote(text: "On the desktop, open the node's profile and choose \"Link this node to your account\". Scan its code, or paste the link it opens.")
            SettingsTextField(label: "Link", text: $linkText)
            HStack(spacing: FmSpace.x2) {
                FmOutlinedButton("Scan", icon: "qr_code_scanner_outlined", onPressed: { showScanner = true })
                FmOutlinedButton("Check link", icon: "link", onPressed: linkText.isEmpty ? nil : { link?.review(linkText) })
            }
            if let model = link {
                if let grant = model.pending {
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        SettingsTitle(text: "Link \(grant.node) to @\(account)?")
                        SettingsNote(text: "The node moves to your account, even if another account owns it now. The desktop signed this request.")
                        HStack(spacing: FmSpace.x2) {
                            FmFilledButton(model.busy ? "Linking…" : "Link node", icon: "link", onPressed: model.busy ? nil : {
                                Task { @MainActor in
                                    if let outcome = await model.confirm() {
                                        switch outcome {
                                        case .linked(let node, _), .alreadyLinked(let node):
                                            linkNotice = FmNotice("\(node) is linked to your account")
                                            linkText = ""
                                        case .failed:
                                            break
                                        }
                                    }
                                }
                            })
                            FmTextButton("Cancel", onPressed: { model.cancel() })
                        }
                    }
                }
                if !model.error.isEmpty {
                    SettingsNote(text: model.error, color: fm.danger)
                        .fmMessageTools(model.error)
                }
            }
            FmNoticeLine($linkNotice)
        }
    }
}

private struct RecoveryKitSheet: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.dismiss) var dismiss
    let identity: DeviceIdentity?
    let account: String
    let onSaved: () -> Void

    @State private var passphrase: String = ""
    @State private var confirm: String = ""
    @State private var working: Bool = false
    @State private var error: String = ""
    @State private var document: JSONTextDocument?
    @State private var exporting: Bool = false
    @State private var fileName: String = "forkmesh-recovery-kit.json"

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    SettingsNote(text: "Choose a passphrase of at least \(RecoveryKit.minPassphrase) characters. Nobody can recover it for you, and ForkMesh never sees it.")
                    SettingsTextField(label: "Passphrase", text: $passphrase, secure: true)
                    SettingsTextField(label: "Passphrase again", text: $confirm, secure: true)
                    StrengthBar(level: RecoveryKit.strength(passphrase))
                    if !error.isEmpty {
                        SettingsNote(text: error, color: fm.danger)
                            .fmMessageTools(error)
                    }
                    FmFilledButton(working ? "Encrypting…" : "Encrypt and save", icon: "save_outlined", fillWidth: true,
                                   onPressed: working ? nil : { Task { @MainActor in await build() } })
                }
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Save a recovery kit")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
            .fileExporter(isPresented: $exporting, document: document, contentType: .json, defaultFilename: fileName) {
                (result: Result<URL, Error>) in
                document = nil
                if case .success = result {
                    onSaved()
                    dismiss()
                } else {
                    error = "The kit was not saved."
                }
            }
        }
    }

    private func build() async {
        if let problem = RecoveryKit.validatePassphrase(passphrase, confirm: confirm) {
            error = problem.description
            return
        }
        guard let identity = identity else {
            error = RecoveryKitError.noIdentity.description
            return
        }
        working = true
        defer { working = false }
        let secret: String = passphrase
        let again: String = confirm
        let created: Int64 = SystemClock().nowMillis
        let result: Result<String, Error> = await Task.detached(priority: .userInitiated) {
            Result { try RecoveryKit.export(identity: identity, passphrase: secret, confirm: again, createdAt: created) }
        }.value
        switch result {
        case .success(let text):
            passphrase = ""
            confirm = ""
            error = ""
            fileName = RecoveryKit.fileName(account: account, createdAt: created)
            document = JSONTextDocument(text: text)
            exporting = true
        case .failure(let failure):
            error = (failure as? RecoveryKitError)?.description ?? "Could not encrypt the kit."
        }
    }
}

private struct VerifyKitSheet: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.dismiss) var dismiss
    let currentPublicKey: String

    @State private var kitText: String = ""
    @State private var passphrase: String = ""
    @State private var importing: Bool = false
    @State private var working: Bool = false
    @State private var message: String = ""
    @State private var ok: Bool = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    SettingsNote(text: "Open a kit and type its passphrase. Nothing is installed or sent anywhere; this only proves the kit opens.")
                    FmOutlinedButton(kitText.isEmpty ? "Choose a kit" : "Kit loaded", icon: "folder_open_outlined", onPressed: { importing = true })
                    SettingsTextField(label: "Passphrase", text: $passphrase, secure: true)
                    FmFilledButton(working ? "Checking…" : "Check kit", icon: "task_alt", fillWidth: true,
                                   onPressed: kitText.isEmpty || working ? nil : { Task { @MainActor in await check() } })
                    if !message.isEmpty {
                        SettingsNote(text: message, color: ok ? fm.success : fm.danger)
                            .fmMessageTools(message)
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Test a recovery kit")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.json, .plainText]) { (result: Result<URL, Error>) in
                guard case .success(let url) = result else { return }
                let scoped: Bool = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                if let data = try? Data(contentsOf: url), data.count <= 256 * 1024, let text = String(data: data, encoding: .utf8) {
                    kitText = text
                    message = ""
                } else {
                    message = "That file could not be read."
                    ok = false
                }
            }
        }
    }

    private func check() async {
        working = true
        defer { working = false }
        let text: String = kitText
        let secret: String = passphrase
        let key: String = currentPublicKey
        let result: Result<RecoveryKitCheck, Error> = await Task.detached(priority: .userInitiated) {
            Result { try RecoveryKit.check(kit: text, passphrase: secret, currentPublicKey: key) }
        }.value
        switch result {
        case .success(let check):
            ok = true
            message = check.matchesThisDevice
                ? "The kit opens and holds this phone's key."
                : "The kit opens and holds another key (\(RecoveryKit.shortKey(check.publicKey)))."
        case .failure(let failure):
            ok = false
            message = (failure as? RecoveryKitError)?.description ?? "The kit did not open."
        }
        passphrase = ""
    }
}

private struct StrengthBar: View {
    @Environment(\.fmPalette) var fm
    let level: Int

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            ForEach(0..<3, id: \.self) { (index: Int) in
                Capsule()
                    .fill(index < level ? color : fm.border)
                    .frame(height: 4)
            }
        }
        .accessibilityElement()
        .accessibilityLabel(Text(["Too short", "Weak", "Good", "Strong"][min(max(level, 0), 3)]))
    }

    private var color: Color {
        switch level {
        case 1: return fm.warning
        case 2, 3: return fm.success
        default: return fm.danger
        }
    }
}

private struct TrustListSheet: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.dismiss) var dismiss
    @State private var text: String = ""
    @State private var summary: TrustListSummary?
    @State private var error: String = ""
    @State private var importing: Bool = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    SettingsNote(text: "Trust lists are signed documents that say which keys a community vouches for. Open one to check its signature and scope; nothing is applied.")
                    HStack(spacing: FmSpace.x2) {
                        FmOutlinedButton("Open file", icon: "folder_open_outlined", onPressed: { importing = true })
                        FmOutlinedButton("Paste", icon: "content_paste", onPressed: {
                            text = String((UIPasteboard.general.string ?? "").prefix(256 * 1024))
                            evaluate()
                        })
                    }
                    if let summary = summary {
                        VStack(alignment: .leading, spacing: FmSpace.x2) {
                            FmKeyValueRow(label: "Kind", value: summary.kind)
                            FmKeyValueRow(label: "Name", value: summary.name.isEmpty ? "—" : summary.name)
                            FmKeyValueRow(label: "Issuer", value: RecoveryKit.shortKey(summary.issuer))
                            FmKeyValueRow(label: "Version", value: "\(summary.version)")
                            FmKeyValueRow(label: "Entries", value: "\(summary.entries)")
                            FmKeyValueRow(label: "Communities", value: summary.communities.isEmpty ? "—" : summary.communities.joined(separator: ", "))
                            FmKeyValueRow(label: "Roles", value: summary.roles.isEmpty ? "—" : summary.roles.joined(separator: ", "))
                            FmKeyValueRow(label: "Document", value: RecoveryKit.shortKey(summary.documentId))
                        }
                        SettingsNote(text: "Signature verified.", color: fm.success)
                            .fmMessageTools("Signature verified.")
                    }
                    if !error.isEmpty {
                        SettingsNote(text: error, color: fm.danger)
                            .fmMessageTools(error)
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Check a trust list")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.json, .plainText]) { (result: Result<URL, Error>) in
                guard case .success(let url) = result else { return }
                let scoped: Bool = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                if let data = try? Data(contentsOf: url), data.count <= 256 * 1024, let loaded = String(data: data, encoding: .utf8) {
                    text = loaded
                    evaluate()
                } else {
                    error = "That file could not be read."
                }
            }
        }
    }

    private func evaluate() {
        switch TrustListSummary.summarize(text) {
        case .success(let result):
            summary = result
            error = ""
        case .failure(let failure):
            summary = nil
            error = "Not a valid signed document (\(failure.code.replacingOccurrences(of: "_", with: " ")))."
        }
    }
}
