// The state and decisions of the vault's main screen (setup, unlock, recovery, the recovery code
// dialog, item actions and the item sheet), so the SwiftUI screen only lays things out. The share screen and the settings screen are in
// VaultShareModel.swift and VaultSettingsModel.swift; user-visible copy is in VaultCopy.

import FMBase
import Foundation
import Observation

/// Every string the vault screens show.
public enum VaultCopy {
    public static let minimumMasterPassword = 12
    /// How long a copied password or username stays on the clipboard.
    public static let clipboardClear: TimeInterval = 30

    static let words: [String: String] = [
        "wrong_password": "That is not the master password.",
        "wrong_recovery_code": "That recovery code does not open this vault.",
        "invalid_recovery_code": "A recovery code is 13 groups of 4 letters and digits.",
        "unsafe_kdf_parameters":
            "The server returned key-stretching settings weaker than this vault allows. Nothing was unlocked.",
        "keyset_mismatch":
            "The keys stored for this account do not match its published keys. Nothing was unlocked.",
        "normalization_unavailable":
            "This device could not normalize the password's special characters, so it was not used.",
        "recipient_has_no_vault":
            "That person has not created a vault yet, so there are no keys to seal to.",
        "recipient_not_found": "No ForkMesh user has that name.",
        "recipient_keys_changed":
            "That person's keys changed since you last shared with them. Compare fingerprints before sharing.",
        "cannot_share_with_yourself": "That is your own account.",
        "organization_not_found": "You are not a member of that organization.",
        "revision_conflict":
            "This login changed somewhere else. It was reloaded; make the change again.",
        "read_only": "This login was shared with you to view, not to edit.",
        "vault_locked": "The vault is locked.",
        "vault_full": "This vault is full.",
        "title_required": "Give the login a title.",
        "invalid_session": "Sign in to ForkMesh to open your vault.",
    ]

    /// The sentence for a vault error code, the code spelled out when
    /// there is none, and a connection hint for anything that is not a vault error.
    public static func errorText(_ error: Error) -> String {
        let code: String
        if let error = error as? VaultError {
            code = error.code
        } else if let error = error as? VaultCryptoError {
            code = error.code
        } else {
            code = ""
        }
        if code.isEmpty { return "Something went wrong. Check your connection and try again." }
        return words[code] ?? code.replacingOccurrences(of: "_", with: " ")
    }

    /// Why a new master password is refused; nil when it is acceptable.
    public static func newPasswordProblem(_ password: String, _ confirmation: String) -> String? {
        if password.utf16.count < minimumMasterPassword { return "Use at least 12 characters for the master password." }
        if password != confirmation { return "The two passwords do not match." }
        return nil
    }

    // Main screen
    public static let title = "Password Vault"
    public static let lockTooltip = "Lock"
    public static let settingsTooltip = "Vault settings"
    public static let addTooltip = "New login"
    public static let signedOutTitle = "Sign in to open your vault"
    public static let signedOutMessage = "The vault belongs to your ForkMesh account."
    public static let searchHint = "Search titles, usernames, and sites"
    public static let emptyTitle = "Nothing here yet"
    public static let emptyMessage =
        "Add a login with the + button. Turn on autofill in Vault settings to fill them in other apps."
    public static let copyPasswordTooltip = "Copy password"
    public static let saved = "Saved, encrypted."
    public static let unreadableTitle = "Unreadable item"

    // Setup, unlock, recovery
    public static let setupTitle = "Create your vault"
    public static let setupMessage =
        "Choose a master password. It never leaves this phone and ForkMesh cannot reset it, so it must be one you will not forget and do not use anywhere else. Four or more unrelated words work well."
    public static let masterPasswordLabel = "Master password"
    public static let typeItAgainLabel = "Type it again"
    public static let createVault = "Create vault"
    public static let unlockTitle = "Unlock your vault"
    public static let unlockMessage =
        "Stretching the master password takes a few seconds on purpose: it is what makes guessing it expensive."
    public static let unlock = "Unlock"
    public static let unlockWithDevice = "Unlock with this device"
    public static let forgotIt = "Forgot it? Use your recovery code"
    public static let recoverTitle = "Recover your vault"
    public static let recoverMessage =
        "The recovery code opens the vault once. You set a new master password and get a new code; the old one stops working."
    public static let recoveryCodeLabel = "Recovery code"
    public static let newMasterPasswordLabel = "New master password"
    public static let recover = "Recover"
    public static let recoveryCodeTitle = "Your recovery code"
    public static let recoveryCodeMessage =
        "This is the only way back into the vault if the master password is forgotten. It is shown once. Write it down and keep it somewhere safe, away from this phone. Do not store it in the vault."
    public static let recoveryCodeSaved = "I have saved this code."
    public static let copy = "Copy"
    public static let done = "Done"
    public static let cancel = "Cancel"

