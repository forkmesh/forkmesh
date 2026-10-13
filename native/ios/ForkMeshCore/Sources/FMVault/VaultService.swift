// App-level state of the Password Vault. One
// client for the signed-in account that stays unlocked across screens, and locks itself when
// idle, when the app has been in the background, and on sign-out.
//
// The request function is handed in (FMVault sits below FMApi), and must throw `VaultError(code)` for
// an error answer. The app wires it to `accountApiCall` and calls `refresh()` / `signedOut()`
// when the session changes and `appPaused()` / `appResumed()` from the scene phase.

import FMBase
import Foundation
import Observation

public enum VaultPhase: Equatable, Sendable {
    case loading, signedOut, notSetUp, locked, unlocked
}

@Observable
@MainActor
public final class VaultService {
    /// Coming back after this long in the background finds the vault locked.
    public static let backgroundGrace: TimeInterval = 60
    public static let autoLockChoices = [1, 5, 15]

    @ObservationIgnored public let client: VaultClient
    @ObservationIgnored private let accountProvider: () -> String
    @ObservationIgnored private var platform: VaultPlatform
    @ObservationIgnored private let deviceAuth: VaultDeviceAuth
    @ObservationIgnored private let store: SecureStore
    @ObservationIgnored private let clock: Clock
    /// Seconds in an auto-lock "minute"; tests shorten it.
    @ObservationIgnored var secondsPerMinute: TimeInterval = 60

    public private(set) var phase: VaultPhase = .loading
    @ObservationIgnored private var boundAccount = ""
    public private(set) var autoLockMinutes = 5
    public private(set) var quickUnlockEnabled = false
    public private(set) var quickUnlockAvailable = false
    public private(set) var autofillEnabled = false
    public private(set) var autofillStatus = VaultAutofillStatus()
    public private(set) var incomingImport: VaultIncomingImport?
    @ObservationIgnored private var idleTask: Task<Void, Never>?
    @ObservationIgnored private var pausedAt: Date?

    /// - Parameters:
    ///   - request: one call under /api/vault (see `VaultRequest`).
    ///   - accountProvider: the signed-in account's name ("" when signed out).
    ///   - client: a prepared client (tests); otherwise one over `request` with Foundation's NFKC.
    public init(
        request: @escaping VaultRequest,
        accountProvider: @escaping () -> String,
        store: SecureStore,
        platform: VaultPlatform? = nil,
        deviceAuth: VaultDeviceAuth? = nil,
        clock: Clock = SystemClock(),
        client: VaultClient? = nil
    ) {
        self.client = client ?? VaultClient(request: request)
        self.accountProvider = accountProvider
        self.store = store
        self.platform = platform ?? NoVaultPlatform()
        self.deviceAuth = deviceAuth ?? NoVaultDeviceAuth()
        self.clock = clock
    }

    public var items: [VaultItem] { client.items }
    public var account: String { client.account }
    public func find(_ id: String) -> VaultItem? { client.find(id) }

    private func key(_ name: String) -> String { "vault/\(boundAccount)/\(name)" }

    /// Reads whether the signed-in account has a vault. A different account than last time locks
    /// first: a vault never follows a sign-in.
    public func refresh() async throws {
        let signedIn = accountProvider().dartTrimmed.lowercased()
        if signedIn != boundAccount {
            lock()
            boundAccount = signedIn
        }
        if signedIn.isEmpty {
            phase = .signedOut
            return
        }
        if client.unlocked {
            phase = .unlocked
            return
        }
        try await client.status()
        autoLockMinutes = Int(store.read(key("autoLockMinutes")) ?? "") ?? 5
        if !Self.autoLockChoices.contains(autoLockMinutes) { autoLockMinutes = 5 }
        quickUnlockAvailable = await deviceAuth.available()
        quickUnlockEnabled = quickUnlockAvailable && !(store.read(key("quickUnlock")) ?? "").isEmpty
        autofillEnabled = store.read(key("autofill")) == "1"
        autofillStatus = await platform.autofillStatus()
        phase = client.installed ? .locked : .notSetUp
    }

    // MARK: - Unlocking

    /// Creates the vault; returns the recovery code, to be shown exactly once.
    public func create(_ masterPassword: String) async throws -> String {
        let recoveryCode = try await client.setUp(masterPassword)
        try await entered()
        return recoveryCode
    }

    public func unlock(_ masterPassword: String) async throws {
        try await client.unlock(masterPassword)
        try await entered()
    }

