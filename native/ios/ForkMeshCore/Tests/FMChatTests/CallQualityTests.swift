import FMBase
import XCTest

@testable import FMChat

final class CallQualityTests: XCTestCase {
    func testBarsFollowTheWebThresholds() {
        XCTAssertEqual(CallQuality.bars(nil), 0)
        XCTAssertEqual(CallQuality.bars(CallQualitySample(jitterMs: 200)), 0, "no rtt and no loss is unknown")
        XCTAssertEqual(CallQuality.bars(CallQualitySample(rttMs: 48, lossPct: 0, jitterMs: 3)), 4)
        XCTAssertEqual(CallQuality.bars(CallQualitySample(rttMs: 150)), 3)
        XCTAssertEqual(CallQuality.bars(CallQualitySample(rttMs: 300)), 2)
        XCTAssertEqual(CallQuality.bars(CallQualitySample(rttMs: 500)), 1)
        XCTAssertEqual(CallQuality.bars(CallQualitySample(rttMs: 40, lossPct: 3)), 2)
        XCTAssertEqual(CallQuality.bars(CallQualitySample(lossPct: 8)), 1)
        XCTAssertEqual(CallQuality.bars(CallQualitySample(rttMs: 40, lossPct: 0, jitterMs: 60)), 2)
        XCTAssertEqual(CallQuality.bars(CallQualitySample(rttMs: 900, lossPct: 20, jitterMs: 300)), 1, "never below one bar")
        XCTAssertEqual(CallQuality.grade(CallQualitySample(rttMs: 48)), .good)
        XCTAssertEqual(CallQuality.grade(CallQualitySample(rttMs: 200)), .fair)
        XCTAssertEqual(CallQuality.grade(CallQualitySample(rttMs: 600)), .poor)
        XCTAssertEqual(CallQuality.grade(nil).label, "Measuring connection")
        XCTAssertEqual(CallQualityGrade.good.label, "Good connection")
    }

    func testLevelsAreDecibelMapped() {
        XCTAssertEqual(CallQuality.level(rms: 0), 0)
        XCTAssertEqual(CallQuality.level(rms: 1), 1)
        XCTAssertEqual(CallQuality.level(rms: 0.001), 0, accuracy: 1e-9)
        XCTAssertEqual(CallQuality.level(rms: 0.1), 2.0 / 3.0, accuracy: 1e-9)
        XCTAssertEqual(CallQuality.level(rms: 4), 1)
        XCTAssertEqual(CallQuality.level(decibels: -30), 0.5, accuracy: 1e-9)
        XCTAssertEqual(CallQuality.level(samples: [0.1, -0.1, 0.1, -0.1]), 2.0 / 3.0, accuracy: 1e-6)
        XCTAssertEqual(CallQuality.level(samples: []), 0)
    }

    func testDescriptionsMatchTheWebStrings() {
        let sample = CallQualitySample(
            rttMs: 47.6, lossPct: 0, jitterMs: 3.2, sendKbps: 900, recvKbps: 1800, inWidth: 1280, inHeight: 720,
            inFps: 30, outWidth: 640, outHeight: 360, outFps: 24, limit: "bandwidth")
        XCTAssertEqual(CallQuality.describeNetwork(sample), "48 ms · 0.0% loss · 3 ms jitter")
        XCTAssertEqual(CallQuality.describeNetwork(CallQualitySample(rttMs: 80, lossPct: 12.4)), "80 ms · 12% loss")
        XCTAssertEqual(CallQuality.describeNetwork(CallQualitySample(lossPct: 1)), "Measuring…")
        XCTAssertEqual(CallQuality.describeVideo(sample, .incoming), "1280×720 · 30 fps · 1.8 Mb/s")
        XCTAssertEqual(CallQuality.describeVideo(sample, .outgoing), "640×360 · 24 fps · 900 kb/s · limited by bandwidth")
        XCTAssertEqual(CallQuality.describeVideo(CallQualitySample(limit: "cpu"), .outgoing), "limited by CPU")
        XCTAssertEqual(CallQuality.describeVideo(CallQualitySample(limit: "other"), .outgoing), "limited")
        XCTAssertEqual(CallQuality.describeVideo(CallQualitySample(), .incoming), "")
        XCTAssertEqual(CallQuality.accessibilityLabel(sample, live: true), "Call quality: Good connection · 48 ms · 0.0% loss · 3 ms jitter")
        XCTAssertEqual(CallQuality.accessibilityLabel(nil, live: true), "Call quality: Measuring connection")
        XCTAssertEqual(CallQuality.accessibilityLabel(sample, live: false), "Call quality: Microphone level")
        XCTAssertEqual(CallQuality.pillText(sample, live: true), "48 ms")
        XCTAssertEqual(CallQuality.pillText(sample, live: false), "")
        XCTAssertEqual(CallQuality.voiceValue(hasPeer: true), "You ▲ · Them ▼")
        XCTAssertEqual(CallQuality.voiceValue(hasPeer: false), "You")
        XCTAssertNil(CallQuality.lossMark(0.5))
        XCTAssertEqual(CallQuality.lossMark(1), .fair)
        XCTAssertEqual(CallQuality.lossMark(8), .poor)
    }

