import FMApi
import FMBase
import Foundation
import Observation

public struct RepoActionSecret: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var updatedAtMs: Int
    public var updatedBy: String

    public var id: String { name }

    public static let limit = 100
    public static let maxValueBytes = 48 * 1024
    public static let namePattern = try! NSRegularExpression(pattern: "^[A-Z_][A-Z0-9_]{0,63}$")

    public init(name: String, updatedAtMs: Int = 0, updatedBy: String = "") {
        self.name = name
        self.updatedAtMs = updatedAtMs
        self.updatedBy = updatedBy
    }

    public init?(json: JSONObject) {
        let name = json.str("name")
        guard RepoActionSecret.isValidName(name) else { return nil }
        self.init(name: name, updatedAtMs: max(0, ModelJSON.int(json["updatedAt"])), updatedBy: String(json.str("updatedBy").prefix(64)))
    }

    public static func isValidName(_ name: String) -> Bool {
        ActionsControlRequest.matches(namePattern, name)
    }

    public static func normalizedName(_ text: String) -> String {
        text.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
    }

    public static func nameProblem(_ name: String) -> String? {
        if name.isEmpty { return "Enter a name." }
        if !isValidName(name) {
            return "A secret name uses A-Z, 0-9 and _, does not start with a digit, and has at most 64 characters."
        }
        return nil
    }

    public static func valueProblem(_ value: String) -> String? {
        if value.isEmpty { return "Enter a value." }
        if value.utf8.count > maxValueBytes { return "A secret value is at most 48 KiB." }
        return nil
    }
}

@MainActor
@Observable
public final class RepoActionSecretsModel {
    public let repo: RepoRef
    public private(set) var secrets: [RepoActionSecret] = []
    public private(set) var loaded = false
    public private(set) var loading = false
    public private(set) var busy = false
    public private(set) var error: String?
    public private(set) var notice: ActionsNotice?

    @ObservationIgnored private let client: CIClient

    public init(client: CIClient, repo: RepoRef) {
        self.client = client
        self.repo = repo
    }

    public var canAdd: Bool { secrets.count < RepoActionSecret.limit }

    public func contains(_ name: String) -> Bool { secrets.contains { $0.name == name } }

    public static func explain(_ error: Error) -> String {
        if let failure = error as? CIError, failure.code == "forbidden" || (failure.status == 403 && failure.code.isEmpty) {
            return "Only the repository owner and its admins can manage secrets."
        }
        return "\(error)"
    }

    public func load() async {
        loading = true
        defer { loading = false }
        do {
            secrets = try await client.actionsSecrets(repo).sorted { $0.name < $1.name }
            loaded = true
            error = nil
        } catch {
            self.error = RepoActionSecretsModel.explain(error)
        }
    }

    @discardableResult
    public func save(name raw: String, value: String) async -> Bool {
        let name = RepoActionSecret.normalizedName(raw)
        if let problem = RepoActionSecret.nameProblem(name) ?? RepoActionSecret.valueProblem(value) {
            error = problem
            return false
        }
        if !contains(name) && !canAdd {
            error = "This repository already has the most secrets it can have."
            return false
        }
        guard !busy else { return false }
        busy = true
        defer { busy = false }
        let replacing = contains(name)
        do {
            try await client.setActionsSecret(repo, name: name, value: value)
            error = nil
            notice = ActionsNotice(text: replacing ? "Replaced \(name)." : "Added \(name).", good: true)
        } catch {
            self.error = RepoActionSecretsModel.explain(error)
            return false
        }
        await load()
        return true
    }

    @discardableResult
    public func delete(_ name: String) async -> Bool {
        guard RepoActionSecret.isValidName(name), !busy else { return false }
        busy = true
        defer { busy = false }
        do {
            try await client.deleteActionsSecret(repo, name: name)
            error = nil
            secrets.removeAll { $0.name == name }
            notice = ActionsNotice(text: "Deleted \(name).", good: true)
            return true
        } catch {
            self.error = RepoActionSecretsModel.explain(error)
            return false
        }
    }
}
