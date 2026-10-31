import FMBase
import XCTest

@testable import FMChat

final class GroupCallFmmTests: XCTestCase {
    static let peer = String(repeating: "c", count: 32)

    private func block(standby: Bool = true) -> JSONObject {
        var fmm: JSONObject = [
            "engine": "fmm", "url": "https://n2.next.forkmesh.com:4433/calls/fmm", "addr": "n2.next.forkmesh.com:4433",
            "alpn": "fmm/1", "hashes": .array(["aGFzaDE=", "aGFzaDI="]), "ticket": "t.primary",
        ]
        if standby {
            fmm["standby"] = .object([
                "url": "https://n3.next.forkmesh.com:4433/calls/fmm", "addr": "n3.next.forkmesh.com:4433", "alpn": "fmm/1",
                "hashes": .array(["aGFzaDM="]), "ticket": "t.standby",
            ])
        }
        return fmm
    }

    func testTheTicketResponseCarriesTheFmmBlockForTheEngine() throws {
        let json: JSONObject = [
            "ticket": "w.ticket", "url": "wss://n2.next.forkmesh.com/calls/ws", "peer": .string(Self.peer), "room": "r",
            "fmm": .object(block()),
        ]
        let ticket = try XCTUnwrap(GroupCallTicket(json: json))
        let fmm = try XCTUnwrap(ticket.fmm)
        XCTAssertEqual(fmm.addr, "n2.next.forkmesh.com:4433")
        XCTAssertTrue(fmm.hasStandby)
        let parsed = JSON.parseObject(fmm.json)
        XCTAssertEqual(parsed["ticket"]?.text, "t.primary")
        XCTAssertEqual(parsed["standby"]?.objectValue?["ticket"]?.text, "t.standby")
        XCTAssertNil(GroupCallTicket(json: ["ticket": "w", "url": "wss://x/ws", "peer": .string(Self.peer)])?.fmm)
    }

    func testABadFmmBlockOrStandbyIsDropped() {
        var wrong = block()
        wrong["alpn"] = "h3"
        XCTAssertNil(GroupCallFmmAccess(json: .object(wrong)))
        var webrtc = block()
        webrtc["engine"] = "webrtc"
        XCTAssertNil(GroupCallFmmAccess(json: .object(webrtc)))
        var noHashes = block()
        noHashes["hashes"] = .array([])
        XCTAssertNil(GroupCallFmmAccess(json: .object(noHashes)))
        var badStandby = block()
        badStandby["standby"] = .object(["addr": "n3:4433"])
        let access = GroupCallFmmAccess(json: .object(badStandby))
        XCTAssertEqual(access?.hasStandby, false)
        XCTAssertNil(JSON.parseObject(access?.json ?? "{}")["standby"])
        XCTAssertEqual(GroupCallFmmAccess(json: .object(block(standby: false)))?.hasStandby, false)
    }

