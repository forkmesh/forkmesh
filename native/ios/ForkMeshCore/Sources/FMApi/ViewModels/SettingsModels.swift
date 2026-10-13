// View models of the Settings screen: the text fields, the node registry
// card, the sessions card and the delete-account dialog. The network calls
// are closures so the cards work with or without an ApiService.

import FMAccount
import FMBase
import FMNet
import Foundation
import Observation

/// The Settings screen's free-text fields. Each is read once from the
/// settings and saved trimmed on every change.
@MainActor
@Observable
public final class SettingsFormModel {
    public enum Field: CaseIterable, Sendable {
        case displayName, room, passphrase, github, gitlab
    }

    public var displayName: String
    public var room: String
    public var passphrase: String
    public var github: String
    public var gitlab: String

    @ObservationIgnored public let settings: SettingsService

    public init(settings: SettingsService) {
        self.settings = settings
        displayName = settings.displayName
        room = settings.room
        passphrase = settings.passphrase
        github = settings.githubToken
        gitlab = settings.gitlabToken
    }

    public func text(_ field: Field) -> String {
        switch field {
        case .displayName: return displayName
        case .room: return room
        case .passphrase: return passphrase
        case .github: return github
        case .gitlab: return gitlab
        }
    }

    public func setText(_ field: Field, _ value: String) {
        switch field {
        case .displayName: displayName = value
        case .room: room = value
        case .passphrase: passphrase = value
        case .github: github = value
        case .gitlab: gitlab = value
        }
        save(field)
    }

    /// Persist the field's trimmed text.
    public func save(_ field: Field) {
        let value = text(field).dartTrimmed
        switch field {
        case .displayName: settings.setDisplayName(value)
        case .room: settings.setRoom(value)
        case .passphrase: settings.setPassphrase(value)
        case .github: settings.setGithubToken(value)
        case .gitlab: settings.setGitlabToken(value)
        }
    }

    /// The build method's `if (accountName.isNotEmpty && (...)) _name.text =
    /// accountName`: shows the account name in the field without saving it.
    public func adoptAccountName(_ session: AuthSession?) {
        if let adopted = adoptedDisplayName(displayName, session: session) {
            displayName = adopted
        }
    }
}

/// `_AccountNodesCard`: GET /api/accounts/devices, falling back to the
/// inventory cached on the login payload while loading or after a failure.
@MainActor
@Observable
public final class AccountNodesModel {
    public private(set) var devices: [AccountDevice]?
    public private(set) var loading = false
    public private(set) var error = ""

    public let session: AuthSession?
    @ObservationIgnored private let listDevices: (() async throws -> [AccountDevice])?

    public init(session: AuthSession?, listDevices: (() async throws -> [AccountDevice])?) {
        self.session = session
        self.listDevices = listDevices
    }

    public var signedIn: Bool { hasRealSession(session) }
    public var canRefresh: Bool { listDevices != nil }

    /// Nil while signed out (the card shows its sign-in note instead).
    public var summary: NodesSummary? {
        guard signedIn, let session else { return nil }
        return nodesSummary(devices, session: session)
    }

    public func isCurrent(_ device: AccountDevice) -> Bool {
        guard let session else { return device.current }
        return isCurrentDevice(device, session: session)
    }

    public func load() async {
        guard signedIn, let listDevices else { return }
        loading = true
        error = ""
        do {
            let fresh = try await listDevices()
            devices = fresh
            loading = false
        } catch {
            loading = false
            self.error = SettingsCopy.nodesRefreshFailed
        }
    }
}

/// `_AccountSessionsCard`: every live sign-in on the account, with a revoke
/// per row and "Log out all other sessions".
@MainActor
@Observable
public final class AccountSessionsModel {
    public private(set) var sessions: [AccountSession]?
    public private(set) var loading = false
    /// The session id being revoked, "others", or "".
    public private(set) var busyId = ""
    public private(set) var error = ""

    @ObservationIgnored private let listSessions: (() async throws -> [AccountSession])?
    @ObservationIgnored private let revokeSession: ((String) async throws -> Bool)?

