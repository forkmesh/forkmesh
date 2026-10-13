// The platform half of the Password Vault, as protocols the app target implements
// (ForkMesh/Platform). Unicode normalization is Foundation's (`VaultCrypto.nfkc`).
//
// iOS's credential provider extension cannot run the vault, so after the vault is unlocked the
// app hands it what it needs to fill a login on its own, behind the device's biometric or
// passcode prompt. The clipboard is a protocol too so it can carry iOS's expiry.

import Foundation

public struct VaultAutofillStatus: Equatable, Sendable {
    /// The platform has an autofill framework this app can plug into.
    public var supported: Bool
    /// ForkMesh is the autofill provider the person chose in system settings.
    public var enabled: Bool
    /// A screen lock is set. Without one the login cache cannot be protected, so autofill stays off.
    public var deviceSecure: Bool
    /// Logins currently available to autofill.
    public var logins: Int

    public init(supported: Bool = false, enabled: Bool = false, deviceSecure: Bool = false, logins: Int = 0) {
        self.supported = supported
        self.enabled = enabled
        self.deviceSecure = deviceSecure
        self.logins = logins
    }
}

/// One login autofill may offer. `host` is already reduced to the exact host it may be filled on.
public struct VaultAutofillLogin: Equatable, Sendable {
    public let id: String
    public let title: String
    public let username: String
    public let password: String
    public let host: String

    public init(id: String, title: String, username: String, password: String, host: String) {
        self.id = id
        self.title = title
        self.username = username
        self.password = password
        self.host = host
    }

    /// The login as a map: {id, title, username, password, host}.
    public var dictionary: [String: String] {
        ["id": id, "title": title, "username": username, "password": password, "host": host]
    }
}

/// A login the person chose to save from another app while the vault was locked.
public struct VaultPendingSave: Equatable, Sendable {
    public let title: String
    public let host: String
    public let username: String
    public let password: String
    /// The app it was saved from ("" when unknown).
    public let app: String

    public init(title: String = "", host: String = "", username: String = "", password: String = "", app: String = "") {
        self.title = title
        self.host = host
        self.username = username
        self.password = password
        self.app = app
    }
}

/// The phone's autofill.
@MainActor
public protocol VaultPlatform: AnyObject {
    func autofillStatus() async -> VaultAutofillStatus
    /// Opens the system screen where the person picks their autofill provider.
    func openAutofillSettings() async
    /// Replaces what autofill may offer.
    @discardableResult
    func syncAutofill(account: String, logins: [VaultAutofillLogin]) async -> Bool
    func clearAutofill() async
    /// Saved while locked; returned once, then forgotten by the native side.
    func takePendingSaves() async -> [VaultPendingSave]
}

/// A platform without autofill: every call answers empty or does nothing.
@MainActor
public final class NoVaultPlatform: VaultPlatform {
    public init() {}
    public func autofillStatus() async -> VaultAutofillStatus { VaultAutofillStatus() }
    public func openAutofillSettings() async {}
    public func syncAutofill(account: String, logins: [VaultAutofillLogin]) async -> Bool { false }
    public func clearAutofill() async {}
    public func takePendingSaves() async -> [VaultPendingSave] { [] }
}

/// The device's own lock: biometrics or passcode (`LAContext` with
/// `.deviceOwnerAuthentication`). Quick unlock is offered only when the platform can really ask;
/// there is no in-app stand-in. Neither method throws: any failure is false.
@MainActor
public protocol VaultDeviceAuth: AnyObject {
    func available() async -> Bool
    func authenticate(reason: String) async -> Bool
}

@MainActor
public final class NoVaultDeviceAuth: VaultDeviceAuth {
    public init() {}
    public func available() async -> Bool { false }
    public func authenticate(reason: String) async -> Bool { false }
}

/// The system clipboard. With `expiresAfter` the text must disappear after that many seconds
/// (UIPasteboard's `.expirationDate`, and `.localOnly` so it never reaches other devices); best
/// effort.
@MainActor
public protocol VaultClipboard: AnyObject {
    func copy(_ text: String, expiresAfter: TimeInterval?)
}
