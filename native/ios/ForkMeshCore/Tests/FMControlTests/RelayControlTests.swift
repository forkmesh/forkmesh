// The phone's half of the relayed control channel (RelayControlConnection) against a desktop
// that speaks desktop/src/LocalControl.cpp's relay transport, the hub that picks LAN or relay,
// and the unified agent list.

import FMApi
import FMBase
import FMChat
import FMCrypto
import Foundation
import XCTest

@testable import FMControl

/// The relay object plus the desktop behind it: takes the phone's control frames, runs the
/// desktop's side of the handshake, and answers with chunked, sealed frames.
@MainActor
final class FakeRelayDesktop: RelayFrameChannel {
    var onFrame: (@MainActor (JSONObject) -> Void)?
    var onConnected: (@MainActor () -> Void)?

    let seed = Ed25519.generateSeed()
    let publicKey: String
    let node: String
    var paired = true
    var socketUp = true
    var silent = false
    var forgotten = false
    /// Chunk size for what the desktop sends, small to exercise reassembly.
    var chunkChars = 64
    var requests: [JSONObject] = []
    var sent: [JSONObject] = []
    var disposed = false
    var handler: ((String, JSONObject) -> JSONObject?)?
    var refuse: ((String, JSONObject) -> String?)?

    private var stage = 0
    private var transcript: [UInt8] = []
    private var clientKey = ""
    private var keys = ControlKeys(clientToServer: [], serverToClient: [])
    private var sendCounter = 0
    private var receiveCounter = 0
    private var sendSeq = 0
    private var expectSeq = 0
    private var pending: [UInt8] = []
    private var session = ""

    init(node: String = "desk") throws {
        self.node = node
        publicKey = controlB64(try Ed25519.publicKey(seed: seed))
    }

    func start() {
        if socketUp { deliver { self.onConnected?() } }
    }

    func send(_ frame: JSONObject) -> Bool {
        if !socketUp { return false }
        sent.append(frame)
        if silent { return true }
        receive(frame)
        return true
    }

    func dispose() async { disposed = true }

    private func deliver(_ work: @escaping @MainActor () -> Void) {
        Task { @MainActor in work() }
    }

    private func receive(_ frame: JSONObject) {
        XCTAssertEqual(frame["type"], .string("control"))
        XCTAssertEqual(frame["node"], .string(node))
        let action = frame.str("action")
        if action == "close" { return }
        if forgotten {
            deliverFrame([
                "type": "control", "action": "close", "session": .string(frame.str("session")),
                "node": .string(node), "from": "node", "reason": "channel_gone",
            ])
            return
        }
        session = frame.str("session")
        XCTAssertEqual(frame["seq"]?.intValue, expectSeq, "phone chunks are numbered from 0")
        expectSeq += 1
        pending += Array(try! B64.decode(frame.str("data")))
        if frame["more"] == .bool(true) { return }
        let payload = pending
        pending = []
        switch stage {
        case 0:
            XCTAssertEqual(action, "open")
            let hello = JSON.parseObject(Data(payload))
            clientKey = hello.str("key")
            let ephPrivate = X25519.generatePrivateKey()
            let ephPub = controlB64(try! X25519.publicKey(privateKey: ephPrivate))
            transcript = controlTranscript(clientKey, hello.str("eph"), hello.str("nonce"), publicKey, ephPub)
            let shared = try! X25519.sharedSecret(privateKey: ephPrivate, remotePublicKey: controlFromB64(hello["eph"], 32)!)
            keys = deriveControlKeys(shared, transcript)
            let sig = try! Ed25519.sign(seed: seed, message: controlServerSignedBytes(transcript))
            let welcome: JSONObject = [
                "t": .string(controlWelcomeTag), "v": 1, "key": .string(publicKey), "eph": .string(ephPub),
                "name": .string(node), "user": "alice", "sig": .string(controlB64(sig)),
            ]
            stage = 1
            sendChunks(Array(JSON.encode(.object(welcome)).utf8))
        case 1:
            let auth = JSON.parseObject(Data(payload))
            XCTAssertTrue(
                Ed25519.verify(
                    publicKey: controlFromB64(clientKey, 32)!, message: controlClientSignedBytes(transcript),
                    signature: controlFromB64(auth["sig"], 64) ?? []),
                "the phone signs the transcript over the relay too")
            stage = 2
            if paired {
                sendSealed(["event": "session", "data": ["paired": true, "name": .string(node), "transport": "relay"]])
            } else {
                sendSealed(["event": "pairing-denied", "data": ["reason": "pair_on_lan"]])
                deliverFrame(["type": "control", "action": "close", "session": .string(session), "node": .string(node),
                              "from": "node", "reason": "Approve this phone on the desktop's own network first."])
            }
        default:
            guard let plain = openControlFrame(keys.clientToServer, receiveCounter, payload) else {
                XCTFail("a phone frame did not authenticate")
                return
            }
            receiveCounter += 1
            let message = JSON.parseObject(Data(plain))
            requests.append(message)
            let id = message.int("id")
            let method = message.str("method")
            let params = message.obj("params")
            if let code = refuse?(method, params) {
                sendSealed(["id": .int(Int64(id)), "ok": false, "error": .string(code)])
                return
            }
            if let custom = handler?(method, params) {
                sendSealed(["id": .int(Int64(id)), "ok": true, "result": .object(custom)])
                return
            }
            switch method {
            case "big":
                sendSealed(["id": .int(Int64(id)), "ok": true, "result": ["blob": .string(String(repeating: "b", count: 30000))]])
            case "fail":
                sendSealed(["id": .int(Int64(id)), "ok": false, "error": "not_owner"])
            default:
                sendSealed(["id": .int(Int64(id)), "ok": true, "result": ["method": .string(method)]])
            }
        }
    }

