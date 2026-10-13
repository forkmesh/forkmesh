import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

private let hour: Int64 = 3_600_000
private let day: Int64 = 86_400_000

private func retentionAnswer(_ retention: JSONObject) -> HTTPResponse {
    portJSON(["ok": true, "nodeName": "alice", "chatRetention": .object(retention)])
}

private final class RetentionServer: @unchecked Sendable {
    let bodies = SvcBox<[JSONObject]>([])
    let current = SvcBox(ChatRetention())
    let refuse = SvcBox("")

    lazy var http = FakeHTTPClient { request in await self.handle(request) }

    private func handle(_ request: HTTPRequest) async -> HTTPResponse {
        guard request.method == "POST", request.url.path == "/api/accounts/chat-retention" else {
            return HTTPResponse(status: 404, text: "not found")
        }
        let body = JSON.parseObject(request.bodyText)
        bodies.mutate { $0.append(body) }
        let refusal = refuse.value
        if !refusal.isEmpty { return HTTPResponse(status: 400, json: ["error": .string(refusal)]) }
        current.mutate { value in
            if let raw = body["defaultMs"] { value = value.settingDefault(raw.isNull ? nil : raw.int64Value) }
            let chat = body.str("chat")
            if !chat.isEmpty, let raw = body["retentionMs"] {
                value = value.settingChat(chat, raw.isNull ? nil : raw.int64Value)
            }
        }
        return retentionAnswer(current.value.toJSON())
    }
}

final class ChatRetentionModelTests: XCTestCase {
    func testTheStandardIsSevenDaysWithNoOverrides() {
        let standard = ChatRetention.standard
        XCTAssertEqual(standard.defaultMs, 604_800_000)
        XCTAssertTrue(standard.chats.isEmpty)
        XCTAssertEqual(standard.effectiveMs(for: "#general"), 604_800_000)
        XCTAssertEqual(ChatRetention(json: nil), .standard)
        XCTAssertEqual(ChatRetention(json: .string("nope")), .standard)
    }

    func testParsingClampsEveryValueAndSkipsJunk() {
        let json: JSONValue = [
            "defaultMs": 60_000,
            "chats": ["#general": 999_999_999_999, "@peer": "7200000", "channel:abc": 3_600_000, "#bad": "x", "#zero": 0],
            "updatedAt": 1_770_000_000_000,
        ]
        let parsed = ChatRetention(json: json)
        XCTAssertEqual(parsed.defaultMs, hour)
        XCTAssertEqual(parsed.chats, ["#general": 604_800_000, "@peer": 2 * hour, "channel:abc": hour])
        XCTAssertEqual(parsed.updatedAt, 1_770_000_000_000)
        XCTAssertEqual(ChatRetention(json: ["defaultMs": .null]).defaultMs, 604_800_000)
        XCTAssertEqual(ChatRetention(json: .object(parsed.toJSON())), parsed)
    }

    func testEffectiveRetentionPrefersTheChatOverride() {
        let retention = ChatRetention(defaultMs: day, chats: ["#random": hour])
        XCTAssertEqual(retention.effectiveMs(for: "#random"), hour)
        XCTAssertEqual(retention.effectiveMs(for: "#general"), day)
        XCTAssertEqual(retention.override(for: "#random"), hour)
        XCTAssertNil(retention.override(for: "#general"))
        XCTAssertEqual(retention.settingChat("#random", nil).effectiveMs(for: "#random"), day)
        XCTAssertEqual(retention.settingDefault(nil).defaultMs, 604_800_000)
        XCTAssertEqual(retention.settingDefault(1).defaultMs, hour)
    }

