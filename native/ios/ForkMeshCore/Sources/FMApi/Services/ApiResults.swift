// The result and error types of ApiService (EventTicketStatus, EventTicket, ApiJsonResult,
// PushRegistration, PushTestDelivery, PushTestResult, BugReportResult, UserStatusException),
// after native/android api/ApiResults.kt, plus `ApiException`, `ApiArgumentError` and
// `ApiUnsupportedError`.

import FMBase
import FMNet
import Foundation

/// Outcome of an account event-ticket request. `signedOut` means the stored session proves
/// nobody and must not be retried; `unavailable` is a transient failure worth another attempt.
public enum EventTicketStatus: String, Sendable {
    case ok, signedOut, unavailable
}

public struct EventTicket: Equatable, Sendable {
    public let status: EventTicketStatus
    public let ticket: String

    public init(_ status: EventTicketStatus, _ ticket: String = "") {
        self.status = status
        self.ticket = ticket
    }
}

/// A JSON answer kept together with its HTTP status.
///
/// Most endpoints are fine with ApiService's throw-on-error handling, but a few carry contract
/// meaning in a non-2xx (or non-200 2xx) answer: a catalog publish answers `202 {"mirror": true}`
/// when this device is not the source authority, and `409 stale_update` when the record is older
/// than the stored one. Those callers need the status.
public struct ApiJsonResult: Equatable, Sendable {
    public let statusCode: Int
    public let body: JSONObject

    public init(_ statusCode: Int, _ body: JSONObject) {
        self.statusCode = statusCode
        self.body = body
    }

    public var ok: Bool { statusCode >= 200 && statusCode < 300 }
    /// `'${body['error'] ?? ''}'`.
    public var error: String { body.str("error") }
}

/// Outcome of binding this phone's APNs token to the signed-in account.
public struct PushRegistration: Equatable, Sendable {
    public let ok: Bool
    /// HTTP status of the registration; 0 when the request never completed.
    public let statusCode: Int
    /// Whether the relay holds APNs provider credentials, when it says so. Nil on a relay that
    /// predates the field. A token registered with a relay that has none never produces a
    /// banner, and this is the phone's only way to know that.
    public let relayConfigured: Bool?

    public init(ok: Bool, statusCode: Int = 0, relayConfigured: Bool? = nil) {
        self.ok = ok
        self.statusCode = statusCode
        self.relayConfigured = relayConfigured
    }
}

/// Apple's answer for one registered device during a push self-test.
public struct PushTestDelivery: Equatable, Sendable {
    /// `development` for a sandbox-minted token, otherwise `production`.
    public let environment: String
    public let delivered: Bool
    /// APNs' HTTP status; 0 when the relay could not reach Apple at all.
    public let status: Int
    /// Apple's reason on a refusal ("BadDeviceToken", ...), else empty.
    public let reason: String

    public init(environment: String, delivered: Bool, status: Int, reason: String) {
        self.environment = environment
        self.delivered = delivered
        self.status = status
        self.reason = reason
    }

    public init(json: JSONObject) {
        self.init(
            environment: json.str("environment"),
            delivered: json.bool("delivered"),
            status: ApiParse.numInt(json["status"]),
            reason: json.str("reason"))
    }
}

/// Result of `POST /api/accounts/push-test`, with a one-line explanation the settings screen
/// can show as-is.
public struct PushTestResult: Equatable, Sendable {
    public let statusCode: Int
    public let ok: Bool
    /// Which relay credential is unusable (`missing_secrets`, `key_id_malformed`,
    /// `team_id_malformed`); empty when none, or on a relay that predates the field.
    public let configProblem: String
    /// Whether the relay has APNs credentials; nil when the reply did not say.
    public let configured: Bool?
    public let devices: Int
    public let delivered: Int
    public let results: [PushTestDelivery]
    public let error: String

    public init(
        statusCode: Int, ok: Bool = false, configured: Bool? = nil, devices: Int = 0, delivered: Int = 0,
        results: [PushTestDelivery] = [], error: String = "", configProblem: String = ""
    ) {
        self.statusCode = statusCode
        self.ok = ok
        self.configured = configured
        self.devices = devices
        self.delivered = delivered
        self.results = results
        self.error = error
        self.configProblem = configProblem
    }

    /// The relay could not be reached at all.
    public static let unreachable = PushTestResult(statusCode: 0)

