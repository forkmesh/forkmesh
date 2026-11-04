import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMPackages

enum RNSMessagingVectors {
    static let token = "9e25630d82e1393104eb23479f40bb658395c76957e7d7625611a9bde14aba43ec01868d737bf0c9c08eddd068e193c2bb9d434a727f36a571e1d7c52d7fc8708c7c0071fc11eea8c29ddab37cbbefb1"
    static let identityEncrypted = "36b2aa98b9294e2e001e5ea250548083ba799f6ba2ad99305ff17c38ff7ed869f82d582d5df6be6a3eeb92b0c3d6b7a99db6a68c21f15b9d70b45ef4c5a99dfc5b672b1e72c1dc74df253491b4ae9d606d8597b84ab1b1ec9accb256fc23b101"
    static let ifacAnnounce = "b63a9657e62d79092183ee5feb5080c3a80e1176e8041d327b62fbc7d20de581c5304a5581d482807ccac43e910c57b5e0727a042e700f524baa6235c927edc71f8f083e1a305f1d35b0c2e950f92ab7cb889b54a1f16bb8ebf4f4c9dfae477ce7ed90655a5b51dadf630f9c7518c0c91f5c3019bb1163126363808ab2cd4cb1f38fd321c552e70cd10673995e8ab00aac7f6a10064784f2762abcfddc6987589bc8cee72fe2e3f55022addc02303f5c1a5c188a43980b132e9a40338d8a056de17ff1f664bb4570"
}

final class ReticulumCryptoTests: XCTestCase {
    func testAesTokenAndIdentityEncryptionMatchRNS() throws {
        var block = rnsHex("00112233445566778899aabbccddeeff")
        let cipher = ReticulumAes256(key: (0..<32).map { UInt8($0) })
        cipher.encrypt(&block)
        XCTAssertEqual(ReticulumWire.hex(block), "8ea2b7ca516745bfeafc49904b496089")
        cipher.decrypt(&block)
        XCTAssertEqual(ReticulumWire.hex(block), "00112233445566778899aabbccddeeff")

        let key = (0..<64).map { UInt8($0) }
        XCTAssertEqual(ReticulumCipher.tokenDecrypt(key: key, token: rnsHex(RNSMessagingVectors.token)), Array("ForkMesh token vector".utf8))
        for length in [0, 1, 15, 16, 17, 100] {
            let plain = ReticulumWire.randomBytes(length)
            XCTAssertEqual(ReticulumCipher.tokenDecrypt(key: key, token: ReticulumCipher.tokenEncrypt(key: key, plaintext: plain)), plain)
        }
        var forged = ReticulumCipher.tokenEncrypt(key: key, plaintext: Array("secret".utf8))
        forged[20] ^= 1
        XCTAssertNil(ReticulumCipher.tokenDecrypt(key: key, token: forged))
        XCTAssertNil(ReticulumCipher.tokenDecrypt(key: key, token: Array(forged.prefix(40))))

        let identity = try XCTUnwrap(ReticulumIdentity(privateKey: rnsHex(RNSVectors.privateKey)))
        XCTAssertEqual(identity.decrypt(rnsHex(RNSMessagingVectors.identityEncrypted)), Array("to the identity".utf8))
        let recipient = ReticulumIdentity.generate()
        XCTAssertEqual(recipient.decrypt(recipient.encrypt(Array("to you".utf8))), Array("to you".utf8))
        XCTAssertNil(ReticulumIdentity.generate().decrypt(recipient.encrypt(Array("to you".utf8))))
        let ratchetPrivate = X25519.generatePrivateKey()
        let sealed = recipient.encrypt(Array("ratchet".utf8), ratchet: ReticulumCipher.x25519Public(ratchetPrivate))
        XCTAssertNil(recipient.decrypt(sealed))
        XCTAssertTrue(ReticulumCipher.x25519Exchange(ratchetPrivate, [UInt8](repeating: 0, count: 32)).isEmpty)
    }

