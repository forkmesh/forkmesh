import FMBase
import Foundation

@testable import FMPackages

@MainActor
final class FakePackagesTransport: PackagesTransport {
    var sent: [(method: String, path: String, query: [(String, String)], body: JSONObject?)] = []
    var routes: [String: [PackagesReply]] = [:]

    func on(_ method: String, _ path: String, status: Int = 200, _ json: JSONObject) {
        routes[method + " " + path, default: []].append(PackagesReply(status: status, json: json))
    }

    func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws
        -> PackagesReply
    {
        sent.append((method, path, query, body))
        let key = method + " " + path
        guard var queue = routes[key], !queue.isEmpty else {
            return PackagesReply(status: 404, json: ["error": "not_found"])
        }
        let next = queue.removeFirst()
        routes[key] = queue.isEmpty ? [next] : queue
        return next
    }

    func last(_ method: String, _ path: String) -> (query: [(String, String)], body: JSONObject?)? {
        sent.last { $0.method == method && $0.path == path }.map { ($0.query, $0.body) }
    }

    var paths: [String] { sent.map { $0.method + " " + $0.path } }
}

let hexA = String(repeating: "a", count: 32)
let hexB = String(repeating: "b", count: 32)