    // Item sheet
    public static let titleLabel = "Title"
    public static let websiteLabel = "Website"
    public static let websiteHint = "https://"
    public static let usernameLabel = "Username"
    public static let passwordLabel = "Password"
    public static let notesLabel = "Notes"
    public static let generatePassword = "Generate a password"
    public static let save = "Save"
}

public enum VaultFilter: String, CaseIterable, Sendable {
    case all, mine, shared

    public var label: String {
        switch self {
        case .all: return "All"
        case .mine: return "Mine"
        case .shared: return "Shared with me"
        }
    }
}

/// One row of the list.
public struct VaultRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let subtitle: String
    /// Material icon name for FmIcon: "key_outlined", "people_outline" or "warning_amber_outlined".
    public let icon: String
    /// False for an item that could not be opened: no copy button, no actions, warning tint.
    public let readable: Bool
}

/// What the actions sheet of an item offers.
public enum VaultItemAction: String, Sendable {
    case username, password, open, edit, share, delete
}

public struct VaultItemActionEntry: Equatable, Identifiable, Sendable {
    public let action: VaultItemAction
    public let label: String
    /// Material icon name for FmIcon.
    public let icon: String
    public let destructive: Bool
    public var id: String { action.rawValue }
}

/// A confirmation dialog's words.
public struct VaultConfirmation: Equatable, Sendable {
    public let title: String
    public let message: String
    public let confirm: String
    public let cancel: String
    public let destructive: Bool
}

/// The recovery code dialog: shown once, not dismissable until the person ticks "saved".
@Observable
@MainActor
public final class VaultRecoveryCodePrompt: Identifiable {
    public nonisolated let id = UUID()
    public let code: String
    public var saved = false
    @ObservationIgnored private let clipboard: VaultClipboard

    public init(code: String, clipboard: VaultClipboard) {
        self.code = code
        self.clipboard = clipboard
    }

    /// Leaving is not the same as having saved the code.
    public var canFinish: Bool { saved }

    public func copy() { clipboard.copy(code, expiresAfter: nil) }
}

/// The recovery sheet's fields.
@Observable
@MainActor
public final class VaultRecoverForm {
    public var code = ""
    public var password = ""
    public var confirm = ""
    public private(set) var problem = ""

    public init() {}

    /// The code and new password to recover with, or nil with `problem` set.
    public func submit() -> (code: String, password: String)? {
        if let problem = VaultCopy.newPasswordProblem(password, confirm) {
            self.problem = problem
            return nil
        }
        problem = ""
        return (code, password)
    }
}

/// The add / edit / view sheet of one login.
@Observable
@MainActor
public final class VaultItemDraft {
    public let item: VaultItem?
    public var title: String
    public var url: String
    public var username: String
    public var password: String
    public var notes: String
    public var reveal = false

    public init(item: VaultItem?) {
        self.item = item
        title = item?.title ?? ""
        url = item?.url ?? ""
        username = item?.username ?? ""
        password = item?.password ?? ""
        notes = item?.notes ?? ""
    }

    /// Shared with this account to view only: every field read-only, no Save, no generator.
    public var readOnly: Bool { item.map { !$0.canEdit } ?? false }

    public var sheetTitle: String { item == nil ? "New login" : readOnly ? "Login" : "Edit login" }

    /// "Shared by alice, last changed by bob." for an item someone else owns.
    public var sharedByLine: String? {
        guard let item, !item.owned else { return nil }
        let changed = !item.updatedBy.isEmpty && item.updatedBy != item.owner ? ", last changed by \(item.updatedBy)" : ""
        return "Shared by \(item.owner)\(changed)."
    }

    public var revealLabel: String { reveal ? "Hide" : "Show" }

