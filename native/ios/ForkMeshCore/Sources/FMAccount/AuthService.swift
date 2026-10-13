// Kotlin counterparts: api/AuthController.kt and app/.../feature/account/AuthService.kt.

import FMBase
import FMCrypto
import FMNet
import Foundation
import Observation

/// Sign-in state for this phone, per relay: the persisted session, guest mode, the "signed out
/// from the web" banner, login with the mobile-node device bind, logout with token revocation,
/// revocation handling (key rotation) and account deletion.
///
/// Sessions are scoped by relay (`auth/session/<relayId>` in the secure store) because an account
/// is a fact *about a relay*, not about the phone. Sessions of relays other than the active one are
/// held in memory so the relay list can show who you are on each.
@MainActor
@Observable
public final class AuthService {
    /// Legacy, unscoped session slot. Read once on startup and migrated into the active relay's
    /// slot; nothing writes it any more.
    nonisolated public static let sessionKey = "auth/session"
    nonisolated public static func sessionKeyFor(_ relayId: String) -> String { "\(sessionKey)/\(relayId)" }

    /// One-shot flag set when the Worker reported this device as revoked (or its device-bound
    /// session as invalid). Scoped per relay.
    nonisolated public static let revokedBannerKey = "auth/revokedBanner"
    nonisolated public static func revokedBannerKeyFor(_ relayId: String) -> String { "\(revokedBannerKey)/\(relayId)" }
    nonisolated public static let revokedBannerMessage = "This device was signed out from the web."

    /// Device kind this app registers as on login (mobile-node-contract §3).
    nonisolated public static let mobileDeviceKind = "mobile_node"
    nonisolated public static let deviceBindPrefix = "forkmesh-device-bind-v1"

    /// Preference set while the user explores without an account. Carries no identity and no
    /// token; a real sign-in clears it. Scoped per relay.
    nonisolated public static let guestModeKey = "auth/guestMode"
    nonisolated public static func guestModeKeyFor(_ relayId: String) -> String { "\(guestModeKey)/\(relayId)" }

    /// `^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$`.
    nonisolated public static func isValidNodeName(_ value: String) -> Bool {
        let b = Array(value.utf8)
        guard let first = b.first, first >= 0x61, first <= 0x7a, b.count <= 63 else { return false }
        if b.count == 1 { return true }
        let isLowerAlnum: (UInt8) -> Bool = { ($0 >= 0x61 && $0 <= 0x7a) || ($0 >= 0x30 && $0 <= 0x39) }
        guard let last = b.last, isLowerAlnum(last) else { return false }
        return b[1..<(b.count - 1)].allSatisfy { isLowerAlnum($0) || $0 == 0x2d }
    }

    @ObservationIgnored public let settings: SettingsService
    @ObservationIgnored private let identity: DeviceIdentity?
    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private let secure: SecureStore
    @ObservationIgnored private let http: HTTPClient
    @ObservationIgnored private let performanceMonitor: PerformanceMonitorService?
    @ObservationIgnored private let deviceLabelProvider: () async throws -> String
    @ObservationIgnored private let deviceTimestampProvider: () -> String
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored public let listeners = Listeners()

    /// The session of the active relay.
    public private(set) var session: AuthSession?
    /// Bumped by every change, so preference-backed getters (`isGuest`, ...) re-render views.
    public private(set) var revision = 0

    /// Sessions held for relays other than the active one, by relay id.
    private var otherSessions: [String: AuthSession] = [:]
    /// The relay the in-memory `session` was loaded for. Tracked separately from `relayScope`
    /// because the directory flips first during a switch.
    @ObservationIgnored private var parkedRelayId = ""
    @ObservationIgnored private var refreshingAvatar = false

