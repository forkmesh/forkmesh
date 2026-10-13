// The RoomSeal unit tests (the relay-harness tests belong to FMChat) and
// native/android/.../crypto/RoomSealCryptoTest.kt.

import FMBase
import XCTest
@testable import FMCrypto

final class RoomSealTests: XCTestCase {
    let vectorRoom = "world-general"
    let senderPrivate = "UE5ZIyTdbSCtuDSqkI3g5TnXBMw1nx4Oz1BfmvE6CGc"
    let receiverPrivate = "cJMcO0LKgE1B6DRDn8z2mCcgt-cKyuWEtafYL2hFjHk"
    let senderKeyId = "SQAm1SbhWWBxNiSt"
    let receiverKeyId = "PA22MdT_F-KVxa5L"
    let senderPub = "SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTw"
    let vectorPlain: JSONObject = [
        "type": "chat",
        "id": "seal-vector-2",
        "sender": "ada",
        "text": "automatic beyond the relay",
    ]

    var vectorFrame: JSONObject {
        [
            "type": "sealed",
            "v": 2,
            "kid": .string(senderKeyId),
            "pub": .string(senderPub),
            "nonce": "43+zI9tUg9t5SdVG",
            "tag": "D9qgU0oVQpoOY05RaI3xDg==",
            "body": "Jn79ws7IdrSi5kFmmLHAB7yUUuARxKFeLZKdzMQBoHbghdPyQgEq8OZanyL0hTh/TYbavVieT2DhVLLZq2F0kH1yeAP7JdwPDqPysTZCETMtZYDTKBLr",
            "keys": [
                [
                    "kid": .string(receiverKeyId),
                    "nonce": "dIYd3AXET2IQhRjH",
                    "tag": "wga2/Tmek1r/0u9oXq4ckw==",
                    "key": "pNMivvZuaR6ZEu3V/GkZOVYDlSP4Lr4MtsG0mIwtOMs=",
                ],
                [
                    "kid": .string(senderKeyId),
                    "nonce": "5vZE4C5wbgSbNENA",
                    "tag": "eiRSoFEo1m5rBwQ0jSgiEw==",
                    "key": "K7BObyf/R3vByKsOWMJQWiRDBb02WFDP3fy4cGaJ5O8=",
                ],
            ],
        ]
    }

    func b64url(_ value: String) -> [UInt8] { Array(try! B64.decode(value + "=")) }

    func testMobileOpensTheBrowserV2RecipientEnvelope() throws {
        let receiver = try RoomSeal.fromPrivateKey(vectorRoom, b64url(receiverPrivate))
        XCTAssertEqual(receiver.keyId, receiverKeyId)
        XCTAssertTrue(RoomSeal.isSealedFrame(vectorFrame))
        XCTAssertEqual(receiver.unseal(vectorFrame), vectorPlain)

        let sender = try RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        XCTAssertEqual(sender.keyId, senderKeyId)
        XCTAssertEqual(sender.unseal(vectorFrame), vectorPlain)

        let stranger = try RoomSeal.fromPrivateKey(vectorRoom, (1...32).map { UInt8($0) })
        XCTAssertNil(stranger.unseal(vectorFrame))
        let wrongRoom = try RoomSeal.fromPrivateKey("chat-channel:42:v1", b64url(receiverPrivate))
        XCTAssertNil(wrongRoom.unseal(vectorFrame))
    }

    func testDevicesDiscoverEachOtherAndWrapWithoutAPhrase() throws {
        let alice = try RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        let bob = try RoomSeal.fromPrivateKey(vectorRoom, b64url(receiverPrivate))
        XCTAssertTrue(RoomSeal.isKeyFrame(alice.keyFrame()))
        XCTAssertTrue(alice.acceptKeyFrame(bob.keyFrame()))
        XCTAssertTrue(bob.acceptKeyFrame(alice.keyFrame()))
        XCTAssertEqual(alice.peerCount, 1)
        XCTAssertEqual(bob.peerCount, 1)

        let frame = try alice.seal(["type": "chat", "text": "no setup required"])
        XCTAssertFalse(JSON.encode(frame).contains("no setup required"))
        XCTAssertEqual(frame["keys"]?.arrayValue?.count, 2) // bob plus Alice's own history
        XCTAssertEqual(bob.unseal(frame)?["text"], "no setup required")
        XCTAssertEqual(alice.unseal(frame)?["text"], "no setup required")

        var relabelled = frame
        relabelled["kid"] = .string(receiverKeyId)
        XCTAssertNil(bob.unseal(relabelled))
        XCTAssertFalse(RoomSeal.isSealedFrame(["type": "chat", "text": "plaintext"]))
        XCTAssertFalse(RoomSeal.isSealedFrame(["type": "sealed", "v": 2, "kid": "short"]))
        var asDouble = frame
        asDouble["v"] = .double(2)
        XCTAssertTrue(RoomSeal.isSealedFrame(asDouble), "Dart num equality accepts 2.0")
        var noKeys = frame
        noKeys["keys"] = .array([])
        XCTAssertFalse(RoomSeal.isSealedFrame(noKeys))
        XCTAssertEqual(RoomSeal.frameKeyId(["type": "chat"]), "")
        XCTAssertEqual(RoomSeal.frameKeyId(frame), senderKeyId)
    }