    /// Max length of each field (the text fields' `maxLength`).
    public static func maxLength(_ field: String) -> Int { VaultFields.limit(field) }

    public func generate() {
        password = (try? VaultCrypto.generatePassword()) ?? password
        reveal = true
    }

    public var fields: JSONObject {
        [
            "title": .string(title),
            "url": .string(url),
            "username": .string(username),
            "password": .string(password),
            "notes": .string(notes),
        ]
    }
}

@Observable
@MainActor
public final class VaultScreenModel {
    @ObservationIgnored public let vault: VaultService
    @ObservationIgnored public let clipboard: VaultClipboard

    public var search = ""
    public var filter: VaultFilter = .all
    public private(set) var error = ""
    public private(set) var notice = ""
    public private(set) var busy = false
    /// Present the recovery code dialog while this is set; the dialog clears it when finished.
    public var recoveryPrompt: VaultRecoveryCodePrompt?

    public var setupPassword = ""
    public var setupConfirm = ""
    public private(set) var setupProblem = ""
    public var unlockPassword = ""

    public init(vault: VaultService, clipboard: VaultClipboard) {
        self.vault = vault
        self.clipboard = clipboard
    }

    public var phase: VaultPhase { vault.phase }
    /// Lock, settings and the + button exist only while unlocked.
    public var showsUnlockedActions: Bool { vault.phase == .unlocked }
    public var quickUnlockOffered: Bool { vault.quickUnlockEnabled }

    /// Call when the screen appears.
    public func start() async {
        await run { try await self.vault.refresh() }
    }

    /// Any touch on the screen pushes the idle lock out again.
    public func touch() { vault.touch() }

    public func lock() { vault.lock() }

    /// Runs one action with the busy bar, clearing and then setting the error or notice banner.
    @discardableResult
    public func run(notice: String = "", _ work: () async throws -> Void) async -> Bool {
        busy = true
        error = ""
        self.notice = ""
        defer { busy = false }
        do {
            try await work()
            self.notice = notice
            return true
        } catch {
            self.error = VaultCopy.errorText(error)
            return false
        }
    }

    // MARK: Setup, unlock, recovery

    public func submitSetup() async {
        let problem = VaultCopy.newPasswordProblem(setupPassword, setupConfirm)
        setupProblem = problem ?? ""
        if problem != nil { return }
        let password = setupPassword
        setupPassword = ""
        setupConfirm = ""
        var code = ""
        if await run({ code = try await self.vault.create(password) }) {
            recoveryPrompt = VaultRecoveryCodePrompt(code: code, clipboard: clipboard)
        }
    }

    public func submitUnlock() async {
        if unlockPassword.isEmpty { return }
        let password = unlockPassword
        unlockPassword = ""
        await run { try await self.vault.unlock(password) }
    }

    public func quickUnlock() async {
        await run { _ = try await self.vault.unlockWithDevice() }
    }

    /// After `VaultRecoverForm.submit()` handed back a code and a new password.
    public func recover(code: String, newPassword: String) async {
        var renewed = ""
        if await run({ renewed = try await self.vault.recover(code, newMasterPassword: newPassword) }) {
            recoveryPrompt = VaultRecoveryCodePrompt(code: renewed, clipboard: clipboard)
        }
    }

    /// Closes the recovery code dialog once the person ticked "saved".
    public func finishRecoveryPrompt() {
        if recoveryPrompt?.canFinish == true { recoveryPrompt = nil }
    }

    // MARK: List

    public var visibleItems: [VaultItem] {
        let query = search.dartTrimmed.lowercased()
        return vault.items.filter { item in
            (filter != .mine || item.owned) && (filter != .shared || !item.owned)
                && (query.isEmpty || [item.title, item.username, item.host, item.owner].contains { $0.lowercased().contains(query) })
        }
    }

    public var rows: [VaultRow] { visibleItems.map(row) }

