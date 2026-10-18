import FMApi
import FMBase
@testable import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class DirectTrustUITests: XCTestCase {
    static let conversation = "direct:" + String(repeating: "d", count: 32)

    func testPendingDevicesBecomeLinesAndBannersInOrder() {
        let base = Date(timeIntervalSince1970: 1_000)
        let devices = [
            ChatDeviceInfo(kid: "self", acct: "me", node: "me-node", state: .approved, firstSeen: base, isSelf: true, isOwnAccount: true),
            ChatDeviceInfo(kid: "pat-1", acct: "pat", node: "pat-node", state: .approved, firstSeen: base),
            ChatDeviceInfo(kid: "pat-2", acct: "pat", node: "pat-new", state: .pending, firstSeen: base.addingTimeInterval(60)),
            ChatDeviceInfo(kid: "mine-2", acct: "me", node: "me-tablet", state: .pending, firstSeen: base.addingTimeInterval(30), isOwnAccount: true),
            ChatDeviceInfo(kid: "gone", acct: "pat", node: "pat-old", state: .blocked, firstSeen: base),
        ]
        let banners = DirectTrustNotices.pending(conversation: Self.conversation, devices: devices, name: "pat")
        XCTAssertEqual(banners.map(\.kid), ["mine-2", "pat-2"])
        XCTAssertEqual(banners.map(\.kind), [.ownDevice(kid: "mine-2"), .pendingDevice(kid: "pat-2")])
        XCTAssertEqual(banners[0].text, "A new device was added to your account. Approve it if it is yours, otherwise block it.")
        XCTAssertEqual(
            banners[1].text, "A new device for pat wants to join this chat. Check the safety number, then approve or block.")
        XCTAssertTrue(banners.allSatisfy(\.asksApproval))

        let changedAt = base.addingTimeInterval(45)
        let lines = DirectTrustNotices.notices(
            conversation: Self.conversation, devices: devices, name: "pat", safetyChanged: true, changedAt: changedAt)
        XCTAssertEqual(lines.map(\.kind), [.ownDevice(kid: "mine-2"), .safetyNumberChanged, .pendingDevice(kid: "pat-2")])
        XCTAssertEqual(lines[1].text, "Your safety number with pat changed")
        XCTAssertNil(lines[1].kid)
        XCTAssertFalse(lines[1].asksApproval)
        XCTAssertEqual(Set(lines.map(\.id)).count, 3)
        let quiet = DirectTrustNotices.notices(
            conversation: Self.conversation, devices: devices, name: "pat", safetyChanged: false, changedAt: changedAt)
        XCTAssertFalse(quiet.contains { $0.kind == .safetyNumberChanged })
    }

    func testTrustLinesSitInTheTranscriptByTime() {
        let base = Date(timeIntervalSince1970: 1_000_000)
        let rows = [0, 120].map { (offset: Int) -> ChatRow in
            ChatRow(
                message: ChatMessage(
                    id: "m\(offset)", conversation: Self.conversation, senderId: "pat-node", senderName: "pat",
                    text: "hi", timestamp: base.addingTimeInterval(TimeInterval(offset))),
                continuesGroup: false, queued: false, deletable: false)
        }
        let notice = DirectTrustNotice(
            id: "trust-x", conversation: Self.conversation, kind: .pendingDevice(kid: "k"), text: "line",
            timestamp: base.addingTimeInterval(60))
        let items = ChatConversationLayout.items(rows: rows, events: [], notices: [notice], group: false)
        XCTAssertEqual(items.map(\.id), ["time-m0", "m0", "trust-x", "m120"])
        guard case .notice(let shown) = items[2] else { return XCTFail("the trust line is a notice") }
        XCTAssertEqual(shown, notice)
        guard case .message(let after) = items[3] else { return XCTFail("a message follows") }
        XCTAssertEqual(after.position, .single)
    }

    func testSafetyNumberRowsAndScans() {
        let computed = ChatSafetyNumber.compute(scope: Self.conversation, identityKeys: [[UInt8](repeating: 1, count: 32)])
        let number = ChatSafetyNumber(
            scope: Self.conversation, digits: computed.digits, qr: computed.qr, verified: false, changed: false)
        XCTAssertEqual(number.rows.count, 3)
        XCTAssertTrue(number.rows.allSatisfy { $0.count == 4 })
        XCTAssertTrue(number.rows.joined().allSatisfy { $0.count == 5 })
        XCTAssertEqual(number.rows.joined().joined(), number.digits)

        XCTAssertEqual(DirectSafetyScan.scope(number.qr), Self.conversation)
        XCTAssertEqual(DirectSafetyScan.result(scanned: number.qr, expected: number.qr, matched: true), .verified)
        XCTAssertEqual(DirectSafetyScan.result(scanned: "https://example.com", expected: number.qr, matched: false), .notSafetyCode)
        let other = ChatSafetyNumber.compute(scope: "direct:" + String(repeating: "e", count: 32), identityKeys: []).qr
        XCTAssertEqual(DirectSafetyScan.result(scanned: other, expected: number.qr, matched: false), .otherConversation)
        let forged = DirectSafetyScan.prefix + Self.conversation + ":AAAA"
        XCTAssertEqual(DirectSafetyScan.result(scanned: forged, expected: number.qr, matched: false), .mismatch)
        XCTAssertEqual(DirectSafetyScan.verified.message, "Safety number verified.")
    }

    @MainActor func testPrivacySwitchesPersistAndTheLockFollowsTheBackground() async {
        let store = InMemoryKeyValueStore()
        let privacy = ChatPrivacy(store: store)
        XCTAssertTrue(privacy.coversAppSwitcher, "the app switcher cover is on by default")
        XCTAssertFalse(privacy.lockEnabled, "the chat lock is off by default")
        XCTAssertTrue(privacy.showsChat)
        privacy.appBackgrounded()
        XCTAssertTrue(privacy.showsChat, "no lock while the switch is off")

        privacy.setCoversAppSwitcher(false)
        privacy.setLockEnabled(true)
        XCTAssertTrue(privacy.showsChat, "turning the lock on does not lock the open chat")
        let reloaded = ChatPrivacy(store: store)
        XCTAssertFalse(reloaded.coversAppSwitcher)
        XCTAssertTrue(reloaded.lockEnabled)
        XCTAssertFalse(reloaded.showsChat, "a fresh launch starts locked")

        privacy.appBackgrounded()
        XCTAssertFalse(privacy.showsChat)
        XCTAssertTrue(privacy.shouldPromptAutomatically)
        var asked = 0
        await privacy.unlock(manual: false) {
            asked += 1
            return .failed
        }
        XCTAssertFalse(privacy.showsChat)
        XCTAssertTrue(privacy.failed)
        XCTAssertFalse(privacy.shouldPromptAutomatically, "a failed or cancelled prompt is not repeated by itself")
        await privacy.unlock(manual: false) {
            asked += 1
            return .unlocked
        }
        XCTAssertEqual(asked, 1)
        await privacy.unlock(manual: true) {
            asked += 1
            return .unlocked
        }
        XCTAssertEqual(asked, 2)
        XCTAssertTrue(privacy.showsChat)
        XCTAssertFalse(privacy.failed)

        privacy.appBackgrounded()
        await privacy.unlock(manual: false) { .unavailable }
        XCTAssertTrue(privacy.showsChat, "with no passcode on the phone there is nothing to ask")

        privacy.appBackgrounded()
        privacy.setLockEnabled(false)
        XCTAssertTrue(privacy.showsChat)

        XCTAssertEqual(ChatPrivacy.lockTitle("Face ID"), "Lock chats with Face ID")
        XCTAssertEqual(ChatPrivacy.lockTitle(nil), "Lock chats with your passcode")
        XCTAssertEqual(ChatPrivacy.lockedMessage("Touch ID"), "Unlock with Touch ID or your passcode to read your chats.")
        XCTAssertEqual(ChatPrivacy.lockedMessage(nil), "Unlock with your passcode to read your chats.")
        XCTAssertTrue(ChatPrivacy.lockNote(nil).contains("asks for your passcode before"))
    }

    @MainActor func testAnOfficePushChannelIdNamesItsRoom() async throws {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        let hub = ChatHub(relay: h.relay, mesh: mesh, isGuest: { false })
        let api = FakeAccountApi()
        let design = String(repeating: "a", count: 32)
        api.replies["GET /api/chat/channels"] = [
            "channels": [["id": .string(design), "name": "design", "visibility": "public"]]
        ]
        let office = OfficeChannelService(
            api: api, connector: FakeWebSocketConnector(), secureStore: InMemorySecureStore(),
            origin: { URL(string: "https://app.forkmesh.com") }, selfId: { "me-node" }, displayName: { "Me" })
        hub.office = office
        await office.refresh()
        let bridge: ShellChatBridge = hub
        XCTAssertEqual(bridge.officeConversation(channelId: design), "#office/design")
        XCTAssertNil(hub.officeConversation(channelId: String(repeating: "b", count: 32)))
        XCTAssertNil(hub.officeConversation(channelId: ""))
        await h.close()
    }

    @MainActor func testFocusIsReportedOnlyForAVisibleDirectTranscript() {
        var calls: [String] = []
        let reporter = DirectFocusReporter { conversation, on in calls.append("\(on ? "on" : "off") \(conversation)") }
        let dm = Self.conversation
        let other = "direct:" + String(repeating: "e", count: 32)
        reporter.update(conversation: "#general", onScreen: true, tabHidden: false, active: true, locked: false)
        XCTAssertEqual(calls, [])
        reporter.update(conversation: dm, onScreen: true, tabHidden: false, active: true, locked: false)
        reporter.update(conversation: dm, onScreen: true, tabHidden: false, active: true, locked: false)
        XCTAssertEqual(calls, ["on \(dm)"])
        reporter.update(conversation: dm, onScreen: true, tabHidden: false, active: false, locked: false)
        XCTAssertEqual(calls.last, "off \(dm)")
        reporter.update(conversation: dm, onScreen: true, tabHidden: false, active: true, locked: false)
        reporter.update(conversation: other, onScreen: true, tabHidden: false, active: true, locked: false)
        XCTAssertEqual(calls.suffix(3), ["on \(dm)", "off \(dm)", "on \(other)"])
        reporter.update(conversation: other, onScreen: true, tabHidden: true, active: true, locked: false)
        XCTAssertEqual(calls.last, "off \(other)")
        reporter.update(conversation: other, onScreen: true, tabHidden: false, active: true, locked: true)
        XCTAssertNil(reporter.reported)
        reporter.update(conversation: other, onScreen: true, tabHidden: false, active: true, locked: false)
        reporter.clear()
        XCTAssertEqual(calls.suffix(2), ["on \(other)", "off \(other)"])
        reporter.clear()
        XCTAssertEqual(calls.count, 8)
        XCTAssertNil(reporter.reported)
    }
}

