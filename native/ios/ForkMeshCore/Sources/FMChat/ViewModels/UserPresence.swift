// The user-presence gate for destructive actions (mobile-node-contract §6): delete a local
// repository, delete/transfer/archive a published one, revoke a device, change security
// settings. Never for login, browse, commit, push, publish, comment, issue or PR.
//
// The platform prompt (LocalAuthentication with passcode fallback) is the app's
// `PresencePrompt` (Features/Chat/LocalAuthPresencePrompt.swift); the in-app Confirm / Cancel
// dialog is the caller's view. The decision is here.

import Foundation

/// The device's own proof of presence (Face ID / Touch ID / passcode).
@MainActor
public protocol PresencePrompt: AnyObject {
    /// true: proven. false: the user cancelled (a real "no", not retried by the dialog). nil: the
    /// prompt is unavailable (no passcode, unsupported, failed), so the dialog decides.
    func authenticate(reason: String) async -> Bool?
}

@MainActor
public final class UserPresence {
    /// When set, `instance` returns this object instead of the shared one (tests, previews).
    public static var override: UserPresence?
    private static let shared = UserPresence()

    public static var instance: UserPresence { override ?? shared }

    /// The platform prompt; the app sets `LocalAuthPresencePrompt()` at startup.
    public var prompt: PresencePrompt?
    /// Skip the platform prompt and always use the in-app dialog.
    public var forceDialog: Bool

    public init(prompt: PresencePrompt? = nil, forceDialog: Bool = false) {
        self.prompt = prompt
        self.forceDialog = forceDialog
    }

    nonisolated public static let defaultReason = "Confirm it's you to continue."
    nonisolated public static let dialogTitle = "Confirm it's you"

    nonisolated public static func cleanReason(_ reason: String) -> String {
        let trimmed = reason.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? defaultReason : trimmed
    }

    /// Ask the user to prove presence before a destructive action. `dialog` shows the in-app
    /// Confirm / Cancel dialog with the reason and answers true for Confirm. Never throws.
    public func confirm(reason: String, dialog: @MainActor (String) async -> Bool) async -> Bool {
        let clean = UserPresence.cleanReason(reason)
        if !forceDialog, let prompt, let result = await prompt.authenticate(reason: clean) {
            return result
        }
        return await dialog(clean)
    }
}