    public func row(_ item: VaultItem) -> VaultRow {
        if !item.readable {
            let subtitle = item.error == "sender_keys_changed"
                ? "\(item.owner)'s keys changed since you first received a share from them. It stays closed until you compare fingerprints."
                : "This item could not be opened with your keys (\(item.error.replacingOccurrences(of: "_", with: " ")))."
            return VaultRow(id: item.id, title: VaultCopy.unreadableTitle, subtitle: subtitle, icon: "warning_amber_outlined", readable: false)
        }
        let sharing: String
        if item.owned {
            sharing = item.shares.map { $0.kind == "organization" ? "org \($0.name)" : $0.name }.joined(separator: ", ")
        } else {
            let via = item.viaKind == "organization" ? " via \(item.viaName)" : ""
            sharing = "from \(item.owner)\(via) · \(item.canEdit ? "can edit" : "view only")"
        }
        let subtitle = [item.username, item.host, sharing].filter { !$0.isEmpty }.joined(separator: " · ")
        return VaultRow(id: item.id, title: item.title, subtitle: subtitle,
                        icon: item.owned ? "key_outlined" : "people_outline", readable: true)
    }

    public func reload() async {
        await run { try await self.vault.reload() }
    }

    // MARK: Item actions

    /// The actions sheet's header: title, then "username · host".
    public func actionsHeader(_ item: VaultItem) -> (title: String, subtitle: String) {
        (item.title, [item.username, item.host].filter { !$0.isEmpty }.joined(separator: " · "))
    }

    public func actions(for item: VaultItem) -> [VaultItemActionEntry] {
        var out = [VaultItemActionEntry]()
        if !item.username.isEmpty {
            out.append(VaultItemActionEntry(action: .username, label: "Copy username", icon: "person_outline", destructive: false))
        }
        out.append(VaultItemActionEntry(action: .password, label: "Copy password", icon: "copy_outlined", destructive: false))
        if !VaultCrypto.loginHref(item.url).isEmpty {
            out.append(VaultItemActionEntry(action: .open, label: "Open site", icon: "open_in_new", destructive: false))
        }
        out.append(VaultItemActionEntry(
            action: .edit, label: item.canEdit ? "Edit" : "View",
            icon: item.canEdit ? "edit_outlined" : "visibility_outlined", destructive: false))
        if item.owned {
            out.append(VaultItemActionEntry(action: .share, label: "Share", icon: "ios_share", destructive: false))
        }
        if item.owned || item.viaKind == "user" {
            out.append(VaultItemActionEntry(
                action: .delete, label: item.owned ? "Delete" : "Remove from my vault", icon: "delete_outline", destructive: true))
        }
        return out
    }

    /// Copies with the 30-second expiry and says so.
    public func copy(_ value: String, label: String) {
        clipboard.copy(value, expiresAfter: VaultCopy.clipboardClear)
        notice = "\(label) copied. The clipboard is cleared in 30 seconds."
    }

    public func copyPassword(_ item: VaultItem) { copy(item.password, label: "Password") }

    public func copyUsername(_ item: VaultItem) { copy(item.username, label: "Username") }

    /// Only an https address is ever handed to the system browser.
    public func siteURL(_ item: VaultItem) -> URL? {
        let href = VaultCrypto.loginHref(item.url)
        return href.isEmpty ? nil : URL(string: href)
    }

    public func draft(for item: VaultItem?) -> VaultItemDraft { VaultItemDraft(item: item) }

    /// Saves the sheet's fields; on a revision conflict the list is reloaded behind the message.
    @discardableResult
    public func save(_ draft: VaultItemDraft) async -> Bool {
        let fields = draft.fields, id = draft.item?.id ?? ""
        let saved = await run(notice: VaultCopy.saved) { try await self.vault.save(fields, id: id) }
        if !saved && error == VaultCopy.words["revision_conflict"] {
            let vault = self.vault
            Task { @MainActor in try? await vault.reload() }
        }
        return saved
    }

    public func deleteConfirmation(_ item: VaultItem) -> VaultConfirmation {
        VaultConfirmation(
            title: item.owned ? "Delete this login?" : "Remove from your vault?",
            message: item.owned
                ? "Everyone \"\(item.title)\" is shared with loses it too. This cannot be undone."
                : "\(item.owner) keeps \"\(item.title)\".",
            confirm: item.owned ? "Delete" : "Remove",
            cancel: VaultCopy.cancel,
            destructive: true)
    }

    /// After the person confirmed `deleteConfirmation`.
    public func delete(_ item: VaultItem) async {
        await run { try await self.vault.remove(item.id) }
    }
}
