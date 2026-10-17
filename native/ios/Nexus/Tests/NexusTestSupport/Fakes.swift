import Foundation
import NexusCore

final class FakeStream: ByteStream, @unchecked Sendable {
    let remoteAddress: String? = nil
    var received: Data
    init(received: Data = Data()) { self.received = received }
    func send(_ data: Data, timeout: Duration) async throws {}
    func receive(maximumLength: Int, timeout: Duration) async throws -> Data { received }
    func close() {}
}

struct FakeConnector: StreamConnector {
    let result: Result<Data, ConnectionFailure>
    func connect(to endpoint: Endpoint, timeout: Duration) async throws -> any ByteStream {
        switch result { case .success(let data): return FakeStream(received: data); case .failure(let error): throw error }
    }
}
