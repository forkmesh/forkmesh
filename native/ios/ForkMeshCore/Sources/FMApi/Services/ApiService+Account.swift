// ApiService, part 2: account-scoped endpoints (event ticket,
// push registration and self-test, bug reports, profile lookups and the user status, devices
// and sessions, the room passphrase, the notification inbox, web links, owner-key enrollment).

import FMAccount
import FMBase
import FMCrypto
import FMNet
import Foundation

extension ApiService {
    // MARK: Account event channel

    /// Trades the session token for a short-lived account event ticket
    /// (`GET /api/accounts/event-ticket`). A WebSocket upgrade cannot carry an `Authorization`
    /// header, so `AccountEventChannel` authenticates with this. The three outcomes stay
    /// distinguishable: never throws.
    public func accountEventTicket() async -> EventTicket {
        if !hasSession { return EventTicket(.signedOut) }
        let response: HTTPResponse
        do {
            response = try await http.send(HTTPRequest(
                method: "GET", url: url("/api/accounts/event-ticket"),
                headers: withAuth(["accept": "application/json"]), timeout: 30))
        } catch {
            return EventTicket(.unavailable)
        }
        if response.status == 401 || response.status == 403 { return EventTicket(.signedOut) }
        if response.status < 200 || response.status >= 300 { return EventTicket(.unavailable) }
        guard let decoded = try? JSON.parse(response.body), let map = decoded.objectValue else {
            return EventTicket(.unavailable)
        }
        if map["authenticated"] == .bool(false) { return EventTicket(.signedOut) }
        let ticket = map.str("ticket")
        return ticket.isEmpty ? EventTicket(.signedOut) : EventTicket(.ok, ticket)
    }

    /// WebSocket URL for this account's payload-free event channel (`/api/nodes/events`),
    /// authorized by a ticket from `accountEventTicket()`. `node` names an owned machine whose own
    /// relay object the channel should join.
    public func accountEventsURL(_ ticket: String, node: String = "") -> URL {
        var query: [(String, String)] = [("ticket", ticket)]
        if !node.isEmpty { query.append(("node", node)) }
        let text = ServerURL.parse(settings.serverUrl).webSocket("/api/nodes/events", query: query)
        return URL(string: text) ?? url("/api/nodes/events", query)
    }

    // MARK: Push

    /// Binds this phone's APNs device token to the signed-in account
    /// (`POST /api/accounts/register-device`). The account is taken from the bearer session
    /// server-side. `bundleId` is the app that minted the token: the relay sends each token under
    /// its own app's apns-topic, and a token sent under another app's is refused by Apple
    /// (DeviceTokenNotForTopic) and pruned. Omitted when empty. Never throws.
    public func registerPushDevice(
        deviceToken: String, environment: String, bundleId: String = ""
    ) async -> PushRegistration {
        if deviceToken.isEmpty { return PushRegistration(ok: false) }
        do {
            var body: JSONObject = [
                "deviceToken": .string(deviceToken), "environment": .string(environment),
            ]
            if !bundleId.isEmpty { body["bundleId"] = .string(bundleId) }
            let result = try await postJsonResult(url("/api/accounts/register-device"), body)
            return PushRegistration(
                ok: result.ok, statusCode: result.statusCode, relayConfigured: result.body["pushConfigured"]?.boolValue)
        } catch {
            return PushRegistration(ok: false)
        }
    }

    /// Asks the relay to send one generic alert to every device registered for this account
    /// (`POST /api/accounts/push-test`) and returns Apple's answer per device. Never throws.
    public func sendTestPush() async -> PushTestResult {
        do {
            let result = try await postJsonResult(url("/api/accounts/push-test"), JSONObject())
            return PushTestResult(statusCode: result.statusCode, json: result.body)
        } catch {
            return .unreachable
        }
    }

    /// Drops this phone's token on sign-out (`DELETE /api/accounts/register-device`). True on 2xx.
    @discardableResult
    public func unregisterPushDevice(_ deviceToken: String) async -> Bool {
        if deviceToken.isEmpty { return false }
        do {
            let response = try await http.send(HTTPRequest(
                method: "DELETE", url: url("/api/accounts/register-device"),
                headers: withAuth(["content-type": "application/json", "accept": "application/json"]),
                body: JSON.encodeData(["deviceToken": .string(deviceToken)]), timeout: 30))
            return response.ok
        } catch {
            return false
        }
    }