    public init(statusCode: Int, json body: JSONObject) {
        self.init(
            statusCode: statusCode,
            ok: body.bool("ok"),
            configured: body["configured"]?.boolValue,
            devices: ApiParse.numInt(body["devices"]),
            delivered: ApiParse.numInt(body["delivered"]),
            results: body["results"]?.arrayValue.map { $0.compactMap(\.objectValue).map(PushTestDelivery.init(json:)) } ?? [],
            error: body.str("error"),
            configProblem: body.str("configProblem"))
    }

    /// What happened, in one sentence, naming the link of the chain that broke.
    public var summary: String {
        if statusCode == 0 {
            return "Could not reach the relay. Check the connection and try again."
        }
        if statusCode == 401 { return "The session has expired. Sign in again." }
        if statusCode == 404 || statusCode == 405 {
            return "This relay does not support push tests yet. Update the relay and try again."
        }
        if statusCode == 429 { return "Try again in a few seconds." }
        if error == "apns_provider_key" {
            return "The relay's APNs key is invalid. Ask the operator to check APNS_AUTH_KEY and APNS_KEY_ID."
        }
        if error == "device_lookup_failed" {
            return "The relay could not read this account's devices. Try again."
        }
        if statusCode < 200 || statusCode >= 300 {
            return "The relay answered HTTP \(statusCode). Try again later."
        }
        if configProblem == "key_id_malformed" || configProblem == "team_id_malformed" {
            let name = configProblem == "key_id_malformed" ? "APNS_KEY_ID" : "APNS_TEAM_ID"
            return "The relay's \(name) is not a valid Apple identifier (ten characters, A-Z and 0-9), "
                + "so Apple refuses every push. Ask the operator to correct it."
        }
        if configured == false {
            return "This relay has no APNs credentials, so it cannot send pushes. "
                + "Ask the operator to set APNS_AUTH_KEY, APNS_KEY_ID and APNS_TEAM_ID."
        }
        if devices == 0 {
            return "This phone is not registered for pushes yet. Allow notifications and try again."
        }
        if delivered > 0 {
            let sandbox = results.filter(\.delivered).allSatisfy { $0.environment == "development" }
            let place = sandbox ? " through the APNs sandbox" : ""
            return "Sent to \(delivered) of \(devices) device\(devices == 1 ? "" : "s")\(place). "
                + "A ForkMesh banner should arrive within a few seconds."
        }
        let failure = results.first { !$0.delivered }
            ?? PushTestDelivery(environment: "", delivered: false, status: 0, reason: "")
        if failure.status == 0 {
            return "The relay could not reach Apple's gateway. Try again in a moment."
        }
        let reason = failure.reason.isEmpty ? "HTTP \(failure.status)" : failure.reason
        return "Apple rejected the push (\(reason)).\(PushTestResult.hint(failure.reason))"
    }

    static func hint(_ reason: String) -> String {
        switch reason {
        case "BadDeviceToken":
            return " The token was minted for the other APNs environment (sandbox versus production), "
                + "so the relay pruned it; reopen the app to register again."
        case "DeviceTokenNotForTopic", "TopicDisallowed":
            return " The relay's APNs key or topic does not match this app's bundle id."
        case "InvalidProviderToken", "MissingProviderToken", "ExpiredProviderToken":
            return " The relay's APNS_KEY_ID or APNS_TEAM_ID does not match its key."
        case "Unregistered":
            return " Apple no longer knows this token; reopen the app to register again."
        default:
            return ""
        }
    }
}

/// Outcome of one bug-report submission.
///
/// The failure carries a finished sentence rather than an error code, because the only thing
/// the composer does with it is show it to the reporter.
public struct BugReportResult: Equatable, Sendable {
    public let ok: Bool
    public let id: String
    public let message: String

    public static func success(_ id: String) -> BugReportResult { BugReportResult(ok: true, id: id, message: "") }
    public static func failure(_ message: String) -> BugReportResult { BugReportResult(ok: false, id: "", message: message) }

    public init(ok: Bool, id: String, message: String) {
        self.ok = ok
        self.id = id
        self.message = message
    }
}

public struct BugReportInboxAttachment: Equatable, Sendable {
    public let id: String
    public let name: String
    public let contentType: String
    public let size: Int

    public init(json: JSONObject) {
        id = json.str("id")
        name = json.str("name", "attachment")
        contentType = json.str("contentType")
        size = json.int("size")
    }
}

public struct BugReportInboxItem: Equatable, Sendable, Identifiable {
    public let id: String
    public let status: String
    public let severity: String
    public let source: String
    public let platform: String
    public let appVersion: String
    public let osVersion: String
    public let device: String
    public let createdAt: Int64
    public let account: String
    public let message: String
    public let contact: String
    public let screen: String
    public let package: String
    public let attachments: [BugReportInboxAttachment]

