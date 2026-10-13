import FMLocalAI
import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieHandIntent: Equatable, Sendable {
    case grab
    case carry(CGPoint)
    case drop(fling: CGSize?)
    case trick(String, cause: String)
    case improvise(String)
    case crawl(to: CGPoint)
    case recoil(from: CGPoint)
    case manipulate(GenieManipulation)
    case settle(GenieManipulation)
    case rate(String, Double)
}

public struct GenieHandScene: Equatable, Sendable {
    public var spider: CGPoint
    public var reach: Double
    public var busy: Bool

    public init(spider: CGPoint, reach: Double, busy: Bool = false) {
        self.spider = spider
        self.reach = reach
        self.busy = busy
    }
}

public struct GenieHandPlay: Sendable {
    public nonisolated static let flingSpeed: Double = 1.4
    public nonisolated static let petAfter: Double = 1.2
    public nonisolated static let petGap: Double = 5
    public nonisolated static let idleAfter: Double = 7
    public nonisolated static let idleGap: Double = 14
    public nonisolated static let repeatLimit: Int = 3
    public nonisolated static let repeatWindow: Double = 30
    public nonisolated static let feedbackWindow: Double = 5

    public nonisolated static let pools: [String: [String]] = [
        "appeared": ["wave", "gasp", "crown", "peekaboo"],
        "returned": ["peekaboo", "gasp", "party"],
        "boop": ["giggle", "gasp", "sneeze", "jump", "hiccups"],
        "boop_hard": ["faint", "dizzy", "spooked", "play_dead"],
        "push": ["gasp", "spooked", "hide", "shrink"],
        "pull": ["follow", "blow_kiss", "zombie", "fetch"],
        "swipe_left": ["zoomies", "moonwalk", "cartwheel", "ninja"],
        "swipe_right": ["zoomies", "moonwalk", "cartwheel", "teleport"],
        "swipe_up": ["jump", "blast_off", "backflip", "trampoline", "levitate", "fly"],
        "swipe_down": ["bow", "limbo", "curtsy", "sulk", "abseil", "nap"],
        "wave": ["wave", "blow_kiss", "crown", "salute", "high_five"],
        "shape_open": ["freeze", "high_five", "levitate", "meditate"],
        "shape_fist": ["fist_bump", "karate", "flex", "pushups"],
        "shape_point": ["fetch", "ponder", "follow"],
        "shape_peace": ["peace", "selfie", "meditate", "disco"],
        "shape_three": ["juggle", "conga", "tap_dance"],
        "shape_four": ["drum_solo", "chicken_dance", "pirouette"],
        "shape_thumbsUp": ["dance", "flex", "on_fire", "glow_up", "party"],
        "shape_thumbsDown": ["sulk", "cry", "shrug", "tantrum", "no_no"],
        "shape_rock": ["headbang", "air_guitar", "beatbox", "breakdance"],
        "shape_callMe": ["phone_call", "whistle", "sing"],
        "shape_ok": ["nod", "wink", "sparkle", "applause"],
        "shape_gun": ["play_dead", "faint", "ninja", "spooked"],
        "circle": ["spin", "pirouette", "chase_tail", "run_circles", "dizzy", "breakdance"],
        "wiggle_near": ["giggle", "belly_laugh", "worm", "roll_over"],
        "wiggle": ["magic", "grant_wish", "rainbow", "catch_fly", "spin_web"],
        "clap": ["applause", "party", "fireworks", "dance", "conga"],
        "pet": ["purr", "love", "blush", "hug", "roll_over", "flowers"],
        "fling": ["dizzy", "web_swing", "trampoline"],
        "twirl": ["dizzy", "spin"],
        "big": ["grow", "flex", "on_fire"],
        "small": ["shrink", "hiccups", "giggle"],
    ]

    public nonisolated static let liked: Set<String> = ["shape_thumbsUp", "shape_ok", "clap", "pet"]
    public nonisolated static let disliked: Set<String> = ["shape_thumbsDown"]