    /// `identity` nil means the device key could not be loaded: sign-in then fails with
    /// `identity_unavailable`. `deviceInfo` builds the bind's `deviceLabel` unless a
    /// `deviceLabelProvider` is given.
    public init(
        settings: SettingsService,
        identity: DeviceIdentity?,
        http: HTTPClient,
        prefs: KeyValueStore? = nil,
        secureStore: SecureStore? = nil,
        performanceMonitor: PerformanceMonitorService? = nil,
        session: AuthSession? = nil,
        deviceInfo: DeviceInfo? = nil,
        deviceLabelProvider: (() async throws -> String)? = nil,
        deviceTimestampProvider: (() -> String)? = nil,
        clock: Clock = SystemClock()
    ) {
        self.settings = settings
        self.identity = identity
        self.http = http
        self.prefs = prefs ?? settings.prefs
        self.secure = secureStore ?? settings.secure
        self.performanceMonitor = performanceMonitor
        self.session = session
        self.clock = clock
        let label = deviceInfo?.label ?? "Mobile device"
        self.deviceLabelProvider = deviceLabelProvider ?? { label }
        self.deviceTimestampProvider = deviceTimestampProvider ?? { String(clock.nowMillis) }
        // A session handed in here belongs to whichever relay is active now.
        parkedRelayId = settings.relays.activeId
    }

    /// `AuthService.create`: migrate a plaintext session into the secure store, move an unscoped
    /// session into the active relay's slot, and load every relay's session.
    public static func create(
        settings: SettingsService,
        identity: DeviceIdentity?,
        http: HTTPClient,
        prefs: KeyValueStore? = nil,
        secureStore: SecureStore? = nil,
        performanceMonitor: PerformanceMonitorService? = nil,
        deviceInfo: DeviceInfo? = nil,
        deviceLabelProvider: (() async throws -> String)? = nil,
        deviceTimestampProvider: (() -> String)? = nil,
        clock: Clock = SystemClock()
    ) -> AuthService {
        let store = secureStore ?? settings.secure
        let preferences = prefs ?? settings.prefs
        store.migrateFromPrefs(preferences, keys: [sessionKey])
        let service = AuthService(
            settings: settings, identity: identity, http: http, prefs: preferences, secureStore: store,
            performanceMonitor: performanceMonitor, deviceInfo: deviceInfo, deviceLabelProvider: deviceLabelProvider,
            deviceTimestampProvider: deviceTimestampProvider, clock: clock)
        service.migrateUnscopedSession()
        service.loadAllSessions()
        return service
    }

    // MARK: Listeners

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    private func notifyListeners() {
        revision &+= 1
        listeners.notify()
    }

    private func track() { _ = revision }

    // MARK: State

    public var isAuthenticated: Bool { session != nil }

    /// The relay these credentials belong to, read from the directory.
    public var relayScope: String { settings.relays.activeId }

    private var sessionSlot: String { AuthService.sessionKeyFor(relayScope) }
    private var guestKey: String { AuthService.guestModeKeyFor(relayScope) }
    private var bannerKey: String { AuthService.revokedBannerKeyFor(relayScope) }

    /// The session held for `relayId`, including the active one.
    public func sessionForRelay(_ relayId: String) -> AuthSession? {
        relayId == relayScope ? session : otherSessions[relayId]
    }

    /// Whether a switch to `relayId` would land signed in (or in guest mode).
    public func isSignedInOnRelay(_ relayId: String) -> Bool {
        track()
        return sessionForRelay(relayId) != nil || prefs.bool(forKey: AuthService.guestModeKeyFor(relayId)) == true
    }

    /// Relays this phone currently holds a real session for, the active one first.
    public var signedInRelayIds: [String] {
        var ids: [String] = []
        if session != nil { ids.append(relayScope) }
        ids.append(contentsOf: otherSessions.keys.sorted())
        return ids
    }

    /// True while the user is exploring without an account. Guests can browse everything that
    /// reads, but every write surface asks them to sign in first.
    public var isGuest: Bool {
        track()
        return !isAuthenticated && prefs.bool(forKey: guestKey) == true
    }

    /// Whether the home shell should be shown: a real session or guest mode.
    public var hasAppAccess: Bool { isAuthenticated || isGuest }

    public func enterGuestMode() {
        prefs.set(true, forKey: guestKey)
        notifyListeners()
    }

    /// Leaves guest mode and returns to the welcome screen.
    public func exitGuestMode() {
        prefs.remove(guestKey)
        notifyListeners()
    }

    public var revokedBannerPending: Bool {
        track()
        return prefs.bool(forKey: bannerKey) == true
    }

