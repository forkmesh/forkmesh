// Local control, plus the on-device AI serving on the same channel.

import FMBase
import FMCrypto
import FMLocalAI
import Foundation
import XCTest

@testable import FMChat

/// The desktop's half of the channel, as desktop/src/LocalControl.cpp speaks it: enough of it to
/// drive the phone's client end to end.
@MainActor
final class FakeDesktop {
    let listener: MeshTCPListener
    let seed = Ed25519.generateSeed()
    let publicKey: String
    var requests: [JSONObject] = []
    /// Event frames the phone sent (`{event, data}`), which a real desktop would ignore.
    var phoneEvents: [JSONObject] = []
    var uploads: [String: Data] = [:]
    var pairingCode = ""
    var approveAtOnce = false
    /// Frames that failed to authenticate (must stay 0).
    var badFrames = 0
    private var connection: MeshTCPConnection?
    private var keys = ControlKeys(clientToServer: [], serverToClient: [])
    private var sendCounter = 0
    private var receiveCounter = 0
    private var acceptTask: Task<Void, Never>?

    var port: Int { listener.port }

    init() throws {
        listener = try MeshTCPListener.bind(address: .loopbackIPv4, port: 0)
        publicKey = controlB64(try Ed25519.publicKey(seed: seed))
        let connections = listener.connections
        acceptTask = Task { [weak self] in
            for await connection in connections {
                self?.accept(connection)
            }
        }
    }

    func close() {
        acceptTask?.cancel()
        connection?.close()
        listener.close()
    }