    func testTheDeviceKeyIsGeneratedAndRetainedAutomatically() throws {
        let store = InMemorySecureStore()
        let first = try RoomSeal.loadOrCreate(vectorRoom, storageScope: "wss://relay-one.example", secureStore: store)
        let peer = try RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        first.acceptKeyFrame(peer.keyFrame())
        let second = try RoomSeal.loadOrCreate(vectorRoom, storageScope: "wss://relay-one.example", secureStore: store)
        let otherController = try RoomSeal.loadOrCreate(vectorRoom, storageScope: "wss://relay-two.example", secureStore: store)
        XCTAssertFalse(first.keyId.isEmpty)
        XCTAssertEqual(second.keyId, first.keyId)
        XCTAssertEqual(second.peerCount, 1)
        XCTAssertEqual(otherController.keyId, first.keyId)
        XCTAssertEqual(otherController.peerCount, 0)
        XCTAssertTrue(first.shouldPersistAdvertisement(now: 100_000_000))
        XCTAssertFalse(second.shouldPersistAdvertisement(now: 100_000_001))
        XCTAssertTrue(otherController.shouldPersistAdvertisement(now: 100_000_001))
        XCTAssertThrowsError(try RoomSeal.loadOrCreate("   ", secureStore: store))
        XCTAssertThrowsError(try RoomSeal.fromPrivateKey(vectorRoom, [UInt8](repeating: 0, count: 31)))
    }

    func testStorageMatchesFlutter() throws {
        let store = InMemorySecureStore()
        let seal = try RoomSeal.loadOrCreate(vectorRoom, storageScope: " wss://relay-one.example ", secureStore: store)
        let storedPrivate = try XCTUnwrap(store.read(RoomSeal.privateKeyStoreKey))
        XCTAssertEqual(storedPrivate.count, 44)
        let privateKey = Array(try B64.decode(storedPrivate))
        XCTAssertTrue(privateKey[0] & 7 == 0 && privateKey[31] & 0xC0 == 0x40, "stored clamped, like Dart")
        XCTAssertEqual(
            String(B64.encodeUrlNoPadding(Sha256.hash(try X25519.publicKey(privateKey: privateKey))).prefix(16)), seal.keyId)

        let peer = try RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        XCTAssertTrue(seal.acceptKeyFrame(peer.keyFrame()))
        let suffix = B64.encodeUrlNoPadding(Array("wss://relay-one.example\n\(vectorRoom)".utf8))
        XCTAssertEqual(
            store.read("chat/e2ee/peersV2/\(suffix)"),
            "[{\"kid\":\"\(senderKeyId)\",\"pub\":\"\(senderPub)\"}]")
        XCTAssertTrue(seal.shouldPersistAdvertisement(now: 1_783_000_000_000))
        XCTAssertEqual(store.read("chat/e2ee/announcedV2/\(suffix)"), "1783000000000")

        // A key the retired Flutter app stored (standard base64 of a clamped key) is reused, and
        // peers load from its JSON.
        let flutterSuffix = B64.encodeUrlNoPadding(Array("\(vectorRoom)\n\(vectorRoom)".utf8))
        let flutterStore = InMemorySecureStore([
            RoomSeal.privateKeyStoreKey: B64.encode(b64url(receiverPrivate)),
            "chat/e2ee/peersV2/\(flutterSuffix)":
                "[{\"kid\":\"\(senderKeyId)\",\"pub\":\"\(senderPub)\"},{\"kid\":\"bogus\",\"pub\":\"x\"},7]",
            "chat/e2ee/announcedV2/\(flutterSuffix)": " 100 ",
        ])
        let restored = try RoomSeal.loadOrCreate(vectorRoom, secureStore: flutterStore)
        XCTAssertEqual(restored.keyId, receiverKeyId)
        XCTAssertEqual(restored.peerCount, 1)
        XCTAssertFalse(restored.shouldPersistAdvertisement(now: 100 + 1000), "Dart int.tryParse trims whitespace")
        XCTAssertEqual(restored.unseal(vectorFrame), vectorPlain)
    }