extension DirectMessagesTests {
    @MainActor func testTheHubShowsPendingDevicesRouteAndSafetyChangesForTheOpenDirectMessage() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let harness = try await RelayHarness.start()
        let mesh = LocalMeshService(
            settings: harness.device.settings, identity: harness.device.identity, secureStore: harness.device.secure)
        let hub = ChatHub(relay: harness.relay, mesh: mesh)
        hub.direct = service
        let conversation = service.conversations[0]
        hub.select(conversation)
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let pat = try peerSeal("peer")
        _ = await teach(pat, from: connection, crypto)
        connection.receive(text: JSON.encode(try crypto.encrypt(pat.keyFrame())))
        let known = await waitUntil { service.deviceKeys(conversation) == [pat.keyId] }
        XCTAssertTrue(known)
        XCTAssertTrue(hub.trustBanners(conversation).isEmpty)
        XCTAssertEqual(hub.conversationRouteLine, "", "no linked device, no direct route")
        XCTAssertFalse(hub.conversationDetailLine.contains("Direct \u{00B7}"))

        let injected = try peerSeal("injected", node: "pat-new")
        connection.receive(text: JSON.encode(try crypto.encrypt(injected.keyFrame())))
        let pending = await waitUntil { hub.trustBanners(conversation).map(\.kid) == [injected.keyId] }
        XCTAssertTrue(pending)
        XCTAssertEqual(hub.trustBanners(conversation).first?.text, DirectTrustText.pendingDevice("pat"))
        XCTAssertTrue(hub.conversationItems.contains { $0.id == "trust-\(conversation)-\(injected.keyId)" })

