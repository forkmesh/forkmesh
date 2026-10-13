import FMApi
import FMBase
import Foundation
import Observation

public struct RepoWebhook: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var url: String
    public var events: [String]
    public var description: String
    public var active: Bool
    public var managed: Bool
    public var lastStatus: String
    public var lastDeliveryAtMs: Int
    public var consecutiveFailures: Int
    public var secret: String

    public init?(json: JSONObject) {
        let id = json.str("id")
        guard !id.isEmpty else { return nil }
        self.id = id
        url = json.str("url")
        events = json.strings("events")
        description = json.str("description")
        active = json["active"] != .bool(false)
        managed = json.bool("managed")
        lastStatus = json.str("lastStatus")
        lastDeliveryAtMs = ModelJSON.int(json["lastDeliveryAt"])
        consecutiveFailures = ModelJSON.int(json["consecutiveFailures"])
        secret = json.str("secret")
    }

    public var healthLabel: String {
        if !active { return "Paused" }
        if consecutiveFailures > 0 { return "\(consecutiveFailures) failed \(consecutiveFailures == 1 ? "delivery" : "deliveries")" }
        if lastDeliveryAtMs > 0 { return lastStatus.isEmpty ? "Delivered" : lastStatus.capitalized }
        return "No deliveries yet"
    }
}

public struct RepoWebhookEventType: Equatable, Hashable, Sendable, Identifiable {
    public var type: String
    public var label: String
    public var hint: String
    public var id: String { type }

    public static let defaults: [RepoWebhookEventType] = [
        RepoWebhookEventType(type: "repository.push", label: "Repository push", hint: "Any push to the repository"),
        RepoWebhookEventType(type: "branch.push", label: "Branch push", hint: "A push to one branch"),
        RepoWebhookEventType(type: "pull.merged", label: "Pull request merged", hint: "A pull request was merged"),
        RepoWebhookEventType(type: "release.published", label: "Release published", hint: "A release was announced"),
    ]
}

public struct RepoWebhookDelivery: Equatable, Hashable, Sendable {
    public var webhookId: String
    public var eventType: String
    public var status: String
    public var attempts: Int
    public var outcome: String
    public var tsMs: Int
}

public struct RepoWebhookDraft: Equatable, Sendable {
    public var url: String = ""
    public var events: Set<String> = []
    public var description: String = ""

    public init(url: String = "", events: Set<String> = [], description: String = "") {
        self.url = url
        self.events = events
        self.description = description
    }

    public var validationError: String? {
        let trimmed = url.trimmingCharacters(in: .whitespaces)
        guard let parsed = URL(string: trimmed), parsed.scheme?.lowercased() == "https", let host = parsed.host, !host.isEmpty else {
            return "Use an https:// address."
        }
        if trimmed.count > 2048 { return "That address is too long." }
        if events.isEmpty { return "Choose at least one event." }
        if description.count > 200 { return "Keep the description under 200 characters." }
        return nil
    }

    public var fields: JSONObject {
        [
            "url": .string(url.trimmingCharacters(in: .whitespaces)),
            "events": .array(events.sorted().map { .string($0) }),
            "description": .string(description.trimmingCharacters(in: .whitespacesAndNewlines)),
        ]
    }
}

@MainActor
@Observable
public final class RepoWebhooksModel {
    public let repo: RepoRef
    public private(set) var webhooks: [RepoWebhook] = []
    public private(set) var eventTypes: [RepoWebhookEventType] = RepoWebhookEventType.defaults
    public private(set) var deliveries: [RepoWebhookDelivery] = []
    public private(set) var maxWebhooks = 20
    public private(set) var loading = false
    public private(set) var busy = false
    public private(set) var error: String?
    public private(set) var revealedSecret: (id: String, secret: String)?

    @ObservationIgnored private let client: CIClient

    public init(client: CIClient, repo: RepoRef) {
        self.client = client
        self.repo = repo
    }

    public var canAdd: Bool { webhooks.count < maxWebhooks }

    public func deliveries(for id: String) -> [RepoWebhookDelivery] { deliveries.filter { $0.webhookId == id } }

    public func dismissSecret() { revealedSecret = nil }

    func apply(_ data: JSONObject) {
        webhooks = data.maps("webhooks").prefix(50).compactMap(RepoWebhook.init(json:))
        let types = data.maps("events").compactMap { map -> RepoWebhookEventType? in
            let type = map.str("type")
            return type.isEmpty ? nil : RepoWebhookEventType(type: type, label: map.str("label", type), hint: map.str("hint"))
        }
        if !types.isEmpty { eventTypes = types }
        deliveries = data.maps("deliveries").prefix(100).map {
            RepoWebhookDelivery(
                webhookId: $0.str("webhookId"), eventType: $0.str("eventType"), status: $0.str("status"),
                attempts: ModelJSON.int($0["attempts"]), outcome: $0.str("outcome"), tsMs: ModelJSON.int($0["ts"]))
        }
        let limit = ModelJSON.int(data.obj("limits")["maxWebhooks"])
        if limit > 0 { maxWebhooks = limit }
    }

    public func load() async {
        loading = true
        do {
            apply(try await client.webhooks(repo, action: "list", fields: [:]))
            error = nil
        } catch {
            self.error = "\(error)"
        }
        loading = false
    }

    private func mutate(_ action: String, _ fields: JSONObject) async -> JSONObject? {
        if busy { return nil }
        busy = true
        defer { busy = false }
        do {
            let data = try await client.webhooks(repo, action: action, fields: fields)
            error = nil
            return data
        } catch {
            self.error = "\(error)"
            return nil
        }
    }

    private func reveal(_ data: JSONObject) {
        if let hook = RepoWebhook(json: data.obj("webhook")), !hook.secret.isEmpty {
            revealedSecret = (hook.id, hook.secret)
        }
    }

    @discardableResult
    public func create(_ draft: RepoWebhookDraft) async -> Bool {
        if let problem = draft.validationError {
            error = problem
            return false
        }
        guard canAdd else {
            error = "A repository can have at most \(maxWebhooks) webhooks."
            return false
        }
        guard let data = await mutate("create", draft.fields) else { return false }
        reveal(data)
        await load()
        return true
    }

    @discardableResult
    public func update(_ id: String, draft: RepoWebhookDraft) async -> Bool {
        if let problem = draft.validationError {
            error = problem
            return false
        }
        var fields = draft.fields
        fields["id"] = .string(id)
        fields["active"] = .bool(webhooks.first { $0.id == id }?.active ?? true)
        guard await mutate("update", fields) != nil else { return false }
        await load()
        return true
    }

    @discardableResult
    public func setActive(_ id: String, _ active: Bool) async -> Bool {
        guard await mutate("update", ["id": .string(id), "active": .bool(active)]) != nil else { return false }
        await load()
        return true
    }

    @discardableResult
    public func rotate(_ id: String) async -> Bool {
        guard let data = await mutate("rotate", ["id": .string(id)]) else { return false }
        reveal(data)
        return true
    }

    @discardableResult
    public func delete(_ id: String) async -> Bool {
        guard await mutate("delete", ["id": .string(id)]) != nil else { return false }
        webhooks.removeAll { $0.id == id }
        return true
    }

    public func test(_ id: String) async -> String {
        guard let data = await mutate("test", ["id": .string(id)]) else { return error ?? "The test was not sent." }
        let outcome = data.str("outcome")
        let status = ModelJSON.int(data["status"])
        if status >= 200 && status < 300 { return "Test delivered (HTTP \(status))." }
        return outcome.isEmpty ? "Test answered HTTP \(status)." : "Test failed: \(outcome)."
    }
}