    /// Unlocks from the keyset kept behind the device's biometric or passcode prompt. False when
    /// the person declined or nothing is stored.
    @discardableResult
    public func unlockWithDevice() async throws -> Bool {
        if !quickUnlockEnabled { return false }
        let stored = store.read(key("quickUnlock")) ?? ""
        if stored.isEmpty { return false }
        if !(await deviceAuth.authenticate(reason: "Unlock your ForkMesh vault")) { return false }
        do {
            guard let saved = (try? JSON.parse(stored))?.objectValue else { throw VaultError("invalid_keyset") }
            try await client.resume(saved)
        } catch let error as VaultError {
            // The vault was deleted and recreated elsewhere: what is stored here opens nothing
            // any more.
            if error.code == "keyset_mismatch" || error.code == "invalid_keyset" {
                _ = await setQuickUnlock(false)
            }
            throw error
        }
        try await entered()
        return true
    }

    public func recover(_ recoveryCode: String, newMasterPassword: String) async throws -> String {
        let renewed = try await client.recover(recoveryCode, newMasterPassword: newMasterPassword)
        try await entered()
        return renewed
    }

    private func entered() async throws {
        await importPendingSaves()
        try await reload()
        // Catch up organization members who joined since the last unlock.
        let client = self.client
        Task { @MainActor in _ = try? await client.syncPending() }
        phase = .unlocked
        touch()
    }

    public func lock() {
        idleTask?.cancel()
        idleTask = nil
        client.lock()
        if phase == .unlocked { phase = .locked }
    }