    public func clearRevokedBanner() {
        if prefs.contains(bannerKey) { prefs.remove(bannerKey) }
    }

    public var signupUrl: URL? { base("/signup") }

    // MARK: Relay scoping

    /// Move a session written before relays were scoped into the active relay's slot. Guest mode
    /// and the revoked banner move with it; the legacy keys are cleared.
    private func migrateUnscopedSession() {
        let scope = relayScope
        if let legacy = secure.read(AuthService.sessionKey), !legacy.isEmpty {
            secure.write(AuthService.sessionKeyFor(scope), legacy)
            secure.delete(AuthService.sessionKey)
            prefs.remove(AuthService.sessionKey)
        }
        if prefs.bool(forKey: AuthService.guestModeKey) == true {
            prefs.set(true, forKey: AuthService.guestModeKeyFor(scope))
            prefs.remove(AuthService.guestModeKey)
        }
        if prefs.bool(forKey: AuthService.revokedBannerKey) == true {
            prefs.set(true, forKey: AuthService.revokedBannerKeyFor(scope))
            prefs.remove(AuthService.revokedBannerKey)
        }
    }

    /// Read the session held for every known relay.
    private func loadAllSessions() {
        otherSessions.removeAll()
        let scope = relayScope
        parkedRelayId = scope
        session = readSession(scope)
        for relay in settings.relays.relays where relay.id != scope {
            if let stored = readSession(relay.id) { otherSessions[relay.id] = stored }
        }
    }

    /// Re-point this service at the active relay's credentials. Called by the relay switch
    /// controller *after* the directory has flipped. The session being left is parked, not
    /// revoked, and is waiting when the user switches back.
    public func adoptActiveRelay() {
        let scope = relayScope
        if scope == parkedRelayId { return }
        if let leaving = session, !parkedRelayId.isEmpty {
            otherSessions[parkedRelayId] = leaving
        }
        parkedRelayId = scope
        session = otherSessions.removeValue(forKey: scope) ?? readSession(scope)
        notifyListeners()
    }

    /// Forget everything held for `relayId`: used when a relay is removed from the list, so a
    /// re-add cannot silently resurrect a live token.
    public func forgetRelay(_ relayId: String) {
        otherSessions.removeValue(forKey: relayId)
        if relayId == relayScope {
            clearLocalSession()
        } else {
            secure.delete(AuthService.sessionKeyFor(relayId))
        }
        prefs.remove(AuthService.guestModeKeyFor(relayId))
        prefs.remove(AuthService.revokedBannerKeyFor(relayId))
        notifyListeners()
    }

    // MARK: Sign-in

    /// Log in and bind this phone as a `mobile_node` on the account.
    ///
    /// Password (+ TOTP when enrolled) authenticates the human; the phone's Ed25519 key is
    /// registered on the same call: `pubkey`, `deviceTs`, `deviceSig` over
    /// `forkmesh-device-bind-v1\n{account}\n{pubkey}\n{deviceTs}`, `deviceKind: "mobile_node"` and
    /// a `deviceLabel`. An email (or otherwise non-username) identifier first logs in without a
    /// key to learn the account name, then binds with that name; the intermediate session is
    /// revoked best-effort. `device_key_conflict` rotates the key and retries once, unless another
    /// relay holds a session bound to the same key.
    @discardableResult
    public func login(identifier: String, password: String, totp: String = "") async throws -> AuthSession {
        let cleanIdentifier = identifier.dartTrimmed.lowercased()
        let cleanTotp = totp.dartTrimmed
        guard let identity else {
            throw AuthException("identity_unavailable", "This device's signing key is not ready yet. Try again in a moment.")
        }
        var accountName = cleanIdentifier
        var intermediateToken: String?
        if cleanIdentifier.contains("@") || !AuthService.isValidNodeName(cleanIdentifier) {
            let probe = try await postJson("/api/accounts/login", [
                "identifier": .string(cleanIdentifier),
                "email": .string(cleanIdentifier),
                "password": .string(password),
                "totp": .string(cleanTotp),
            ])
            let probed = AuthSession.fromJSON(probe, now: clock.nowMillis)
            if probed.nodeName.isEmpty {
                // Worker without device binding, or an account with no node name: keep the plain
                // account session rather than failing the login.
                return saveSession(probed)
            }
            accountName = probed.nodeName.lowercased()
            intermediateToken = probed.sessionToken
        }

        var body: JSONObject
        do {
            body = try await bindLogin(identity, accountName, password, cleanTotp)
        } catch let error as AuthException where error.code == "device_key_conflict" {
            // The key is the whole phone's, not this relay's: rotating it would break the device
            // binding of every other relay already signed in. Refuse rather than sign the user
            // out of a relay they are not even looking at.
            if !otherSessions.isEmpty {
                throw AuthException(
                    "device_key_conflict",
                    "This phone’s device key is already claimed by another account on this relay. "
                        + "Sign out of your other relays first, or use a different account here.")
            }
            Identity.clearStoredSeed(secureStore: secure, prefs: prefs)
            try identity.rotate()
            body = try await bindLogin(identity, accountName, password, cleanTotp)
        }
        if let intermediateToken, !intermediateToken.isEmpty {
            Task { await self.revokeToken(intermediateToken) }
        }
        clearRevokedBanner()
        return saveSession(AuthSession.fromJSON(body, now: clock.nowMillis))
    }

