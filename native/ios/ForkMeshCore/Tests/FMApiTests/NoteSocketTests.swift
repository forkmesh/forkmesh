import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

final class NoteSocketTests: XCTestCase {
    @MainActor
    func testTheNoteSocketCarriesTheSessionAsABearerHeader() async throws {
        let id = String(repeating: "a", count: 32)
        let api = makeTestApi(FakeHTTPClient { _ in apiJSON(["ok": true]) }, token: "tok", url: "wss://forkmesh.com/api/relay")
        XCTAssertEqual(api.noteSocketURL(id).absoluteString, "wss://forkmesh.com/api/notes/\(id)/ws")
        let connector = FakeWebSocketConnector()
        _ = try await api.noteSocket(id, connector: connector)
        XCTAssertEqual(connector.attempts.count, 1)
        XCTAssertEqual(connector.attempts[0].url.absoluteString, "wss://forkmesh.com/api/notes/\(id)/ws")
        XCTAssertEqual(connector.attempts[0].headers["Authorization"], "Bearer tok")

        let local = makeTestApi(FakeHTTPClient { _ in apiJSON(["ok": true]) }, url: "ws://test.local/ws")
        XCTAssertEqual(local.noteSocketURL(id).absoluteString, "ws://test.local/api/notes/\(id)/ws")
    }
}