        let before = hub.trustRevision
        await hub.approveDevice(conversation, kid: injected.keyId)
        XCTAssertGreaterThan(hub.trustRevision, before)
        XCTAssertTrue(hub.trustBanners(conversation).isEmpty)
        let changedLine = hub.conversationItems.first {
            if case .notice(let notice) = $0 { return notice.kind == .safetyNumberChanged }
            return false
        }
        XCTAssertNotNil(changedLine, "an approved new device changes the safety number")
        let number = try XCTUnwrap(hub.safetyNumber(conversation))
        XCTAssertTrue(number.changed)
        XCTAssertEqual(hub.verifySafetyCode(conversation, scanned: "nope"), .notSafetyCode)
        hub.acknowledgeSafetyChange(conversation)
        XCTAssertEqual(hub.safetyNumber(conversation)?.changed, false)
        XCTAssertFalse(hub.conversationItems.contains { $0.id == "trust-\(conversation)-safety" })
        XCTAssertEqual(hub.verifySafetyCode(conversation, scanned: number.qr), .verified)
        XCTAssertEqual(hub.safetyNumber(conversation)?.verified, true)
        XCTAssertTrue(hub.safetyVerified(conversation))
        XCTAssertFalse(hub.safetyVerified("#general"))
        XCTAssertNil(hub.safetyNumber("#general"))
        XCTAssertNil(hub.officeConversation(channelId: String(repeating: "a", count: 32)))
        await service.closeAll()
        await harness.close()
    }
}