    // MARK: Bug reports

    /// Files one in-app bug report (`POST /api/bug-reports`). Usable signed out; with a session
    /// the Worker credits the account from the bearer header. Returns the outcome rather than
    /// throwing.
    public func submitBugReport(
        message: String, contact: String = "", screen: String = "", severity: String = "normal",
        platform: String = "", appVersion: String = "", osVersion: String = "", device: String = "", package: String = ""
    ) async -> BugReportResult {
        let trimmed = message.dartTrimmed
        if trimmed.isEmpty { return .failure("Describe the problem first.") }
        var body: JSONObject = ["message": .string(trimmed)]
        if !contact.dartTrimmed.isEmpty { body["contact"] = .string(contact.dartTrimmed) }
        if !screen.isEmpty { body["screen"] = .string(screen) }
        body["severity"] = .string(severity)
        body["source"] = "mobile"
        if !platform.isEmpty { body["platform"] = .string(platform) }
        if !appVersion.isEmpty { body["appVersion"] = .string(appVersion) }
        if !osVersion.isEmpty { body["osVersion"] = .string(osVersion) }
        if !device.isEmpty { body["device"] = .string(device) }
        if !package.dartTrimmed.isEmpty { body["package"] = .string(package.dartTrimmed) }
        let response: HTTPResponse
        do {
            response = try await http.send(HTTPRequest(
                method: "POST", url: url("/api/bug-reports"),
                headers: withAuth(["Accept": "application/json", "Content-Type": "application/json"]),
                body: JSON.encodeData(.object(body)), timeout: 20))
        } catch HTTPError.timedOut {
            return .failure("The report timed out. Check your connection and try again.")
        } catch {
            return .failure("Could not reach ForkMesh. Check your connection and try again.")
        }
        if response.ok {
            var id = ""
            if let decoded = try? JSON.parse(response.body), let map = decoded.objectValue { id = map.str("id") }
            return .success(id)
        }
        switch response.status {
        case 429: return .failure("You have sent several reports in the last hour. Try again later.")
        case 413: return .failure("That report is too long. Shorten it and try again.")
        case 400: return .failure("Add a little more detail so this can be reproduced.")
        default: return .failure("ForkMesh could not accept the report. Try again in a moment.")
        }
    }

    // MARK: Profiles and user status

    /// `GET /api/accounts/<name>` for a user account, or nil when `key` is not one. A miss is
    /// remembered for the session; a hit's status is reused for `accountStatusTtl`.
    func accountLookup(_ key: String) async -> JSONObject? {
        let now = clock.now
        if let existing = accountLookupCache[key], let task = existing.task,
           existing.permanent || now.timeIntervalSince(existing.createdAt) < ApiService.accountStatusTtl {
            return await task.value
        }
        let entry = AccountLookupEntry(createdAt: now)
        let target = url("/api/accounts/\(DartURI.encodeComponent(key))")
        let task = Task<JSONObject?, Never> { @MainActor in
            do {
                let data = try await self.getJson(target)
                let kind = data["kind"].flatMap { $0.isNull ? nil : $0.text } ?? "user"
                guard let map = data.objectValue, map["exists"] != .bool(false), kind == "user" else {
                    entry.permanent = true
                    return nil
                }
                return map
            } catch {
                entry.permanent = true
                entry.unreachable = error is HTTPError
                return nil
            }
        }
        entry.task = task
        accountLookupCache[key] = entry
        return await task.value
    }

    /// Base64 PNG avatar a user account uploaded, or "" when it has none (or is not a user
    /// account). Read once per session.
    public func accountAvatarPng(_ name: String) async -> String {
        await accountAvatarPngIfReached(name) ?? ""
    }

    public func accountAvatarPngIfReached(_ name: String) async -> String? {
        let key = name.dartTrimmed.lowercased()
        if key.isEmpty { return "" }
        if let task = accountAvatarCache[key] { return await task.value }
        let task = Task<String?, Never> { @MainActor in
            let data = await self.accountLookup(key)
            if data == nil, self.accountLookupCache[key]?.unreachable == true { return nil }
            return data?["avatarPng"].flatMap { $0.isNull ? nil : $0.text } ?? ""
        }
        accountAvatarCache[key] = task
        return await task.value
    }