    private func accept(_ socket: MeshTCPConnection) {
        connection = socket
        // What the mesh writes to every new link before it knows its purpose.
        socket.write(meshFrameBytes(Array(#"{"iv":"mesh","ct":"key"}"#.utf8)))
        let buffer = MeshByteBuffer()
        var stage = 0
        var transcript: [UInt8] = []
        var clientKey = ""
        Task { [weak self] in
            for await event in socket.events {
                guard let self, case .data(let data) = event else { return }
                buffer.add(data)
                while let payload = try? meshTakeFrame(buffer) {
                    if stage == 0 {
                        let hello = JSON.parseObject(payload)
                        clientKey = hello["key"]?.stringValue ?? ""
                        let ephPrivate = X25519.generatePrivateKey()
                        let ephPub = controlB64(try! X25519.publicKey(privateKey: ephPrivate))
                        transcript = controlTranscript(
                            clientKey, hello["eph"]?.stringValue ?? "", hello["nonce"]?.stringValue ?? "", self.publicKey, ephPub)
                        let shared = try! X25519.sharedSecret(
                            privateKey: ephPrivate, remotePublicKey: controlFromB64(hello["eph"], 32)!)
                        self.keys = deriveControlKeys(shared, transcript)
                        let sig = try! Ed25519.sign(seed: self.seed, message: controlServerSignedBytes(transcript))
                        let welcome: JSONObject = [
                            "t": .string(controlWelcomeTag), "v": 1, "key": .string(self.publicKey), "eph": .string(ephPub),
                            "name": "desk", "user": "alice", "sig": .string(controlB64(sig)),
                        ]
                        socket.write(meshFrameBytes(Array(JSON.encode(.object(welcome)).utf8)))
                        stage = 1
                    } else if stage == 1 {
                        let auth = JSON.parseObject(payload)
                        let ok = Ed25519.verify(
                            publicKey: controlFromB64(clientKey, 32)!, message: controlClientSignedBytes(transcript),
                            signature: controlFromB64(auth["sig"], 64) ?? [])
                        XCTAssertTrue(ok, "the phone signs the transcript")
                        stage = 2
                        self.pairingCode = controlPairingCode(transcript)
                        let data: JSONObject =
                            self.approveAtOnce
                            ? ["paired": true, "name": "desk"]
                            : ["paired": false, "code": .string(self.pairingCode), "name": "desk"]
                        self.send(["event": "session", "data": .object(data)])
                    } else {
                        guard let plain = openControlFrame(self.keys.clientToServer, self.receiveCounter, Array(payload)) else {
                            self.badFrames += 1
                            continue
                        }
                        self.receiveCounter += 1
                        self.handle(JSON.parseObject(Data(plain)))
                    }
                }
            }
        }
    }

    func send(_ message: JSONObject) {
        let sealed = sealControlFrame(keys.serverToClient, sendCounter, Array(JSON.encode(.object(message)).utf8))
        sendCounter += 1
        connection?.write(meshFrameBytes(sealed))
    }

    func approve() { send(["event": "session", "data": ["paired": true, "name": "desk"]]) }

    func event(_ name: String, _ data: JSONObject = [:]) { send(["event": .string(name), "data": .object(data)]) }

    /// LocalControlServer::handleMessage: anything without a positive id and a method is dropped.
    private func handle(_ message: JSONObject) {
        guard case .int(let id)? = message["id"], id > 0, let method = message["method"]?.stringValue, !method.isEmpty else {
            phoneEvents.append(message)
            return
        }
        requests.append(message)
        let params = message["params"]?.objectValue ?? [:]
        var result: JSONObject = [:]
        switch method {
        case "agents.list":
            result = [
                "sessions": [
                    [
                        "id": 7, "repositoryOwner": "alice", "repositoryName": "app", "status": "running",
                        "provider": "claude-code", "prompt": "Fix the login page\nand add a test", "stoppable": true,
                    ]
                ],
                "repositories": [["owner": "alice", "name": "app"]],
                "providers": [["id": "claude-code", "label": "Claude Code"]],
                "defaultProvider": "claude-code",
                "running": 1,
            ]
        case "agents.start":
            result = ["id": 8]
        case "files.begin":
            uploads["t1"] = Data()
            result = ["transfer": "t1"]
        case "files.chunk":
            let transfer = params["transfer"]?.stringValue ?? ""
            uploads[transfer, default: Data()].append(try! B64.decode(params["data"]?.stringValue ?? ""))
        case "files.end":
            result = ["name": "photo.jpg"]
        default:
            break
        }
        send(["id": .int(id), "ok": true, "result": .object(result)])
    }

    func requests(_ method: String) -> [JSONObject] { requests.filter { $0["method"]?.stringValue == method } }

    func events(_ name: String) -> [JSONObject] {
        phoneEvents.filter { $0["event"]?.stringValue == name }.map { $0["data"]?.objectValue ?? [:] }
    }
}

final class LocalControlPrimitiveTests: XCTestCase {
    private func hex(_ bytes: [UInt8]) -> String { Hex.encode(bytes) }

    func testPrimitivesMatchTheDesktopByteForByte() {
        // The same vectors desktop/tests/test_local_control.cpp asserts.
        let t = controlTranscript("CK", "CE", "CN", "SK", "SE")
        XCTAssertEqual(String(decoding: t, as: UTF8.self), "forkmesh-local-control-v1\nCK\nCE\nCN\nSK\nSE")
        let keys = deriveControlKeys([UInt8](repeating: 7, count: 32), t)
        XCTAssertEqual(hex(keys.clientToServer), "6e7a4a8c0002d3118fbc2d04ddbc268e61b1b45af566060d38e3235b2bd1e239")
        XCTAssertEqual(hex(keys.serverToClient), "b76be7e0ae92fe18b561280e896315649f905d4ebed4685676c4dc4b1158596f")
        XCTAssertEqual(controlPairingCode(t), "796568")
        let sealed = sealControlFrame(keys.clientToServer, 3, Array(#"{"id":1}"#.utf8))
        XCTAssertEqual(hex(sealed), "4fb9fc7fc067c05ecc5591ec3f59b2f37b2727354152adc0")
        XCTAssertEqual(openControlFrame(keys.clientToServer, 3, sealed).map { String(decoding: $0, as: UTF8.self) }, #"{"id":1}"#)
        XCTAssertNil(openControlFrame(keys.clientToServer, 4, sealed))
        XCTAssertNil(openControlFrame(keys.serverToClient, 3, sealed))
    }

    func testBase64FieldsMustBeExactUnpaddedBase64Url() {
        let key = controlB64([UInt8](repeating: 9, count: 32))
        XCTAssertEqual(controlFromB64(key, 32), [UInt8](repeating: 9, count: 32))
        XCTAssertNil(controlFromB64(key, 64))
        XCTAssertNil(controlFromB64(key + "=", 32))
        XCTAssertNil(controlFromB64("a+b/", 3))
        XCTAssertNil(controlFromB64(String?.none, 32))
        XCTAssertNil(controlFromB64("", 32))
    }

    func testFileNamesFromTheDesktopCannotEscapeTheFolder() {
        XCTAssertEqual(LocalControlService.safeName("../report.txt"), "report.txt")
        XCTAssertEqual(LocalControlService.safeName("..\\..\\etc\\passwd"), "passwd")
        XCTAssertEqual(LocalControlService.safeName(".hidden"), "hidden")
        XCTAssertEqual(LocalControlService.safeName("photo (1).jpg"), "photo (1).jpg")
        XCTAssertEqual(LocalControlService.safeName("naïve$.txt"), "na_ve_.txt")
        XCTAssertEqual(LocalControlService.safeName(""), "file")
        XCTAssertEqual(LocalControlService.safeName(String(repeating: "a", count: 130)).count, 120)
    }

    func testAgentRowsReadTheDesktopsAgentsList() {
        let snapshot = LocalAgentsSnapshot.fromJSON([
            "sessions": [["id": 3, "repositoryOwner": "", "repositoryName": "solo", "prompt": "  \n  "]],
            "repositories": [["owner": "", "name": "x"], ["owner": "a", "name": "b"]],
            "providers": [["id": "", "label": "none"], ["id": "codex", "label": "Codex"]],
            "running": "2",
        ])
        XCTAssertEqual(snapshot.sessions.first?.repoLabel, "solo")
        XCTAssertEqual(snapshot.repositories.map(\.label), ["a/b"])
        XCTAssertEqual(snapshot.providers.map(\.id), ["codex"])
        XCTAssertEqual(snapshot.running, 0, "only an int counts")
        let long = LocalAgentRow.fromJSON(["id": 1, "prompt": .string(String(repeating: "w", count: 100))])
        XCTAssertEqual(long.title, String(repeating: "w", count: 89) + "…")
    }

    func testAgentFoldersReadFromTheAgentsList() {
        let snapshot = LocalAgentsSnapshot.fromJSON([
            "folders": [
                ["id": "f1", "repositoryOwner": "a", "repositoryName": "b", "name": " Docs ", "collapsed": true,
                 "members": ["3", "", 4, "cloud:x"]],
                ["id": "f2", "repositoryOwner": "a", "repositoryName": "b", "name": ""],
                ["id": "", "repositoryName": "b", "name": "no id"],
                ["id": "f3", "repositoryName": "", "name": "no repo"],
            ],
        ])
        XCTAssertEqual(snapshot.folders, [
            LocalAgentFolder(id: "f1", owner: "a", repoName: "b", name: "Docs", collapsed: true, members: ["3", "cloud:x"]),
            LocalAgentFolder(id: "f2", owner: "a", repoName: "b", name: "Folder"),
        ])
        XCTAssertEqual(snapshot.with(queue: nil).folders, snapshot.folders)
        XCTAssertEqual(LocalAgentsSnapshot.fromJSON([:]).folders, [], "an older desktop sends no folders")
    }

    func testTheDesktopsQueueReadsFromTheAgentsList() {
        let snapshot = LocalAgentsSnapshot.fromJSON([
            "sessions": [["id": 4, "status": "queued"], ["id": 9, "status": "queued"]],
            "running": 2,
            "queue": ["running": 2, "limit": 5, "limitMax": 99, "queue": [9, 4, 0, "x"]],
        ])
        XCTAssertEqual(snapshot.queue, LocalAgentQueue(running: 2, limit: 5, limitMax: 99, order: [9, 4]))
        XCTAssertEqual(snapshot.queue?.summary, "2 of 5 running · 2 waiting")
        XCTAssertEqual(snapshot.queue?.position(of: 4), 1)
        XCTAssertNil(snapshot.queue?.position(of: 7))
        XCTAssertNil(LocalAgentsSnapshot.fromJSON(["running": 1]).queue, "an older desktop sends no queue")
        XCTAssertNil(LocalAgentQueue.fromJSON("not a map"))
        XCTAssertNil(LocalAgentQueue.fromJSON(["running": 1]))
        let bare = LocalAgentQueue.fromJSON(["limit": 0, "limitMax": 0])
        XCTAssertEqual(bare, LocalAgentQueue(running: 0, limit: 1, limitMax: 1, order: []))
        XCTAssertEqual(bare?.summary, "0 of 1 running")
        XCTAssertEqual(snapshot.with(queue: nil).sessions, snapshot.sessions)
    }

    func testMovingAQueuedAgentReordersTheQueue() {
        let queue = LocalAgentQueue(running: 1, limit: 1, limitMax: 99, order: [1, 2, 3, 4])
        XCTAssertEqual(queue.moved(3, to: 0).order, [3, 1, 2, 4])
        XCTAssertEqual(queue.moved(1, to: 2).order, [2, 3, 1, 4])
        XCTAssertEqual(queue.moved(2, to: 50).order, [1, 3, 4, 2])
        XCTAssertEqual(queue.moved(4, to: -3).order, [4, 1, 2, 3])
        XCTAssertEqual(queue.moved(8, to: 0), queue)
        XCTAssertEqual(LocalAgentQueue.target(from: 0, offset: 4), 3)
        XCTAssertEqual(LocalAgentQueue.target(from: 3, offset: 0), 0)
        XCTAssertEqual(LocalAgentQueue.target(from: 1, offset: 3), 2)
    }

    func testPairedDesktopsRoundTripAndRefuseBadKeys() {
        let key = controlB64([UInt8](repeating: 1, count: 32))
        let desktop = PairedDesktop(key: key, name: "desk", user: "alice", pairedAtMs: 5)
        XCTAssertEqual(PairedDesktop.fromJSON(.object(desktop.toJSON())), desktop)
        XCTAssertNil(PairedDesktop.fromJSON(["key": "short"]))
        XCTAssertNil(PairedDesktop.fromJSON("not a map"))
    }
}

@MainActor
final class LocalControlServiceTests: XCTestCase {
    private var desktop: FakeDesktop!
    private var device: ChatDevice!
    private var mesh: LocalMeshService!
    private var control: LocalControlService!
    private var files: URL!

    override func setUp() async throws {
        desktop = try FakeDesktop()
        device = ChatDevice()
        // Never started: the host is handed over directly, as the mesh would.
        mesh = LocalMeshService(settings: device.settings, identity: device.identity, secureStore: device.secure)
        files = temporaryDirectory("local-control-test")
        let folder = files!
        control = LocalControlService(
            mesh: mesh, identity: device.identity, prefs: device.prefs, userName: { "alice" }, filesDirectory: { folder })
    }

    override func tearDown() async throws {
        control.dispose()
        mesh.dispose()
        desktop.close()
        try? FileManager.default.removeItem(at: files)
    }

    private func host(_ key: String? = nil) -> MeshControlHost {
        MeshControlHost(nodeId: key ?? desktop.publicKey, address: .loopbackIPv4, port: desktop.port, nodeName: "desk")
    }

    private func pairedAndConnected() async {
        desktop.approveAtOnce = true
        await control.connect(host())
        let connected = await waitUntil(5) { self.control.connected && self.control.agents != nil }
        XCTAssertTrue(connected)
    }

    func testASwitchToAnotherProjectDropsThisProjectsDesktop() async throws {
        await pairedAndConnected()
        XCTAssertTrue(control.isPaired(desktop.publicKey))
        device.prefs.set("[]", forKey: LocalControlService.pairedKey)
        control.resetForProjectSwitch()
        XCTAssertFalse(control.connected)
        XCTAssertNil(control.agents)
        XCTAssertTrue(control.paired.isEmpty)
        XCTAssertEqual(control.desktopKey, "")
    }

    func testPairsThenDrivesAgentsAndFiles() async throws {
        await control.connect(host())
        let waiting = await waitUntil(5) { self.control.status == .awaitingApproval }
        XCTAssertTrue(waiting)
        XCTAssertEqual(control.pairingCode, desktop.pairingCode)
        XCTAssertEqual(control.pairingCode.count, 6)
        XCTAssertFalse(control.isPaired(desktop.publicKey))

        desktop.approve()
        let connected = await waitUntil(5) { self.control.connected && self.control.agents != nil }
        XCTAssertTrue(connected)
        XCTAssertTrue(control.isPaired(desktop.publicKey))
        XCTAssertEqual(control.desktopName, "desk")
        let row = try XCTUnwrap(control.agents?.sessions.first)
        XCTAssertEqual(row.session.id, 7)
        XCTAssertEqual(row.repoLabel, "alice/app")
        XCTAssertEqual(row.title, "Fix the login page")
        XCTAssertTrue(row.stoppable)
        XCTAssertEqual(control.agents?.repositories.first?.label, "alice/app")
        // The pairing is remembered for next time.
        XCTAssertTrue((device.prefs.string(forKey: LocalControlService.pairedKey) ?? "").contains(desktop.publicKey))

        let id = try await control.startAgent(owner: "alice", name: "app", task: "Add dark mode", provider: "claude-code")
        XCTAssertEqual(id, 8)
        let start = try XCTUnwrap(desktop.requests("agents.start").first)
        XCTAssertEqual(start["params"]?["task"], "Add dark mode")

        // A bell from the desktop re-reads the list.
        let lists = desktop.requests("agents.list").count
        desktop.event("agents.changed")
        let reread = await waitUntil(5) { self.desktop.requests("agents.list").count > lists }
        XCTAssertTrue(reread)

        // Phone to desktop, across more than one chunk.
        let upload = files.appendingPathComponent("upload.bin")
        let content = Data((0..<(controlFileChunkBytes + 1000)).map { UInt8($0 % 251) })
        try content.write(to: upload)
        let saved = try await control.sendFile(upload)
        XCTAssertEqual(saved, "photo.jpg")
        XCTAssertEqual(desktop.uploads["t1"], content)
        XCTAssertEqual(desktop.requests("files.chunk").count, 2)
        XCTAssertNil(control.uploadProgress)

        // Desktop to phone.
        desktop.event("file.begin", ["transfer": "d1", "name": "../report.txt", "size": 5])
        desktop.event("file.chunk", ["transfer": "d1", "data": .string(B64.encode(Data("hello".utf8)))])
        desktop.event("file.end", ["transfer": "d1"])
        let received = await waitUntil(5) { !self.control.receivedFiles.isEmpty }
        XCTAssertTrue(received)
        let file = try XCTUnwrap(control.receivedFiles.first)
        XCTAssertEqual(file.name, "report.txt")
        XCTAssertEqual(try String(contentsOfFile: file.path, encoding: .utf8), "hello")

        await control.disconnect()
        XCTAssertEqual(control.status, .idle)
        XCTAssertEqual(desktop.badFrames, 0, "every frame authenticated in order")
    }

    func testAPairedDesktopReconnectsStraightIntoControl() async {
        desktop.approveAtOnce = true
        await control.connect(host())
        let connected = await waitUntil(5) { self.control.connected }
        XCTAssertTrue(connected)
        XCTAssertEqual(control.pairingCode, "")
        XCTAssertEqual(control.localDesktop, .connected("desk"))
    }

    func testAMachineThatIsNotTheDesktopItClaimsIsRefused() async {
        let other = controlB64([UInt8](repeating: 9, count: 32))
        await control.connect(host(other))
        let failed = await waitUntil(5) { self.control.status == .failed }
        XCTAssertTrue(failed)
        XCTAssertTrue(control.error.contains("different machine"), control.error)
    }

    func testANoOneAtTheAddressFailsReadably() async throws {
        let closed = try MeshTCPListener.bind(address: .loopbackIPv4, port: 0)
        let port = closed.port
        closed.close()
        await control.connect(MeshControlHost(nodeId: desktop.publicKey, address: .loopbackIPv4, port: port))
        XCTAssertEqual(control.status, .failed)
        XCTAssertEqual(control.error, "The desktop could not be reached on this network.")
    }

    func testADesktopOnThePhonesAccountIsConnectedToWithoutPairingFirst() {
        func desk(_ id: String, owner: String) -> MeshControlHost {
            MeshControlHost(nodeId: id, address: .loopbackIPv4, port: 1, ownerUser: owner)
        }
        let mine = desk("mine", owner: " Alice ")
        let theirs = desk("theirs", owner: "mallory")
        let paired = desk("paired", owner: "")

        XCTAssertEqual(
            LocalControlService.automaticTarget([theirs, mine], paired: [], account: "alice", tried: []), mine)
        XCTAssertEqual(
            LocalControlService.automaticTarget([mine, paired], paired: ["paired"], account: "alice", tried: []),
            paired)
        XCTAssertNil(LocalControlService.automaticTarget([theirs], paired: [], account: "alice", tried: []))
        XCTAssertNil(LocalControlService.automaticTarget([mine], paired: [], account: "", tried: []))
        XCTAssertNil(LocalControlService.automaticTarget([mine], paired: [], account: "alice", tried: ["mine"]))
        XCTAssertEqual(
            LocalControlService.automaticTarget([paired], paired: ["paired"], account: "alice", tried: ["paired"]),
            paired)
    }

    func testADesktopTheAccountListsIsDialledOnTheLANWithoutAnOwnerMatch() {
        func desk(_ id: String, owner: String) -> MeshControlHost {
            MeshControlHost(nodeId: id, address: .loopbackIPv4, port: 1, ownerUser: owner)
        }
        let owned = desk("owned", owner: "")
        let paired = desk("paired", owner: "")
        let stranger = desk("stranger", owner: "")

        XCTAssertEqual(
            LocalControlService.automaticTarget(
                [stranger, owned], paired: [], accountDesktops: ["owned"], account: "", tried: ["owned"]),
            owned)
        XCTAssertEqual(
            LocalControlService.automaticTarget(
                [owned, paired], paired: ["paired"], accountDesktops: ["owned"], account: "alice", tried: []),
            paired)
        XCTAssertNil(
            LocalControlService.automaticTarget(
                [stranger], paired: [], accountDesktops: ["owned"], account: "alice", tried: []))
    }

    func testForgettingADesktopDropsItsApproval() async {
        await pairedAndConnected()
        XCTAssertTrue(control.isPaired(desktop.publicKey))
        await control.forget(desktop.publicKey)
        XCTAssertFalse(control.isPaired(desktop.publicKey))
        XCTAssertEqual(control.status, .idle)
    }

    // MARK: On-device AI for paired desktops

    private func readyInference(reply: [String] = ["Hel", "lo"]) async throws -> (LocalInferenceService, FakeLocalChatEngine) {
        let engine = FakeLocalChatEngine()
        try await engine.load(modelDirectory: files, model: LocalModelCatalog.gemma4E2B)
        engine.enqueueReply(reply)
        let service = LocalInferenceService(engine: engine, deviceName: "phone") { LocalModelCatalog.gemma4E2B }
        return (service, engine)
    }

    func testAIIsOffByDefaultAndSaysSo() async throws {
        let (service, _) = try await readyInference()
        control.inferenceProvider = { service }
        await pairedAndConnected()
        XCTAssertFalse(control.servesLocalAI)
        // Announced as not offered.
        let announced = await waitUntil(5) { !self.desktop.events(LocalControlService.capabilitiesEvent).isEmpty }
        XCTAssertTrue(announced)
        XCTAssertEqual(desktop.events(LocalControlService.capabilitiesEvent).last?["ai"]?["enabled"], false)

        desktop.event("ai.request", ["rid": 1, "method": "ai.info"])
        let answered = await waitUntil(5) { !self.desktop.events("ai.response").isEmpty }
        XCTAssertTrue(answered)
        let reply = try XCTUnwrap(desktop.events("ai.response").first)
        XCTAssertEqual(reply["rid"], 1)
        XCTAssertEqual(reply["ok"], false)
        XCTAssertEqual(reply["error"], "disabled")
        XCTAssertEqual(desktop.badFrames, 0)
    }

    func testAPairedDesktopRunsAChatOnThePhonesModel() async throws {
        let (service, engine) = try await readyInference()
        control.inferenceProvider = { service }
        await pairedAndConnected()
        control.setServesLocalAI(true)
        XCTAssertEqual(device.prefs.bool(forKey: "localControl/serveLocalAI"), true)
        let offered = await waitUntil(5) {
            self.desktop.events(LocalControlService.capabilitiesEvent).last?["ai"]?["enabled"] == .bool(true)
        }
        XCTAssertTrue(offered)
        XCTAssertEqual(
            desktop.events(LocalControlService.capabilitiesEvent).last?["ai"]?["methods"], ["ai.info", "ai.chat"])

        desktop.event("ai.request", ["rid": 5, "method": "ai.chat", "params": ["messages": [["role": "user", "content": "hi"]]]])
        let answered = await waitUntil(5) { !self.desktop.events("ai.response").isEmpty }
        XCTAssertTrue(answered)
        let reply = try XCTUnwrap(desktop.events("ai.response").first)
        XCTAssertEqual(reply["rid"], 5)
        XCTAssertEqual(reply["ok"], true)
        XCTAssertEqual(reply["message"]?["content"], "Hello")
        XCTAssertEqual(reply["finish_reason"], "stop")
        XCTAssertNil(reply["event"], "the inference service's own tag stays on the phone")
        XCTAssertEqual(engine.requests.count, 1)
        XCTAssertEqual(control.aiRequestsServed, 1)
        XCTAssertTrue(desktop.events("ai.delta").isEmpty, "no stream was asked for")
        XCTAssertEqual(desktop.badFrames, 0)
        XCTAssertTrue(desktop.requests.allSatisfy { $0["method"] != nil }, "answers never look like requests")
    }

    func testAStreamedChatSendsDeltasThenOneResponse() async throws {
        let (service, _) = try await readyInference(reply: ["a", "b", "c"])
        control.inferenceProvider = { service }
        await pairedAndConnected()
        control.setServesLocalAI(true)

        desktop.event(
            "ai.request",
            ["rid": 9, "method": "ai.chat", "stream": true, "params": ["messages": [["role": "user", "content": "go"]]]])
        let done = await waitUntil(5) { !self.desktop.events("ai.response").isEmpty }
        XCTAssertTrue(done)
        let deltas = desktop.events("ai.delta")
        XCTAssertEqual(deltas.map { $0["text"]?.stringValue ?? "" }, ["a", "b", "c"])
        XCTAssertTrue(deltas.allSatisfy { $0["rid"] == 9 })
        XCTAssertEqual(desktop.events("ai.response").count, 1)
        XCTAssertEqual(desktop.events("ai.response").first?["message"]?["content"], "abc")
        // Deltas come before the response on the wire.
        let order = desktop.phoneEvents.compactMap { $0["event"]?.stringValue }.filter { $0.hasPrefix("ai.") }
        XCTAssertEqual(order.last, "ai.response")
    }

    func testABadRequestAndAnUnknownMethodAreAnsweredNotDropped() async throws {
        let (service, _) = try await readyInference()
        control.inferenceProvider = { service }
        await pairedAndConnected()
        control.setServesLocalAI(true)

        desktop.event("ai.request", ["rid": 2, "method": "ai.chat", "params": ["messages": "nope"]])
        desktop.event("ai.request", ["rid": 3, "method": "ai.train"])
        desktop.event("ai.request", ["rid": 0, "method": "ai.info"])  // no id: ignored
        let answered = await waitUntil(5) { self.desktop.events("ai.response").count == 2 }
        XCTAssertTrue(answered)
        let byRid = Dictionary(
            uniqueKeysWithValues: desktop.events("ai.response").map { ($0["rid"]?.intValue ?? 0, $0) })
        XCTAssertEqual(byRid[2]?["error"], "bad_request")
        XCTAssertEqual(byRid[3]?["error"], "unknown_method")
        XCTAssertNil(byRid[0])
    }

    func testWithoutAModelInThisBuildTheDesktopIsToldSo() async throws {
        await pairedAndConnected()
        control.setServesLocalAI(true)
        XCTAssertFalse(control.localAIAvailable)
        desktop.event("ai.request", ["rid": 4, "method": "ai.info"])
        let answered = await waitUntil(5) { !self.desktop.events("ai.response").isEmpty }
        XCTAssertTrue(answered)
        XCTAssertEqual(desktop.events("ai.response").first?["error"], "unavailable")
    }

    func testTheInfoRequestDescribesThePhone() async throws {
        let (service, _) = try await readyInference()
        control.inferenceProvider = { service }
        await pairedAndConnected()
        control.setServesLocalAI(true)
        desktop.event("ai.request", ["rid": 11, "method": "ai.info"])
        let answered = await waitUntil(5) { !self.desktop.events("ai.response").isEmpty }
        XCTAssertTrue(answered)
        let info = try XCTUnwrap(desktop.events("ai.response").first)
        XCTAssertEqual(info["ok"], true)
        XCTAssertEqual(info["available"], true)
        XCTAssertEqual(info["device"], "phone")
        XCTAssertEqual(info["model"], .string(LocalModelCatalog.gemma4E2B.id))
    }

    func testTurningTheSwitchOffIsAnnounced() async throws {
        let (service, _) = try await readyInference()
        control.inferenceProvider = { service }
        await pairedAndConnected()
        control.setServesLocalAI(true)
        control.setServesLocalAI(false)
        let announced = await waitUntil(5) { self.desktop.events(LocalControlService.capabilitiesEvent).count >= 3 }
        XCTAssertTrue(announced)
        XCTAssertEqual(desktop.events(LocalControlService.capabilitiesEvent).last?["ai"]?["enabled"], false)
    }
}