    public private(set) var grabbed = false
    public private(set) var recent: [String] = []
    var turns: [String: Int] = [:]
    var uses: [String: [Date]] = [:]
    var lastEvent: Date = .distantPast
    var lastTrickAt: Date = .distantPast
    var lastIdle: Date = .distantPast
    var petSince: Date?
    var lastPet: Date = .distantPast
    var lastAppeared: Date = .distantPast
    var handsSince: Date?

    public init() {}

    func near(_ point: CGPoint?, _ scene: GenieHandScene, by factor: Double = 1) -> Bool {
        guard let point else { return false }
        return GenieHandPose.gap(point, scene.spider) <= scene.reach * factor
    }

    public mutating func played(_ id: String, at now: Date) {
        recent = Array((recent + [id]).suffix(12))
        lastTrickAt = now
    }

    mutating func pick(_ pool: String, cause: String, at now: Date) -> GenieHandIntent? {
        guard let options = GenieHandPlay.pools[pool], !options.isEmpty else { return nil }
        let used = (uses[pool] ?? []).filter { now.timeIntervalSince($0) < GenieHandPlay.repeatWindow } + [now]
        uses[pool] = used
        if used.count == GenieHandPlay.repeatLimit {
            return .improvise("They have done the \(cause) gesture \(used.count) times in a row; surprise them with something new that still fits \(cause).")
        }
        let turn = turns[pool] ?? 0
        turns[pool] = turn + 1
        var choice = options[turn % options.count]
        if let last = recent.last, choice == last, options.count > 1 { choice = options[(turn + 1) % options.count] }
        return .trick(choice, cause: cause)
    }

    public mutating func react(_ event: GenieHandEvent, state: GenieHandState, scene: GenieHandScene, at now: Date) -> [GenieHandIntent] {
        lastEvent = now
        var out: [GenieHandIntent] = []
        if let last = recent.last, now.timeIntervalSince(lastTrickAt) <= GenieHandPlay.feedbackWindow {
            if GenieHandPlay.liked.contains(event.key) { out.append(.rate(last, 1)) }
            if GenieHandPlay.disliked.contains(event.key) { out.append(.rate(last, -1)) }
        }
        func add(_ pool: String, _ cause: String) {
            if let intent = pick(pool, cause: cause, at: now) { out.append(intent) }
        }
        switch event {
        case .appeared:
            if now.timeIntervalSince(lastAppeared) > 20, !scene.busy { add("appeared", "hello") }
            lastAppeared = now
        case .vanished:
            petSince = nil
        case .returned:
            add("returned", "peekaboo")
        case .pinchBegan(let point):
            if near(point, scene, by: 1.25) {
                grabbed = true
                out.append(.grab)
            } else {
                out.append(.crawl(to: point))
            }
        case .pinchEnded(_, let velocity):
            guard grabbed else { break }
            grabbed = false
            let speed = Double(hypot(velocity.width, velocity.height))
            if speed >= GenieHandPlay.flingSpeed {
                out.append(.drop(fling: velocity))
                add("fling", "throw")
            } else {
                out.append(.drop(fling: nil))
            }
        case .push(let amount):
            if near(state.cursor, scene, by: 1.8) {
                out.append(.recoil(from: state.cursor ?? scene.spider))
                add(amount >= 0.16 ? "boop_hard" : "boop", "boop")
            } else {
                add("push", "push toward the screen")
            }
        case .pull:
            add("pull", "pull away")
        case .swipe(let way):
            add("swipe_\(way.rawValue)", "swipe \(way.rawValue)")
        case .wave:
            add("wave", "wave")
        case .shape(let shape):
            if shape == .point, let cursor = state.cursor, !near(cursor, scene, by: 1.4) {
                out.append(.crawl(to: cursor))
            }
            add("shape_\(shape.rawValue)", GenieHandPlay.describe(shape))
        case .circle:
            add("circle", "draw a circle")
        case .wiggle:
            if near(state.cursor, scene, by: 1.6) {
                add("wiggle_near", "tickle")
            } else {
                add("wiggle", "wiggle fingers like magic")
            }
        case .clap:
            add("clap", "clap")
        case .twoHandsBegan:
            if grabbed {
                grabbed = false
                out.append(.drop(fling: nil))
            }
        case .twoHandsEnded(let done):
            out.append(.settle(done))
            if abs(done.rotation) >= 150 {
                add("twirl", "twirl")
            } else if done.scale >= 1.5 {
                add("big", "stretch bigger")
            } else if done.scale <= 0.66 {
                add("small", "squeeze smaller")
            }
        }
        return out
    }

