import FMAccount
import FMBase
import FMCrypto
import Foundation
import XCTest

@testable import FMChat

/// One simulated device: its own preferences, secure store and Ed25519 key.
@MainActor
struct ChatDevice {
    let prefs: InMemoryKeyValueStore
    let secure: InMemorySecureStore
    let settings: SettingsService
    let identity: Identity
    let account: String

    init(name: String = "", seed: JSONObject = [:]) {
        account = name
        var values = seed
        if !name.isEmpty { values["profile/name"] = .string(name) }
        prefs = InMemoryKeyValueStore(values)
        secure = InMemorySecureStore()
        settings = SettingsService(prefs: prefs, secureStore: secure)
        identity = try! Identity.fromSeed(Ed25519.generateSeed())
    }
}

/// Polls `predicate` every 10 ms until it holds or `timeout` passes; the last answer.
@MainActor
func waitUntil(_ timeout: TimeInterval = 8, _ predicate: @MainActor () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if predicate() { return true }
        try? await Task.sleep(nanoseconds: 10_000_000)
    }
    return predicate()
}

/// A mesh with nothing but direct links: no multicast, no Bluetooth, no subnet sweep.
@MainActor
func makeMesh(_ device: ChatDevice, historyFile: URL? = nil) -> LocalMeshService {
    let mesh = LocalMeshService(
        settings: device.settings, identity: device.identity, secureStore: device.secure,
        historyFile: historyFile.map { file in { file } })
    let account = device.account
    mesh.accountProvider = { account }
    mesh.multicastEnabled = false
    mesh.bluetoothEnabled = false
    mesh.addressSweepEnabled = false
    mesh.loopbackIsLocalNetwork = true
    return mesh
}

func temporaryDirectory(_ name: String) -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(name)-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}

@MainActor
final class OfficePeer {
    let key: Identity
    let seal: RoomSeal
    let crypto: RoomCrypto
    let room: String
    var node: String { key.nodeId }

    init(room: String, crypto: RoomCrypto, acct: String = "pat") throws {
        key = try Identity.fromSeed(Ed25519.generateSeed())
        self.room = room
        self.crypto = crypto
        seal = try RoomSeal.loadOrCreate(
            room, scope: room, policy: .open, acct: acct, node: key.nodeId, secureStore: InMemorySecureStore(),
            nodeIdentity: key)
    }

    func bundleEnvelope() throws -> JSONObject { try crypto.encrypt(seal.keyFrame()) }

    func learnPhone(store: SecureStore, identity: DeviceIdentity, acct: String = "me") throws {
        let phone = try RoomSeal.loadOrCreate(
            room, scope: room, policy: .open, acct: acct, node: identity.nodeId, secureStore: store, nodeIdentity: identity)
        XCTAssertNotNil(seal.acceptBundle(phone.keyFrame()))
    }

    func envelope(_ plain: JSONObject) throws -> JSONObject {
        var signed = plain
        signed["senderId"] = .string(node)
        return try crypto.encrypt(try seal.seal(signed))
    }

    func history(_ entries: [(Int64, JSONObject)]) throws -> [JSONValue] {
        var out: [JSONValue] = [.object(["ts": 0, "body": .string(JSON.encode(try bundleEnvelope()))])]
        for (ts, envelope) in entries { out.append(.object(["ts": .int(ts), "body": .string(JSON.encode(envelope))])) }
        return out
    }

    func read(_ text: String) -> JSONObject? {
        guard let framed = crypto.decrypt(JSON.parseObject(text)), !RoomSeal.isKeyFrame(framed) else { return nil }
        if !RoomSeal.isSealedFrame(framed) { return framed }
        return seal.unseal(framed)
    }

    func readAll(_ texts: [String]) -> [JSONObject] { texts.compactMap(read) }
}

func serverEnvelope(_ plain: JSONObject, crypto: RoomCrypto) throws -> JSONObject {
    var envelope = try crypto.encrypt(plain)
    envelope["srv"] = true
    return envelope
}
