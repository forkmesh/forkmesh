// What one beat feels like in the hand. The phone plays these through Core
// Haptics; the watch has no Core Haptics and maps a pattern to one
// WKHapticType with `WatchHapticKind.for(_:)` (docs/native/PARITY.md, section 4.3).

import FMBase

/// One touch of the motor: when, how long, how hard and how crisp.
///
/// Sharpness is Core Haptics' own axis (a dull thud at 0, a bright tick at 1).
public struct Pulse: Equatable, Sendable {
    /// Milliseconds after the pattern starts.
    public let atMs: Int
    /// How long it lasts, in milliseconds.
    public let ms: Int
    /// 0..1 (clamp before handing to a motor; see `clamped`).
    public let intensity: Double
    /// 0..1.
    public let sharpness: Double

    public init(_ atMs: Int, _ ms: Int, _ intensity: Double, _ sharpness: Double = 0.5) {
        self.atMs = atMs
        self.ms = ms
        self.intensity = intensity
        self.sharpness = sharpness
    }

    /// Intensity and sharpness held to 0..1.
    public var clamped: Pulse {
        Pulse(atMs, ms, min(max(intensity, 0), 1), min(max(sharpness, 0), 1))
    }
}

/// What one beat feels like in the hand. Every kind of work has its own
/// rhythm (a command is one knock, a search a quick flurry, a build a rumble,
/// an error a long buzz) and the agent doing it shades it: its instrument sets
/// how crisp the taps are, its register how hard.
///
/// Most patterns are over within a quarter of a second, so the next beat cuts
/// in rather than queues; only a run ending, well or badly, is let run longer.
public func feelFor(_ motion: AgentMotion, _ voice: Voice) -> [Pulse] {
    let s = voice.sharpness
    // Low voices press harder, high ones lighter: the hand hears register too.
    let weight = 1.0 - Double(voice.octave) * 0.12
    func p(_ at: Int, _ ms: Int, _ intensity: Double, _ sharpness: Double? = nil) -> Pulse {
        Pulse(at, ms, intensity * weight, sharpness ?? s)
    }
    switch motion {
    case .speak: return [p(0, 10, 0.3)]
    case .think: return [p(0, 160, 0.14, 0.1)]
    case .read, .scan: return [p(0, 8, 0.35, 0.9)]
    case .search: return [p(0, 8, 0.3, 0.9), p(55, 8, 0.35, 0.9), p(110, 8, 0.4, 0.9)]
    case .web, .mcp: return [p(0, 8, 0.4, 0.9), p(40, 60, 0.15, 0.3)]
    case .write: return [p(0, 18, 0.7), p(120, 12, 0.3)]
    case .create: return [p(0, 12, 0.35), p(60, 20, 0.75)]
    case .run: return [p(0, 14, 0.65, 0.95)]
    case .delegate, .spawn: return [p(0, 10, 0.3), p(70, 10, 0.5), p(140, 14, 0.75)]
    case .plan, .todo: return [p(0, 8, 0.35), p(90, 8, 0.35)]
    case .arrive: return [p(0, 8, 0.22, 0.4)]
    case .prompt: return [p(0, 24, 0.9)]
    case .notice: return [p(0, 40, 0.55, 0.7), p(90, 40, 0.55, 0.7)]
    case .finish: return [p(0, 12, 0.4), p(160, 12, 0.6), p(320, 40, 1.0)]
    case .test: return [p(0, 8, 0.4, 0.9), p(55, 8, 0.4, 0.9), p(110, 8, 0.4, 0.9), p(165, 8, 0.4, 0.9)]
    case .build, .install: return [p(0, 140, 0.6, 0.05)]
    case .git: return [p(0, 14, 0.6, 0.6), p(90, 14, 0.4, 0.3)]
    case .wait: return [p(0, 220, 0.12, 0.05)]
    case .ask: return [p(0, 14, 0.4), p(140, 20, 0.85)]
    case .skill: return [p(0, 10, 0.55, 0.8), p(50, 30, 0.2, 0.2)]
    case .fumble: return [p(0, 180, 0.8, 0.3)]
    case .fail: return [p(0, 120, 0.9, 0.3), p(200, 260, 1.0, 0.1)]
    case .start: return [p(0, 20, 0.7), p(120, 12, 0.35)]
    case .denied: return [p(0, 60, 0.9, 0.4), p(110, 120, 0.8, 0.2)]
    case .image: return [p(0, 10, 0.35, 0.8), p(60, 10, 0.25, 0.8)]
    case .compact: return [p(0, 260, 0.2, 0.05)]
    case .retry: return [p(0, 8, 0.35, 0.9), p(60, 8, 0.35, 0.9)]
    // The log's bookkeeping, the network and the desktop's own cues are the
    // quietest things on the field: a tick, if anything.
    case .frame, .compose, .usage, .system, .tick, .close, .seal, .hook, .network, .connect, .task, .logStall,
        .logSync, .logGit, .logCloud, .logTask, .logAccount, .logInfo:
        return [p(0, 6, 0.15)]
    case .logError: return [p(0, 40, 0.5, 0.3)]
    }
}

/// The one haptic a watch plays for a beat (watchOS has no Core Haptics; its
/// `WKInterfaceDevice.play(_:)` takes a fixed set). The watch app maps each
/// case to a `WKHapticType` and rate-limits to one every 0.6 s.
public enum WatchHapticKind: String, Sendable, CaseIterable {
    /// A tool call, a command, anything small: `.click`.
    case click
    /// A file written or created: `.directionUp`.
    case directionUp
    /// An error, a refusal, a failed run: `.failure`.
    case failure
    /// A run finished well: `.success`.
    case success
    /// The agent needs you (a question, a prompt, a notice): `.notification`.
    case notification

    /// The haptic for a kind of work, or nil for the faint ones the watch
    /// does not tap for.
    public static func `for`(_ motion: AgentMotion) -> WatchHapticKind? {
        switch motion {
        case .fumble, .denied, .fail, .logError: return .failure
        case .finish: return .success
        case .ask, .prompt, .notice: return .notification
        case .write, .create: return .directionUp
        default: return motion.isFaint || motion.isCue ? nil : .click
        }
    }
}