    func testAMessageExpiresAtTheCutoffInclusive() {
        let now = Date(epochMillis: 1_770_000_000_000)
        let retention = ChatRetention(defaultMs: day, chats: ["#fast": hour])
        XCTAssertTrue(retention.isExpired(Date(epochMillis: now.epochMillis - day), key: "#general", now: now))
        XCTAssertFalse(retention.isExpired(Date(epochMillis: now.epochMillis - day + 1), key: "#general", now: now))
        XCTAssertTrue(retention.isExpired(Date(epochMillis: now.epochMillis - hour - 5), key: "#fast", now: now))
        XCTAssertFalse(retention.isExpired(Date(epochMillis: now.epochMillis - hour + 5), key: "#fast", now: now))
        XCTAssertEqual(retention.cutoff(for: "#fast", now: now).epochMillis, now.epochMillis - hour)
    }

    func testKeysAndLabels() {
        XCTAssertEqual(ChatRetention.key(conversation: "#office/ops", officeRoomId: "abc123"), "channel:abc123")
        XCTAssertEqual(ChatRetention.key(conversation: "#office/ops", officeRoomId: ""), "#office/ops")
        XCTAssertEqual(ChatRetention.key(conversation: "@peer"), "@peer")
        XCTAssertEqual(ChatRetention.presets.map(\.label), ["7 days", "3 days", "1 day", "8 hours", "1 hour"])
        XCTAssertEqual(ChatRetention.presets.map(\.ms), [604_800_000, 259_200_000, 86_400_000, 28_800_000, 3_600_000])
        XCTAssertEqual(ChatRetention.label(2 * day), "2 days")
        XCTAssertEqual(ChatRetention.label(2 * hour), "2 hours")
        XCTAssertEqual(ChatRetention.label(hour + 30 * 60_000), "90 minutes")
        XCTAssertEqual(ChatRetention(defaultMs: day).useDefaultLabel(), "Use default (1 day)")
    }
}

@MainActor
final class ApiChatRetentionTests: XCTestCase {
    private func api(_ http: FakeHTTPClient, token: String = "session-token") -> ApiService {
        ApiPortFixture.api(http, token: token, serverUrl: "ws://test.local/ws")
    }

    func testAnEmptyBodyReadsWithTheBearerSession() async throws {
        let http = FakeHTTPClient { _ in
            retentionAnswer(["defaultMs": 86_400_000, "chats": ["#general": 3_600_000], "updatedAt": 5])
        }
        let retention = try await api(http, token: "tok").updateChatRetention()
        let sent = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(portMethodPath(sent), "POST /api/accounts/chat-retention")
        XCTAssertEqual(sent.headers["Authorization"], "Bearer tok")
        XCTAssertEqual(sent.bodyText, "{}")
        XCTAssertEqual(retention, ChatRetention(defaultMs: day, chats: ["#general": hour], updatedAt: 5))
    }

    func testClearingSendsAJsonNull() async throws {
        let http = FakeHTTPClient { _ in retentionAnswer(["defaultMs": 604_800_000, "chats": [:]]) }
        let service = api(http)
        try await service.updateChatRetention(["defaultMs": .null])
        try await service.updateChatRetention(["chat": "#general", "retentionMs": .null])
        XCTAssertEqual(http.requests.map(\.bodyText), [
            #"{"defaultMs":null}"#,
            ##"{"chat":"#general","retentionMs":null}"##,
        ])
    }

