// New for the native app (WS7): the local control channel (FMChat/LocalControl, the desktop's
// LocalControl.h) carried through the relay, so the phone drives its desktop away from home.
//
// The phone holds a browser-role socket on its account's relay object (an event ticket, as the
// provider sign-in terminal does), the desktop its node-role socket; the object hands each
// `{"type":"control"}` frame to the other side (app/src/node_control.py). Inside those frames runs
// exactly the LAN protocol: the plain hello and welcome, both transcripts signed with the device
// keys, then AES-256-GCM frames under keys only the two devices derive. The relay sees a channel
// id, a machine name, sequence numbers and ciphertext.
//
//   phone -> {type:"control", action:"open", session, node, seq, data:b64(hello), more}
//   phone -> {type:"control", action:"data", session, node, seq, data:b64(frame chunk), more}
//   desk  -> {type:"control", action:"data" | "close" | "error", session, node, seq, data, reason}
//
// Sequence numbers run per direction over the whole channel (the hello's chunks included), and a
// gap ends the channel: the AES-GCM nonce is the frame counter, so a lost frame cannot be skipped.
// The desktop only accepts a relayed channel from a phone it approved in person on its network,
// and only the desktop key paired then is accepted here.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMCrypto
import Foundation

/// What carries relay frames: the account event socket (`AccountEventChannel`), or a fake.
@MainActor
public protocol RelayFrameChannel: AnyObject {
    var onFrame: (@MainActor (JSONObject) -> Void)? { get set }
    var onConnected: (@MainActor () -> Void)? { get set }
    func start()
    @discardableResult func send(_ frame: JSONObject) -> Bool
    func dispose() async
}

extension AccountEventChannel: RelayFrameChannel {}

/// One control channel to the desktop through the relay.
@MainActor
public final class RelayControlConnection: DesktopControlLink {
    /// Base64 characters per chunk: inside the relay's 12000 and a headless node's 16 KiB frame.
    public static let chunkChars = 9000
    /// The largest protocol frame this side reassembles.
    public static let maxFrameBytes = 1024 * 1024

    private enum Stage {
        case connecting
        case awaitingWelcome
        case open
        case closed
    }

    public let session: String
    public let node: String
    public let desktopKey: String
    public private(set) var desktopName = ""
    public private(set) var desktopUser = ""
    public private(set) var closeReason = ""
    public var transport: ControlTransport { .relay }
    public var isOpen: Bool { stage == .open }

    /// Fired once when the channel ends, from either side.
    public var onClosed: (@MainActor (String) -> Void)?

