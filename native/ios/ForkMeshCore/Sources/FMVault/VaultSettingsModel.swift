// The state and decisions of the vault settings screen:
// autofill, locking, quick unlock, the key fingerprint, changing the master password, a new
// recovery code, the encrypted export and deleting the vault.

import FMBase
import Foundation
import Observation

@Observable
@MainActor
public final class VaultSettingsModel {
    public static let title = "Vault settings"
    public static let autofillHeader = "Fill logins in other apps"
    public static let autofillUnsupported =
        "This device cannot fill logins into other apps. Copy a login from the vault instead."
    public static let autofillToggle = "Autofill from this vault"
    public static let openAutofillSettingsButton = "Open autofill settings"
    public static let lockingHeader = "Locking"
    public static let lockAfterLabel = "Lock after"
    public static let lockingNote = "The vault also locks after a minute in the background and when you sign out."
    public static let quickUnlockToggle = "Unlock with this device"
    public static let quickUnlockSubtitle =
        "Keeps your vault keys in this phone's secure storage and opens them with your fingerprint, face, or screen lock instead of the master password. Anyone who can unlock this phone can then open the vault."
    public static let fingerprintHeader = "Your key fingerprint"
    public static let fingerprintNote =
        "Someone sharing with you for the first time can read this back to you to be sure the keys they were given are yours."
    public static let masterPasswordHeader = "Master password"
    public static let changePasswordButton = "Change master password"
    public static let passwordChanged = "Master password changed. Other devices need the new one at their next unlock."
    public static let newRecoveryCodeButton = "New recovery code"
    public static let copyExportButton = "Copy encrypted export"
    public static let exportCopied = "Encrypted export copied. It opens only with your master password or recovery code."
    public static let deleteVaultButton = "Delete vault"

    @ObservationIgnored public let vault: VaultService
    @ObservationIgnored public let clipboard: VaultClipboard

    public private(set) var fingerprint = ""
    public private(set) var message = ""
    public var password = ""
    public var confirm = ""
    /// Present the recovery code dialog while this is set.
    public var recoveryPrompt: VaultRecoveryCodePrompt?

    public init(vault: VaultService, clipboard: VaultClipboard) {
        self.vault = vault
        self.clipboard = clipboard
    }

    /// Locked underneath the screen (idle timer): nothing here may stay on screen, so pop it.
    public var shouldClose: Bool { vault.phase != .unlocked }

    /// Call when the screen appears.
    public func start() async {
        if let text = try? await vault.myFingerprint() { fingerprint = text }
        await vault.refreshAutofillStatus()
    }

    /// Back from the system's autofill settings: read the choice again.
    public func appResumed() async {
        await vault.refreshAutofillStatus()
    }

    private func perform(done: String = "", _ work: () async throws -> Void) async {
        do {
            try await work()
            message = done
        } catch {
            message = VaultCopy.errorText(error)
        }
    }

    // MARK: Autofill

    public var autofill: VaultAutofillStatus { vault.autofillStatus }

    /// The toggle works only with a screen lock set.
    public var autofillToggleEnabled: Bool { vault.autofillStatus.deviceSecure }

    public var autofillSubtitle: String {
        vault.autofillStatus.deviceSecure
            ? "Keeps your logins on this phone, encrypted under a key in its secure hardware that only your fingerprint, face, or screen lock releases. Each fill asks for it."
            : "Set a screen lock on this phone first: it is what protects the logins autofill keeps here."
    }

    /// Shown under the toggle while autofill is on; nil while it is off.
    public var autofillState: String? {
        guard vault.autofillEnabled else { return nil }
        let status = vault.autofillStatus
        if !status.enabled {
            return "One more step: in Settings \u{2192} General \u{2192} AutoFill & Passwords, turn on ForkMesh."
        }
        return "ForkMesh is your autofill provider. \(status.logins) login\(status.logins == 1 ? "" : "s") ready. Websites and apps get the logins saved for their own site above the keyboard; anywhere else, tap Passwords and pick one, and it is suggested there next time."
    }

    public var showsOpenAutofillSettings: Bool { vault.autofillEnabled && !vault.autofillStatus.enabled }

    public func setAutofill(_ enabled: Bool) async {
        await perform { await self.vault.setAutofill(enabled) }
    }

    public func openAutofillSettings() async { await vault.openAutofillSettings() }

    // MARK: Locking

    public var autoLockChoices: [Int] { VaultService.autoLockChoices }

    public func autoLockLabel(_ minutes: Int) -> String { "\(minutes) minute\(minutes == 1 ? "" : "s") idle" }

    public func setAutoLock(_ minutes: Int) async {
        await perform { self.vault.setAutoLockMinutes(minutes) }
    }

    public var showsQuickUnlock: Bool { vault.quickUnlockAvailable }

    public func setQuickUnlock(_ enabled: Bool) async {
        await perform { _ = await self.vault.setQuickUnlock(enabled) }
    }

    // MARK: Keys

    public func changePassword() async {
        if let problem = VaultCopy.newPasswordProblem(password, confirm) {
            message = problem
            return
        }
        let password = self.password
        self.password = ""
        confirm = ""
        // The stored quick-unlock copy is the keyset, which did not change.
        await perform(done: Self.passwordChanged) { try await self.vault.changePassword(password) }
    }

    public func renewRecoveryCode() async {
        await perform {
            let code = try await self.vault.renewRecoveryCode()
            self.recoveryPrompt = VaultRecoveryCodePrompt(code: code, clipboard: self.clipboard)
        }
    }

    public func finishRecoveryPrompt() {
        if recoveryPrompt?.canFinish == true { recoveryPrompt = nil }
    }

    public func copyExport() async {
        await perform(done: Self.exportCopied) {
            let payload = try await self.vault.exportData()
            self.clipboard.copy(JSON.encodePretty(payload["export"] ?? .null), expiresAfter: nil)
        }
    }

    public var destroyConfirmation: VaultConfirmation {
        VaultConfirmation(
            title: "Delete your vault?",
            message: "Every login in it is erased here and for everyone you shared it with. The encrypted export is copied to the clipboard first; it opens only with your master password or recovery code.",
            confirm: "Delete",
            cancel: VaultCopy.cancel,
            destructive: true)
    }

    /// After the person confirmed `destroyConfirmation`.
    public func destroy() async {
        await perform {
            let payload = try await self.vault.destroy()
            self.clipboard.copy(JSON.encodePretty(payload["export"] ?? .null), expiresAfter: nil)
        }
    }
}
