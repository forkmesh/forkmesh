import FMBase
import Foundation

public enum SettingsSectionId: String, CaseIterable, Sendable, Hashable {
    case quickSetup
    case profile
    case general
    case repos
    case agents
    case voice
    case identity
    case sshKeys
    case mcp
    case organizations
    case apps
    case device
    case desktops
    case nodes
    case sessions
    case notifications
    case relay
    case imports
    case data
    case security
    case account
    case sponsored
    case onDeviceAI
    case legal
}

public struct SettingsSectionEntry: Equatable, Sendable, Identifiable {
    public let id: SettingsSectionId
    public let title: String
    public let icon: String
    public let keywords: [String]
}

public enum SettingsCatalog {
    public static let entries: [SettingsSectionEntry] = [
        .init(id: .quickSetup, title: "Quick setup", icon: "task_alt", keywords: ["start", "onboarding", "first run", "pair"]),
        .init(id: .profile, title: "Profile", icon: "person_outline", keywords: ["name", "display name", "status", "away", "avatar", "node id"]),
        .init(id: .general, title: "General", icon: "tune", keywords: ["appearance", "theme", "dark", "light", "accent", "color", "haptics", "vibration", "vibrate", "sounds", "audio", "mute", "silent", "noise", "volume", "beep", "click", "genie", "spider", "hide spider", "home screen", "icons", "icon style", "categories", "sort", "highlight", "frequently used"]),
        .init(id: .repos, title: "Repositories on this phone", icon: "folder_outlined", keywords: ["commit", "author", "email", "branch", "local", "clone"]),
        .init(id: .agents, title: "Agents", icon: "smart_toy_outlined", keywords: ["provider", "model", "claude", "codex", "desktop", "default"]),
        .init(id: .voice, title: "Voice", icon: "tune_rounded", keywords: ["dictation", "speech", "language", "microphone", "listen", "speech to text", "on-device"]),
        .init(id: .identity, title: "Identity and recovery", icon: "fingerprint", keywords: ["key", "backup", "recovery kit", "ed25519", "qr", "trust list"]),
        .init(id: .sshKeys, title: "SSH keys", icon: "key_outlined", keywords: ["ssh", "public key", "git", "gateway"]),
        .init(id: .mcp, title: "MCP connector", icon: "hub_outlined", keywords: ["mcp", "api token", "token", "claude code", "codex", "bot", "remote"]),
        .init(id: .organizations, title: "Organizations", icon: "apartment_outlined", keywords: ["org", "team", "members"]),
        .init(id: .apps, title: "Installed apps", icon: "extension_outlined", keywords: ["apps", "integrations", "bots", "slash commands", "oauth", "revoke", "app directory"]),
        .init(id: .device, title: "This device", icon: "phone_iphone", keywords: ["udid", "serve as node", "phone"]),
        .init(id: .desktops, title: "Desktops", icon: "desktop_mac_outlined", keywords: ["pair", "paired", "desktop", "rename", "forget", "backups"]),
        .init(id: .nodes, title: "Nodes on this account", icon: "dns_outlined", keywords: ["node", "link", "link node", "scan", "qr", "devices"]),
        .init(id: .sessions, title: "Where you're signed in", icon: "devices_other", keywords: ["sessions", "sign out", "revoke"]),
        .init(id: .notifications, title: "Notifications", icon: "notifications_none_rounded", keywords: ["push", "pings", "email", "quiet hours"]),
        .init(id: .relay, title: "Relay", icon: "cloud_outlined", keywords: ["server", "relay", "room", "passphrase", "network"]),
        .init(id: .imports, title: "Import tokens", icon: "download_outlined", keywords: ["github", "gitlab", "import"]),
        .init(id: .data, title: "Data", icon: "storage_outlined", keywords: ["export", "download", "cache", "clear", "backup"]),
        .init(id: .security, title: "Report a vulnerability", icon: "verified_user_outlined", keywords: ["security", "vulnerability", "report", "bug bounty"]),
        .init(id: .account, title: "Account", icon: "person_outline_rounded", keywords: ["sign out", "log out", "switch account", "delete account", "guest"]),
        .init(id: .sponsored, title: "Sponsored messages", icon: "info_outline", keywords: ["ads", "sponsored"]),
        .init(id: .onDeviceAI, title: "On-device AI", icon: "memory_outlined", keywords: ["gemma", "local model", "ai"]),
        .init(id: .legal, title: "Legal", icon: "description_outlined", keywords: ["terms", "privacy"]),
    ]

