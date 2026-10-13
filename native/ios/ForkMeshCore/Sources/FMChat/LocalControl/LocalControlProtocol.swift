// The phone's half of the desktop's local control channel (desktop/src/LocalControl.h is the
// other half and the reference).
//
// The channel rides the desktop's mesh port. The mesh room key is shared by every ForkMesh install
// and proves nothing about who is at either end, so the channel authenticates and encrypts on its
// own:
//
//   phone   -> {t:"fm-control-hello",   key, eph, nonce, name, user, platform}
//   desktop -> {t:"fm-control-welcome", key, eph, name, user, sig}
//   phone   -> {t:"fm-control-auth",    sig}
//
// `key` is each side's Ed25519 identity and `eph` a fresh X25519 key. Each side signs the
// transcript, and every later frame is AES-256-GCM under a per-direction key HKDF'd from the
// ephemeral secret, with the frame counter as its nonce. Byte-for-byte compatibility with the
// desktop is pinned by the vectors in LocalControlTests (the same ones
// desktop/tests/test_local_control.cpp asserts).
//
// `sendEvent` lets the phone answer the desktop with event frames (`{event, data}`) as well as
// make requests. See LocalControlService's on-device AI serving.

import FMBase
import FMCrypto
import Foundation

public let controlProtocolVersion = 1
public let controlHelloTag = "fm-control-hello"
public let controlWelcomeTag = "fm-control-welcome"
public let controlAuthTag = "fm-control-auth"
public let controlMaxFrameBytes = 4 * 1024 * 1024
public let controlFileChunkBytes = 256 * 1024

private let controlLabel = Array("forkmesh-local-control-v1".utf8)

public func controlTranscript(
    _ clientKey: String, _ clientEph: String, _ clientNonce: String, _ serverKey: String, _ serverEph: String
) -> [UInt8] {
    Array("forkmesh-local-control-v1\n\(clientKey)\n\(clientEph)\n\(clientNonce)\n\(serverKey)\n\(serverEph)".utf8)
}

public func controlServerSignedBytes(_ transcript: [UInt8]) -> [UInt8] {
    controlLabel + Array(" server\n".utf8) + transcript
}

public func controlClientSignedBytes(_ transcript: [UInt8]) -> [UInt8] {
    controlLabel + Array(" client\n".utf8) + transcript
}

public struct ControlKeys: Equatable, Sendable {
    public let clientToServer: [UInt8]
    public let serverToClient: [UInt8]

    public init(clientToServer: [UInt8], serverToClient: [UInt8]) {
        self.clientToServer = clientToServer
        self.serverToClient = serverToClient
    }
}

/// HKDF-SHA256: the transcript hash salts the extract, one block per direction expands.
public func deriveControlKeys(_ sharedSecret: [UInt8], _ transcript: [UInt8]) -> ControlKeys {
    let salt = Sha256.hash(transcript)
    let prk = HmacSha256.mac(key: salt, sharedSecret)
    func expand(_ direction: String) -> [UInt8] {
        HmacSha256.mac(key: prk, controlLabel + Array(" \(direction)".utf8) + [1])
    }
    return ControlKeys(clientToServer: expand("c2s"), serverToClient: expand("s2c"))
}

/// The six digits both screens show while the desktop is asked to approve this phone. A
/// substituted key anywhere on the way changes them.
public func controlPairingCode(_ transcript: [UInt8]) -> String {
    let digest = Sha256.hash(controlLabel + Array(" pair\n".utf8) + transcript)
    let value = (UInt32(digest[0]) << 24) | (UInt32(digest[1]) << 16) | (UInt32(digest[2]) << 8) | UInt32(digest[3])
    let code = String(value % 1_000_000)
    return String(repeating: "0", count: max(0, 6 - code.count)) + code
}

private func controlNonce(_ counter: Int) -> [UInt8] {
    var nonce = [UInt8](repeating: 0, count: 12)
    var value = UInt64(truncatingIfNeeded: counter)
    var i = 11
    while i >= 4 {
        nonce[i] = UInt8(value & 0xff)
        value >>= 8
        i -= 1
    }
    return nonce
}