    func testAccessCodesMatchRNS() {
        let raw = rnsHex(RNSVectors.announce)
        let code = ReticulumAccessCode(networkName: "testnet", passphrase: "secret")
        let masked = code.apply(raw)
        XCTAssertEqual(ReticulumWire.hex(masked), RNSMessagingVectors.ifacAnnounce)
        XCTAssertEqual(code.remove(masked), raw)
        XCTAssertNil(ReticulumAccessCode(networkName: "testnet", passphrase: "wrong").remove(masked))
        XCTAssertNil(code.remove(raw))
        var flipped = masked
        flipped[40] ^= 1
        XCTAssertNil(code.remove(flipped))
        XCTAssertFalse(ReticulumAccessCode(networkName: "", passphrase: "").enabled)
        XCTAssertFalse(ReticulumAccessCode(networkName: "a", passphrase: "", size: 65).enabled)
        let small = ReticulumAccessCode(networkName: "net", passphrase: "", size: 1)
        XCTAssertEqual(small.remove(small.apply(raw)), raw)
        XCTAssertEqual(small.apply(raw).count, raw.count + 1)
    }

    func testLxmfStampsProofsAndAdvertisements() throws {
        let payload = ReticulumLxmf.payload(timestamp: 1_767_225_600.5, title: "t", content: "hello")
        XCTAssertEqual(Array(payload.prefix(2)), [0x94, 0xcb])
        let sender = ReticulumIdentity.generate()
        let source = ReticulumWire.destinationHash(name: "lxmf.delivery", identityHash: sender.hash)
        let destination = [UInt8](repeating: 0x64, count: 16)
        let stamp = [UInt8](repeating: 0x73, count: 32)
        let packed = try XCTUnwrap(ReticulumLxmf.pack(source: sender, sourceHash: source, destinationHash: destination,
                                                       timestamp: 1_767_225_600.5, title: "t", content: "hello", stamp: stamp))
        let unpacked = try ReticulumLxmf.unpack(packed.packed)
        XCTAssertEqual(unpacked.content, "hello")
        XCTAssertEqual(unpacked.title, "t")
        XCTAssertEqual(unpacked.stamp, stamp)
        XCTAssertEqual(unpacked.hash, packed.hash)
        XCTAssertEqual(unpacked.timestamp, 1_767_225_600.5)
        XCTAssertTrue(ReticulumLxmf.verify(unpacked, sourcePublicKey: sender.publicKey))
        XCTAssertFalse(ReticulumLxmf.verify(unpacked, sourcePublicKey: ReticulumIdentity.generate().publicKey))
        XCTAssertThrowsError(try ReticulumLxmf.unpack(Array(packed.packed.prefix(96))))

        let fields = ReticulumMsgPackWriter().map(1).integer(5).array(1).array(2).bin(Array("a.txt".utf8)).bin([1, 2, 3]).bytes
        let withFile = destination + source + [UInt8](repeating: 0, count: 64) + ReticulumMsgPackWriter().array(4).real(1).bin([]).bin(Array("x".utf8)).raw(fields).bytes
        XCTAssertEqual(try ReticulumLxmf.unpack(withFile).attachments, [ReticulumAttachment(name: "a.txt", data: [1, 2, 3])])

        XCTAssertEqual(ReticulumLxmf.announceData(displayName: "Bob"), [0x93, 0xc4, 0x03, 0x42, 0x6f, 0x62, 0xc0, 0x90])
        XCTAssertEqual(ReticulumLxmf.announceData(displayName: "", stampCost: 8), [0x93, 0xc0, 0x08, 0x90])
        XCTAssertEqual(ReticulumLxmf.stampCost(appData: ReticulumLxmf.announceData(displayName: "x", stampCost: 8)), 8)
        XCTAssertEqual(ReticulumLxmf.stampCost(appData: ReticulumLxmf.announceData(displayName: "x")), 0)
        XCTAssertEqual(ReticulumLxmf.stampCost(appData: Array("plain".utf8)), 0)

        let id = ReticulumWire.fullHash(Array("message".utf8))
        let generated = try XCTUnwrap(ReticulumLxmf.generateStamp(messageId: id, cost: 6))
        XCTAssertGreaterThanOrEqual(generated.value, 6)
        XCTAssertTrue(ReticulumLxmf.validateStamp(messageId: id, stamp: generated.stamp, cost: 6))
        XCTAssertEqual(ReticulumLxmf.stampValue(workblock: ReticulumLxmf.workblock(id, rounds: ReticulumLxmf.stampRounds), stamp: generated.stamp), generated.value)
        XCTAssertFalse(ReticulumLxmf.validateStamp(messageId: id, stamp: [1, 2], cost: 6))
        XCTAssertNil(ReticulumLxmf.generateStamp(messageId: id, cost: 25))
        let cancel = ReticulumCancel()
        cancel.cancel()
        XCTAssertNil(ReticulumLxmf.generateStamp(messageId: id, cost: 24, cancel: cancel))
        let material = ReticulumWire.fullHash([1, 2, 3])
        XCTAssertEqual(ReticulumLxmf.stampValue(workblock: ReticulumLxmf.workblock(material, rounds: 20), stamp: stamp),
                       ReticulumWire.stampValue(material: material, stamp: stamp, expandRounds: 20))

        let proofKey = ReticulumIdentity.generate()
        let hash = ReticulumWire.fullHash([9])
        let proof = try ReticulumWire.parsePacket(ReticulumCipher.buildProof(signingPrivateKey: proofKey.signingPrivateKey, hash: hash, explicit: true,
                                                                               destination: [UInt8](repeating: 1, count: 16), destinationType: .link))
        XCTAssertEqual(proof.packetType, .proof)
        XCTAssertTrue(ReticulumCipher.validateProof(signingPublicKey: proofKey.signingPublicKey, hash: hash, proof: proof.data))
        XCTAssertTrue(ReticulumCipher.validateProof(signingPublicKey: proofKey.signingPublicKey, hash: hash, proof: Array(proof.data.suffix(64))))
        XCTAssertFalse(ReticulumCipher.validateProof(signingPublicKey: proofKey.signingPublicKey, hash: ReticulumWire.fullHash([8]), proof: proof.data))

        var advertisement = ReticulumResourceAdvertisement()
        advertisement.transferSize = 5000
        advertisement.dataSize = 4900
        advertisement.parts = 11
        advertisement.hash = ReticulumWire.fullHash([1])
        advertisement.randomHash = [1, 2, 3, 4]
        advertisement.originalHash = advertisement.hash
        advertisement.flags = 1
        advertisement.hashmap = (0..<44).map { UInt8($0) }
        let round = try XCTUnwrap(ReticulumLxmf.unpackAdvertisement(ReticulumLxmf.packAdvertisement(advertisement)))
        XCTAssertEqual(round, advertisement)
        advertisement.parts = 0
        XCTAssertNil(ReticulumLxmf.unpackAdvertisement(ReticulumLxmf.packAdvertisement(advertisement)))
        XCTAssertEqual(ReticulumWire.msgPackArrayItems([0x93, 0x01, 0xa1, 0x61, 0x91, 0xc0]), [1..<2, 2..<4, 4..<6])
        XCTAssertNil(ReticulumWire.msgPackArrayItems([0x93, 0x01]))
        XCTAssertEqual(ReticulumMsgPackWriter().integer(-1).integer(200).integer(-200).integer(70000).integer(5_000_000_000).bytes,
                       rnsHex("ffccc8d1ff38ce00011170cf000000012a05f200"))
    }
}