    /// The away flag and custom status an account's public profile carries, or `UserStatus.empty`
    /// when `name` is not a user account.
    public func accountUserStatus(_ name: String) async -> UserStatus {
        let key = name.dartTrimmed.lowercased()
        if key.isEmpty { return .empty }
        let data = await accountLookup(key)
        return UserStatus(json: data?["userStatus"])
    }

    /// `POST /api/accounts/user-status` for the signed-in account. `changes` carries any of
    /// `away`, `emoji`, `text`, `expiresAt` and `clear`; an empty body reads the current status.
    /// Throws `UserStatusException` with a message for people.
    @discardableResult
    public func updateUserStatus(_ changes: JSONObject = JSONObject()) async throws -> UserStatus {
        if !hasSession { throw UserStatusException("unauthorized", "Sign in to set a status.") }
        let result: ApiJsonResult
        do {
            result = try await postJsonResult(url("/api/accounts/user-status"), changes)
        } catch {
            throw UserStatusException("unreachable", "Could not reach ForkMesh. Check your connection and try again.")
        }
        if !result.ok {
            let error = result.body["error"]
            let code = error.flatMap { $0.isNull ? nil : $0.text } ?? "http_\(result.statusCode)"
            throw UserStatusException(code, userStatusErrorMessage(code))
        }
        let nodeName = result.body.str("nodeName").dartTrimmed.lowercased()
        // The next profile read of this account must not serve the old status.
        if !nodeName.isEmpty { accountLookupCache.removeValue(forKey: nodeName) }
        return UserStatus(json: result.body["userStatus"])
    }

    @discardableResult
    public func updateChatRetention(_ changes: JSONObject = JSONObject()) async throws -> ChatRetention {
        if !hasSession { throw ChatRetentionError("unauthorized", chatRetentionErrorMessage("unauthorized")) }
        let result: ApiJsonResult
        do {
            result = try await postJsonResult(url("/api/accounts/chat-retention"), changes)
        } catch {
            throw ChatRetentionError("unreachable", chatRetentionErrorMessage("unreachable"))
        }
        if !result.ok {
            let error = result.body["error"]
            let code = error.flatMap { $0.isNull ? nil : $0.text } ?? "http_\(result.statusCode)"
            throw ChatRetentionError(code, chatRetentionErrorMessage(code))
        }
        return ChatRetention(json: result.body["chatRetention"])
    }

    public func syncActivityToday(_ payload: JSONObject) async throws -> ApiJsonResult {
        try await postJsonResult(url("/api/accounts/activity-today"), payload)
    }

    // MARK: Devices and sessions

    /// Node registry for the signed-in account (`GET /api/accounts/devices`), revoked rows
    /// included. Read-only on mobile.
    public func listDevices() async throws -> [AccountDevice] {
        let data = try await getJson(url("/api/accounts/devices"))
        let list: [JSONValue]?
        if let map = data.objectValue { list = map["devices"]?.arrayValue } else { list = data.arrayValue }
        return (list ?? []).compactMap(\.objectValue).map(AccountDevice.fromJSON)
    }

    /// Every live sign-in on the account, most recently active first (`GET /api/accounts/sessions`).
    public func listSessions() async throws -> [AccountSession] {
        let data = try await getJson(url("/api/accounts/sessions"))
        let list = data.objectValue?["sessions"]?.arrayValue ?? []
        return list.compactMap(\.objectValue).map(AccountSession.fromJSON).filter { !$0.id.isEmpty }
    }

    /// Signs one session out server-side. `sessionId` may also be `others` or `all`. True when the
    /// call also revoked this app's own session, so the caller must clear it locally.
    public func revokeSession(_ sessionId: String) async throws -> Bool {
        let data = try await deleteJson(url("/api/accounts/sessions/\(DartURI.encodeComponent(sessionId))"), JSONObject())
        return data.objectValue?["currentRevoked"] == .bool(true)
    }

    /// The shared room-chat passphrase, derived server-side and handed only to authenticated
    /// clients (`GET /api/chat/room-key`).
    public func roomChatPassphrase() async throws -> String {
        let data = try await getJson(url("/api/chat/room-key", [
            ("owner", "mainnode"), ("repo", "forkmesh"), ("room", settings.room),
        ]))
        if let passphrase = data.objectValue?["passphrase"]?.stringValue { return passphrase }
        throw ApiException("room key unavailable")
    }

