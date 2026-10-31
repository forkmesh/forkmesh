import FMApi
import Foundation

public enum TourTrack: String, CaseIterable, Codable, Hashable, Sendable, Identifiable {
    case essentials
    case advanced
    case everything

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .essentials: return "Essentials"
        case .advanced: return "Advanced"
        case .everything: return "The whole shebang"
        }
    }

    public var summary: String {
        switch self {
        case .essentials: return "Home, agents, the new agent button, Inbox and your hub."
        case .advanced: return "Adds voice, review and merge, commands, search and the bar."
        case .everything: return "Every corner: chat, projects, desktops, plugins, widgets and looks."
        }
    }

    public var symbol: String {
        switch self {
        case .essentials: return "leaf"
        case .advanced: return "bolt"
        case .everything: return "sparkles"
        }
    }

    public var rank: Int {
        switch self {
        case .essentials: return 0
        case .advanced: return 1
        case .everything: return 2
        }
    }

    public func includes(_ other: TourTrack) -> Bool { other.rank <= rank }
}

public enum TourScene: String, CaseIterable, Codable, Hashable, Sendable {
    case home
    case agents
    case newAgent
    case inbox
    case hub
    case voice
    case code
    case palette
    case search
    case bar
    case chat
    case projects
    case desktops
    case workspace
    case market
    case personal
    case system
    case looks
}

public enum TourJump: Hashable, Sendable {
    case tab(AppTab)
    case home
    case hub
    case composer
    case palette
    case projects
    case route(Route)
}

public struct TourStop: Identifiable, Hashable, Sendable {
    public let scene: TourScene
    public let track: TourTrack
    public let symbol: String
    public let title: String
    public let message: String
    public let tips: [String]
    public let tryIt: String
    public let jump: TourJump?
    public let jumpLabel: String
    public let needs: String?
    public let offers: [String]
    public var fromMarket: Bool = false

    public var id: String { scene.rawValue }

    public init(
        scene: TourScene, track: TourTrack, symbol: String, title: String, message: String, tips: [String],
        tryIt: String, jump: TourJump?, jumpLabel: String, needs: String? = nil, offers: [String] = []
    ) {
        self.scene = scene
        self.track = track
        self.symbol = symbol
        self.title = title
        self.message = message
        self.tips = tips
        self.tryIt = tryIt
        self.jump = jump
        self.jumpLabel = jumpLabel
        self.needs = needs
        self.offers = offers
    }

    func resolved(installed: (String) -> Bool) -> TourStop? {
        if let needs = needs, !installed(needs) { return nil }
        guard !offers.isEmpty, !offers.contains(where: installed) else { return self }
        var out: TourStop = TourStop(
            scene: scene, track: track, symbol: symbol, title: title, message: message, tips: tips, tryIt: tryIt,
            jump: .route(.market(filter: nil)), jumpLabel: "Find it in the Market", needs: needs, offers: offers)
        out.fromMarket = true
        return out
    }
}

public struct TourPlace: Equatable, Sendable {
    public let track: TourTrack
    public let stop: String

    public init(track: TourTrack, stop: String) {
        self.track = track
        self.stop = stop
    }

    public var encoded: String { "\(track.rawValue):\(stop)" }

    public init?(encoded: String) {
        let parts: [Substring] = encoded.split(separator: ":", maxSplits: 1)
        guard parts.count == 2, let track = TourTrack(rawValue: String(parts[0])), !parts[1].isEmpty else {
            return nil
        }
        self.track = track
        self.stop = String(parts[1])
    }
}

public enum TourCatalog {
    public static let secondsPerStop: Int = 20