    private func bindLogin(_ identity: DeviceIdentity, _ accountName: String, _ password: String, _ totp: String) async throws -> JSONObject {
        let pubkey = identity.publicKeyB64url
        let ts = deviceTimestampProvider()
        let sig = identity.sign(utf8: "\(AuthService.deviceBindPrefix)\n\(accountName)\n\(pubkey)\n\(ts)")
        return try await postJson("/api/accounts/login", [
            "identifier": .string(accountName),
            "email": .string(accountName),
            "password": .string(password),
            "totp": .string(totp),
            "pubkey": .string(pubkey),
            "deviceTs": .string(ts),
            "deviceSig": .string(sig),
            "deviceKind": .string(AuthService.mobileDeviceKind),
            "deviceLabel": .string(await safeDeviceLabel()),
        ])
    }

    private func safeDeviceLabel() async -> String {
        do {
            let label = try await deviceLabelProvider().dartTrimmed
            return label.isEmpty ? "Mobile device" : label
        } catch {
            return "Mobile device"
        }
    }

    private func revokeToken(_ token: String) async {
        do {
            _ = try await postJson("/api/accounts/logout", ["sessionToken": .string(token)], timeout: 5)
        } catch {
            Log.debug("auth", "Auth intermediate session revocation failed: \(error)")
        }
    }

    /// Called by the API layer when a Bearer-authenticated request returns 401; `code` is the
    /// Worker's `error` field when present.
    ///
    /// A plain expired session just clears local auth. `device_revoked`, or any invalid session
    /// while this session was device-bound, means the web app revoked this node: clear the
    /// session, rotate the Ed25519 seed (unless another relay's session is bound to it) and set the
    /// one-shot login banner. Local repositories are user data and are never deleted here.
    public func handleUnauthorized(code: String = "") {
        guard let current = session else { return }
        let revoked = code == "device_revoked" || (!current.deviceId.isEmpty && current.sessionKind != "preview")
        let scope = relayScope
        clearLocalSession()
        if !revoked { return }
        if otherSessions.isEmpty {
            do {
                Identity.clearStoredSeed(secureStore: secure, prefs: prefs)
                try identity?.rotate()
            } catch {
                Log.warning("auth", "Identity rotation after revocation failed: \(error)")
            }
        } else {
            Log.info("auth", "Identity rotation skipped: \(otherSessions.count) other relay session(s) are bound to this key.")
        }
        prefs.set(true, forKey: AuthService.revokedBannerKeyFor(scope))
        notifyListeners()
    }