    public static func entry(_ id: SettingsSectionId) -> SettingsSectionEntry {
        entries.first { $0.id == id } ?? SettingsSectionEntry(id: id, title: id.rawValue, icon: "settings_outlined", keywords: [])
    }

    public static func fold(_ text: String) -> String {
        text.folding(options: [.caseInsensitive, .diacriticInsensitive, .widthInsensitive], locale: Locale(identifier: "en_US_POSIX"))
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public static func matches(_ entry: SettingsSectionEntry, _ query: String) -> Bool {
        let words = fold(query).split(whereSeparator: { $0.isWhitespace }).map(String.init)
        if words.isEmpty { return true }
        let haystack = ([entry.title] + entry.keywords).map(fold).joined(separator: " | ")
        return words.allSatisfy { haystack.contains($0) }
    }

    public static func visible(_ query: String, available: Set<SettingsSectionId>) -> [SettingsSectionEntry] {
        entries.filter { available.contains($0.id) && matches($0, query) }
    }

    public static func section(named raw: String?) -> SettingsSectionId? {
        guard let raw, !raw.isEmpty else { return nil }
        let folded = fold(raw).replacingOccurrences(of: "-", with: "").replacingOccurrences(of: "_", with: "")
        if let exact = SettingsSectionId.allCases.first(where: { $0.rawValue.lowercased() == folded }) { return exact }
        let aliases: [String: SettingsSectionId] = [
            "ssh": .sshKeys, "keys": .sshKeys, "appearance": .general, "theme": .general, "sounds": .general, "haptics": .general, "genie": .general, "spider": .general,
            "recovery": .identity, "backup": .identity, "export": .data, "vulnerability": .security, "phone": .desktops,
            "devices": .nodes, "linknode": .nodes, "apitoken": .mcp, "token": .mcp, "orgs": .organizations, "installedapps": .apps, "integrations": .apps,
            "pings": .notifications, "push": .notifications, "setup": .quickSetup, "quicksetup": .quickSetup,
        ]
        return aliases[folded]
    }
}

public struct QuickSetupInput: Equatable, Sendable {
    public var signedIn: Bool
    public var guest: Bool
    public var projectName: String
    public var desktopsPaired: Int
    public var identityBackedUp: Bool
    public var pushEnabled: Bool

    public init(
        signedIn: Bool, guest: Bool = false, projectName: String = "", desktopsPaired: Int = 0,
        identityBackedUp: Bool = false, pushEnabled: Bool = false
    ) {
        self.signedIn = signedIn
        self.guest = guest
        self.projectName = projectName
        self.desktopsPaired = desktopsPaired
        self.identityBackedUp = identityBackedUp
        self.pushEnabled = pushEnabled
    }
}

public enum QuickSetupStepId: String, Sendable, CaseIterable {
    case signIn, project, desktop, backup, notifications
}

public struct QuickSetupStep: Equatable, Sendable, Identifiable {
    public let id: QuickSetupStepId
    public let title: String
    public let detail: String
    public let done: Bool
}

public enum QuickSetup {
    public static func steps(_ input: QuickSetupInput) -> [QuickSetupStep] {
        let account = input.signedIn && !input.guest
        return [
            QuickSetupStep(
                id: .signIn, title: "Sign in",
                detail: account ? "Signed in." : (input.guest ? "You are exploring as a guest." : "Sign in or create an account."),
                done: account),
            QuickSetupStep(
                id: .project, title: "Pick a project",
                detail: input.projectName.isEmpty ? "Choose which ForkMesh instance this app talks to." : "Using \(input.projectName).",
                done: !input.projectName.isEmpty),
            QuickSetupStep(
                id: .desktop, title: "Pair a desktop",
                detail: input.desktopsPaired > 0
                    ? "\(input.desktopsPaired) desktop\(input.desktopsPaired == 1 ? "" : "s") paired."
                    : "Drive your desktop's agents from this phone.",
                done: input.desktopsPaired > 0),
            QuickSetupStep(
                id: .backup, title: "Back up your identity key",
                detail: input.identityBackedUp ? "A recovery kit exists for this key." : "Save an encrypted recovery kit to Files.",
                done: input.identityBackedUp),
            QuickSetupStep(
                id: .notifications, title: "Turn on notifications",
                detail: input.pushEnabled ? "Notifications are on." : "Get pings, approvals and review requests.",
                done: input.pushEnabled),
        ]
    }

    public static func remaining(_ input: QuickSetupInput) -> Int { steps(input).filter { !$0.done }.count }

    public static func next(_ input: QuickSetupInput) -> QuickSetupStep? { steps(input).first { !$0.done } }
}
