import FMBase
import Foundation

public struct AccountEndpointError: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String
    public let status: Int

    public init(_ code: String, status: Int = 0) {
        self.code = code
        self.status = status
    }

    public var description: String { "AccountEndpointError(\(code), \(status))" }
}

@MainActor
public protocol AccountEndpointCalling: AnyObject {
    func accountCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject
    var serverOrigin: String { get }
}

public enum AccountErrorText {
    public static func text(_ error: Error, fallback: String) -> String {
        guard let failure = error as? AccountEndpointError else { return fallback }
        switch failure.code {
        case "invalid_session", "unauthorized": return "Sign in again to continue."
        case "forbidden": return "Your role does not allow that."
        case "not_found": return "That was not found. It may have been removed."
        case "origin_not_allowed": return "The server refused this app's origin. Check the server address in Settings."
        case "ssh_key_already_registered": return "That key is already registered. Make a fresh key pair for this account."
        case "ssh_key_limit_reached": return "This account has the most SSH keys allowed. Remove one first."
        case "unsupported_key_type": return "That key type is not supported. Use ssh-ed25519, ECDSA or RSA of 3072 bits or more."
        case "invalid_public_key", "invalid_key_encoding", "invalid_key_blob", "one_public_key_required":
            return "That is not a single OpenSSH public key line."
        case "too_many_bot_tokens": return "This organization has the most active tokens allowed. Revoke one first."
        case "invalid_bot_permissions": return "Those permissions are not allowed."
        case "grant_used": return "That link was already used. Ask the desktop for a new one."
        case "no_such_node": return "The relay does not know that node yet."
        case "no_such_user", "not_a_user": return "Only a user account can own nodes."
        case "bad_signature", "unauthorized_grant": return "The desktop's signature did not verify."
        case "invalid_report": return "Fill in a title, a description and a component."
        case "http_0", "offline": return fallback
        default:
            if failure.code.hasPrefix("http_") { return fallback }
            let words = failure.code.replacingOccurrences(of: "_", with: " ")
            return words.prefix(1).uppercased() + words.dropFirst() + "."
        }
    }
}

public enum AccountTime {
    public static func label(_ millis: Int64, nowMillis: Int64) -> String {
        millis > 0 ? Format.relativeTime(millis, nowMillis: nowMillis) : "never"
    }
}
