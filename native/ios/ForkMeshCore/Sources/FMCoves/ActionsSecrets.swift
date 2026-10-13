import FMBase
import Foundation
import Observation

@MainActor
public protocol SecretsControlCalling: AnyObject {
    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject
}

public struct ActionsSecretRow: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var updatedAtMs: Int64

    public var id: String { name }

    public init(name: String, updatedAtMs: Int64 = 0) {
        self.name = name
        self.updatedAtMs = updatedAtMs
    }
}

public struct ActionsSecretConfirmation: Equatable, Sendable {
    public var token: String
    public var action: String
    public var name: String
    public var summary: String
}

public enum ActionsSecretError: Error, Equatable, CustomStringConvertible {
    case invalidName
    case valueTooLarge
    case desktop(String)

    public var description: String {
        switch self {
        case .invalidName:
            return "Use letters, digits and underscores, starting with a letter or underscore (at most 128)."
        case .valueTooLarge: return "Values are limited to 16 KB."
        case .desktop(let code): return ActionsSecrets.message(code)
        }
    }
}

public enum ActionsSecrets {
    public static let family = "secrets."
    public static let maxNameLength = 128
    public static let maxValueBytes = 16 * 1024
    public static let maxRows = 500

    public static func validName(_ name: String) -> Bool {
        let scalars = Array(name.unicodeScalars)
        guard !scalars.isEmpty, scalars.count <= maxNameLength else { return false }
        for (index, scalar) in scalars.enumerated() {
            let v = scalar.value
            let letter = (0x41...0x5A).contains(v) || (0x61...0x7A).contains(v) || v == 0x5F
            let digit = (0x30...0x39).contains(v)
            if !(letter || (digit && index > 0)) { return false }
        }
        return true
    }

    public static func message(_ code: String) -> String {
        switch code {
        case "unsupported": return "Your desktop needs an update to manage Actions secrets from the phone."
        case "not_owner": return "Only this desktop's owner can manage its Actions secrets."
        case "confirmation_expired": return "The confirmation expired. Try again."
        case "confirmation_mismatch", "confirmation_missing": return "The confirmation did not match. Try again."
        case "invalid_name": return "That name is not a valid variable name."
        case "value_too_large": return "Values are limited to 16 KB."
        case "not_found": return "That variable no longer exists on the desktop."
        default: return "The desktop refused: \(code)."
        }
    }

    public static func rows(_ answer: JSONObject) -> [ActionsSecretRow] {
        let raw = answer["variables"]?.arrayValue ?? []
        var seen = Set<String>()
        var out: [ActionsSecretRow] = []
        for value in raw.prefix(maxRows) {
            guard let object = value.objectValue, let name = object["name"]?.stringValue, validName(name), !seen.contains(name)
            else { continue }
            seen.insert(name)
            out.append(ActionsSecretRow(name: name, updatedAtMs: object["updatedAtMs"].map { $0.int64(or: 0) } ?? 0))
        }
        return out.sorted { $0.name.lowercased() < $1.name.lowercased() }
    }
}

@MainActor
public final class ActionsSecretsClient {
    private let channel: SecretsControlCalling

    public init(channel: SecretsControlCalling) {
        self.channel = channel
    }

    private func call(_ method: String, _ params: JSONObject = [:]) async throws -> JSONObject {
        let answer = try await channel.request(ActionsSecrets.family + method, params, timeout: 20)
        if let error = answer["error"]?.stringValue, !error.isEmpty { throw ActionsSecretError.desktop(error) }
        return answer
    }

    public func list() async throws -> [ActionsSecretRow] {
        ActionsSecrets.rows(try await call("list"))
    }

    public func prepare(action: String, name: String) async throws -> ActionsSecretConfirmation {
        guard ActionsSecrets.validName(name) else { throw ActionsSecretError.invalidName }
        let answer = try await call("prepare", ["action": .string(action), "name": .string(name)])
        let token = answer["token"]?.stringValue ?? ""
        if token.isEmpty { throw ActionsSecretError.desktop("confirmation_missing") }
        return ActionsSecretConfirmation(
            token: token, action: action, name: name, summary: answer["summary"]?.stringValue ?? "")
    }

    public func set(name: String, value: String, confirmation: ActionsSecretConfirmation) async throws {
        guard ActionsSecrets.validName(name) else { throw ActionsSecretError.invalidName }
        if value.utf8.count > ActionsSecrets.maxValueBytes { throw ActionsSecretError.valueTooLarge }
        _ = try await call("set", ["name": .string(name), "value": .string(value), "token": .string(confirmation.token)])
    }

    public func delete(name: String, confirmation: ActionsSecretConfirmation) async throws {
        guard ActionsSecrets.validName(name) else { throw ActionsSecretError.invalidName }
        _ = try await call("delete", ["name": .string(name), "token": .string(confirmation.token)])
    }
}

@MainActor
@Observable
public final class ActionsSecretsModel {
    public private(set) var rows: [ActionsSecretRow] = []
    public private(set) var loading = false
    public private(set) var error: String?
    public private(set) var busy = false

    @ObservationIgnored private let client: ActionsSecretsClient

    public init(client: ActionsSecretsClient) {
        self.client = client
    }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            rows = try await client.list()
            error = nil
        } catch {
            self.error = ActionsSecretsModel.text(error)
        }
    }

    public func save(name: String, value: String) async -> Bool {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        busy = true
        defer { busy = false }
        do {
            let confirmation = try await client.prepare(action: "set", name: trimmed)
            try await client.set(name: trimmed, value: value, confirmation: confirmation)
            error = nil
            await load()
            return true
        } catch {
            self.error = ActionsSecretsModel.text(error)
            return false
        }
    }

    public func delete(name: String) async -> Bool {
        busy = true
        defer { busy = false }
        do {
            let confirmation = try await client.prepare(action: "delete", name: name)
            try await client.delete(name: name, confirmation: confirmation)
            error = nil
            await load()
            return true
        } catch {
            self.error = ActionsSecretsModel.text(error)
            return false
        }
    }

    static func text(_ error: Error) -> String {
        if let error = error as? ActionsSecretError { return error.description }
        return "\(error)"
    }
}