    public static let stops: [TourStop] = [
        TourStop(
            scene: .home, track: .essentials, symbol: "square.grid.2x2", title: "Home holds everything",
            message: "Every destination is an icon on Home, with live counts and a ring around anything busy. The glass bar along the bottom keeps your favourites one tap away.",
            tips: ["Counts show what is new or waiting.", "Home leaves out what is already in the bar, so each icon shows once."],
            tryIt: "Tap an icon", jump: .home, jumpLabel: "Show me Home"),
        TourStop(
            scene: .agents, track: .essentials, symbol: "sparkles", title: "Agents first",
            message: "The Agents tab lists every agent: on your desktop, in the cloud and on runners. Ones that need you come first, then what is running, then what finished.",
            tips: ["Tap a run to read its transcript and steer it.", "Anything waiting on an answer sits at the top under Needs you."],
            tryIt: "Tap Allow", jump: .tab(.agents), jumpLabel: "Open Agents", needs: "agents"),
        TourStop(
            scene: .newAgent, track: .essentials, symbol: "plus.circle.fill", title: "One tap starts an agent",
            message: "The glowing button in the top bar opens the composer, aimed at whatever is on screen. Each star inside it is an agent running right now.",
            tips: ["Chips pick the repository, model and mode.", "Starting from a pull request or issue attaches it for you."],
            tryIt: "Tap the glowing button", jump: .composer, jumpLabel: "Open the composer", needs: "agents"),
        TourStop(
            scene: .inbox, track: .essentials, symbol: "tray", title: "One inbox for what needs you",
            message: "Pings, review requests, assigned issues and tasks, mentions and agent approvals land in Inbox. The app icon badge matches its unread count.",
            tips: ["Filters narrow it to reviews, mentions or approvals.", "Notifications carry Approve, Reply and Merge buttons of their own."],
            tryIt: "Tap a row to read it", jump: .tab(.inbox), jumpLabel: "Open Inbox", needs: "inbox"),
        TourStop(
            scene: .hub, track: .essentials, symbol: "person.crop.circle", title: "Your avatar is your hub",
            message: "Tap your avatar for your status, Pocket mode, Suggestion mode, Settings and signing out. The dot on it shows whether you are connected.",
            tips: ["Set a status so teammates know you are away.", "What's new and Send diagnostics sit at the bottom."],
            tryIt: "Tap your avatar", jump: .hub, jumpLabel: "Open my hub"),
        TourStop(
            scene: .voice, track: .advanced, symbol: "waveform", title: "Hold to talk",
            message: "Touch and hold the new agent button and say what the agent should do. Words appear as you speak, and letting go starts the agent.",
            tips: ["Type moves the words into the composer instead.", "Commands opens the command palette from the same screen."],
            tryIt: "Touch and hold the button", jump: nil, jumpLabel: "", needs: "agents"),
        TourStop(
            scene: .code, track: .advanced, symbol: "arrow.triangle.merge", title: "Review and merge from your phone",
            message: "Repos lists your repositories and Code returns to the one you were in. Open a pull request to read the diff, review it and merge it.",
            tips: ["Browse files, commits, branches and releases too.", "The new agent button on a pull request attaches it to the agent."],
            tryIt: "Tap Merge", jump: .tab(.code), jumpLabel: "Open Repos", needs: "code"),
        TourStop(
            scene: .palette, track: .advanced, symbol: "command", title: "Commands for everything",
            message: "The command palette runs actions and jumps anywhere by name. Open it with Commands on the voice screen, or press Command-K on a keyboard.",
            tips: ["Suggestions change with the screen you are on.", "Switch project lives here too."],
            tryIt: "Watch it find a command", jump: .palette, jumpLabel: "Open the palette"),
        TourStop(
            scene: .search, track: .advanced, symbol: "magnifyingglass", title: "Search everywhere at once",
            message: "One field searches repositories, people, commits, issues, pull requests, code and chat. Pick a scope to narrow it, or keep All.",
            tips: ["Save a search you run often.", "Offline, it searches what is already on the phone."],
            tryIt: "Tap a scope", jump: .route(.search(query: "", scope: .everything)), jumpLabel: "Open Search"),
        TourStop(
            scene: .bar, track: .advanced, symbol: "dock.rectangle", title: "Make the bar yours",
            message: "Touch and hold any icon in the bottom bar to move it, put it in a folder or hide it. Bottom bar settings change its shape, glass, size and counts.",
            tips: ["Sort it by usual order, recently used or most used.", "Two rows hold twice as many icons."],
            tryIt: "Tap the bar to change its shape", jump: .home, jumpLabel: "Show me the bar"),
        TourStop(
            scene: .chat, track: .everything, symbol: "bubble.left.and.bubble.right", title: "Chat where the work lives",
            message: "Channels, direct messages, #mesh and office rooms share one tab. Send text, files, photos, voice and reactions; private rooms are encrypted.",
            tips: ["In a room, the composer posts to that room.", "Mentions and direct messages badge the Chat icon."],
            tryIt: "Tap a message to react", jump: .tab(.chat), jumpLabel: "Open Chat", needs: "chat"),
        TourStop(
            scene: .projects, track: .everything, symbol: "square.stack.3d.up", title: "Projects keep work apart",
            message: "Each project has its own account, repositories and layout. Tap the project icon in the top bar to switch, or use Switch project in the palette.",
            tips: ["Invite people from the project switcher.", "Your bottom bar layout is shared by every project."],
            tryIt: "Tap a project", jump: .projects, jumpLabel: "Switch project"),
        TourStop(
            scene: .desktops, track: .everything, symbol: "desktopcomputer", title: "Your desktops in your pocket",
            message: "Desktops shows every ForkMesh desktop you are signed in to, with its agents and shortcuts. Hosts and Network show the machines and relays behind them.",
            tips: ["Run a desktop shortcut and watch its progress.", "An orange dot means a desktop went offline."],
            tryIt: "Tap Run", jump: .route(.desktops), jumpLabel: "Open Desktops",
            offers: ["desktops", "hosts", "network"]),
        TourStop(
            scene: .workspace, track: .everything, symbol: "note.text", title: "Notes, tasks and your calendar",
            message: "Notes, Tasks, Calendar, Timers and Files cover the work around the code, each one a plugin you can add or remove.",
            tips: ["Turn a note into a task or an issue.", "Timers keep counting on the Lock Screen."],
            tryIt: "Tap a task to finish it", jump: .route(.notes), jumpLabel: "Open Notes",
            offers: ["notes", "tasks", "calendar", "timers", "files"]),
        TourStop(
            scene: .market, track: .everything, symbol: "bag", title: "Add more from the Market",
            message: "Plugins add icons to Home and pages to the app. Install and remove them from the Market; anything you remove disappears everywhere it showed.",
            tips: ["Each plugin lists its permissions before you install it.", "Touch and hold an icon for its settings or to remove it."],
            tryIt: "Tap Get", jump: .route(.market(filter: nil)), jumpLabel: "Open the Market"),
        TourStop(
            scene: .personal, track: .everything, symbol: "lock.shield", title: "Private tools for you",
            message: "Vault keeps secrets, Coves share encrypted files, and trackers count water, steps, sleep and breaks. On-device AI answers without leaving the phone.",
            tips: ["Each one is a plugin, so keep only what you use.", "Vault passwords can fill in other apps."],
            tryIt: "Tap the glass to add water", jump: .route(.vault(section: nil)), jumpLabel: "Open Vault",
            offers: ["vault", "coves", "water", "steps", "sleep", "on-device-ai"]),
        TourStop(
            scene: .system, track: .everything, symbol: "app.badge", title: "Outside the app",
            message: "Notifications let you approve, reply or merge without opening the app. Widgets, the Live Activity and Siri show and steer your agents from anywhere.",
            tips: ["Say \"What are my ForkMesh agents doing\" to Siri.", "Add the New agent control to Control Center."],
            tryIt: "Tap Approve", jump: nil, jumpLabel: ""),
        TourStop(
            scene: .looks, track: .everything, symbol: "paintpalette", title: "Make it look like yours",
            message: "Pick outline, filled, accent or tile icons, how many fit in a row, and the app's theme and wallpaper in Settings.",
            tips: ["Home screen settings also hide categories you never use.", "Reduce Motion and Reduce Transparency calm the effects."],
            tryIt: "Tap to change the icons", jump: .route(.settings(section: nil)), jumpLabel: "Open Settings"),
    ]

    public static func stops(for track: TourTrack, installed: (String) -> Bool = { _ in true }) -> [TourStop] {
        stops.filter { track.includes($0.track) }.compactMap { $0.resolved(installed: installed) }
    }

    public static func minutes(for count: Int) -> Int {
        max(1, Int((Double(count * secondsPerStop) / 60).rounded(.up)))
    }

    public static func index(of place: TourPlace, in stops: [TourStop]) -> Int? {
        stops.firstIndex { $0.id == place.stop }
    }
}