    func sendSealed(_ message: JSONObject) {
        let sealed = sealControlFrame(keys.serverToClient, sendCounter, Array(JSON.encode(.object(message)).utf8))
        sendCounter += 1
        sendChunks(sealed)
    }

    func event(_ name: String, _ data: JSONObject = [:]) {
        sendSealed(["event": .string(name), "data": .object(data)])
    }

    private func sendChunks(_ payload: [UInt8]) {
        let encoded = Array(B64.encode(payload).utf8)
        var at = 0
        repeat {
            let end = min(encoded.count, at + chunkChars)
            deliverFrame([
                "type": "control", "action": "data", "session": .string(session), "node": .string(node),
                "from": "node", "seq": .int(Int64(sendSeq)),
                "data": .string(String(decoding: encoded[at..<end], as: UTF8.self)), "more": .bool(end < encoded.count),
            ])
            sendSeq += 1
            at = end
        } while at < encoded.count
    }

    func deliverFrame(_ frame: JSONObject) {
        deliver { self.onFrame?(frame) }
    }

    /// A chunk that skips a sequence number, as a lossy relay would produce.
    func sendOutOfOrder() {
        sendSeq += 1
        event("agents.changed")
    }

    func reconnectSocket() {
        deliver { self.onConnected?() }
    }
}

final class RelayControlConnectionTests: XCTestCase {
    private func phone() -> Identity { try! Identity.fromSeed(Ed25519.generateSeed()) }

    @MainActor func testHandshakeRequestsAndEventsThroughTheRelay() async throws {
        let desktop = try FakeRelayDesktop()
        let connection = try await RelayControlConnection.open(
            channel: desktop, identity: phone(), desktopKey: desktop.publicKey, node: "Desk", deviceName: "Phone",
            user: "alice", platform: "ios")
        XCTAssertTrue(connection.isOpen)
        XCTAssertEqual(connection.transport, .relay)
        XCTAssertEqual(connection.desktopName, "desk")
        XCTAssertEqual(connection.session.count, 32)

        let answer = try await connection.request("agents.list", ["x": 1])
        XCTAssertEqual(answer.str("method"), "agents.list")
        XCTAssertEqual(desktop.requests.last?.obj("params").int("x"), 1)

        // A response many chunks long is joined back together.
        let big = try await connection.request("big", [:])
        XCTAssertEqual(big.str("blob").count, 30000)

        // A request larger than one relay chunk is split on the way out.
        let before = desktop.sent.count
        _ = try await connection.request("echo", ["text": .string(String(repeating: "p", count: 20000))])
        XCTAssertGreaterThan(desktop.sent.count - before, 1)
        XCTAssertTrue(desktop.sent.allSatisfy { $0.str("data").count <= RelayControlConnection.chunkChars })

        do {
            _ = try await connection.request("fail", [:])
            XCTFail("the desktop's refusal is thrown")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "not_owner")
        }

