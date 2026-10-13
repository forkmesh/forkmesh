// The behaviour the relay, account-event and node-sync services rely on from
// web_socket_channel, exercised through the scriptable fake. The cases follow
// native/android core/src/test net/WebSocketClientTest.kt where they are about
// the surface rather than RFC 6455 framing (URLSession owns the framing here).

import FMBase
import XCTest
@testable import FMNet

final class WebSocketTests: XCTestCase {
    private let url = URL(string: "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws")!

    func testConnectRecordsTheAttempt() async throws {
        let connector = FakeWebSocketConnector()
        let socket = try await connector.connect(
            url: url, headers: ["Authorization": "Bearer t"], protocols: ["forkmesh.v1", "other"],
            options: WebSocketOptions(connectTimeout: 12, pingInterval: 25)
        )
        XCTAssertEqual(connector.attempts.count, 1)
        XCTAssertEqual(connector.attempts[0].url, url)
        XCTAssertEqual(connector.attempts[0].headers["authorization"], "Bearer t")
        XCTAssertEqual(connector.attempts[0].options.pingInterval, 25)
        XCTAssertEqual(socket.negotiatedProtocol, "forkmesh.v1")
        XCTAssertNil(socket.closeCode)
        let plain = try await connector.connect(url: url)
        XCTAssertNil(plain.negotiatedProtocol)
        XCTAssertEqual(connector.connections.count, 2)
    }

    func testMessagesArriveInOrderAndTheStreamEndsOnServerClose() async throws {
        let connector = FakeWebSocketConnector()
        connector.onConnect { $0.receive(json: ["type": "hello"]) }
        let socket = try await connector.connect(url: url)
        let server = try XCTUnwrap(connector.lastConnection)
        server.receive(text: "two")
        server.receive(.data(Data([1, 2, 3])))
        server.serverClose(code: 4001, reason: "replaced")

        var seen = [WebSocketMessage]()
        for try await message in socket.messages { seen.append(message) }
        XCTAssertEqual(seen, [.text(#"{"type":"hello"}"#), .text("two"), .data(Data([1, 2, 3]))])
        XCTAssertEqual(socket.closeCode, 4001)
        XCTAssertEqual(socket.closeReason, "replaced")
        XCTAssertEqual(seen[0].text, #"{"type":"hello"}"#)
        XCTAssertEqual(seen[2].data, Data([1, 2, 3]))
        XCTAssertNil(seen[2].text)
    }

    func testSendsAreRecordedAndAnswered() async throws {
        let connector = FakeWebSocketConnector()
        connector.onConnect { connection in
            connection.onSend { connection, message in
                if message.text?.contains("ping") == true { connection.receive(json: ["type": "pong"]) }
            }
        }
        let socket = try await connector.connect(url: url)
        let server = try XCTUnwrap(connector.lastConnection)
        try await socket.send(json: ["type": "ping", "n": 1])
        try await socket.send(text: "plain")
        try await socket.send(data: Data([9]))
        try await socket.ping()
        XCTAssertEqual(server.sentTexts, [#"{"type":"ping","n":1}"#, "plain"])
        XCTAssertEqual(server.sentJSON.first?.str("type"), "ping")
        XCTAssertEqual(server.sent.count, 3)
        XCTAssertEqual(server.pingCount, 1)
        let sentEnough = await server.waitForSent(3, timeout: 1)
        XCTAssertTrue(sentEnough)

        var iterator = socket.messages.makeAsyncIterator()
        let reply = try await iterator.next()
        XCTAssertEqual(reply, .text(#"{"type":"pong"}"#))
    }

    func testClientCloseEndsTheStreamAndIsIdempotent() async throws {
        let connector = FakeWebSocketConnector()
        let socket = try await connector.connect(url: url)
        let server = try XCTUnwrap(connector.lastConnection)
        await socket.close()
        await socket.close(code: 4000, reason: "again")
        XCTAssertEqual(server.closedByClient?.code, WebSocketStatus.normalClosure)
        XCTAssertEqual(socket.closeCode, 1000)
        XCTAssertFalse(server.isOpen)
        let closed = await server.waitUntilClosed(timeout: 1)
        XCTAssertTrue(closed)
        var count = 0
        for try await _ in socket.messages { count += 1 }
        XCTAssertEqual(count, 0)
        do {
            try await socket.send(text: "late")
            XCTFail("expected notConnected")
        } catch {
            XCTAssertEqual(error as? WebSocketError, .notConnected)
        }
        do {
            try await socket.ping()
            XCTFail("expected notConnected")
        } catch {
            XCTAssertEqual(error as? WebSocketError, .notConnected)
        }
    }

    func testBrokenConnectionThrowsFromTheStream() async throws {
        let connector = FakeWebSocketConnector()
        let socket = try await connector.connect(url: url)
        let server = try XCTUnwrap(connector.lastConnection)
        server.receive(text: "last")
        server.fail()
        var seen = [WebSocketMessage]()
        do {
            for try await message in socket.messages { seen.append(message) }
            XCTFail("expected the stream to throw")
        } catch {
            XCTAssertEqual(error as? WebSocketError, .transport("connection reset"))
        }
        XCTAssertEqual(seen, [.text("last")])
        XCTAssertNil(socket.closeCode, "no close frame was seen")
    }

    func testConnectFailures() async throws {
        let connector = FakeWebSocketConnector()
        connector.failNext(WebSocketError.handshakeFailed(status: 401, message: ""))
        connector.failNext(WebSocketError.connectTimedOut)
        for expected in [WebSocketError.handshakeFailed(status: 401, message: ""), .connectTimedOut] {
            do {
                _ = try await connector.connect(url: url)
                XCTFail("expected \(expected)")
            } catch {
                XCTAssertEqual(error as? WebSocketError, expected)
            }
        }
        _ = try await connector.connect(url: url)
        XCTAssertEqual(connector.attempts.count, 3)
        XCTAssertEqual(connector.connections.count, 1)

        connector.refuseConnections(WebSocketError.transport("offline"))
        do {
            _ = try await connector.connect(url: url)
            XCTFail("expected a refusal")
        } catch {
            XCTAssertEqual(error as? WebSocketError, .transport("offline"))
        }
        connector.refuseConnections(nil)
        _ = try await connector.connect(url: url)
        let reconnected = await connector.waitForConnections(2, timeout: 1)
        XCTAssertTrue(reconnected)
    }

    func testSendFailuresCanBeScripted() async throws {
        let connector = FakeWebSocketConnector()
        let socket = try await connector.connect(url: url)
        let server = try XCTUnwrap(connector.lastConnection)
        server.failSends(with: WebSocketError.transport("buffer full"))
        do {
            try await socket.send(text: "x")
            XCTFail("expected the scripted failure")
        } catch {
            XCTAssertEqual(error as? WebSocketError, .transport("buffer full"))
        }
        server.failSends(with: nil)
        try await socket.send(text: "y")
        XCTAssertEqual(server.sentTexts, ["y"])
    }

    func testErrorDescriptions() {
        XCTAssertEqual(WebSocketError.handshakeFailed(status: 403, message: "").description, "WebSocket handshake failed: HTTP 403")
        XCTAssertEqual(WebSocketError.handshakeFailed(status: 0, message: "refused").description, "WebSocket handshake failed: refused")
        XCTAssertEqual(HTTPError.responseTooLarge(limit: 5).description, "response larger than 5 bytes")
    }
}