    public mutating func tick(state: GenieHandState, scene: GenieHandScene, at now: Date) -> [GenieHandIntent] {
        var out: [GenieHandIntent] = []
        if let manipulation = state.manipulation {
            out.append(.manipulate(manipulation))
            return out
        }
        if grabbed, state.pinching, let cursor = state.cursor {
            out.append(.carry(cursor))
            return out
        }
        guard state.hands > 0, !scene.busy else {
            petSince = nil
            if state.hands == 0 { handsSince = nil }
            return out
        }
        let present = handsSince ?? now
        handsSince = present
        let hovering = near(state.cursor, scene, by: 1.3) && !state.pinching && state.speed < 0.45
            && (state.shape == .open || state.shape == .four)
        if hovering {
            let since = petSince ?? now
            petSince = since
            if now.timeIntervalSince(since) >= GenieHandPlay.petAfter, now.timeIntervalSince(lastPet) >= GenieHandPlay.petGap {
                lastPet = now
                lastEvent = now
                petSince = nil
                if let last = recent.last, now.timeIntervalSince(lastTrickAt) <= GenieHandPlay.feedbackWindow {
                    out.append(.rate(last, 1))
                }
                if let intent = pick("pet", cause: "pet", at: now) { out.append(intent) }
                return out
            }
        } else {
            petSince = nil
        }
        let quiet = min(now.timeIntervalSince(lastEvent), now.timeIntervalSince(lastTrickAt), now.timeIntervalSince(present))
        if quiet >= GenieHandPlay.idleAfter, now.timeIntervalSince(lastIdle) >= GenieHandPlay.idleGap, !state.pinching {
            lastIdle = now
            let near = Int((state.nearness * 100).rounded())
            let shape = state.shape.map { GenieHandPlay.describe($0) } ?? "a hand"
            out.append(.improvise("Their hand is held up as \(shape), \(near)% of the way to the screen, and nothing has happened for a while."))
        }
        return out
    }

    public static func describe(_ shape: GenieHandShape) -> String {
        switch shape {
        case .open: return "an open palm"
        case .fist: return "a fist"
        case .point: return "a pointing finger"
        case .peace: return "a peace sign"
        case .three: return "three fingers"
        case .four: return "four fingers"
        case .thumbsUp: return "a thumbs up"
        case .thumbsDown: return "a thumbs down"
        case .rock: return "rock-on horns"
        case .callMe: return "a call-me sign"
        case .ok: return "an OK sign"
        case .gun: return "a finger gun"
        case .pinch: return "a pinch"
        case .other: return "a hand"
        }
    }
}

public struct GenieImprovisation: Equatable, Sendable {
    public var trick: GenieTrick
    public var line: String
    public var byModel: Bool

    public init(trick: GenieTrick, line: String, byModel: Bool) {
        self.trick = trick
        self.line = line
        self.byModel = byModel
    }
}

public enum GenieImproviser {
    public nonisolated static let maxLine = 70
    public nonisolated static let timeout: Double = 4

    public static func messages(scene: String, recent: [String], taste: GenieTrickTaste, vitals: GenieVitals?) -> [LocalChatMessage] {
        let ids = GenieTricks.all.map(\.id).joined(separator: ", ")
        var system = """
            You are the ForkMesh Genie: a playful spider in a violet turban who lives on the user's phone screen. \
            The user is playing with you by moving their hands in front of the front camera. Choose one trick that \
            fits the moment and is fun, and write a short line (at most eight words) you say while doing it. \
            Reply with only the trick id, a colon and the line, like: backflip: Wheee, watch this!
            Trick ids: \(ids).
            """
        if !taste.favourites.isEmpty { system += "\nThey loved: " + taste.favourites.joined(separator: ", ") + "." }
        if !taste.dislikes.isEmpty { system += "\nThey did not like: " + taste.dislikes.joined(separator: ", ") + "." }
        if let vitals { system += "\nYou feel \(vitals.level.title.lowercased()) today." }
        var user = scene
        if !recent.isEmpty { user += " You just did: " + recent.suffix(5).joined(separator: ", ") + ". Pick something different." }
        return [.system(system), .user(user)]
    }