    private let channel: RelayFrameChannel
    private var stage = Stage.connecting
    private var keys = ControlKeys(clientToServer: [], serverToClient: [])
    private var sendSeq = 0
    private var receiveSeq = 0
    private var sendCounter = 0
    private var receiveCounter = 0
    private var pending: [UInt8] = []
    private var nextId = 1
    private var waiters: [Int: CheckedContinuation<JSONObject, Error>] = [:]
    private var stageWaiter: CheckedContinuation<Void, Error>?
    private var sessionWaiter: CheckedContinuation<JSONObject, Error>?
    private var socketUp = false
    private var welcome: JSONObject?
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]
    private var earlyEvents: [(String, JSONObject)] = []
    private var lastHeard = Date()
    private let keepalive: TimeInterval
    private var keepaliveTask: Task<Void, Never>?

    public var idleSeconds: TimeInterval { Date().timeIntervalSince(lastHeard) }

    private init(
        channel: RelayFrameChannel, node: String, desktopKey: String, session: String, keepalive: TimeInterval
    ) {
        self.channel = channel
        self.node = node.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        self.desktopKey = desktopKey
        self.session = session
        self.keepalive = keepalive
    }

    /// A channel id the relay accepts: 32 lowercase hex characters.
    nonisolated public static func newSessionId() -> String {
        CryptoBytes.hex(CryptoBytes.random(16))
    }

    /// Opens the relay socket, runs the handshake against `desktopKey` (the desktop paired on the
    /// LAN), and returns once the desktop has said whether this phone is paired.
    public static func open(
        channel: RelayFrameChannel,
        identity: DeviceIdentity,
        desktopKey: String,
        node: String,
        deviceName: String = "",
        user: String = "",
        platform: String = "",
        session: String = RelayControlConnection.newSessionId(),
        timeout: TimeInterval = 20,
        keepalive: TimeInterval = 30
    ) async throws -> RelayControlConnection {
        let connection = RelayControlConnection(
            channel: channel, node: node, desktopKey: desktopKey, session: session, keepalive: keepalive)
        do {
            try await connection.handshake(
                identity: identity, deviceName: deviceName, user: user, platform: platform, timeout: timeout)
        } catch {
            connection.finish(reason: (error as? LocalControlException)?.code ?? "closed", notifyDesktop: true)
            await channel.dispose()
            throw error
        }
        connection.startKeepalive()
        return connection
    }

    public func confirmAlive(timeout: TimeInterval = 8) async -> Bool {
        guard stage == .open else { return false }
        do {
            _ = try await request("ping", [:], timeout: timeout)
            return true
        } catch let answered as LocalControlException
            where stage == .open && !["timeout", "closed", "relay_unavailable"].contains(answered.code)
        {
            return true
        } catch {
            finish(reason: "desktop_unreachable", notifyDesktop: true)
            return false
        }
    }

    private func startKeepalive() {
        guard keepalive > 0, stage == .open else { return }
        let interval = keepalive
        keepaliveTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(interval * 1_000_000_000))
                guard let self, !Task.isCancelled, self.stage == .open else { return }
                if self.idleSeconds >= interval { _ = await self.confirmAlive(timeout: min(interval, 10)) }
            }
        }
    }

    private func handshake(
        identity: DeviceIdentity, deviceName: String, user: String, platform: String, timeout: TimeInterval
    ) async throws {
        channel.onFrame = { [weak self] frame in self?.onFrame(frame) }
        channel.onConnected = { [weak self] in self?.onSocketConnected() }
        channel.start()
        try await awaitStage(timeout: timeout)  // the socket is up

        let ephPrivate = X25519.generatePrivateKey()
        let ephPublic = try X25519.publicKey(privateKey: ephPrivate)
        let ephB64 = controlB64(ephPublic)
        let nonceB64 = controlB64(CryptoBytes.random(16))
        var hello: JSONObject = [
            "t": .string(controlHelloTag),
            "v": .int(Int64(controlProtocolVersion)),
            "key": .string(identity.publicKeyB64url),
            "eph": .string(ephB64),
            "nonce": .string(nonceB64),
        ]
        if !deviceName.isEmpty { hello["name"] = .string(deviceName) }
        if !user.isEmpty { hello["user"] = .string(user) }
        if !platform.isEmpty { hello["platform"] = .string(platform) }
        stage = .awaitingWelcome
        guard sendChunks(action: "open", Array(JSON.encode(.object(hello)).utf8)) else {
            throw LocalControlException("relay_unavailable")
        }
        try await awaitStage(timeout: timeout)  // the welcome arrived
        guard let w = welcome else { throw LocalControlException("bad_welcome") }
        let serverKey = w["key"]?.stringValue ?? ""
        let serverEph = w["eph"]?.stringValue ?? ""
        guard let version = w["v"], version.int(or: -1) == controlProtocolVersion,
            let serverKeyBytes = controlFromB64(serverKey, 32),
            let serverEphBytes = controlFromB64(serverEph, 32),
            let sigBytes = controlFromB64(w["sig"], 64)
        else { throw LocalControlException("bad_welcome") }
        // Only the desktop this phone paired with in person; the relay cannot substitute one.
        if serverKey != desktopKey { throw LocalControlException("wrong_desktop") }
        let transcript = controlTranscript(identity.publicKeyB64url, ephB64, nonceB64, serverKey, serverEph)
        if !Ed25519.verify(publicKey: serverKeyBytes, message: controlServerSignedBytes(transcript), signature: sigBytes) {
            throw LocalControlException("bad_signature")
        }
        let shared = try X25519.sharedSecret(privateKey: ephPrivate, remotePublicKey: serverEphBytes)
        keys = deriveControlKeys(shared, transcript)
        desktopName = w["name"]?.stringValue ?? ""
        desktopUser = w["user"]?.stringValue ?? ""
        let auth: JSONObject = [
            "t": .string(controlAuthTag),
            "sig": .string(identity.sign(controlClientSignedBytes(transcript))),
        ]
        stage = .open
        guard sendChunks(action: "data", Array(JSON.encode(.object(auth)).utf8)) else {
            throw LocalControlException("relay_unavailable")
        }
        // The desktop answers with its "session" event: paired, or refused (a phone it never
        // approved gets "pairing-denied" and a close).
        let session = try await firstSessionEvent(timeout: timeout)
        if session["paired"] != .bool(true) { throw LocalControlException("pair_on_lan") }
    }

    private func firstSessionEvent(timeout: TimeInterval) async throws -> JSONObject {
        if let index = earlyEvents.firstIndex(where: { $0.0 == "session" || $0.0 == "pairing-denied" }) {
            let (name, data) = earlyEvents.remove(at: index)
            if name == "pairing-denied" { throw LocalControlException(data.str("reason", "pair_on_lan")) }
            return data
        }
        if stage == .closed { throw LocalControlException(closeReason.isEmpty ? "closed" : closeReason) }
        let timer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(max(0, timeout) * 1_000_000_000))
            if Task.isCancelled { return }
            self?.resumeSession(.failure(LocalControlException("timeout")))
        }
        defer { timer.cancel() }
        return try await withCheckedThrowingContinuation { continuation in
            sessionWaiter = continuation
        }
    }

    private func resumeSession(_ result: Result<JSONObject, Error>) {
        guard let waiter = sessionWaiter else { return }
        sessionWaiter = nil
        waiter.resume(with: result)
    }

    // MARK: Frames

    private func onSocketConnected() {
        switch stage {
        case .connecting:
            socketUp = true
            resumeStage(nil)
        case .awaitingWelcome, .open:
            // The socket dropped and came back: whatever was in flight is gone, and with it the
            // frame counters. The channel cannot continue; the caller reconnects.
            finish(reason: "relay_reconnected", notifyDesktop: true)
        case .closed:
            break
        }
    }

    func onFrame(_ frame: JSONObject) {
        if stage == .closed || frame["type"] != .string("control") { return }
        if frame["session"] != .string(session) || frame["from"] != .string("node") { return }
        lastHeard = Date()
        switch frame["action"]?.stringValue ?? "" {
        case "close", "error":
            let reason = frame.str("reason")
            finish(reason: reason.isEmpty ? "closed" : reason, notifyDesktop: false)
        case "data":
            guard frame["seq"]?.intValue == receiveSeq else {
                finish(reason: "relay_out_of_order", notifyDesktop: true)
                return
            }
            receiveSeq += 1
            guard let chunk = B64.decodeOrNil(frame.str("data")), !chunk.isEmpty else {
                finish(reason: "bad_frame", notifyDesktop: true)
                return
            }
            pending.append(contentsOf: chunk)
            if pending.count > RelayControlConnection.maxFrameBytes {
                finish(reason: "frame_too_large", notifyDesktop: true)
                return
            }
            if frame["more"] == .bool(true) { return }
            let payload = pending
            pending = []
            handlePayload(payload)
        default:
            break
        }
    }

    private func handlePayload(_ payload: [UInt8]) {
        PerformanceMonitorService.section("relay.control") { openPayload(payload) }
    }

    private func openPayload(_ payload: [UInt8]) {
        switch stage {
        case .awaitingWelcome:
            if case .object(let decoded)? = try? JSON.parse(Data(payload)), decoded["t"] == .string(controlWelcomeTag) {
                welcome = decoded
                resumeStage(nil)
            }
        case .open:
            guard let plain = openControlFrame(keys.serverToClient, receiveCounter, payload) else {
                finish(reason: "frame_did_not_authenticate", notifyDesktop: true)
                return
            }
            receiveCounter += 1
            guard case .object(let message)? = try? JSON.parse(Data(plain)) else { return }
            dispatch(message)
        case .connecting, .closed:
            break
        }
    }

    private func dispatch(_ message: JSONObject) {
        if case .string(let event)? = message["event"] {
            let data = message["data"]?.objectValue ?? JSONObject()
            if sessionWaiter != nil && (event == "session" || event == "pairing-denied") {
                resumeSession(
                    event == "session"
                        ? .success(data) : .failure(LocalControlException(data.str("reason", "pair_on_lan"))))
                return
            }
            if taps.isEmpty {
                earlyEvents.append((event, data))
                if earlyEvents.count > 64 { earlyEvents.removeFirst() }
            }
            for tap in taps.values { tap(event, data) }
            return
        }
        guard let id = message["id"]?.intValue, let waiter = waiters.removeValue(forKey: id) else { return }
        if message["ok"] == .bool(true) {
            waiter.resume(returning: message["result"]?.objectValue ?? JSONObject())
        } else {
            let code = message["error"].map { $0.isNull ? "failed" : $0.text } ?? "failed"
            waiter.resume(throwing: LocalControlException(code, message: message["message"]?.stringValue ?? ""))
        }
    }

    /// Splits one protocol frame into relay chunks. False when the relay socket cannot take one.
    private func sendChunks(action firstAction: String, _ payload: [UInt8]) -> Bool {
        let encoded = Array(B64.encode(payload).utf8)
        var at = 0
        let action = firstAction
        repeat {
            let end = min(encoded.count, at + RelayControlConnection.chunkChars)
            let frame: JSONObject = [
                "type": "control",
                "action": .string(action),
                "session": .string(session),
                "node": .string(node),
                "seq": .int(Int64(sendSeq)),
                "data": .string(String(decoding: encoded[at..<end], as: UTF8.self)),
                "more": .bool(end < encoded.count),
            ]
            sendSeq += 1
            if !channel.send(frame) { return false }
            at = end
        } while at < encoded.count
        return true
    }

    private func sendSealed(_ message: JSONObject) -> Bool {
        let body = Array(JSON.encode(.object(message)).utf8)
        let sealed = sealControlFrame(keys.clientToServer, sendCounter, body)
        sendCounter += 1
        return sendChunks(action: "data", sealed)
    }

    // MARK: DesktopControlLink

    public func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        if stage != .open { throw LocalControlException("closed") }
        let id = nextId
        nextId += 1
        let timer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(max(0, timeout) * 1_000_000_000))
            if Task.isCancelled { return }
            self?.waiters.removeValue(forKey: id)?.resume(throwing: LocalControlException("timeout"))
        }
        defer { timer.cancel() }
        return try await withCheckedThrowingContinuation { continuation in
            waiters[id] = continuation
            if !sendSealed(["id": .int(Int64(id)), "method": .string(method), "params": .object(params)]) {
                waiters.removeValue(forKey: id)
                continuation.resume(throwing: LocalControlException("relay_unavailable"))
                finish(reason: "relay_unavailable", notifyDesktop: false)
            }
        }
    }

    @discardableResult
    public func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    public func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    /// Ends the channel and drops the relay socket.
    public func close() async {
        finish(reason: "closed", notifyDesktop: true)
        await channel.dispose()
    }

    // MARK: Lifecycle

    private func awaitStage(timeout: TimeInterval) async throws {
        let timer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(max(0, timeout) * 1_000_000_000))
            if Task.isCancelled { return }
            self?.resumeStage(LocalControlException("timeout"))
        }
        defer { timer.cancel() }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            if stage == .closed {
                continuation.resume(throwing: LocalControlException(closeReason.isEmpty ? "closed" : closeReason))
                return
            }
            if (stage == .connecting && socketUp) || (stage == .awaitingWelcome && welcome != nil) {
                continuation.resume()
                return
            }
            stageWaiter = continuation
        }
    }

    private func resumeStage(_ error: Error?) {
        guard let waiter = stageWaiter else { return }
        stageWaiter = nil
        if let error { waiter.resume(throwing: error) } else { waiter.resume() }
    }

    private func finish(reason: String, notifyDesktop: Bool) {
        if stage == .closed { return }
        let wasStarted = stage != .connecting || sendSeq > 0
        stage = .closed
        closeReason = reason
        keepaliveTask?.cancel()
        keepaliveTask = nil
        if notifyDesktop && wasStarted {
            channel.send([
                "type": "control", "action": "close", "session": .string(session), "node": .string(node),
                "reason": .string(String(reason.prefix(160))),
            ])
        }
        resumeStage(LocalControlException(reason))
        resumeSession(.failure(LocalControlException(reason)))
        let pendingWaiters = waiters
        waiters.removeAll()
        for (_, waiter) in pendingWaiters { waiter.resume(throwing: LocalControlException("closed")) }
        for tap in taps.values { tap("__closed", ["reason": .string(reason)]) }
        onClosed?(reason)
        onClosed = nil
    }
}