    /// Any interaction with the vault screen pushes the idle lock out again.
    public func touch() {
        idleTask?.cancel()
        idleTask = nil
        if !client.unlocked { return }
        let delay = Double(autoLockMinutes) * secondsPerMinute
        idleTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            if Task.isCancelled { return }
            self?.lock()
        }
    }

    /// The app left the foreground. Coming back after `backgroundGrace` finds the vault locked,
    /// however long the idle timer had left.
    public func appPaused() {
        if pausedAt == nil { pausedAt = clock.now }
    }

    public func appResumed() {
        let pausedAt = self.pausedAt
        self.pausedAt = nil
        if let pausedAt, clock.now.timeIntervalSince(pausedAt) >= Self.backgroundGrace {
            lock()
        }
    }

    // MARK: - Items

    public func reload() async throws {
        _ = try await client.load()
        await syncAutofill()
    }

    @discardableResult
    public func save(_ fields: JSONObject, id: String = "") async throws -> String {
        touch()
        let saved = try await client.save(fields, id: id)
        try await reload()
        return saved
    }

    public func remove(_ id: String) async throws {
        touch()
        try await client.remove(id)
        try await reload()
    }

    public func previewShare(kind: String, name: String) async throws -> [VaultRecipient] {
        touch()
        return try await client.previewShare(kind: kind, name: name)
    }

    public func share(_ id: String, kind: String, name: String, permission: String, accepted: [String] = []) async throws -> VaultShareOutcome {
        touch()
        let outcome = try await client.share(id, kind: kind, name: name, permission: permission, accepted: accepted)
        try await reload()
        return outcome
    }

    public func unshare(_ id: String, kind: String, name: String) async throws {
        touch()
        try await client.unshare(id, kind: kind, name: name)
        try await reload()
    }

    public func importLogins(_ logins: [VaultTransferLogin]) async throws -> VaultImportReport {
        touch()
        if !client.unlocked { throw VaultError("vault_locked") }
        let plan = VaultTransfer.plan(logins, existing: client.items)
        var added = 0
        var stoppedBy = ""
        for login in plan.fresh {
            do {
                try await client.save(login.fields)
                added += 1
                touch()
            } catch {
                stoppedBy = VaultCopy.errorText(error)
                break
            }
        }
        try? await reload()
        return VaultImportReport(
            added: added, duplicates: plan.duplicates, remaining: plan.fresh.count - added, stoppedBy: stoppedBy)
    }

    public func exportLogins() throws -> [VaultTransferLogin] {
        touch()
        if !client.unlocked { throw VaultError("vault_locked") }
        return VaultTransfer.logins(from: client.items)
    }

    public func receiveImport(_ incoming: VaultIncomingImport) {
        incomingImport = incoming.logins.isEmpty ? nil : incoming
    }

    public func takeIncomingImport() -> VaultIncomingImport? {
        let incoming = incomingImport
        incomingImport = nil
        return incoming
    }

    public func organizations() async throws -> [String] { try await client.organizations() }
    public func myFingerprint() async throws -> String { try await client.myFingerprint() }
    public func exportData() async throws -> JSONObject { try await client.exportData() }

    public func changePassword(_ masterPassword: String) async throws {
        touch()
        try await client.changePassword(masterPassword)
    }

    public func renewRecoveryCode() async throws -> String {
        touch()
        return try await client.renewRecoveryCode()
    }

    @discardableResult
    public func destroy() async throws -> JSONObject {
        let payload = try await client.destroy()
        await forgetDeviceCopies()
        phase = .notSetUp
        return payload
    }

    // MARK: - Settings

    public func setAutoLockMinutes(_ minutes: Int) {
        autoLockMinutes = Self.autoLockChoices.contains(minutes) ? minutes : 5
        store.write(key("autoLockMinutes"), String(autoLockMinutes))
        touch()
    }

    /// Keeps the unlocked keyset in the platform's secure storage (the Keychain), released only
    /// after the device's own prompt. It trades the master password's strength for the device
    /// lock's: opt-in.
    @discardableResult
    public func setQuickUnlock(_ enabled: Bool) async -> Bool {
        if enabled {
            if !client.unlocked { return false }
            if !(await deviceAuth.available()) { return false }
            if !(await deviceAuth.authenticate(reason: "Turn on quick unlock for your vault")) { return false }
            guard let keyset = try? client.exportKeyset() else { return false }
            store.write(key("quickUnlock"), JSON.encode(keyset))
        } else {
            store.delete(key("quickUnlock"))
        }
        quickUnlockEnabled = enabled
        return true
    }

    public func setAutofill(_ enabled: Bool) async {
        autofillEnabled = enabled
        store.write(key("autofill"), enabled ? "1" : "0")
        if enabled {
            await syncAutofill()
        } else {
            await platform.clearAutofill()
        }
        autofillStatus = await platform.autofillStatus()
    }

    public func openAutofillSettings() async { await platform.openAutofillSettings() }

    public func attach(platform: VaultPlatform) async {
        self.platform = platform
        autofillStatus = await platform.autofillStatus()
        await syncAutofill()
    }

    public func refreshAutofillStatus() async {
        autofillStatus = await platform.autofillStatus()
    }

    /// Hands the native autofill provider the logins it may offer: only ones with a host to match
    /// and something to fill. Off means it holds nothing.
    private func syncAutofill() async {
        if !autofillEnabled || !client.unlocked { return }
        let logins = client.items.filter { $0.readable && !$0.password.isEmpty }.map {
            VaultAutofillLogin(id: $0.id, title: $0.title, username: $0.username, password: $0.password, host: $0.host)
        }
        await platform.syncAutofill(account: client.account, logins: logins)
        autofillStatus = await platform.autofillStatus()
    }

    /// Logins the person saved from another app while the vault was locked are encrypted into the
    /// vault now that it is open.
    private func importPendingSaves() async {
        if !autofillEnabled { return }
        for pending in await platform.takePendingSaves() {
            let host = pending.host
            do {
                try await client.save([
                    "title": .string(pending.title.isEmpty ? host : pending.title),
                    "username": .string(pending.username),
                    "password": .string(pending.password),
                    "url": .string(host.isEmpty ? "" : "https://\(host)"),
                    "notes": .string(pending.app.isEmpty ? "" : "Saved from \(pending.app) by autofill."),
                ])
            } catch let error as VaultError {
                Log.warning("Vault", "a pending autofill save was dropped (\(error.code))")
            } catch {
                Log.warning("Vault", "a pending autofill save was dropped (\(error))")
            }
        }
    }

    private func forgetDeviceCopies() async {
        await platform.clearAutofill()
        if !boundAccount.isEmpty {
            store.delete(key("quickUnlock"))
            store.delete(key("autofill"))
        }
        quickUnlockEnabled = false
        autofillEnabled = false
    }

    /// Sign-out: nothing of this account's vault stays on the device.
    public func signedOut() async {
        lock()
        incomingImport = nil
        await forgetDeviceCopies()
        boundAccount = ""
        phase = .signedOut
    }

    /// Stops the idle timer and wipes the keys.
    public func dispose() {
        idleTask?.cancel()
        idleTask = nil
        incomingImport = nil
        client.lock()
    }
}

public struct VaultIncomingImport: Equatable, Sendable {
    public let source: String
    public let logins: [VaultTransferLogin]

    public init(source: String, logins: [VaultTransferLogin]) {
        self.source = source
        self.logins = logins
    }
}