    public static func parse(_ text: String) -> (trick: GenieTrick, line: String)? {
        var body = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if let data = body.data(using: .utf8), let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            let name = (object["trick"] ?? object["id"] ?? object["name"]) as? String ?? ""
            let line = (object["line"] ?? object["say"] ?? object["reply"]) as? String ?? ""
            if let trick = GenieTricks.named(name) ?? GenieTricks.find(name) { return (trick, clean(line, fallback: trick.line)) }
        }
        body = body.replacingOccurrences(of: "`", with: "").replacingOccurrences(of: "*", with: "")
        if let colon = body.firstIndex(of: ":") {
            let head = String(body[..<colon]).trimmingCharacters(in: .whitespacesAndNewlines)
            let tail = String(body[body.index(after: colon)...])
            if let trick = GenieTricks.named(head.lowercased().replacingOccurrences(of: " ", with: "_")) ?? GenieTricks.find(head) {
                return (trick, clean(tail, fallback: trick.line))
            }
        }
        let lowered = body.lowercased()
        let named = GenieTricks.all
            .compactMap { trick -> (GenieTrick, String.Index)? in
                lowered.range(of: trick.id).map { (trick, $0.lowerBound) }
            }
            .min { $0.1 < $1.1 }
        if let (trick, _) = named { return (trick, trick.line) }
        if let trick = GenieTricks.find(body) { return (trick, trick.line) }
        return nil
    }

    static func clean(_ raw: String, fallback: String) -> String {
        var line = raw.trimmingCharacters(in: .whitespacesAndNewlines.union(CharacterSet(charactersIn: "\"'“”")))
        if let newline = line.firstIndex(of: "\n") { line = String(line[..<newline]) }
        guard !line.isEmpty else { return fallback }
        if line.count > maxLine { line = String(line.prefix(maxLine - 1)).trimmingCharacters(in: .whitespaces) + "…" }
        return line
    }

    public static func fallback(recent: [String], taste: GenieTrickTaste, mood: GenieTrickMood? = nil, random: () -> Double) -> GenieImprovisation {
        let trick = GenieTricks.pick(recent: recent, taste: taste, mood: mood, random: random)
        return GenieImprovisation(trick: trick, line: trick.line, byModel: false)
    }

    @MainActor
    public static func improvise(
        scene: String, recent: [String], taste: GenieTrickTaste, vitals: GenieVitals?, model: GenieModel?,
        timeout: Double = GenieImproviser.timeout, random: @escaping () -> Double = { Double.random(in: 0..<1) }
    ) async -> GenieImprovisation {
        let fallback = GenieImproviser.fallback(recent: recent, taste: taste, random: random)
        guard let model, model.engine.availability.isAvailable else { return fallback }
        let messages = GenieImproviser.messages(scene: scene, recent: recent, taste: taste, vitals: vitals)
        let engine = model.engine
        var parameters = model.parameters
        parameters.maxTokens = 48
        parameters.temperature = 0.9
        let asked = parameters
        let prepare = model.prepare
        let text: String? = await withTaskGroup(of: String?.self) { group in
            group.addTask {
                do {
                    try await prepare()
                } catch {
                    return ""
                }
                let turn = try? await engine.complete(messages: messages, tools: [], parameters: asked)
                return turn?.text ?? ""
            }
            group.addTask {
                try? await Task.sleep(nanoseconds: UInt64(max(timeout, 0.1) * 1_000_000_000))
                return nil
            }
            let first = await group.next() ?? nil
            group.cancelAll()
            return first
        }
        guard let text, let (trick, line) = parse(text) else { return fallback }
        return GenieImprovisation(trick: trick, line: line, byModel: true)
    }
}