/// ciphertext || 16-byte tag.
public func sealControlFrame(_ key: [UInt8], _ counter: Int, _ plain: [UInt8]) -> [UInt8] {
    guard let box = try? AesGcm.encrypt(plain, key: key, nonce: controlNonce(counter), aad: controlLabel) else {
        return []
    }
    return box.cipherText + box.mac
}

/// nil when the frame does not authenticate under this key and counter (or opens to nothing).
public func openControlFrame(_ key: [UInt8], _ counter: Int, _ frame: [UInt8]) -> [UInt8]? {
    if frame.count < 16 { return nil }
    let plain = AesGcm.decryptOrNull(
        cipherText: Array(frame[0..<(frame.count - 16)]), nonce: controlNonce(counter),
        mac: Array(frame[(frame.count - 16)...]), key: key, aad: controlLabel)
    guard let plain, !plain.isEmpty else { return nil }
    return plain
}

public func controlB64(_ bytes: [UInt8]) -> String { B64.encodeUrlNoPadding(bytes) }

/// nil unless `text` is unpadded base64url of exactly `length` bytes.
public func controlFromB64(_ text: String?, _ length: Int) -> [UInt8]? {
    guard let text, !text.isEmpty, text.utf8.count <= 200 else { return nil }
    let allowed = text.utf8.allSatisfy { c in
        (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 45 || c == 95
    }
    if !allowed { return nil }
    guard let bytes = try? B64.decodeUnpadded(text), bytes.count == length else { return nil }
    return Array(bytes)
}

public func controlFromB64(_ value: JSONValue?, _ length: Int) -> [UInt8]? {
    controlFromB64(value?.stringValue, length)
}

public struct LocalControlException: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String
    public let message: String

    public init(_ code: String, message: String = "") {
        self.code = code
        self.message = message
    }

    public var description: String { "LocalControlException(\(code))" }
}

/// One open, authenticated channel to a desktop.
@MainActor
public final class LocalControlConnection {
    private enum Stage {
        case awaitingWelcome
        case welcomed
        case open
    }

    private let socket: MeshTCPConnection
    /// The desktop's Ed25519 identity, proven by its welcome signature.
    public private(set) var serverKey = ""
    public private(set) var serverName = ""
    public private(set) var serverUser = ""
    public private(set) var transcript: [UInt8] = []
    private var keys = ControlKeys(clientToServer: [], serverToClient: [])

    private var stage = Stage.awaitingWelcome
    private let buffer = MeshByteBuffer()
    private var welcomeWaiter: CheckedContinuation<JSONObject, Error>?
    private var welcomeFailure: Error?
    private var welcomeFrame: JSONObject?
    private var pending: [Int: CheckedContinuation<JSONObject, Error>] = [:]
    private var closedWaiters: [CheckedContinuation<Void, Never>] = []
    private let eventContinuation: AsyncStream<JSONObject>.Continuation
    private var reader: Task<Void, Never>?
    private var sendCounter = 0
    private var receiveCounter = 0
    private var nextId = 1
    public private(set) var isClosed = false

    /// `{event, data}` frames from the desktop, in order (the first is sent the moment the
    /// handshake ends and waits here for whoever takes the channel). One consumer.
    public let events: AsyncStream<JSONObject>

    private init(socket: MeshTCPConnection) {
        self.socket = socket
        var continuation: AsyncStream<JSONObject>.Continuation!
        events = AsyncStream(bufferingPolicy: .unbounded) { continuation = $0 }
        eventContinuation = continuation
    }

    public var pairingCode: String { controlPairingCode(transcript) }

    /// Dials `address:port` and runs the handshake. With `expectedKey` (a desktop paired before),
    /// a different identity is refused.
    public static func open(
        address: MeshAddress,
        port: Int,
        identity: DeviceIdentity,
        deviceName: String = "",
        user: String = "",
        platform: String = "",
        expectedKey: String? = nil,
        timeout: TimeInterval = 10
    ) async throws -> LocalControlConnection {
        let socket = try await MeshTCPConnection.connect(to: address, port: port, timeout: timeout)
        socket.setNoDelay(true)
        let connection = LocalControlConnection(socket: socket)
        do {
            try await connection.handshake(
                identity: identity, deviceName: deviceName, user: user, platform: platform, expectedKey: expectedKey,
                timeout: timeout)
        } catch {
            connection.destroy()
            throw error
        }
        return connection
    }