    private func entry(_ id: String, _ type: String, _ values: [String: CallStatsValue]) -> CallStatsEntry {
        CallStatsEntry(id: id, type: type, values: values)
    }

    private func report(sent: Double, received: Double, lost: Double, packets: Double, decoded: Double) -> [CallStatsEntry] {
        [
            entry("T1", "transport", ["selectedCandidatePairId": .text("P1")]),
            entry("P1", "candidate-pair", ["state": .text("succeeded"), "currentRoundTripTime": .number(0.048)]),
            entry("P2", "candidate-pair", ["state": .text("succeeded"), "nominated": .flag(true), "currentRoundTripTime": .number(0.9)]),
            entry("OA", "outbound-rtp", ["kind": .text("audio"), "bytesSent": .number(sent / 4)]),
            entry("OV", "outbound-rtp", [
                "kind": .text("video"), "bytesSent": .number(sent * 3 / 4), "frameWidth": .number(640), "frameHeight": .number(360),
                "framesPerSecond": .number(24), "qualityLimitationReason": .text("bandwidth"),
            ]),
            entry("IA", "inbound-rtp", [
                "kind": .text("audio"), "bytesReceived": .number(received / 4), "packetsLost": .number(lost),
                "packetsReceived": .number(packets / 2), "jitter": .number(0.003), "audioLevel": .number(0.1),
            ]),
            entry("IV", "inbound-rtp", [
                "kind": .text("video"), "bytesReceived": .number(received * 3 / 4), "packetsLost": .number(0),
                "packetsReceived": .number(packets / 2), "frameWidth": .number(1280), "frameHeight": .number(720),
                "framesDecoded": .number(decoded),
            ]),
            entry("MS", "media-source", ["kind": .text("audio"), "audioLevel": .number(0.5)]),
        ]
    }

    func testSummarizerReadsAWebRTCReport() {
        let first = CallStatsSummary.summarize(
            [report(sent: 0, received: 0, lost: 0, packets: 0, decoded: 0)], previous: nil, at: 10)
        XCTAssertEqual(first.sample.rttMs ?? 0, 48, accuracy: 1e-9, "the transport's selected pair wins")
        XCTAssertNil(first.sample.lossPct)
        XCTAssertNil(first.sample.sendKbps)
        XCTAssertEqual(first.sample.jitterMs ?? 0, 3, accuracy: 1e-9)
        XCTAssertEqual(first.sample.outWidth, 640)
        XCTAssertEqual(first.sample.limit, "bandwidth")
        XCTAssertEqual(first.sample.micLevel, 0.5)
        XCTAssertEqual(first.sample.peerLevel, 0.1)
        let second = CallStatsSummary.summarize(
            [report(sent: 112_500, received: 225_000, lost: 2, packets: 98, decoded: 30)], previous: first.counters, at: 11)
        XCTAssertEqual(second.sample.sendKbps ?? 0, 900, accuracy: 1e-6)
        XCTAssertEqual(second.sample.recvKbps ?? 0, 1800, accuracy: 1e-6)
        XCTAssertEqual(second.sample.lossPct ?? 0, 2, accuracy: 1e-9)
        XCTAssertEqual(second.sample.inWidth, 1280)
        XCTAssertEqual(second.sample.inFps ?? 0, 30, accuracy: 1e-9, "fps from decoded frames when the report has none")
        XCTAssertEqual(CallQuality.describeVideo(second.sample, .incoming), "1280×720 · 30 fps · 1.8 Mb/s")
    }