        var heard: [String] = []
        connection.addEventTap { name, _ in heard.append(name) }
        desktop.event("hosts.changed")
        let got = await waitFor { heard.contains("hosts.changed") }
        XCTAssertTrue(got)

        await connection.close()
        XCTAssertFalse(connection.isOpen)
        XCTAssertTrue(desktop.disposed)
        XCTAssertEqual(desktop.sent.last?.str("action"), "close")
    }

    @MainActor func testOnlyTheDesktopPairedOnTheLanIsAccepted() async throws {
        let desktop = try FakeRelayDesktop()
        let other = controlB64(try Ed25519.publicKey(seed: Ed25519.generateSeed()))
        do {
            _ = try await RelayControlConnection.open(channel: desktop, identity: phone(), desktopKey: other, node: "desk")
            XCTFail("a substituted desktop is refused")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "wrong_desktop")
        }
        XCTAssertTrue(desktop.disposed)
    }

    @MainActor func testADesktopThatNeverApprovedThisPhoneSaysPairOnTheLan() async throws {
        let desktop = try FakeRelayDesktop()
        desktop.paired = false
        do {
            _ = try await RelayControlConnection.open(
                channel: desktop, identity: phone(), desktopKey: desktop.publicKey, node: "desk")
            XCTFail("an unpaired phone cannot control through the relay")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "pair_on_lan")
        }
    }

    @MainActor func testALostChunkOrADroppedSocketEndsTheChannel() async throws {
        let desktop = try FakeRelayDesktop()
        let connection = try await RelayControlConnection.open(
            channel: desktop, identity: phone(), desktopKey: desktop.publicKey, node: "desk")
        var closedWith = ""
        connection.onClosed = { closedWith = $0 }
        desktop.sendOutOfOrder()
        let closed = await waitFor { !connection.isOpen }
        XCTAssertTrue(closed)
        XCTAssertEqual(closedWith, "relay_out_of_order")

        let desktop2 = try FakeRelayDesktop()
        let second = try await RelayControlConnection.open(
            channel: desktop2, identity: phone(), desktopKey: desktop2.publicKey, node: "desk")
        desktop2.reconnectSocket()
        let dropped = await waitFor { !second.isOpen }
        XCTAssertTrue(dropped)
        XCTAssertEqual(second.closeReason, "relay_reconnected")
    }

    @MainActor func testADesktopThatStopsAnsweringIsNoticedByTheKeepalive() async throws {
        let desktop = try FakeRelayDesktop()
        let connection = try await RelayControlConnection.open(
            channel: desktop, identity: phone(), desktopKey: desktop.publicKey, node: "desk", keepalive: 0.2)
        let quiet = try await connection.request("agents.list", [:])
        XCTAssertEqual(quiet.str("method"), "agents.list")
        let pinged = await waitFor { desktop.requests.contains { $0.str("method") == "ping" } }
        XCTAssertTrue(pinged, "an idle channel is pinged")
        XCTAssertTrue(connection.isOpen, "a desktop that answers the ping keeps the channel")
        desktop.silent = true
        let closed = await waitFor { !connection.isOpen }
        XCTAssertTrue(closed)
        XCTAssertEqual(connection.closeReason, "desktop_unreachable")
    }

    @MainActor func testADesktopThatForgotTheChannelEndsItAtOnce() async throws {
        let desktop = try FakeRelayDesktop()
        let connection = try await RelayControlConnection.open(
            channel: desktop, identity: phone(), desktopKey: desktop.publicKey, node: "desk")
        desktop.forgotten = true
        let alive = await connection.confirmAlive(timeout: 5)
        XCTAssertFalse(alive)
        XCTAssertFalse(connection.isOpen)
        XCTAssertEqual(connection.closeReason, "channel_gone")
    }

    @MainActor func testNoRelaySocketTimesOutReadably() async throws {
        let desktop = try FakeRelayDesktop()
        desktop.socketUp = false
        do {
            _ = try await RelayControlConnection.open(
                channel: desktop, identity: phone(), desktopKey: desktop.publicKey, node: "desk", timeout: 0.2)
            XCTFail("no socket, no channel")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "timeout")
        }
    }
}