    private func handshake(
        identity: DeviceIdentity, deviceName: String, user: String, platform: String, expectedKey: String?,
        timeout: TimeInterval
    ) async throws {
        let ephPrivate = X25519.generatePrivateKey()
        let ephPublic = try X25519.publicKey(privateKey: ephPrivate)
        let nonce = CryptoBytes.random(16)
        let ephB64 = controlB64(ephPublic)
        let nonceB64 = controlB64(nonce)
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
        socket.write(meshFrameBytes(Array(JSON.encode(.object(hello)).utf8)))

        // Until the welcome arrives, frames are plain: the mesh writes its own key frame to every
        // new link before it knows what the link is for.
        startReader()
        let w = try await awaitWelcome(timeout: timeout)

        let serverKey = dartString(w["key"])
        let serverEph = dartString(w["eph"])
        guard let version = w["v"], version.numEquals(Int64(controlProtocolVersion)),
            let serverKeyBytes = controlFromB64(serverKey, 32),
            let serverEphBytes = controlFromB64(serverEph, 32),
            let sigBytes = controlFromB64(w["sig"], 64)
        else { throw LocalControlException("bad_welcome") }
        if let expectedKey, expectedKey != serverKey { throw LocalControlException("wrong_desktop") }
        let transcript = controlTranscript(identity.publicKeyB64url, ephB64, nonceB64, serverKey, serverEph)
        if !Ed25519.verify(publicKey: serverKeyBytes, message: controlServerSignedBytes(transcript), signature: sigBytes) {
            throw LocalControlException("bad_signature")
        }
        let shared = try X25519.sharedSecret(privateKey: ephPrivate, remotePublicKey: serverEphBytes)
        keys = deriveControlKeys(shared, transcript)
        let sig = identity.sign(controlClientSignedBytes(transcript))
        let auth: JSONObject = ["t": .string(controlAuthTag), "sig": .string(sig)]
        socket.write(meshFrameBytes(Array(JSON.encode(.object(auth)).utf8)))

        self.serverKey = serverKey
        serverName = dartString(w["name"])
        serverUser = dartString(w["user"])
        self.transcript = transcript
        stage = .open
        // Bytes that arrived behind the welcome belong to the channel.
        processChannel()
    }