    func testE2eeMessagesRideTheChatFramesWithTheSealedSender() throws {
        let presence = Data(#"{"v":2,"call":"room","peer":"p","epoch":1}"#.utf8)
        let fields = try XCTUnwrap(GroupCallE2ee.fields(.presence, peer: Self.peer, message: presence, media: "video"))
        XCTAssertEqual(fields["call"]?.text, "in")
        XCTAssertEqual(fields["media"]?.text, "video")
        XCTAssertEqual(fields["e2ee"]?.objectValue?["epoch"], .int(1))
        var plain = fields
        plain["type"] = .string(GroupCallE2ee.type(.presence))
        plain["sealKid"] = "forged-by-sender"
        XCTAssertNil(GroupCallE2ee.message(GroupCallE2ee.annotate(plain, kid: nil, acct: nil)))
        let annotated = GroupCallE2ee.annotate(plain, kid: "KID0123456789abc", acct: "ana")
        let message = try XCTUnwrap(GroupCallE2ee.message(annotated))
        XCTAssertEqual(message.kind, .presence)
        XCTAssertEqual(message.senderKid, "KID0123456789abc")
        XCTAssertEqual(message.senderAcct, "ana")
        XCTAssertEqual(JSON.parseObject(message.data)["call"]?.text, "room")
        var verify = try XCTUnwrap(GroupCallE2ee.fields(.verify, peer: Self.peer, message: Data(#"{"t":"commit"}"#.utf8)))
        verify["type"] = .string(GroupCallFrames.verifyType)
        XCTAssertEqual(GroupCallE2ee.message(GroupCallE2ee.annotate(verify, kid: "k", acct: nil))?.kind, .verify)
        XCTAssertNil(GroupCallE2ee.fields(.key, peer: Self.peer, message: Data("not json".utf8)))
        var chat: JSONObject = ["type": "chat", "sealKid": "x"]
        chat = GroupCallE2ee.annotate(chat, kid: "k", acct: "a")
        XCTAssertNil(chat["sealKid"])
    }

    @MainActor func testRosterIgnoresVerifyAndE2eeKeyFrames() {
        let roster = GroupCallRoster(now: { 1000 })
        let verify: JSONObject = ["type": .string(GroupCallFrames.verifyType), "peer": .string(Self.peer), "sender": "ana"]
        XCTAssertNil(roster.handle(verify, conversation: "#team", selfPeer: ""))
        let key: JSONObject = ["type": .string(GroupCallFrames.keyType), "peer": .string(Self.peer), "sender": "ana", "e2ee": .object([:])]
        XCTAssertNil(roster.handle(key, conversation: "#team", selfPeer: ""))
        XCTAssertTrue(GroupCallFrames.types.contains(GroupCallFrames.verifyType))
    }

    func testVerificationTextMatchesTheDesign() {
        XCTAssertEqual(GroupCallVerifyText.waiting(["Bob", "Carol"]), "Waiting for Bob and Carol to confirm…")
        XCTAssertEqual(GroupCallVerifyText.list(["Bob", "Carol", "Dave"]), "Bob, Carol and Dave")
        XCTAssertEqual(GroupCallVerifyText.numbers("123 456"), "Or compare numbers: 123 456")
        XCTAssertEqual(GroupCallVerifyText.changed("Erin"), "The verification code changed because Erin joined")
        XCTAssertEqual(GroupCallVerifyText.newcomer("Erin"), "Erin is not verified. Compare the new code to verify everyone.")
        XCTAssertEqual(GroupCallVerifyText.tileNote(.missing, name: "Bob"), "Can't confirm this is Bob right now")
        XCTAssertEqual(
            GroupCallVerifyText.tileNote(.forged, name: "Bob"), "Bob's video failed a check. Someone in this call may be faking it.")
        XCTAssertNil(GroupCallVerifyText.tileNote(.verified, name: "Bob"))
        XCTAssertTrue(GroupCallVerifyText.interfered(unfinished: 3, rounds: 4))
        XCTAssertTrue(GroupCallVerifyText.interfered(unfinished: 0, rounds: 33))
        XCTAssertFalse(GroupCallVerifyText.interfered(unfinished: 2, rounds: 32))
    }

    func testAudioPlanRaisesTheBitrateForDredOnlyWhenLossIsExpected() {
        let clean = GroupCallAudioPlan.make(engineBitrate: 24000, targetBitrate: 600_000, redundancyMs: 200, loss: 0)
        XCTAssertEqual(clean, GroupCallAudioPlan(bitrate: 24000, fec: false, lossPercent: 0, dredFrames: 0, depthMs: 0))
        let lossy = GroupCallAudioPlan.make(engineBitrate: 24000, targetBitrate: 600_000, redundancyMs: 800, loss: 0.12)
        XCTAssertEqual(lossy.bitrate, 48000)
        XCTAssertTrue(lossy.fec)
        XCTAssertEqual(lossy.lossPercent, 12)
        XCTAssertEqual(lossy.dredFrames, 80)
        XCTAssertEqual(lossy.depthMs, 400)
        let thin = GroupCallAudioPlan.make(engineBitrate: 16000, targetBitrate: 60000, redundancyMs: 500, loss: 0.3)
        XCTAssertEqual(thin.bitrate, 16000)
        XCTAssertEqual(thin.depthMs, 0)
        XCTAssertEqual(thin.lossPercent, 30)
    }

    func testJitterInstructionsMapToOpusDecodeSteps() {
        let d = GroupCallAudioDecode.self
        XCTAssertEqual(d.step(status: d.waiting, offset: 0, hasData: false, hasLBRR: false, dredSamples: 0), .silence)
        XCTAssertEqual(d.step(status: d.play, offset: 0, hasData: true, hasLBRR: false, dredSamples: 0), .play)
        XCTAssertEqual(d.step(status: d.play, offset: 0, hasData: false, hasLBRR: false, dredSamples: 0), .conceal)
        XCTAssertEqual(d.step(status: d.dred, offset: 3, hasData: true, hasLBRR: true, dredSamples: 19200), .dred(samples: 2880))
        XCTAssertEqual(d.step(status: d.dred, offset: 1, hasData: true, hasLBRR: true, dredSamples: 19200), .fec)
        XCTAssertEqual(d.step(status: d.dred, offset: 5, hasData: true, hasLBRR: false, dredSamples: 960), .conceal)
        XCTAssertEqual(d.step(status: d.conceal, offset: 0, hasData: true, hasLBRR: true, dredSamples: 0), .fec)
        XCTAssertEqual(d.step(status: d.conceal, offset: 0, hasData: true, hasLBRR: false, dredSamples: 0), .conceal)
        XCTAssertEqual(d.step(status: d.insert, offset: 0, hasData: false, hasLBRR: false, dredSamples: 0), .conceal)
    }

    func testLongTermReferencesMapFramesToEncoderTokens() {
        var refs = GroupCallLongTermRefs()
        for frame in Int64(0)..<40 { refs.note(frame: frame * 2, token: 1000 + frame) }
        XCTAssertNil(refs.token(for: 2))
        XCTAssertEqual(refs.token(for: 78), 1039)
        XCTAssertEqual(refs.acknowledged(upTo: 30), [1012, 1013, 1014, 1015])
        XCTAssertEqual(refs.acknowledged(upTo: -1), [])
        refs.reset()
        XCTAssertTrue(refs.isEmpty)
    }

    func testH264ConvertsBetweenLengthPrefixedAndAnnexB() {
        let sps: [UInt8] = [0x67, 1, 2, 3]
        let pps: [UInt8] = [0x68, 4]
        let idr: [UInt8] = [0x65, 9, 0, 0, 7]
        let slice: [UInt8] = [0x41, 8]
        let avcc = H264Bitstream.avcc([idr[...], slice[...]])
        XCTAssertEqual(avcc, [0, 0, 0, 5] + idr + [0, 0, 0, 2] + slice)
        let annexB = H264Bitstream.annexB(avcc: avcc, parameterSets: [sps, pps])
        XCTAssertEqual(annexB, [0, 0, 0, 1] + sps + [0, 0, 0, 1] + pps + [0, 0, 0, 1] + idr + [0, 0, 0, 1] + slice)
        let units = H264Bitstream.units(annexB: annexB)
        XCTAssertEqual(units.map { Array($0) }, [sps, pps, idr, slice])
        XCTAssertEqual(units.map(H264Bitstream.type), [7, 8, 5, 1])
        let short = H264Bitstream.units(annexB: [0, 0, 1] + slice + [0, 0, 1] + pps)
        XCTAssertEqual(short.map { Array($0) }, [slice, pps])
        XCTAssertEqual(H264Bitstream.annexB(avcc: [0, 0, 0, 9, 1]), [])
    }

    func testVerifyStateSplitsTheEngineFieldsAndPicksTheLine() {
        let names = ["p1": "Bob", "p2": "Carol", "p3": "Erin"]
        let name: (String) -> String = { names[$0] ?? "Someone" }
        let shown = GroupCallVerifyState(
            round: "r", hasCode: true, emoji: "\u{1F436} \u{1F431} \u{1F981} \u{1F40E}", names: "Dog, Cat, Lion, Horse",
            digits: "123 456", waiting: "", late: false, verified: false, newcomers: "", unfinished: 0, rounds: 1)
        XCTAssertEqual(shown.emoji.count, 4)
        XCTAssertEqual(shown.names, ["Dog", "Cat", "Lion", "Horse"])
        XCTAssertEqual(shown.summary(name: name), GroupCallVerifyText.title)
        let waiting = GroupCallVerifyState(
            round: "r", hasCode: false, emoji: "", names: "", digits: "", waiting: "p1\np2", late: true, verified: false,
            newcomers: "", unfinished: 0, rounds: 1)
        XCTAssertEqual(waiting.summary(name: name), "Waiting for Bob and Carol to confirm\u{2026}")
        let newcomer = GroupCallVerifyState(
            round: "r", hasCode: true, emoji: "", names: "", digits: "", waiting: "", late: false, verified: false,
            newcomers: "p3", unfinished: 0, rounds: 2)
        XCTAssertEqual(newcomer.summary(name: name), GroupCallVerifyText.newcomer("Erin"))
        let attacked = GroupCallVerifyState(
            round: "r", hasCode: false, emoji: "", names: "", digits: "", waiting: "", late: false, verified: true,
            newcomers: "", unfinished: 3, rounds: 4)
        XCTAssertEqual(attacked.summary(name: name), GroupCallVerifyText.interference)
    }
}