    // MARK: Notification inbox

    public func notifications(_ node: String, limit: Int = 40) async throws -> NotificationPage {
        let cleanNode = node.dartTrimmed.lowercased()
        if cleanNode.isEmpty { return NotificationPage(notifications: [], unread: 0) }
        let data = try await getJson(url("/api/notifications", [("node", cleanNode), ("limit", "\(limit)")]))
        return NotificationPage(json: data.objectValue ?? JSONObject())
    }

    public func markNotificationsRead(_ node: String, ids: [String] = [], all: Bool = false) async throws {
        let cleanNode = node.dartTrimmed.lowercased()
        if cleanNode.isEmpty { return }
        var body: JSONObject = ["node": .string(cleanNode)]
        if all { body["all"] = true }
        if !all && !ids.isEmpty { body["ids"] = JSONValue(ids) }
        _ = try await postJson(url("/api/notifications"), body)
    }

    /// Empties the account inbox for good (`DELETE /api/notifications` with `all`). Throws when the
    /// Worker refuses.
    public func deleteAllNotifications(_ node: String) async throws {
        let cleanNode = node.dartTrimmed.lowercased()
        if cleanNode.isEmpty { return }
        _ = try await deleteJson(url("/api/notifications"), ["node": .string(cleanNode), "all": true])
    }

    /// Deletes one notification from the account inbox.
    public func deleteNotification(_ node: String, _ id: String) async throws {
        let cleanNode = node.dartTrimmed.lowercased()
        let cleanId = id.dartTrimmed.lowercased()
        if cleanNode.isEmpty || cleanId.isEmpty { return }
        _ = try await deleteJson(url("/api/notifications"), ["node": .string(cleanNode), "id": .string(cleanId)])
    }

    /// The web page a notification link names. Site paths resolve against the relay's origin;
    /// absolute web links pass through. API paths and anything that is not plain http(s) with a
    /// host and no user info are nil.
    public func webLink(_ href: String) -> URL? {
        let trimmed = href.dartTrimmed
        if trimmed.isEmpty { return nil }
        guard let target = URL(string: trimmed, relativeTo: url("/"))?.absoluteURL else { return nil }
        let scheme = target.scheme?.lowercased() ?? ""
        if scheme != "https" && scheme != "http" { return nil }
        if (target.host ?? "").isEmpty || target.user != nil || target.password != nil { return nil }
        let path = ApiService.encodedPath(target)
        if path == "/api" || path.hasPrefix("/api/") { return nil }
        return target
    }

    // MARK: Owner-key enrollment

    /// Register this device's public recipient bundle (`POST /api/security/owner-keys`) so the
    /// desktop starts sealing agent snapshots to it too. Returns the accepted key id, or "".
    public func registerOwnerKey(_ owner: OwnerKeys) async -> String {
        do {
            let data = try await postJson(url("/api/security/owner-keys"), ["publicBundle": .object(owner.publicBundle)])
            guard let map = data.objectValue, map["ok"] == .bool(true) else { return "" }
            // Refuse an enrollment the relay describes differently from what this device holds,
            // and one claiming custody of a private half.
            if map["keyId"] != .string(owner.keyId) || map["privateKeysStored"] == .bool(true) { return "" }
            return owner.keyId
        } catch {
            return ""
        }
    }

    /// Keep this device enrolled as an owner device while signed in: registers on every
    /// transition into a signed-in state (listening to `auth`), and forgets the memo on sign-out
    /// so the next account enrolls in turn. No-op without `ownerCrypto`.
    public func enrollOwnerDeviceWhenSignedIn(_ auth: AuthService) async {
        guard ownerCrypto != nil else { return }
        if let token = ownerEnrollmentToken { auth.removeListener(token) }
        ownerEnrollmentToken = auth.addListener { [weak self] in
            Task { @MainActor in await self?.syncOwnerEnrollment() }
        }
        await syncOwnerEnrollment()
    }

    func syncOwnerEnrollment() async {
        guard let owner = ownerCrypto else { return }
        let token = sessionToken
        if token.isEmpty {
            ownerEnrolledFor = ""
            return
        }
        if ownerEnrolledFor == token { return }
        let keyId = await registerOwnerKey(owner)
        // Only memoize a confirmed enrollment: a transient failure is retried on the next change.
        if !keyId.isEmpty { ownerEnrolledFor = token }
    }
}