    public func checkNodeName(_ nodeName: String) async -> AccountAvailability {
        let clean = nodeName.dartTrimmed.lowercased()
        if !AuthService.isValidNodeName(clean) {
            return AccountAvailability(
                ok: false, exists: false, available: false, name: clean,
                message: "Use lowercase letters, numbers and hyphens. Start with a letter and end with a letter or number.")
        }
        do {
            let data = try await getJson("/api/accounts/\(clean)")
            let takenFlag = data["available"] == .bool(false)
            return AccountAvailability(
                ok: true,
                exists: data.bool("exists"),
                available: !takenFlag,
                name: data.str("name", clean),
                message: takenFlag ? "That name is already taken." : "“\(clean)” is available.")
        } catch {
            return AccountAvailability(
                ok: false, exists: false, available: true, name: clean,
                message: "Could not check availability. You can still try creating the account.")
        }
    }

    /// Bring the session's avatar up to date with the account's public profile. Best-effort: a
    /// failure keeps what the session already shows.
    public func refreshAvatar() async {
        guard !refreshingAvatar, let current = session, current.sessionKind != "preview", !current.nodeName.isEmpty else {
            return
        }
        refreshingAvatar = true
        defer { refreshingAvatar = false }
        do {
            let data = try await getJson("/api/accounts/\(DartURI.encodeComponent(current.nodeName))")
            if data["exists"] == .bool(false) || !data.contains("avatarPng") { return }
            let avatarPng = data.str("avatarPng")
            if avatarPng == current.avatarPng { return }
            // The session may have changed (sign-out, re-login) while this ran.
            guard let latest = session, latest.nodeName == current.nodeName else { return }
            var json = latest.toJSON()
            json["avatarPng"] = .string(avatarPng)
            json["avatarUpdatedAt"] = data["avatarUpdatedAt"].flatMap { $0.isNull ? nil : $0 } ?? .int(latest.avatarUpdatedAt)
            let next = AuthSession.fromJSON(json, now: clock.nowMillis)
            secure.write(sessionSlot, JSON.encode(next.toJSON()))
            session = next
            notifyListeners()
        } catch {
            Log.debug("auth", "Avatar refresh failed: \(error)")
        }
    }

    /// The offline design preview's session. Never persisted across launches (`readSession`
    /// drops a stored preview).
    public func authenticatePreview() {
        if let current = session, current.sessionKind != "preview" { return }
        let now = clock.nowMillis
        saveSession(AuthSession(
            nodeName: "preview-node",
            email: "preview@forkmesh.local",
            status: "active",
            pubkey: identity?.publicKeyB64url ?? "",
            emailVerified: true,
            isAdmin: false,
            avatarPng: "",
            avatarUpdatedAt: 0,
            createdAt: now,
            savedAt: now,
            sessionKind: "preview",
            deviceKind: "mobile",
            capabilities: ["browse", "comment", "submit_issue", "submit_pr"]))
    }

    public func deleteAccount(password: String) async throws {
        guard let current = session, current.sessionKind != "preview" else {
            throw AuthException("not_authenticated", "Sign in with a real account before deleting it.")
        }
        let cleanPassword = password.dartTrimmed
        if cleanPassword.isEmpty {
            throw AuthException("password_required", "Enter your current password to delete this account.")
        }
        _ = try await postJson("/api/accounts/profile", [
            "nodeName": .string(current.nodeName),
            "email": .string(current.email),
            "sessionToken": .string(current.sessionToken),
            "password": .string(cleanPassword),
            "deleteAccount": .bool(true),
        ])
        clearLocalSession()
    }

    /// Sign out: clear the local session first (so a slow Worker cannot keep the signed-in shell
    /// on screen), then revoke the token server-side, best-effort with a 5 s bound.
    public func logout() async {
        let sessionToken = session?.sessionToken ?? ""
        clearLocalSession()
        var payload = JSONObject()
        if !sessionToken.isEmpty { payload["sessionToken"] = .string(sessionToken) }
        do {
            _ = try await postJson("/api/accounts/logout", payload, timeout: 5)
        } catch {
            // Local sign-out must still complete when the Worker is unavailable.
            Log.debug("auth", "Auth logout revocation failed: \(error)")
        }
    }

    private func clearLocalSession() {
        let key = sessionSlot
        session = nil
        otherSessions.removeValue(forKey: relayScope)
        notifyListeners()
        secure.delete(key)
        prefs.remove(AuthService.sessionKey)
    }