final class RelayHub {
    let scheduler: ManualScheduler
    private(set) var links: [RelayLink] = []

    init(scheduler: ManualScheduler) { self.scheduler = scheduler }

    func attach(_ link: RelayLink) { links.append(link) }

    func forward(_ bytes: [UInt8], from source: RelayLink) {
        for link in links where link !== source && !link.closed {
            _ = scheduler.after(0.002) { link.handler?(.received(bytes)) }
        }
    }
}

final class RelayLink: ReticulumLink {
    let hub: RelayHub
    var handler: ((ReticulumLinkEvent) -> Void)?
    var closed = false

    init(hub: RelayHub) { self.hub = hub }

    func open(_ handler: @escaping (ReticulumLinkEvent) -> Void) {
        self.handler = handler
        hub.attach(self)
        _ = hub.scheduler.after(0.01) { [weak self] in self?.handler?(.connected) }
    }

    func write(_ bytes: [UInt8]) -> Int {
        guard !closed else { return -1 }
        hub.forward(bytes, from: self)
        return bytes.count
    }

    func close() {
        closed = true
        handler = nil
    }
}

final class RelayConnector: ReticulumConnector {
    let hub: RelayHub
    init(hub: RelayHub) { self.hub = hub }
    func tcp(host: String, port: Int) -> ReticulumLink? { RelayLink(hub: hub) }
}