    /// `revokeSession` returns whether the revoke also ended this app's own
    /// session (`ApiService.revokeSession`).
    public init(
        listSessions: (() async throws -> [AccountSession])?,
        revokeSession: ((String) async throws -> Bool)?
    ) {
        self.listSessions = listSessions
        self.revokeSession = revokeSession
    }

    public var apiAvailable: Bool { listSessions != nil }
    public var visible: [AccountSession] { sessions ?? [] }
    public var othersCount: Int { visible.filter { !$0.current }.count }
    public var headline: String { sessionsHeadline(sessions) }
    public var emptyText: String {
        sessionsEmptyText(apiAvailable: apiAvailable, loading: loading, loaded: sessions != nil)
    }

    public func isBusy(_ session: AccountSession) -> Bool { busyId == session.id || busyId == "others" }
    public var canRevoke: Bool { busyId.isEmpty }

    public func load() async {
        guard let listSessions else { return }
        loading = true
        error = ""
        do {
            let fresh = try await listSessions()
            sessions = fresh
            loading = false
        } catch {
            loading = false
            self.error = SettingsCopy.sessionsLoadFailed
        }
    }

    /// What a revoke did.
    public enum RevokeOutcome: Equatable, Sendable {
        /// Nothing ran (no API, or another revoke is in flight).
        case ignored
        /// This app's own session ended: sign out locally.
        case currentSignedOut
        /// The inline notice for a revoke of other sessions.
        case revoked(String)
        case failed
    }

    /// `_revoke` after the confirmation was accepted.
    public func revoke(_ target: String) async -> RevokeOutcome {
        guard let revokeSession, busyId.isEmpty else { return .ignored }
        busyId = target
        error = ""
        do {
            let currentRevoked = try await revokeSession(target)
            if currentRevoked {
                return .currentSignedOut
            }
            busyId = ""
            let remaining = sessions ?? []
            sessions = target == "others" ? remaining.filter { $0.current } : remaining.filter { $0.id != target }
            return .revoked(target == "others" ? "Logged out of every other session." : "Session logged out.")
        } catch {
            busyId = ""
            self.error = SettingsCopy.sessionRevokeFailed
            return .failed
        }
    }
}

/// The delete-account dialog (`_confirmDeleteAccount`).
@MainActor
@Observable
public final class DeleteAccountModel {
    public var password = ""
    public var confirm = ""
    public private(set) var error: String?
    public private(set) var busy = false

    @ObservationIgnored private let delete: (String) async throws -> Void

    public init(delete: @escaping (String) async throws -> Void) {
        self.delete = delete
    }

    public convenience init(auth: AuthService) {
        self.init(delete: { password in try await auth.deleteAccount(password: password) })
    }

    /// True when the account was deleted and the dialog should close.
    public func submit() async -> Bool {
        if let problem = deleteAccountConfirmError(confirm) {
            error = problem
            return false
        }
        busy = true
        defer { busy = false }
        do {
            try await delete(password)
            return true
        } catch {
            self.error = deleteAccountFailure(error)
            return false
        }
    }
}

extension ApiService: AccountEndpointCalling {
    public var serverOrigin: String { gitOrigin }

    public func accountCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        guard ["GET", "POST", "DELETE", "PUT", "PATCH"].contains(method), path.hasPrefix("/api/") else {
            throw AccountEndpointError("unsupported_request")
        }
        var headers: HTTPHeaders = ["Accept": "application/json"]
        if method != "GET" {
            headers["Content-Type"] = "application/json"
            headers["Origin"] = gitOrigin
        }
        let payload: Data? = method == "GET" ? nil : JSON.encodeData(.object(body ?? JSONObject()))
        let response: HTTPResponse
        do {
            response = try await rawRequest(method, url(path), headers: headers, body: payload)
        } catch {
            throw AccountEndpointError("offline")
        }
        var map = JSONObject()
        if !response.body.isEmpty, let decoded = try? JSON.parse(response.body), let object = decoded.objectValue {
            map = object
        }
        if response.ok && map["ok"] != .bool(false) { return map }
        let code = map.str("error")
        throw AccountEndpointError(code.isEmpty ? "http_\(response.status)" : code, status: response.status)
    }
}