    private func readSession(_ relayId: String) -> AuthSession? {
        let key = AuthService.sessionKeyFor(relayId)
        guard let raw = secure.read(key), !raw.isEmpty,
              let json = (try? JSON.parse(raw))?.objectValue else { return nil }
        let stored = AuthSession.fromJSON(json, now: clock.nowMillis)
        if stored.sessionKind == "preview" {
            secure.delete(key)
            return nil
        }
        return stored.nodeName.isEmpty && stored.email.isEmpty ? nil : stored
    }

    /// A sign-in always lands on the relay that is active right now.
    @discardableResult
    private func saveSession(_ newSession: AuthSession) -> AuthSession {
        parkedRelayId = relayScope
        secure.write(sessionSlot, JSON.encode(newSession.toJSON()))
        prefs.remove(AuthService.sessionKey)
        // Signing in ends guest mode, so a later sign-out lands on the welcome screen.
        prefs.remove(guestKey)
        if !newSession.nodeName.isEmpty { settings.setDisplayName(newSession.nodeName) }
        session = newSession
        notifyListeners()
        return newSession
    }

    // MARK: HTTP

    private func base(_ path: String) -> URL? {
        settings.server.httpURL(path)
    }

    private func getJson(_ path: String) async throws -> JSONObject {
        guard let url = base(path) else { throw HTTPError.transport("Invalid relay URL") }
        let request = HTTPRequest(method: "GET", url: url, headers: ["accept": "application/json"], timeout: 20)
        return try await measured("auth.GET \(path)", path) {
            try AuthService.decodeResponse(try await self.http.send(request))
        }
    }

    private func postJson(_ path: String, _ payload: JSONObject, timeout: TimeInterval = 20) async throws -> JSONObject {
        guard let url = base(path) else { throw HTTPError.transport("Invalid relay URL") }
        let request = HTTPRequest(
            method: "POST", url: url,
            headers: ["content-type": "application/json", "accept": "application/json"],
            body: JSON.encodeData(.object(payload)), timeout: timeout)
        return try await measured("auth.POST \(path)", path) {
            try AuthService.decodeResponse(try await self.http.send(request))
        }
    }

    private func measured(_ name: String, _ path: String, _ load: () async throws -> JSONObject) async throws -> JSONObject {
        guard let monitor = performanceMonitor else { return try await load() }
        return try await monitor.track(name, details: ["path": .string(path)], load)
    }

    nonisolated static func decodeResponse(_ response: HTTPResponse) throws -> JSONObject {
        let body = response.jsonObject
        if (200..<300).contains(response.status) { return body }
        let code = body["error"].flatMap { $0.isNull ? nil : $0.text } ?? "http_\(response.status)"
        throw AuthException(code, messageFor(code))
    }

    nonisolated public static func messageFor(_ code: String) -> String {
        switch code {
        case "bad_totp": return "Enter your authenticator code."
        case "too_many_attempts": return "Too many failed attempts. Wait a few minutes and try again."
        case "account_disabled": return "This account has been disabled."
        case "invalid_credentials": return "Incorrect email, username, or password."
        case "pubkey_mismatch": return "This account is linked to a different device key."
        case "device_proof_required":
            return "Could not prove this phone's key to the server. Check the device clock and try again."
        case "device_key_conflict":
            return "This phone's key is already registered to another account. Try again to sign in with a fresh key."
        case "invalid_device_kind":
            return "The server does not accept this app as a mobile node yet. Update the app or the server."
        case "device_revoked": return revokedBannerMessage
        case "account_pending_verification":
            return "This account is pending verification. Check your email and try again."
        case "invalid_node_name": return "Choose a valid username."
        case "valid_email_required": return "Enter a valid email address."
        case "password_too_short": return "Password must be at least 8 characters."
        case "node_name_taken": return "That username is already taken."
        case "email_taken": return "That email is already registered."
        case "invalid_json": return "Could not send the request. Please try again."
        case "password_required": return "Enter your current password to delete this account."
        case "not_authenticated": return "Sign in with a real account before deleting it."
        case "bad_password", "invalid_password": return "Incorrect password. Check it and try again."
        default: return "Could not complete the request. Please try again."
        }
    }
}
