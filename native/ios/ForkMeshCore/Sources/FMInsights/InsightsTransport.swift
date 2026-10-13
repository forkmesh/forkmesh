import FMApi
import FMBase
import Foundation

@MainActor
public protocol InsightsRequesting: AnyObject {
    func call(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject
}

extension ApiService: InsightsRequesting {
    public func call(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        try await accountApiCall(method, path, body)
    }
}

public struct InsightsError: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String
    public let message: String

    public init(_ code: String, _ message: String = "") {
        self.code = code
        self.message = message.isEmpty ? InsightsError.describe(code) : message
    }

    public var description: String { message }

    public static func describe(_ code: String) -> String {
        switch code {
        case "admin_required", "unauthorized", "not_admin", "http_401", "http_403":
            return "This needs an administrator account."
        case "http_404", "not_found":
            return "This server does not offer that yet."
        case "self_quarantine_forbidden": return "You cannot quarantine your own account."
        case "self_delete_forbidden": return "You cannot delete your own account."
        case "target_is_admin": return "Administrators cannot be quarantined or deleted here."
        case "no_such_user", "no_such_quarantined_user": return "That account no longer exists."
        case "restricted": return "This table is only readable through its own page."
        case "unknown_table": return "That table no longer exists."
        default: return code.replacingOccurrences(of: "_", with: " ")
        }
    }

    public static func from(_ error: Error) -> InsightsError {
        if let insights = error as? InsightsError { return insights }
        if let org = error as? OrgApiException { return InsightsError(org.code) }
        return InsightsError("request_failed", dartErrorText(error))
    }

    public var needsAdmin: Bool {
        ["admin_required", "unauthorized", "not_admin", "http_401", "http_403"].contains(code)
    }
}

public enum InsightsQuery {
    public static func encode(_ value: String) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return value.addingPercentEncoding(withAllowedCharacters: allowed) ?? ""
    }

    public static func path(_ base: String, _ items: [(String, String)]) -> String {
        let pairs = items.filter { !$0.1.isEmpty }.map { "\(encode($0.0))=\(encode($0.1))" }
        return pairs.isEmpty ? base : base + "?" + pairs.joined(separator: "&")
    }
}

public enum InsightsLoad<Value: Equatable & Sendable>: Equatable, Sendable {
    case idle
    case loading
    case failed(InsightsError)
    case loaded(Value)

    public var value: Value? {
        if case .loaded(let value) = self { return value }
        return nil
    }

    public var error: InsightsError? {
        if case .failed(let error) = self { return error }
        return nil
    }

    public var isLoading: Bool { self == .loading }
}