final class DesktopControlHubTests: XCTestCase {
    @MainActor func testWithNoLanTheHubDialsTheRelayAndKeepsTapsAcrossIt() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        let paired = PairedDesktop(key: desktop.publicKey, name: "desk", user: "alice", pairedAtMs: 5)
        var dials = 0
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                dials += 1
                XCTAssertEqual(target.key, desktop.publicKey)
                return try await RelayControlConnection.open(
                    channel: desktop, identity: identity, desktopKey: target.key, node: target.name)
            },
            paired: { [paired] })
        XCTAssertEqual(hub.reach, .offline(""))
        XCTAssertEqual(hub.localDesktop, .hidden)
        var heard: [String] = []
        hub.addEventTap { name, _ in heard.append(name) }

        let answer = try await hub.request("agents.list")
        XCTAssertEqual(answer.str("method"), "agents.list")
        XCTAssertEqual(hub.transport, .relay)
        XCTAssertEqual(hub.reach, .connected(.relay, "desk"))
        XCTAssertEqual(hub.localDesktop, .relay("desk"))
        _ = try await hub.request("status")
        XCTAssertEqual(dials, 1, "an open channel is reused")

        desktop.event("agents.changed")
        let got = await waitFor { heard.contains("agents.changed") }
        XCTAssertTrue(got, "a tap added before the channel existed hears it")
    }

    @MainActor func testTheHubChecksAnIdleRelayBeforeUsingIt() async throws {
        let first = try FakeRelayDesktop()
        let second = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        var fakes = [first, second]
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                let fake = fakes.removeFirst()
                return try await RelayControlConnection.open(
                    channel: fake, identity: identity, desktopKey: fake.publicKey, node: target.name)
            },
            paired: { [PairedDesktop(key: first.publicKey, name: "desk", user: "alice", pairedAtMs: 5)] })
        _ = try await hub.request("agents.list")
        hub.relayIdleProbe = 0
        first.forgotten = true
        let answer = try await hub.request("agents.start", ["task": "x"])
        XCTAssertEqual(answer.str("method"), "agents.start")
        XCTAssertFalse(first.requests.contains { $0.str("method") == "agents.start" })
        XCTAssertEqual(second.requests.filter { $0.str("method") == "agents.start" }.count, 1,
                       "the request goes once, on a fresh channel")
        XCTAssertTrue(fakes.isEmpty)
    }

    @MainActor func testWithNothingPairedTheHubDialsTheAccountsDesktops() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        var dialled: [String] = []
        var loads = 0
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                dialled.append(target.name)
                if target.name == "old-laptop" { throw LocalControlException("timeout") }
                return try await RelayControlConnection.open(
                    channel: desktop, identity: identity, desktopKey: target.key, node: target.name)
            },
            paired: { [] },
            accountDesktops: {
                loads += 1
                return [
                    PairedDesktop(key: "A" + String(repeating: "B", count: 42), name: "old-laptop", user: "alice"),
                    PairedDesktop(key: desktop.publicKey, name: "desk", user: "alice"),
                ]
            })
        XCTAssertEqual(hub.reach, .notPaired)
        let answer = try await hub.request("agents.list")
        XCTAssertEqual(answer.str("method"), "agents.list")
        XCTAssertEqual(dialled, ["old-laptop", "desk"], "an unreachable desktop falls through to the next")
        XCTAssertEqual(hub.reach, .connected(.relay, "desk"))
        XCTAssertEqual(hub.desktop?.name, "desk", "the desktop that answered is tried first next time")
        await hub.refreshAccountDesktops()
        XCTAssertEqual(loads, 1, "the account list is reused for a while")
        await hub.refreshAccountDesktops(force: true)
        XCTAssertEqual(loads, 2)
    }

    @MainActor func testTheChosenDesktopIsTheOneDialledAndTheOthersAreChecked() async throws {
        let desk = try FakeRelayDesktop()
        let laptop = try FakeRelayDesktop(node: "laptop")
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        let store = InMemoryKeyValueStore()
        var dialled: [String] = []
        var spare: [String: FakeRelayDesktop] = ["desk": desk, "laptop": laptop]
        let opener: RelayControlOpener = { target in
            dialled.append(target.name)
            guard let fake = spare.removeValue(forKey: target.name) else { throw LocalControlException("timeout") }
            return try await RelayControlConnection.open(
                channel: fake, identity: identity, desktopKey: target.key, node: target.name)
        }
        let listed: AccountDesktopsLoader = {
            [
                PairedDesktop(key: desk.publicKey, name: "desk", user: "alice"),
                PairedDesktop(key: laptop.publicKey, name: "laptop", user: "alice"),
            ]
        }
        let hub = DesktopControlHub(
            local: nil, openRelay: opener, paired: { [] }, accountDesktops: listed, store: store)
        await hub.refreshAccountDesktops()
        XCTAssertEqual(hub.choices.map(\.name), ["desk", "laptop"])
        XCTAssertEqual(hub.choices.map(\.presence), [.unknown, .unknown])
        hub.choose(key: laptop.publicKey)
        XCTAssertEqual(store.string(forKey: DesktopControlHub.preferredKeyStoreKey), laptop.publicKey)
        XCTAssertEqual(hub.desktop?.name, "laptop")

        _ = try await hub.request("agents.list")
        XCTAssertEqual(dialled, ["laptop"], "only the chosen desktop is dialled")
        XCTAssertEqual(hub.reach, .connected(.relay, "laptop"))
        XCTAssertEqual(hub.choices.first, ShellDesktopLight(id: laptop.publicKey, name: "laptop", presence: .online, active: true))

        await hub.checkDesktops(force: true)
        XCTAssertEqual(dialled, ["laptop", "desk"])
        XCTAssertEqual(hub.presenceOf(desk.publicKey), .online, "a second running desktop is detected")
        XCTAssertEqual(hub.desktopLights.count, 2)

        hub.choose(key: desk.publicKey)
        XCTAssertNil(hub.currentLink, "the other desktop's channel is let go")
        XCTAssertFalse(hub.reach.isConnected)
        XCTAssertEqual(hub.desktop?.name, "desk")

        let reopened = DesktopControlHub(local: nil, openRelay: opener, paired: { [] }, store: store)
        XCTAssertEqual(reopened.preferredKey, desk.publicKey)
    }

    @MainActor func testTwinNamesAreToldApart() async {
        let hub = DesktopControlHub(
            local: nil, openRelay: { _ in throw LocalControlException("timeout") }, paired: { [] },
            accountDesktops: {
                [
                    PairedDesktop(key: "abcdef0123", name: "desk", user: "alice"),
                    PairedDesktop(key: "987654abcd", name: "desk", user: "alice"),
                ]
            })
        await hub.refreshAccountDesktops()
        XCTAssertEqual(hub.choices.map(\.name), ["desk · abcdef", "desk · 987654"])
        hub.choose(name: "desk · 987654")
        XCTAssertEqual(hub.preferredKey, "987654abcd")
    }

    @MainActor func testNothingPairedMeansNothingToDial() async {
        let hub = DesktopControlHub(local: nil, openRelay: { _ in XCTFail("no dial"); throw LocalControlException("x") }, paired: { [] })
        XCTAssertEqual(hub.reach, .notPaired)
        do {
            _ = try await hub.request("agents.list")
            XCTFail("no desktop")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "not_paired")
        } catch {
            XCTFail("\(error)")
        }
    }
}

@MainActor
func waitFor(_ timeout: TimeInterval = 5, _ predicate: @MainActor () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if predicate() { return true }
        try? await Task.sleep(nanoseconds: 10_000_000)
    }
    return predicate()
}
