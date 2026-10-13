// Agent credentials, plus the on-device provider.

import FMBase
import FMLocalAI
import FMNet
import XCTest
@testable import FMAgents

@MainActor
final class AgentCredentialsTests: XCTestCase {
    private var temp: TempDirectory!

    override func setUp() async throws {
        temp = TempDirectory()
    }

    override func tearDown() async throws {
        temp.cleanUp()
        Services.remove(LocalModelManager.self)
    }

    func testRoundTripThroughTheKeychainAndClearingForgetsTheKey() async {
        let secure = InMemorySecureStore()
        let store = AgentCredentialStore(secureStore: secure, http: FakeHTTPClient())
        XCTAssertNil(store.read())

        store.write(AgentCredentials(
            kind: .anthropic,
            apiKey: "sk-ant-secret-value-1234567890", // forkmesh-secret-scan:ignore-line
            model: "claude-opus-5"
        ))
        XCTAssertEqual(
            secure.read(AgentCredentialStore.storageKey),
            #"{"kind":"anthropic","apiKey":"sk-ant-secret-value-1234567890","model":"claude-opus-5"}"# // forkmesh-secret-scan:ignore-line
        )
        let read = store.read()
        XCTAssertEqual(read?.kind, .anthropic)
        XCTAssertEqual(read?.model, "claude-opus-5")
        XCTAssertTrue(read?.build() is AnthropicProvider)
        // The masked form is what any UI or log line should show.
        XCTAssertFalse(read?.maskedKey.contains("secret") ?? true)
        XCTAssertEqual(read?.maskedKey, "sk-ant…7890")

        store.clear()
        XCTAssertNil(store.read())
    }