final class RelayPeer {
    let node: ReticulumNode
    let messenger: ReticulumMessenger

    init(_ name: String, hub: RelayHub, networkName: String = "", passphrase: String = "") {
        node = ReticulumNode(scheduler: hub.scheduler, connector: RelayConnector(hub: hub))
        messenger = ReticulumMessenger(node: node, scheduler: hub.scheduler)
        node.setIdentity(.generate())
        messenger.setDisplayName(name)
        node.onAnnounce = { [weak self] hash in self?.messenger.destinationAnnounced(hash) }
        node.packetHandler = { [weak self] id, packet, raw, hops in
            self?.messenger.handlePacket(interfaceId: id, packet: packet, raw: raw, hops: hops)
        }
        var config = ReticulumInterfaceConfig(id: "hub", name: "Hub", host: "127.0.0.1", port: 4242)
        config.networkName = networkName
        config.passphrase = passphrase
        node.setInterfaces([config])
        node.start()
    }
}

final class ReticulumMessagingTests: XCTestCase {
    private func run(_ scheduler: ManualScheduler, seconds: Double, until condition: () -> Bool) -> Bool {
        var elapsed = 0.0
        while !condition() {
            if elapsed > seconds { return false }
            scheduler.advance(0.05)
            elapsed += 0.05
        }
        return true
    }

    private func exchange(ifac: Bool) throws {
        let scheduler = ManualScheduler()
        let hub = RelayHub(scheduler: scheduler)
        let net = ifac ? "testnet" : ""
        let pass = ifac ? "secret" : ""
        let alice = RelayPeer("Alice", hub: hub, networkName: net, passphrase: pass)
        let bob = RelayPeer("Bob", hub: hub, networkName: net, passphrase: pass)
        let stranger = ifac ? RelayPeer("Stranger", hub: hub, networkName: net, passphrase: "wrong") : nil
        XCTAssertTrue(run(scheduler, seconds: 2) { alice.node.onlineCount == 1 && bob.node.onlineCount == 1 })
        XCTAssertTrue(alice.node.announce())
        XCTAssertTrue(bob.node.announce())
        XCTAssertTrue(run(scheduler, seconds: 2) {
            bob.node.destinations[alice.messenger.address] != nil && alice.node.destinations[bob.messenger.address] != nil
        })
        XCTAssertEqual(bob.node.destinations[alice.messenger.address]?.displayName, "Alice")
        XCTAssertEqual(bob.node.destinations[alice.node.destinationHash]?.label, "ForkMesh device")
        if let stranger {
            XCTAssertTrue(stranger.node.destinations.isEmpty)
            XCTAssertGreaterThan(stranger.node.status("hub").invalidFrames, 0)
        }
        let longText = (0..<300).map { "long line \($0)\n" }.joined()
        let shortId = bob.messenger.send(to: alice.messenger.address, content: "hi alice")
        let mediumId = bob.messenger.send(to: alice.messenger.address, content: String(repeating: "x", count: 300))
        let longId = bob.messenger.send(to: alice.messenger.address, content: longText)
        XCTAssertFalse(shortId.isEmpty || mediumId.isEmpty || longId.isEmpty)
        XCTAssertEqual(bob.messenger.message(shortId)?.state, .queued)
        func state(_ id: String) -> ReticulumMessageState? { bob.messenger.message(id)?.state }
        XCTAssertTrue(run(scheduler, seconds: 30) {
            state(shortId) == .delivered && state(mediumId) == .delivered && state(longId) == .delivered
        }, "\(String(describing: state(shortId))) \(String(describing: state(mediumId))) \(String(describing: state(longId))) \(bob.messenger.message(longId)?.note ?? "")")
        XCTAssertEqual(bob.messenger.message(shortId)?.method, "opportunistic")
        XCTAssertEqual(bob.messenger.message(mediumId)?.method, "link")
        XCTAssertEqual(bob.messenger.message(longId)?.method, "resource")
        XCTAssertEqual(bob.messenger.message(longId)?.progress, 100)
        let got = alice.messenger.messages(with: bob.messenger.address)
        XCTAssertEqual(got.count, 3)
        XCTAssertTrue(got.allSatisfy { $0.verified && !$0.read && $0.state == .received })
        XCTAssertTrue(got.contains { $0.content == longText && $0.method == "resource" })
        XCTAssertEqual(alice.messenger.unreadCount, 3)
        XCTAssertEqual(alice.messenger.conversations().first?.name, "Bob")
        XCTAssertEqual(alice.messenger.conversations().first?.unread, 3)
        XCTAssertGreaterThan(bob.messenger.activeLinks, 0)
        let restored = ReticulumMessenger(node: alice.node, scheduler: scheduler)
        restored.load(alice.messenger.saveJson())
        XCTAssertEqual(restored.messages(with: bob.messenger.address), got)
        alice.messenger.markRead(bob.messenger.address)
        XCTAssertEqual(alice.messenger.unreadCount, 0)
        XCTAssertTrue(run(scheduler, seconds: 200) { bob.messenger.activeLinks == 0 })
        if let stranger { XCTAssertTrue(stranger.messenger.conversations().isEmpty) }
    }