    public init(json: JSONObject) {
        id = json.str("id")
        status = json.str("status", "new")
        severity = json.str("severity", "normal")
        source = json.str("source")
        platform = json.str("platform")
        appVersion = json.str("appVersion")
        osVersion = json.str("osVersion")
        device = json.str("device")
        createdAt = json.int64("createdAt")
        account = json.str("account")
        message = json.str("message")
        contact = json.str("contact")
        screen = json.str("screen")
        package = json.str("package")
        attachments = json.maps("attachments").map(BugReportInboxAttachment.init(json:))
    }
}

public struct BugReportInbox: Equatable, Sendable {
    public let ok: Bool
    public let reports: [BugReportInboxItem]
    public let counts: [String: Int]
    public let nextOffset: Int?
    public let message: String

    public init(ok: Bool, reports: [BugReportInboxItem] = [], counts: [String: Int] = [:], nextOffset: Int? = nil,
                message: String = "") {
        self.ok = ok
        self.reports = reports
        self.counts = counts
        self.nextOffset = nextOffset
        self.message = message
    }

    public init(json: JSONObject) {
        let counts = json.obj("counts")
        self.init(
            ok: true, reports: json.maps("reports").map(BugReportInboxItem.init(json:)),
            counts: Dictionary(uniqueKeysWithValues: counts.keys.map { ($0, counts.int($0)) }),
            nextOffset: json["nextOffset"]?.isNull == false ? json.int("nextOffset") : nil)
    }

    public static func failure(_ message: String) -> BugReportInbox { BugReportInbox(ok: false, message: message) }
}

/// A refused or failed `POST /api/accounts/user-status`, with the server's error code and a
/// message ready for an inline notice.
public struct UserStatusException: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String
    public let message: String

    public init(_ code: String, _ message: String) {
        self.code = code
        self.message = message
    }

    public var description: String { message }
}

/// What ApiService throws for HTTP failures. Several screens show the error's text as-is, so
/// `description` keeps the "Exception: message" prefix; `message` is the bare sentence.
public struct ApiException: Error, Equatable, Sendable, CustomStringConvertible, LocalizedError {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { "Exception: \(message)" }
    public var errorDescription: String? { message }
}

public struct ApiArgumentError: Error, Equatable, Sendable, CustomStringConvertible {
    public let name: String
    public let value: String
    public let message: String

    public init(value: String, name: String, message: String) {
        self.value = value
        self.name = name
        self.message = message
    }

    public var description: String { "Invalid argument (\(name)): \(message): \(value)" }
}

public struct ApiUnsupportedError: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { "Unsupported operation: \(message)" }
}

/// The display text for the errors this module and FMNet throw. Anything else prints with
/// `String(describing:)`.
public func dartErrorText(_ error: Error) -> String {
    switch error {
    case let error as ApiException: return error.description
    case let error as HTTPError:
        switch error {
        case .timedOut: return "TimeoutException: request timed out"
        default: return "ClientException: \(error.description)"
        }
    default: return String(describing: error)
    }
}

/// The small `is num ? toInt() : 0` style readers the service writes by hand.
enum ApiParse {
    /// `v is num ? v.toInt() : 0`.
    static func numInt(_ value: JSONValue?) -> Int {
        guard let value else { return 0 }
        switch value {
        case .int(let n): return Int(truncatingIfNeeded: n)
        case .double(let d): return d.isFinite ? Int(d.rounded(.towardZero)) : 0
        default: return 0
        }
    }

    /// The body decoded as a JSON object, or empty.
    static func jsonMap(_ body: Data) -> JSONObject {
        if body.isEmpty { return JSONObject() }
        return JSON.parseObject(body)
    }

    static func parseAsync(_ body: Data) async -> JSONValue? {
        if body.isEmpty { return nil }
        if body.count < ApiService.detachedParseBytes { return try? JSON.parse(body) }
        return await Task.detached(priority: .userInitiated) { try? JSON.parse(body) }.value
    }

    static func jsonMapAsync(_ body: Data) async -> JSONObject {
        await parseAsync(body)?.objectValue ?? JSONObject()
    }

    static func modelAsync<Model: Sendable>(
        _ json: JSONObject, _ build: @escaping @Sendable (JSONObject) -> Model
    ) async -> Model {
        await Task.detached(priority: .userInitiated) { build(json) }.value
    }

    /// `'${decoded['error']}'` of a JSON error body, or "" (`_errorCode`).
    static func errorCode(_ body: Data) -> String {
        guard !body.isEmpty, let value = try? JSON.parse(body), let error = value["error"], !error.isNull else { return "" }
        return error.text
    }
}
