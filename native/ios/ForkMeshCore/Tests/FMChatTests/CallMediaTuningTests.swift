import XCTest

@testable import FMChat

final class CallMediaTuningTests: XCTestCase {
    func testOpusGetsTheWebPagesParametersInTheSameOrder() {
        let sdp = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111 63\r\na=rtpmap:111 opus/48000/2\r\n"
            + "a=fmtp:111 minptime=10;useinbandfec=0;x=a=b\r\na=rtpmap:63 red/48000/2\r\n"
        XCTAssertEqual(
            CallMediaTuning.tune(sdp),
            "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111 63\r\na=rtpmap:111 opus/48000/2\r\n"
                + "a=fmtp:111 minptime=10;useinbandfec=1;x=a=b;usedtx=0;stereo=0;maxplaybackrate=48000;"
                + "maxaveragebitrate=128000;sprop-maxcapturerate=48000\r\na=rtpmap:63 red/48000/2\r\n")
    }

    func testAMissingFmtpLineIsAddedAfterTheRtpmap() {
        let sdp = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 109\r\na=rtpmap:109 OPUS/48000/2\r\na=rtcp-fb:109 nack\r\n"
        XCTAssertEqual(
            CallMediaTuning.tune(sdp),
            "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 109\r\na=rtpmap:109 OPUS/48000/2\r\n"
                + "a=fmtp:109 useinbandfec=1;usedtx=0;stereo=0;maxplaybackrate=48000;maxaveragebitrate=128000;"
                + "sprop-maxcapturerate=48000\r\n"
                + "a=rtcp-fb:109 nack\r\n")
    }

    func testAnSdpWithoutOpusIsUntouched() {
        let sdp = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 VP8/90000\r\n"
        XCTAssertEqual(CallMediaTuning.tune(sdp), sdp)
        XCTAssertEqual(CallMediaTuning.tune(""), "")
    }

    func testAudioGetsTheWebPagesBitratePrioritiesAndRedFirst() {
        XCTAssertEqual(CallMediaTuning.audioMaxBitrate, 128_000)
        XCTAssertEqual(CallMediaTuning.audioPriority, "high")
        XCTAssertEqual(CallMediaTuning.videoPriority, "medium")
        XCTAssertEqual(
            CallMediaTuning.orderedAudioCodecs(["audio/opus", "audio/G722", "audio/red", "audio/PCMU"]),
            ["audio/red", "audio/opus", "audio/G722", "audio/PCMU"])
        XCTAssertNil(CallMediaTuning.orderedAudioCodecs(["audio/opus", "audio/PCMU"]))
    }
}