    func testSummarizerFallsBackAndAggregatesAcrossReports() {
        let one = [
            entry("P", "candidate-pair", ["state": .text("succeeded"), "nominated": .flag(true), "currentRoundTripTime": .number(0.1)]),
            entry("I", "inbound-rtp", ["kind": .text("video"), "frameWidth": .number(320), "frameHeight": .number(240), "bytesReceived": .number(0)]),
        ]
        let two = [
            entry("R", "remote-inbound-rtp", ["kind": .text("audio"), "roundTripTime": .number(0.25), "fractionLost": .number(0.05)]),
            entry("I", "inbound-rtp", ["mediaType": .text("video"), "frameWidth": .number(640), "frameHeight": .number(480), "bytesReceived": .number(0)]),
        ]
        let start = CallStatsSummary.summarize([one, two], previous: nil, at: 0)
        XCTAssertEqual(start.sample.rttMs ?? 0, 250, accuracy: 1e-9, "worst RTT across connections")
        XCTAssertEqual(start.sample.lossPct ?? 0, 5, accuracy: 1e-9, "remote loss counts even with no previous sample")
        XCTAssertEqual(start.sample.inWidth, 640, "largest incoming video")
        let next = CallStatsSummary.summarize([one], previous: start.counters, at: 1)
        XCTAssertEqual(next.sample.lossPct, 0, "no packets and no losses while receiving is zero loss")
    }

    func testLevelsComeFromAudioLevelStats() {
        let levels = CallStatsSummary.levels([report(sent: 0, received: 0, lost: 0, packets: 0, decoded: 0)])
        XCTAssertEqual(levels.mic, CallQuality.level(rms: 0.5), accuracy: 1e-9)
        XCTAssertEqual(levels.peer ?? 0, CallQuality.level(rms: 0.1), accuracy: 1e-9)
        XCTAssertEqual(CallStatsSummary.levels([report(sent: 0, received: 0, lost: 0, packets: 0, decoded: 0)], muted: true).mic, 0)
        XCTAssertNil(CallStatsSummary.levels([[]]).peer)
    }

    func testPageEventParsesIntoTheHistory() {
        var message = JSONObject()
        message["event"] = .string("quality")
        message["live"] = .bool(true)
        message["grade"] = .string("good")
        message["bars"] = .int(4)
        message["rttMs"] = .int(48)
        message["lossPct"] = .double(0)
        message["jitterMs"] = .int(3)
        message["sendKbps"] = .null
        message["inWidth"] = .int(1280)
        message["inHeight"] = .int(720)
        message["limit"] = .string("")
        message["mic"] = .array([.double(0.2), .double(0.4), .double(1.5)])
        message["peer"] = .array([.null, .double(0.3)])
        let event = try? XCTUnwrap(CallQualityEvent(message, at: 5))
        XCTAssertEqual(event?.live, true)
        XCTAssertEqual(event?.sample.rttMs, 48)
        XCTAssertNil(event?.sample.sendKbps)
        XCTAssertEqual(event?.sample.inWidth, 1280)
        XCTAssertEqual(event?.levels, [
            CallVoiceLevel(mic: 0.2, peer: nil), CallVoiceLevel(mic: 0.4, peer: 0.3), CallVoiceLevel(mic: 1, peer: nil),
        ])
        message["event"] = .string("route")
        XCTAssertNil(CallQualityEvent(message))

        var history = CallQualityHistory()
        XCTAssertEqual(history.accessibilityLabel, "Call quality: Microphone level")
        if let event { history.apply(event) }
        XCTAssertTrue(history.live)
        XCTAssertEqual(history.grade, .good)
        XCTAssertEqual(history.bars, 4)
        XCTAssertEqual(history.pillText, "48 ms")
        XCTAssertEqual(history.levels.count, 0, "page levels play out one per tick")
        XCTAssertTrue(history.advance())
        XCTAssertTrue(history.advance())
        XCTAssertTrue(history.advance())
        XCTAssertFalse(history.advance())
        XCTAssertEqual(history.levels.count, 3)
        XCTAssertTrue(history.hasPeer)
    }

    func testHistoryKeepsBoundedRings() {
        var history = CallQualityHistory()
        for index in 0..<120 {
            history.push(level: CallVoiceLevel(mic: Double(index) / 120))
            history.push(sample: CallQualitySample(at: Double(index), rttMs: Double(index) * 5))
        }
        XCTAssertEqual(history.levels.count, CallQuality.levelSamples)
        XCTAssertEqual(history.samples.count, CallQuality.statsSamples)
        XCTAssertEqual(history.latest?.rttMs, 595)
        XCTAssertEqual(history.grade, .poor)
        XCTAssertEqual(history.heading, "Poor connection")
        let preview = CallQualityEvent(
            live: false, sample: CallQualitySample(), levels: Array(repeating: CallVoiceLevel(mic: 0.5), count: 30))
        history.apply(preview)
        XCTAssertFalse(history.live, "a preview event drops the call's stats")
        XCTAssertTrue(history.samples.isEmpty)
        XCTAssertEqual(history.grade, .unknown)
        XCTAssertEqual(history.heading, "Microphone")
        XCTAssertLessThanOrEqual(history.pending.count, CallQualityHistory.backlog, "a stalled playout catches up")
        history.reset()
        XCTAssertEqual(history, CallQualityHistory())
    }
}
