import FMBase
import FMControl
import Foundation

@testable import FMAgents

struct FakeControlError: Error {
    let code: String
}

@MainActor
final class FakeAgentOpsChannel: DesktopControlCalling {
    var requests: [(method: String, params: JSONObject)] = []
    var replies: [String: [JSONObject]] = [:]
    var failures: [String: Error] = [:]
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func reply(_ method: String, _ object: JSONObject) {
        replies[method, default: []].append(object)
    }

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        if let failure = failures[method] { throw failure }
        guard var queue = replies[method], !queue.isEmpty else { return [:] }
        let next = queue.removeFirst()
        replies[method] = queue.isEmpty ? [next] : queue
        return next
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    var tapCount: Int { taps.count }

    func emit(_ event: String, _ data: JSONObject = [:]) {
        for tap in taps.values { tap(event, data) }
    }

    func methods() -> [String] { requests.map(\.method) }

    func params(_ method: String) -> JSONObject? { requests.last { $0.method == method }?.params }
}

let approvalA = String(repeating: "a", count: 32)
let approvalB = String(repeating: "b", count: 32)

func approvalJSON(_ id: String, session: Int, askedAt: Int64 = 0, command: String = "") -> JSONValue {
    .object([
        "id": .string(id), "sessionId": .int(Int64(session)), "title": "Fix the build", "repo": "me/app",
        "provider": "codex", "capability": "process.execute", "kind": "command",
        "question": .string(command.isEmpty ? "Allow?" : "Run command:\n\(command)"), "command": .string(command),
        "askedAtMs": .int(askedAt), "lifetimes": ["once", "session", "worktree", "persistent"],
    ])
}

func confirmationJSON(_ action: String, token: String = "tok", typeToConfirm: String = "") -> JSONObject {
    [
        "token": .string(token), "action": .string(action), "summary": "Do it", "destructive": .bool(action == "merge"),
        "typeToConfirm": .string(typeToConfirm), "expiresInMs": 60_000,
    ]
}
