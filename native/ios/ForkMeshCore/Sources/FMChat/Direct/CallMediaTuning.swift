import Foundation

public enum CallMediaTuning {
    public static let videoMaxBitrate = 2_500_000
    public static let audioMaxBitrate = 96_000
    public static let videoWidth = 1280
    public static let videoHeight = 720
    public static let videoFrameRate = 30
    public static let groupVideoMaxBitrate = 1_200_000
    public static let groupVideoWidth = 640
    public static let groupVideoHeight = 360
    public static let groupVideoFrameRate = 24
    public static let opusParameters: [(String, String)] = [
        ("useinbandfec", "1"),
        ("usedtx", "0"),
        ("stereo", "0"),
        ("maxplaybackrate", "48000"),
        ("maxaveragebitrate", String(audioMaxBitrate)),
    ]

    public static func tune(_ sdp: String) -> String {
        var lines = sdp.components(separatedBy: "\r\n")
        guard let rtpmapIndex = lines.firstIndex(where: isOpusRtpmap) else { return sdp }
        let rtpmap = lines[rtpmapIndex]
        let payload = rtpmap.dropFirst("a=rtpmap:".count).split(separator: " ").first.map(String.init) ?? ""
        let prefix = "a=fmtp:\(payload) "
        let at = lines.firstIndex { $0.hasPrefix(prefix) }
        var keys: [String] = []
        var values: [String: String] = [:]
        if let at {
            for part in lines[at].dropFirst(prefix.count).split(separator: ";", omittingEmptySubsequences: false) {
                let pieces = part.trimmingCharacters(in: .whitespaces).split(separator: "=", omittingEmptySubsequences: false)
                guard let key = pieces.first.map(String.init), !key.isEmpty else { continue }
                if values[key] == nil { keys.append(key) }
                values[key] = pieces.dropFirst().joined(separator: "=")
            }
        }
        for (key, value) in opusParameters {
            if values[key] == nil { keys.append(key) }
            values[key] = value
        }
        let line = prefix + keys.map { "\($0)=\(values[$0] ?? "")" }.joined(separator: ";")
        if let at {
            lines[at] = line
        } else {
            lines.insert(line, at: rtpmapIndex + 1)
        }
        return lines.joined(separator: "\r\n")
    }

    static func isOpusRtpmap(_ line: String) -> Bool {
        guard line.hasPrefix("a=rtpmap:") else { return false }
        let rest = line.dropFirst("a=rtpmap:".count)
        let digits = rest.prefix { $0.isASCII && $0.isNumber }
        guard !digits.isEmpty else { return false }
        return rest.dropFirst(digits.count).lowercased().hasPrefix(" opus/48000")
    }
}