extension DirectMessagesTests {
    @MainActor func testAServerDirectMessageShowsThePersonOnlineFromTheRoster() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let harness = try await RelayHarness.start()
        let mesh = LocalMeshService(
            settings: harness.device.settings, identity: harness.device.identity, secureStore: harness.device.secure)
        let hub = ChatHub(relay: harness.relay, mesh: mesh)
        hub.direct = service
        let conversation = service.conversations[0]
        hub.select(conversation)
        XCTAssertEqual(hub.conversationSubtitle, "Offline")
        XCTAssertFalse(hub.conversationPeerOnline)
        XCTAssertTrue(hub.conversationStatusLine.hasPrefix("Offline"))
        XCTAssertEqual(hub.directRows.first { $0.conversation == conversation }?.online, false)

        try await harness.sendProtected([
            "type": "hello", "id": "hello-1", "senderId": "pat-laptop", "sender": "Pat", "ownerUser": "Pat",
            "ts": .int(Date().epochMillis),
        ])
        let online = await waitUntil { hub.conversationSubtitle == "Online" }
        XCTAssertTrue(online, "the DM names the account; any of its nodes on the relay makes it online")
        XCTAssertEqual(hub.peerPresence(conversation), .online)
        XCTAssertTrue(hub.conversationPeerOnline)
        XCTAssertFalse(hub.conversationStatusLine.contains("Online"))
        XCTAssertTrue(hub.conversationDetailLine.hasPrefix("Online"))
        XCTAssertEqual(hub.directRows.first { $0.conversation == conversation }?.online, true)
        XCTAssertEqual(hub.conversationPeerGroup?.account, "pat")
        await service.closeAll()
        await harness.close()
    }

    @MainActor func testAServerDirectMessageMatchesTheTrustedDeviceNode() async throws {
        let api = preparedApi()
        let ws = FakeWebSocketConnector()
        let service = makeService(api: api, ws: ws)
        await service.refresh()
        let harness = try await RelayHarness.start()
        let mesh = LocalMeshService(
            settings: harness.device.settings, identity: harness.device.identity, secureStore: harness.device.secure)
        let hub = ChatHub(relay: harness.relay, mesh: mesh)
        hub.direct = service
        let conversation = service.conversations[0]
        hub.select(conversation)
        _ = await service.open(conversation)
        let connection = try XCTUnwrap(ws.lastConnection)
        let crypto = await roomCrypto()
        let pat = try peerSeal("peer")
        _ = await teach(pat, from: connection, crypto)
        connection.receive(text: JSON.encode(try crypto.encrypt(pat.keyFrame())))
        let known = await waitUntil { service.deviceKeys(conversation) == [pat.keyId] }
        XCTAssertTrue(known)
        XCTAssertEqual(hub.conversationSubtitle, "Offline")
        try await harness.sendProtected([
            "type": "hello", "id": "hello-2", "senderId": "pat-node", "sender": "Patricia", "ts": .int(Date().epochMillis),
        ])
        let online = await waitUntil { hub.conversationSubtitle == "Online" }
        XCTAssertTrue(online, "a roster node with no owner still matches the device the DM trusts")
        await service.closeAll()
        await harness.close()
    }
}