    private func awaitWelcome(timeout: TimeInterval) async throws -> JSONObject {
        if let welcomeFrame { return welcomeFrame }
        if let welcomeFailure { throw welcomeFailure }
        let timer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(max(0, timeout) * 1_000_000_000))
            if Task.isCancelled { return }
            self?.failWelcome(MeshSocketError.timedOut)
        }
        defer { timer.cancel() }
        return try await withCheckedThrowingContinuation { continuation in
            welcomeWaiter = continuation
        }
    }

    private func failWelcome(_ error: Error) {
        if stage != .awaitingWelcome { return }
        if welcomeFailure == nil { welcomeFailure = error }
        if let waiter = welcomeWaiter {
            welcomeWaiter = nil
            waiter.resume(throwing: error)
        }
    }

    private func startReader() {
        let events = socket.events
        reader = Task { [weak self] in
            for await event in events {
                guard let self else { return }
                switch event {
                case .data(let data):
                    self.onData(data)
                case .closed(let error):
                    if self.stage == .awaitingWelcome {
                        self.failWelcome(error ?? LocalControlException("closed"))
                    }
                    self.close()
                    return
                }
            }
        }
    }

    private func onData(_ data: Data) {
        buffer.add(data)
        switch stage {
        case .awaitingWelcome:
            while stage == .awaitingWelcome {
                let payload: Data?
                do {
                    payload = try meshTakeFrame(buffer, maxBytes: controlMaxFrameBytes)
                } catch {
                    failWelcome(LocalControlException("bad_frame"))
                    return
                }
                guard let payload else { return }
                if case .object(let decoded)? = try? JSON.parse(payload), decoded["t"] == .string(controlWelcomeTag) {
                    // Everything behind the welcome waits for the keys.
                    stage = .welcomed
                    welcomeFrame = decoded
                    if let waiter = welcomeWaiter {
                        welcomeWaiter = nil
                        waiter.resume(returning: decoded)
                    }
                }
            }
        case .welcomed:
            return
        case .open:
            processChannel()
        }
    }

    /// Frames are opened strictly in order: each one's nonce is its position.
    private func processChannel() {
        while !isClosed {
            let payload: Data?
            do {
                payload = try meshTakeFrame(buffer, maxBytes: controlMaxFrameBytes)
            } catch {
                close()
                return
            }
            guard let payload else { return }
            guard let plain = openControlFrame(keys.serverToClient, receiveCounter, Array(payload)) else {
                close()
                return
            }
            receiveCounter += 1
            guard case .object(let decoded)? = try? JSON.parse(Data(plain)) else { continue }
            dispatch(decoded)
        }
    }

    private func dispatch(_ message: JSONObject) {
        if case .string(let event)? = message["event"] {
            let data = message["data"]?.objectValue ?? JSONObject()
            eventContinuation.yield(["event": .string(event), "data": .object(data)])
            return
        }
        guard case .int(let raw)? = message["id"] else { return }
        guard let waiter = pending.removeValue(forKey: Int(raw)) else { return }
        if message["ok"] == .bool(true) {
            waiter.resume(returning: message["result"]?.objectValue ?? JSONObject())
        } else {
            waiter.resume(throwing: LocalControlException(
                message["error"].map { $0.isNull ? "failed" : $0.text } ?? "failed",
                message: message["message"]?.stringValue ?? ""))
        }
    }

    private func sendSealed(_ message: JSONObject) {
        let body = Array(JSON.encode(.object(message)).utf8)
        let sealed = sealControlFrame(keys.clientToServer, sendCounter, body)
        sendCounter += 1
        socket.write(meshFrameBytes(sealed))
    }

    /// Sends one request and waits for its answer.
    public func request(_ method: String, _ params: JSONObject = [:], timeout: TimeInterval = 20) async throws -> JSONObject {
        if isClosed { throw LocalControlException("closed") }
        let id = nextId
        nextId += 1
        let timer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(max(0, timeout) * 1_000_000_000))
            if Task.isCancelled { return }
            self?.timeOut(id)
        }
        defer { timer.cancel() }
        return try await withCheckedThrowingContinuation { continuation in
            pending[id] = continuation
            sendSealed(["id": .int(Int64(id)), "method": .string(method), "params": .object(params)])
        }
    }

    private func timeOut(_ id: Int) {
        pending.removeValue(forKey: id)?.resume(throwing: LocalControlException("timeout"))
    }

    /// Sends an `{event, data}` frame to the desktop. A desktop that does not know the event
    /// ignores it: LocalControlServer drops any frame without a positive `id` and a `method`.
    /// False when the channel is closed.
    @discardableResult
    public func sendEvent(_ event: String, _ data: JSONObject = [:]) -> Bool {
        if isClosed || stage != .open { return false }
        sendSealed(["event": .string(event), "data": .object(data)])
        return true
    }

    /// Returns when the channel has closed, from either end.
    public func done() async {
        if isClosed { return }
        await withCheckedContinuation { continuation in closedWaiters.append(continuation) }
    }

    public func close() {
        if isClosed { return }
        isClosed = true
        let waiting = pending
        pending.removeAll()
        for (_, waiter) in waiting { waiter.resume(throwing: LocalControlException("closed")) }
        reader?.cancel()
        socket.close()
        eventContinuation.finish()
        let waiters = closedWaiters
        closedWaiters.removeAll()
        for waiter in waiters { waiter.resume() }
    }

    private func destroy() {
        failWelcome(LocalControlException("closed"))
        close()
    }
}
