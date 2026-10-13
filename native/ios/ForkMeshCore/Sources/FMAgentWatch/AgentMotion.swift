// The motion table: the desktop's desktop/src/AgentSonifier.h `Motion`,
// styled by AgentActivityModel.cpp `kStyles`. Same order, same colours, same words, so
// the same run looks and sounds the same on the desktop, the phone and the
// watch. The beats a transcript is read into are in AgentWatchBeats.swift.

import FMBase

/// One kind of thing an agent (or the desktop under it) does, exactly as the
/// desktop's Watch window and Listen button tell them apart.
///
/// A phone sees fewer of them (a pushed transcript carries tool calls, prose,
/// prompts and status lines, and an on-device run adds tool results), but
/// every kind is listed, as it is on the desktop. The order is part of the
/// contract: `rawValue` is the case's position.
public enum AgentMotion: Int, CaseIterable, Sendable, Hashable {
    case start
    case speak
    case think
    case read
    case search
    case web
    case write
    case run
    case delegate
    case plan
    case arrive
    case prompt
    case notice
    case finish
    case scan
    case create
    case test
    case build
    case git
    case install
    case spawn
    case wait
    case ask
    case skill
    case mcp
    case todo
    case fumble
    case denied
    case image
    case compact
    case retry
    case fail
    case frame
    case compose
    case usage
    case system
    case tick
    case close
    case seal
    case hook
    case network
    case connect
    case task
    case logError
    case logStall
    case logSync
    case logGit
    case logCloud
    case logTask
    case logAccount
    case logInfo

    /// The enum case name ("logError"): what a beat's seed hashes.
    public var name: String { Self.names[rawValue] }

    /// 0..1 around the colour wheel: the mark's own colour.
    public var hue: Double { spec.hue }

    /// How far the ripple travels, in body radii.
    public var reach: Double { spec.reach }

    /// How long the mark lives on the field, in seconds.
    public var seconds: Double { spec.seconds }

    /// Stroke width, relative.
    public var weight: Double { spec.weight }

    /// 0 a plain ring, n an n-armed star.
    public var spokes: Int { Int(spec.spokes) }

    /// Data coming back falls in; work flies out.
    public var inbound: Bool { spec.inbound }

    /// A searching, unsettled mark.
    public var dashed: Bool { spec.dashed }

    /// The sound list's word for it ("Searching"). Also the key the phone
    /// persists mute and hide choices under.
    public var label: String { spec.label }

    /// What it stands for in the log.
    public var description: String { spec.description }

    /// What you hear for it.
    public var sound: String { spec.sound }

    /// The log's bookkeeping: always a grace note, and never work (it does not
    /// wake a body or move a melody).
    public var isBookkeeping: Bool {
        switch self {
        case .frame, .compose, .usage, .system, .tick, .close, .seal, .hook: return true
        default: return false
        }
    }

    /// Bookkeeping and the network: the small marks and the quiet notes.
    public var isFaint: Bool { isBookkeeping || self == .network }

    /// A line of the desktop's own log, by the badge the Logs page gives it.
    public var isLog: Bool { rawValue >= AgentMotion.logError.rawValue && rawValue <= AgentMotion.logInfo.rawValue }

    /// What the field hears outside any transcript: the log, a new host, the
    /// desktop's background work. Always grace notes.
    public var isCue: Bool { isLog || self == .connect || self == .task }

    /// The motion whose `label` is `label`, if any.
    public static func withLabel(_ label: String) -> AgentMotion? {
        allCases.first { $0.label == label }
    }

    private var spec: MotionSpec { Self.specs[rawValue] }

    private static let names: [String] = [
        "start",
        "speak",
        "think",
        "read",
        "search",
        "web",
        "write",
        "run",
        "delegate",
        "plan",
        "arrive",
        "prompt",
        "notice",
        "finish",
        "scan",
        "create",
        "test",
        "build",
        "git",
        "install",
        "spawn",
        "wait",
        "ask",
        "skill",
        "mcp",
        "todo",
        "fumble",
        "denied",
        "image",
        "compact",
        "retry",
        "fail",
        "frame",
        "compose",
        "usage",
        "system",
        "tick",
        "close",
        "seal",
        "hook",
        "network",
        "connect",
        "task",
        "logError",
        "logStall",
        "logSync",
        "logGit",
        "logCloud",
        "logTask",
        "logAccount",
        "logInfo",
    ]

