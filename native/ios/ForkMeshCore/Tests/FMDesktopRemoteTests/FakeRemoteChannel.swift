import FMBase
import FMChat
import FMControl
import Foundation

@MainActor
final class FakeRemoteChannel: DesktopControlCalling {
    var requests: [(String, JSONObject)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        if let failure = failures[method] { throw failure }
        return replies[method] ?? [:]
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

    func count(_ method: String) -> Int { requests.filter { $0.0 == method }.count }

    func last(_ method: String) -> JSONObject? { requests.last { $0.0 == method }?.1 }
}

@MainActor
func settle() async {
    for _ in 0..<5 { await Task.yield() }
}
