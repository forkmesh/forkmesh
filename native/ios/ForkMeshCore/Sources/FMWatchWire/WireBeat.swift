// One beat as it crosses the WatchConnectivity link: an FMAgentWatch
// `WatchBeat` (what the phone reads out of a transcript with
// `beatsFromTranscript`) in a compact, name-keyed JSON shape, so a watch
// running an older build still reads a newer phone's beats.

import FMAgentWatch
import Foundation

public struct WireBeat: Codable, Equatable, Hashable, Sendable {
    public var motion: AgentMotion
    /// What it was, in a few words: "Bash git status", "Edit main.dart".
    public var text: String
    /// Roughly how much text it carried (capped as `WatchBeat.bytes` is).
    public var bytes: Int
    /// The reply as one plain line, for a paragraph of prose; else empty.
    public var said: String

    public init(_ motion: AgentMotion, _ text: String, bytes: Int? = nil, said: String = "") {
        self.motion = motion
        self.text = text
        self.bytes = min(bytes ?? text.utf16.count, kPayloadCap)
        self.said = said
    }

    public init(_ beat: WatchBeat) {
        self.init(beat.motion, beat.text, bytes: beat.bytes, said: beat.said)
    }

    /// Back to the sonifier's own type, for the synth and the haptics.
    public var watchBeat: WatchBeat { WatchBeat(motion, text, bytes: bytes, said: said) }

    /// The desktop's `eventSeed`: the same command always sounds and looks the same.
    public var seed: Int { watchBeat.seed }

    /// The tap a watch plays for it, or nil for the faint kinds.
    public var haptic: WatchHapticKind? { WatchHapticKind.for(motion) }

    /// A motion by its wire name; one this build does not know reads as `tick`.
    public static func motion(named name: String) -> AgentMotion {
        AgentMotion.allCases.first { $0.name == name } ?? .tick
    }

    enum CodingKeys: String, CodingKey {
        case motion = "m", text = "t", bytes = "b", said = "s"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        motion = WireBeat.motion(named: try c.decodeIfPresent(String.self, forKey: .motion) ?? "")
        text = try c.decodeIfPresent(String.self, forKey: .text) ?? ""
        bytes = try c.decodeIfPresent(Int.self, forKey: .bytes) ?? 0
        said = try c.decodeIfPresent(String.self, forKey: .said) ?? ""
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(motion.name, forKey: .motion)
        try c.encode(text, forKey: .text)
        if bytes > 0 { try c.encode(bytes, forKey: .bytes) }
        if !said.isEmpty { try c.encode(said, forKey: .said) }
    }
}