    func testMessagingOverOpenInterface() throws { try exchange(ifac: false) }

    func testMessagingOverAccessCodeInterface() throws { try exchange(ifac: true) }

    func testLargeMessagesToOnePeerGoOneResourceAtATime() throws {
        let scheduler = ManualScheduler()
        let hub = RelayHub(scheduler: scheduler)
        let alice = RelayPeer("Alice", hub: hub)
        let bob = RelayPeer("Bob", hub: hub)
        XCTAssertTrue(run(scheduler, seconds: 2) { alice.node.onlineCount == 1 && bob.node.onlineCount == 1 })
        alice.node.announce()
        bob.node.announce()
        XCTAssertTrue(run(scheduler, seconds: 2) { bob.node.destinations[alice.messenger.address] != nil })
        let opener = bob.messenger.send(to: alice.messenger.address, content: String(repeating: "o", count: 300))
        XCTAssertTrue(run(scheduler, seconds: 10) { bob.messenger.message(opener)?.state == .delivered })
        let first = bob.messenger.send(to: alice.messenger.address, content: String(repeating: "a", count: 2000))
        let second = bob.messenger.send(to: alice.messenger.address, content: String(repeating: "b", count: 3000))
        var queued: String?
        var order: [String] = []
        XCTAssertTrue(run(scheduler, seconds: 30) {
            XCTAssertLessThanOrEqual(bob.messenger.outgoingResources.count, 1)
            for id in [first, second] where bob.messenger.message(id)?.note == "Waiting for the previous transfer" {
                queued = id
                XCTAssertTrue(order.isEmpty)
            }
            for id in [first, second] where bob.messenger.message(id)?.state == .delivered && !order.contains(id) { order.append(id) }
            return order.count == 2
        })
        XCTAssertNotNil(queued)
        XCTAssertEqual(order.last, queued)
        XCTAssertTrue([first, second].allSatisfy { bob.messenger.message($0)?.method == "resource" })
        XCTAssertTrue(bob.messenger.queuedResources.isEmpty)
        XCTAssertEqual(alice.messenger.messages(with: bob.messenger.address).count, 3)
    }