    func testPeerListIsCappedAndEvictsTheOldest() throws {
        let store = InMemorySecureStore()
        let seal = try RoomSeal.loadOrCreate(vectorRoom, secureStore: store)
        let peers = try (0..<70).map { _ in try RoomSeal.fromPrivateKey(vectorRoom, X25519.generatePrivateKey()) }
        for peer in peers { XCTAssertTrue(seal.acceptKeyFrame(peer.keyFrame())) }
        XCTAssertEqual(seal.peerCount, 63)
        let frame = try seal.seal(["type": "chat", "text": "to many"])
        XCTAssertEqual(frame["keys"]?.arrayValue?.count, 64)
        XCTAssertNil(peers[0].unseal(frame), "the oldest peer was evicted")
        XCTAssertEqual(peers[69].unseal(frame)?["text"], "to many")
        XCTAssertEqual(try RoomSeal.loadOrCreate(vectorRoom, secureStore: store).peerCount, 63)
    }

    func testUnsealRemembersSendersAndRejectsMalformedFrames() throws {
        let alice = try RoomSeal.fromPrivateKey(vectorRoom, b64url(senderPrivate))
        let bob = try RoomSeal.fromPrivateKey(vectorRoom, b64url(receiverPrivate))
        alice.acceptKeyFrame(bob.keyFrame())
        let frame = try alice.seal(["type": "dm", "text": "hi bob"])
        XCTAssertEqual(bob.peerCount, 0)
        XCTAssertEqual(bob.unseal(frame)?["text"], "hi bob")
        XCTAssertEqual(bob.peerCount, 1, "a verified sender becomes a peer")

        func mutate(_ change: (inout JSONObject) -> Void) -> JSONObject {
            var copy = frame
            change(&copy)
            return copy
        }
        func mutateWrap(_ change: @escaping (inout JSONObject) -> Void) -> JSONObject {
            mutate { copy in
                copy["keys"] = .array((frame["keys"]?.arrayValue ?? []).map { entry in
                    var map = entry.objectValue ?? [:]
                    change(&map)
                    return .object(map)
                })
            }
        }
        var body = Array(try B64.decode(frame["body"]?.stringValue ?? ""))
        body[0] ^= 1
        let bad: [JSONObject] = [
            mutate { $0["nonce"] = "AAAA" },
            mutate { $0["tag"] = "not base64!" },
            mutate { $0["body"] = "" },
            mutate { $0["pub"] = "SJXRaOihp8_2nTX69yDoRqkDhapmBc30VI8lARlbQTA" },
            mutate { $0["body"] = .string(B64.encode(body)) },
            mutateWrap { $0["key"] = .string(B64.encode([UInt8](repeating: 0, count: 31))) },
            mutateWrap { $0["tag"] = .string(B64.encode([UInt8](repeating: 0, count: 16))) },
            mutateWrap { $0["nonce"] = 12 },
            mutate { $0["keys"] = ["not a map"] },
        ]
        for (index, frame) in bad.enumerated() {
            XCTAssertNil(bob.unseal(frame), "case \(index)")
        }
    }

    func testDartIntTryParse() {
        XCTAssertEqual(RoomSeal.dartIntTryParse("1783000000000"), 1_783_000_000_000)
        XCTAssertEqual(RoomSeal.dartIntTryParse(" 100 "), 100)
        XCTAssertEqual(RoomSeal.dartIntTryParse("-5"), -5)
        XCTAssertEqual(RoomSeal.dartIntTryParse("0x1F"), 31)
        XCTAssertNil(RoomSeal.dartIntTryParse(""))
        XCTAssertNil(RoomSeal.dartIntTryParse("12a"))
        XCTAssertNil(RoomSeal.dartIntTryParse("1.5"))
    }
}