    private static let specs: [MotionSpec] = [
        MotionSpec(0.33, 2.6, 1.8, 1.6, 0, false, false, "Start",
                   "a run opening, or a session announcing itself",
                   "a bell on the root: the phrase opens"), // start
        MotionSpec(0.60, 1.5, 1.3, 1.0, 0, false, false, "Speaking",
                   "the agent writing prose to you",
                   "a mallet wandering up and down the scale"), // speak
        MotionSpec(0.78, 1.2, 2.0, 0.7, 0, false, false, "Thinking",
                   "reasoning, under its breath",
                   "a soft pad breathed in slowly, drifting down"), // think
        MotionSpec(0.52, 1.8, 1.1, 0.8, 0, false, false, "Reading",
                   "a file read, a folder listed",
                   "a bright glass note, a step or two up"), // read
        MotionSpec(0.16, 2.2, 1.3, 0.8, 3, false, true, "Searching",
                   "grep, glob, find: looking through the tree",
                   "a glass note leaping two to four steps up"), // search
        MotionSpec(0.555, 2.6, 1.5, 0.9, 0, false, false, "Web",
                   "a page fetched, the web searched",
                   "a far glass note leaping three to five steps up"), // web
        MotionSpec(0.87, 1.7, 1.4, 1.9, 4, false, false, "Writing",
                   "an edit or a new file landing on disk",
                   "a ringing bell, climbing"), // write
        MotionSpec(0.08, 1.4, 0.9, 1.2, 4, false, false, "Command",
                   "a shell command being run",
                   "a dry wooden knock that bends down, climbing"), // run
        MotionSpec(0.70, 2.4, 1.6, 1.1, 6, false, false, "Delegating",
                   "work handed to a subagent, a skill or an MCP server",
                   "a bell leaping three or four steps up"), // delegate
        MotionSpec(0.28, 1.6, 1.5, 1.0, 0, false, false, "Planning",
                   "a todo list written, a plan put forward",
                   "a mallet ticking off a list, up a step and back"), // plan
        MotionSpec(0.42, 1.9, 1.1, 0.9, 0, true, false, "Result",
                   "a tool's answer coming back",
                   "a mallet falling, further for a bigger answer"), // arrive
        MotionSpec(0.95, 2.2, 1.7, 1.5, 0, true, false, "Prompt",
                   "you, steering it",
                   "a loud bell back on the root"), // prompt
        MotionSpec(0.00, 2.0, 1.4, 1.4, 5, false, true, "Notice",
                   "a guardrail, a protocol error, a refusal, a stop",
                   "a bright glass note held level; a failed run falls to the root"), // notice
        MotionSpec(0.24, 3.0, 2.4, 1.7, 0, false, false, "Done",
                   "a turn's result landing, a run finishing",
                   "a bell on the root; a finished run rings a rising cadence"), // finish
        MotionSpec(0.50, 1.6, 1.0, 0.8, 0, false, true, "Listing",
                   "a folder listed, a glob expanded",
                   "a short pure ping, a step or two up"), // scan
        MotionSpec(0.82, 2.2, 1.6, 1.8, 0, false, false, "New file",
                   "a file written from nothing",
                   "a note swooping up into place, climbing"), // create
        MotionSpec(0.30, 1.8, 1.5, 1.2, 0, false, false, "Tests",
                   "a test suite running",
                   "a trill, shaken back and forth"), // test
        MotionSpec(0.05, 2.0, 1.4, 1.6, 0, false, false, "Build",
                   "a compile, a bundle, a make",
                   "a low drum an octave down, bent hard"), // build
        MotionSpec(0.98, 1.9, 1.4, 1.2, 0, false, false, "Git",
                   "git or gh: history read, a commit, a push",
                   "a sagging string, through history and back"), // git
        MotionSpec(0.14, 2.0, 1.6, 1.2, 0, true, false, "Install",
                   "a package manager fetching dependencies",
                   "a low drum piling up a step at a time"), // install
        MotionSpec(0.62, 2.8, 1.6, 1.2, 0, false, false, "Background",
                   "a command sent off to run on its own",
                   "a swoop leaping four or five steps up"), // spawn
        MotionSpec(0.66, 1.3, 2.2, 0.8, 0, false, true, "Waiting",
                   "polling background work, sleeping on it",
                   "a slow swell, held nearly level"), // wait
        MotionSpec(0.90, 2.4, 2.0, 1.6, 0, false, false, "Question",
                   "the agent asking you something",
                   "a bell going up at the end, like a question"), // ask
        MotionSpec(0.74, 2.2, 1.5, 1.2, 0, false, false, "Skill",
                   "a skill called in",
                   "a twanging string leaping up"), // skill
        MotionSpec(0.48, 2.4, 1.5, 1.1, 0, false, false, "MCP",
                   "a tool on an MCP server",
                   "a far glass note, two to five steps up"), // mcp
        MotionSpec(0.26, 1.5, 1.4, 1.0, 0, false, false, "Todo",
                   "the todo list or task list updated",
                   "a mallet ticking up and back"), // todo
        MotionSpec(0.02, 2.0, 1.3, 1.5, 0, true, false, "Error",
                   "a tool call that came back as an error",
                   "a rough buzz, falling"), // fumble
        MotionSpec(0.97, 2.2, 1.5, 1.7, 0, true, true, "Refused",
                   "a tool call refused: a permission, a hook, you",
                   "a harder buzz, falling further"), // denied
        MotionSpec(0.20, 2.0, 1.4, 1.0, 0, true, false, "Image",
                   "a picture back: a screenshot, an image read",
                   "a glass note, shaded by the picture"), // image
        MotionSpec(0.68, 3.0, 2.2, 1.4, 0, true, false, "Compacting",
                   "the context folded down to make room",
                   "a swell sinking four or five steps"), // compact
        MotionSpec(0.11, 1.6, 1.3, 1.0, 0, false, true, "Retry",
                   "an API call failed and is being tried again",
                   "a trill held level"), // retry
        MotionSpec(0.00, 3.0, 2.4, 1.8, 0, true, false, "Failed",
                   "a turn that ended in an error",
                   "a buzz sinking to the fifth under the root"), // fail
        MotionSpec(0.64, 0.9, 0.7, 0.6, 0, false, false, "Framing",
                   "a message or block opening in the stream",
                   "a dry wooden click just above the line"), // frame
        MotionSpec(0.46, 1.1, 0.9, 0.6, 0, false, false, "Tool call",
                   "a tool call's arguments streaming in, piece by piece",
                   "soft glass taps stepping upwards, like typing"), // compose
        MotionSpec(0.12, 1.0, 1.1, 0.7, 0, true, false, "Usage",
                   "tokens counted, rate limits reported, a turn's stop reason",
                   "a low soft mallet tap under the line"), // usage
        MotionSpec(0.36, 1.2, 1.2, 0.8, 6, false, false, "System",
                   "a hook firing, a status change, a compaction, a retry",
                   "a small bell a fifth above, held a moment"), // system
        MotionSpec(0.58, 0.7, 0.6, 0.5, 0, false, false, "Other",
                   "anything else the log records: heartbeats, control frames",
                   "a faint high sparkle between the beats"), // tick
        MotionSpec(0.60, 0.9, 0.7, 0.6, 0, true, false, "Closing",
                   "a message or block closing in the stream",
                   "a dry wooden click, under the opening one"), // close
        MotionSpec(0.15, 0.8, 0.6, 0.5, 0, false, false, "Seal",
                   "a thinking block's signature, a citation",
                   "a glinting ping at the very top"), // seal
        MotionSpec(0.40, 1.1, 1.1, 0.8, 0, false, false, "Hook",
                   "a hook starting or answering",
                   "a sagging string a little above the line"), // hook
        MotionSpec(0.50, 1.0, 1.0, 0.6, 0, false, false, "Network",
                   "bytes an agent sent or received over the network",
                   "a faint tick: higher for sent, lower for received"), // network
        MotionSpec(0.53, 2.2, 1.4, 1.0, 0, false, false, "New host",
                   "a host joining the field: the first bytes to somewhere new",
                   "a chirp swooping up high"), // connect
        MotionSpec(0.10, 1.4, 1.2, 0.9, 0, false, false, "Desktop work",
                   "the desktop's background work starting or finishing",
                   "a soft wooden knock a step above the line"), // task
        MotionSpec(0.99, 2.2, 1.6, 1.5, 0, true, false, "Log: error",
                   "an ERROR line in the desktop's log",
                   "a rough buzz falling below the line"), // logError
        MotionSpec(0.11, 2.0, 1.8, 1.2, 0, false, true, "Log: stall",
                   "a STALL or BGBLOCK line: the interface held up, work turned away",
                   "a low swell, breathed in"), // logStall
        MotionSpec(0.50, 1.6, 1.2, 0.9, 0, false, false, "Log: mesh",
                   "a MIRROR, SYNC, PIN, PEER, NODE, FORKED or STATUS line",
                   "a glass note a few steps up"), // logSync
        MotionSpec(0.61, 1.7, 1.3, 1.0, 0, false, false, "Log: git",
                   "a GIT, PULL, MERGE, PUBLISH, ISSUE, BOUNTY or ACTIONS line",
                   "a twanging string a step or two up"), // logGit
        MotionSpec(0.08, 1.6, 1.1, 0.9, 0, true, false, "Log: cloud",
                   "a CLOUD, NETWORK or DB line: the site and its database",
                   "a short pure ping high over the line"), // logCloud
        MotionSpec(0.04, 1.3, 1.1, 0.8, 0, false, false, "Log: background",
                   "a BGTASK or BGNOTE line: background work reported",
                   "a dry wooden tap on the line"), // logTask
        MotionSpec(0.79, 1.8, 1.4, 1.0, 0, false, false, "Log: account",
                   "a SESSION, ACCOUNT, IDENTITY, CRYPTO, ADMIN or PROMPT line",
                   "a small bell a fifth up"), // logAccount
        MotionSpec(0.57, 1.2, 1.0, 0.7, 0, false, false, "Log: info",
                   "an INFO line, or any badge without a family",
                   "the faintest glass glint at the top"), // logInfo
    ]
}

private struct MotionSpec: Sendable {
    let hue: Double
    let reach: Double
    let seconds: Double
    let weight: Double
    let spokes: Double
    let inbound: Bool
    let dashed: Bool
    let label: String
    let description: String
    let sound: String

    init(
        _ hue: Double, _ reach: Double, _ seconds: Double, _ weight: Double, _ spokes: Double,
        _ inbound: Bool, _ dashed: Bool, _ label: String, _ description: String, _ sound: String
    ) {
        self.hue = hue
        self.reach = reach
        self.seconds = seconds
        self.weight = weight
        self.spokes = spokes
        self.inbound = inbound
        self.dashed = dashed
        self.label = label
        self.description = description
        self.sound = sound
    }
}
