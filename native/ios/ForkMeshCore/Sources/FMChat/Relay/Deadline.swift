// The deadline RelayProbe and CrossRelayCatalog put on their requests: whatever the transport's
// own timeout does, the caller stops waiting at the deadline and the request is cancelled.

import FMNet
import Foundation

enum Deadline {
    /// `http.send(request)`, throwing `HTTPError.timedOut` once `seconds` have passed.
    static func send(_ http: HTTPClient, _ request: HTTPRequest, seconds: TimeInterval) async throws -> HTTPResponse {
        try await withThrowingTaskGroup(of: HTTPResponse.self) { group in
            group.addTask { try await http.send(request) }
            group.addTask {
                try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
                throw HTTPError.timedOut
            }
            defer { group.cancelAll() }
            guard let first = try await group.next() else { throw HTTPError.timedOut }
            return first
        }
    }
}