    func testARefusalCarriesTheCodeAndAMessage() async {
        let http = FakeHTTPClient { _ in HTTPResponse(status: 400, text: #"{"error":"invalid_chat_retention"}"#) }
        let error = await portCaughtError { try await self.api(http).updateChatRetention(["defaultMs": 5]) }
        let refusal = error as? ChatRetentionError
        XCTAssertEqual(refusal?.code, "invalid_chat_retention")
        XCTAssertEqual(refusal?.message, chatRetentionErrorMessage("invalid_chat_retention"))
        let signedOut = await portCaughtError {
            try await self.api(FakeHTTPClient { _ in retentionAnswer([:]) }, token: "").updateChatRetention()
        } as? ChatRetentionError
        XCTAssertEqual(signedOut?.code, "unauthorized")
    }
}

@MainActor
final class ChatRetentionServiceTests: XCTestCase {
    private struct Rig {
        let server: RetentionServer
        let token: SvcBox<String>
        let account: SvcBox<String>
        let store: InMemoryKeyValueStore
        let service: ChatRetentionService
    }

    private func rig(signedIn: Bool = true, store: InMemoryKeyValueStore = InMemoryKeyValueStore()) -> Rig {
        let server = RetentionServer()
        let token = SvcBox(signedIn ? "session-token" : "")
        let account = SvcBox(signedIn ? "alice" : "")
        let api = svcApi(server.http, token: { token.value })
        let service = ChatRetentionService(
            api: api, store: store, sessionToken: { token.value }, account: { account.value })
        return Rig(server: server, token: token, account: account, store: store, service: service)
    }

    func testASignInReadsAndASignOutFallsBackToTheStandard() async {
        let rig = rig(signedIn: false)
        rig.server.current.value = ChatRetention(defaultMs: day)
        await rig.service.syncSession()
        XCTAssertTrue(rig.server.bodies.value.isEmpty)

        rig.token.value = "session-token"
        rig.account.value = "alice"
        await rig.service.syncSession()
        XCTAssertEqual(rig.server.bodies.value, [[:]])
        XCTAssertTrue(rig.service.known)
        XCTAssertEqual(rig.service.retention.defaultMs, day)

        rig.token.value = ""
        await rig.service.syncSession()
        XCTAssertFalse(rig.service.known)
        XCTAssertEqual(rig.service.retention, .standard)
    }

    func testChangesAreSentAndAdopted() async throws {
        let rig = rig()
        await rig.service.syncSession()
        let notified = SvcBox(0)
        rig.service.addListener { notified.mutate { $0 += 1 } }
        try await rig.service.setDefault(8 * hour)
        try await rig.service.setChat("#general", hour)
        XCTAssertEqual(rig.service.retention.defaultMs, 8 * hour)
        XCTAssertEqual(rig.service.retention.effectiveMs(for: "#general"), hour)
        try await rig.service.setChat("#general", nil)
        try await rig.service.setDefault(nil)
        XCTAssertEqual(rig.service.retention, .standard)
        XCTAssertEqual(Array(rig.server.bodies.value.dropFirst()), [
            ["defaultMs": .int(8 * hour)],
            ["chat": "#general", "retentionMs": .int(hour)],
            ["chat": "#general", "retentionMs": .null],
            ["defaultMs": .null],
        ])
        XCTAssertFalse(rig.service.busy)
        XCTAssertGreaterThan(notified.value, 0)
    }

    func testARefusedChangeRollsBack() async {
        let rig = rig()
        await rig.service.syncSession()
        rig.server.refuse.value = "invalid_chat_retention"
        let error = await portCaughtError { try await rig.service.setDefault(hour) } as? ChatRetentionError
        XCTAssertEqual(error?.code, "invalid_chat_retention")
        XCTAssertEqual(rig.service.retention, .standard)
    }

    func testTheCacheServesTheSameAccountOnlyForAnOfflineStart() async throws {
        let store = InMemoryKeyValueStore()
        let first = rig(store: store)
        await first.service.syncSession()
        try await first.service.setDefault(day)
        XCTAssertNotNil(store.string(forKey: ChatRetentionService.cacheKey))

        let offline = rig(store: store)
        offline.server.refuse.value = "server_error"
        XCTAssertEqual(offline.service.retention.defaultMs, day)
        await offline.service.syncSession()
        XCTAssertEqual(offline.service.retention.defaultMs, day)
        XCTAssertFalse(offline.service.known)

        let other = rig(store: store)
        other.account.value = "bob"
        other.server.refuse.value = "server_error"
        await other.service.syncSession()
        XCTAssertEqual(other.service.retention, .standard)
    }

    func testTheAccountPayloadIsAdopted() async {
        let rig = rig()
        rig.service.adoptAccountPayload(["defaultMs": 3_600_000, "chats": ["@peer": 86_400_000]])
        XCTAssertTrue(rig.service.known)
        XCTAssertEqual(rig.service.retention.effectiveMs(for: "@peer"), day)
        XCTAssertEqual(rig.service.retention.effectiveMs(for: "#general"), hour)
    }
}