    func testUnreachablePeerFailsAfterPathWait() {
        let scheduler = ManualScheduler()
        let hub = RelayHub(scheduler: scheduler)
        let bob = RelayPeer("Bob", hub: hub)
        XCTAssertTrue(run(scheduler, seconds: 2) { bob.node.onlineCount == 1 })
        let nobody = [UInt8](repeating: 0x42, count: 16)
        let id = bob.messenger.send(to: nobody, content: "anyone?")
        XCTAssertTrue(run(scheduler, seconds: 3) { bob.messenger.message(id)?.state == .findingPath })
        XCTAssertNotNil(bob.node.lookups[nobody])
        XCTAssertTrue(run(scheduler, seconds: 40) { bob.messenger.message(id)?.state == .failed })
        XCTAssertEqual(bob.messenger.message(id)?.note, "No path to this contact. They may be offline; retry later.")
        bob.messenger.retry(id)
        XCTAssertEqual(bob.messenger.message(id)?.state, .findingPath)
        bob.messenger.forget(id)
        XCTAssertNil(bob.messenger.message(id))
        XCTAssertEqual(bob.messenger.send(to: bob.messenger.address, content: "me"), "")
        XCTAssertEqual(bob.messenger.send(to: nobody, content: "   "), "")
        XCTAssertEqual(bob.messenger.send(to: nobody, content: String(repeating: "a", count: ReticulumMessenger.maxContentBytes + 1)), "")
    }

    func testIncomingLinkThatNeverFinishesIsDropped() throws {
        let scheduler = ManualScheduler()
        let hub = RelayHub(scheduler: scheduler)
        let alice = RelayPeer("Alice", hub: hub)
        let mallory = RelayPeer("Mallory", hub: hub)
        XCTAssertTrue(run(scheduler, seconds: 2) { alice.node.onlineCount == 1 && mallory.node.onlineCount == 1 })
        var request = ReticulumPacket()
        request.packetType = .linkRequest
        request.destinationHash = alice.messenger.address
        request.data = ReticulumIdentity.generate().publicKey
        mallory.node.sendPacket(request)
        XCTAssertTrue(run(scheduler, seconds: 1) { alice.messenger.links.count == 1 })
        XCTAssertTrue(run(scheduler, seconds: 70) { alice.messenger.links.isEmpty })
    }

    func testRoutedSendUsesHeaderTwo() throws {
        let scheduler = ManualScheduler()
        let connector = FakeConnector(scheduler: scheduler)
        let node = ReticulumNode(scheduler: scheduler, connector: connector)
        node.setIdentity(.generate())
        node.setInterfaces([ReticulumInterfaceConfig(id: "fake", name: "Fake", host: "127.0.0.1", port: 4242)])
        node.start()
        scheduler.advance(0.1)
        let link = try XCTUnwrap(connector.last)
        let remote = ReticulumIdentity.generate()
        var announce = ReticulumWire.buildAnnounce(identity: remote, name: "lxmf.delivery", appData: [], pathResponse: false, unixTime: Int64(scheduler.now))
        var routed = try ReticulumWire.parsePacket(announce)
        routed.headerType = 1
        routed.transportType = 1
        routed.transportId = [UInt8](repeating: 0x77, count: 16)
        routed.hops = 2
        announce = ReticulumWire.packPacket(routed)
        link.send(announce)
        let target = ReticulumWire.destinationHash(name: "lxmf.delivery", identityHash: remote.hash)
        XCTAssertEqual(node.destinations[target]?.hops, 3)
        var data = ReticulumPacket()
        data.destinationHash = target
        data.data = [1, 2, 3]
        node.sendPacket(data)
        let sent = try XCTUnwrap(link.frames.last.flatMap { try? ReticulumWire.parsePacket($0) })
        XCTAssertEqual(sent.headerType, 1)
        XCTAssertEqual(sent.transportType, 1)
        XCTAssertEqual(sent.transportId, [UInt8](repeating: 0x77, count: 16))
        var forwarded = data
        forwarded.headerType = 1
        forwarded.transportId = [UInt8](repeating: 0x55, count: 16)
        let before = node.counters.duplicates
        link.send(ReticulumWire.packPacket(forwarded))
        XCTAssertEqual(node.counters.duplicates, before + 1)
    }
}
