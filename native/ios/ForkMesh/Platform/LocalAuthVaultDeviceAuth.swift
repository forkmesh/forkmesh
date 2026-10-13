// The Password Vault's quick unlock: FMVault's VaultDeviceAuth over LAContext
// with `.deviceOwnerAuthentication`, so Face ID, Touch ID or the passcode may
// confirm it is the phone's owner.

import FMVault
import Foundation
import LocalAuthentication

@MainActor
final class LocalAuthVaultDeviceAuth: VaultDeviceAuth {
    func available() async -> Bool {
        let context: LAContext = LAContext()
        var error: NSError?
        return context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error)
    }

    /// Never throws: a cancel, a lockout or no enrolled biometry is false.
    func authenticate(reason: String) async -> Bool {
        let context: LAContext = LAContext()
        do {
            return try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)
        } catch {
            return false
        }
    }
}