    func testAStoredEntryWithoutAModelTakesTheProvidersDefault() async {
        let secure = InMemorySecureStore()
        secure.write(AgentCredentialStore.storageKey, #"{"kind":"openai","apiKey":"sk-proj-abc123456789012345"}"#)
        let store = AgentCredentialStore(secureStore: secure, http: FakeHTTPClient())
        XCTAssertEqual(store.read()?.model, "gpt-5.3-codex")
        XCTAssertTrue(store.read()?.build() is OpenAiProvider)

        secure.write(AgentCredentialStore.storageKey, #"{"kind":"anthropic","apiKey":""}"#)
        XCTAssertNil(store.read(), "no key is not configured")
        secure.write(AgentCredentialStore.storageKey, "not json")
        XCTAssertNil(store.read())
        secure.write(AgentCredentialStore.storageKey, #"{"kind":"mystery","apiKey":"sk-ant-x"}"#)
        XCTAssertEqual(store.read()?.kind, .anthropic, "an unknown kind falls back as in Dart")
    }

    func testTheOnDeviceModelNeedsNoKey() async {
        let secure = InMemorySecureStore()
        let store = AgentCredentialStore(secureStore: secure, http: FakeHTTPClient())
        store.write(.onDevice())
        let read = store.read()
        XCTAssertEqual(read?.kind, .onDevice)
        XCTAssertEqual(read?.apiKey, "")
        XCTAssertEqual(read?.model, LocalModelCatalog.defaultModel.id)
        XCTAssertTrue(read?.isUsable ?? false)
        XCTAssertEqual(read?.modelLabel, "Gemma 4 E2B")
        XCTAssertTrue(read?.build() is GemmaAgentProvider)
        XCTAssertEqual(AgentProviderKind.onDevice.pickerLabel, "Gemma 4 (on this iPhone)")
        XCTAssertEqual(AgentProviderKind.onDevice.models.first?.id, LocalModelCatalog.defaultModel.id)
        XCTAssertFalse(AgentProviderKind.onDevice.needsKey)
        XCTAssertEqual(AgentProviderKind.allCases.map(\.id), ["claude-api", "openai", "on-device"])
    }

    func testAnObviouslyWrongKeyIsCaughtBeforeARequestIsSpent() async {
        XCTAssertTrue(AgentProviderKind.anthropic.looksLikeKey("sk-ant-api03-aaaaaaaaaaaaaaa")) // forkmesh-secret-scan:ignore-line
        XCTAssertFalse(AgentProviderKind.anthropic.looksLikeKey("hello"))
        // An OpenAI key pasted into the Claude field is a real mistake to catch.
        XCTAssertFalse(AgentProviderKind.anthropic.looksLikeKey("sk-proj-abc123456789012345"))
        XCTAssertTrue(AgentProviderKind.openai.looksLikeKey("sk-proj-abc123456789012345"))
    }

    func testVerificationTurnsARejectedKeyIntoAdviceNotAStatusCode() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 401, json: ["error": ["message": "invalid x-api-key"]])
        let message = await AgentCredentialStore(secureStore: InMemorySecureStore(), http: http).verify(
            AgentCredentials(kind: .anthropic, apiKey: "sk-ant-bad", model: "claude-opus-5")
        )
        XCTAssertEqual(message, "Claude rejected this key. Check that you copied all of it and that it is still active.")
        let sent = JSON.parseObject(http.lastRequest?.bodyText ?? "")
        XCTAssertEqual(sent["system"], "Reply with the single word: ok")
        XCTAssertEqual(sent["tools"], [])
    }

    func testAGoodKeyVerifiesWithNoMessage() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["content": [["type": "text", "text": "ok"]], "stop_reason": "end_turn"])
        let message = await AgentCredentialStore(secureStore: InMemorySecureStore(), http: http).verify(
            AgentCredentials(kind: .anthropic, apiKey: "sk-ant-good", model: "claude-opus-5")
        )
        XCTAssertNil(message)
    }

    func testRateLimitsAndUnreachableProvidersAreExplained() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 429, json: ["error": ["message": "slow down"]])
        http.enqueue(error: HTTPError.transport("no route to host"))
        let store = AgentCredentialStore(secureStore: InMemorySecureStore(), http: http)
        let credentials = AgentCredentials(kind: .openai, apiKey: "sk-proj-abc123456789012345", model: "gpt-5.3")  // forkmesh-secret-scan:ignore-line
        let limited = await store.verify(credentials)
        XCTAssertEqual(limited, "The key is valid but the account is rate limited right now.")
        let offline = await store.verify(credentials)
        XCTAssertEqual(offline, "Could not reach OpenAI: no route to host")
    }

    func testVerifyingTheOnDeviceModelChecksItIsReady() async throws {
        let engine = FakeLocalChatEngine()
        let off = try localModels(engine, in: temp, enabled: false)
        let store = AgentCredentialStore(secureStore: InMemorySecureStore(), http: FakeHTTPClient(), localModels: off)
        let problem = await store.verify(.onDevice(model: tinyModel.id))
        XCTAssertEqual(problem, "On-device AI is off. Turn it on and download Tiny to run agents on this iPhone.")

        off.setEnabled(true)
        let fine = await store.verify(.onDevice(model: tinyModel.id))
        XCTAssertNil(fine)
        XCTAssertTrue(engine.requests.isEmpty, "verifying loads nothing")
    }

    func testBuildingSelectsTheChosenOnDeviceModel() async throws {
        let manager = try localModels(FakeLocalChatEngine(), in: temp)
        let provider = AgentCredentials.onDevice(model: tinyModel.id).build(localModels: manager)
        XCTAssertEqual(provider.model, tinyModel.id)
        XCTAssertEqual(manager.selectedModelId, tinyModel.id)
        // An id the catalog does not have leaves the selection alone.
        _ = AgentCredentials.onDevice(model: "unknown").build(localModels: manager)
        XCTAssertEqual(manager.selectedModelId, tinyModel.id)
    }

    func testMaskedKeysOfShortKeysRevealNothing() async {
        XCTAssertEqual(AgentCredentials(kind: .openai, apiKey: "sk-1234", model: "m").maskedKey, "••••")
        XCTAssertEqual(AgentCredentials(kind: .openai, apiKey: "", model: "m").isUsable, false)
        XCTAssertEqual(AgentCredentials(kind: .openai, apiKey: "k", model: " ").isUsable, false)
    }
}
