import Foundation

public struct GenieAbility: Equatable, Sendable {
    public var topic: String
    public var words: Set<String>
    public var summary: String
    public var examples: String
    public var tools: [String]

    public init(topic: String, words: Set<String>, summary: String, examples: String, tools: [String]) {
        self.topic = topic
        self.words = words
        self.summary = summary
        self.examples = examples
        self.tools = tools
    }
}

public enum GenieCapabilities {
    public nonisolated static let tool = "list_capabilities"
    public nonisolated static let page = "Genie"
    public nonisolated static let symbol = "sparkles"

    public nonisolated static let abilities: [GenieAbility] = [
        GenieAbility(
            topic: "screens",
            words: ["screen", "screens", "page", "pages", "navigate", "navigation", "navigating", "open", "opening", "around", "tabs", "tab"],
            summary: "open any screen, repository, issue or person",
            examples: "Say \"open my inbox\", \"take me to settings\", \"show forkmesh/forkmesh#12\" or \"go to @ada\".",
            tools: ["open_screen"]),
        GenieAbility(
            topic: "latest",
            words: ["latest", "newest", "last", "recent", "inbox", "news", "updates", "new", "reading", "read", "commits", "commit"],
            summary: "read out your newest pings, commits, chats and more",
            examples: "Try \"read my inbox\", \"what was the last commit\", \"read the last three chats\" or \"what's my next meeting\".",
            tools: ["read_latest"]),
        GenieAbility(
            topic: "search",
            words: ["search", "searching", "find", "finding", "look", "lookup", "web", "internet", "online", "google", "research"],
            summary: "search ForkMesh and the web",
            examples: "Say \"search for the login bug\", \"open the search prompt\", \"find notes about pricing\" or \"look up Ada Lovelace on the web\".",
            tools: ["search_app", "open_search", "search_forkmesh", "search_web", "read_web_page"]),
        GenieAbility(
            topic: "agents",
            words: ["agent", "agents", "coding", "code", "runs", "jobs", "claude", "bots", "bot"],
            summary: "start, check, message and stop your coding agents",
            examples: "Say \"start an agent to fix the login page\", \"how is my latest agent doing\", \"tell the agent to add tests\" or \"stop that agent\". I ask before I start, message or stop one.",
            tools: ["start_agent", "list_agents", "message_agent", "stop_agent"]),
        GenieAbility(
            topic: "work",
            words: ["tasks", "task", "todo", "todos", "notes", "note", "issues", "issue", "bugs", "bug", "pull", "pulls", "prs", "pr", "repositories", "repository", "repos", "repo", "projects", "work"],
            summary: "go through your tasks, notes, issues, pull requests and repositories",
            examples: "Ask \"what are my open tasks\", \"read my note about the launch\", \"list open issues in forkmesh/forkmesh\" or \"show pull request 12\".",
            tools: ["list_tasks", "read_notes", "list_issues", "list_pull_requests", "list_repositories"]),
        GenieAbility(
            topic: "day",
            words: ["chat", "chats", "messages", "message", "pings", "ping", "notifications", "calendar", "schedule", "meetings", "meeting", "events", "event", "day", "agenda"],
            summary: "check your chat, pings and calendar",
            examples: "Ask \"any unread pings\", \"what did @ada say in chat\" or \"what's on my calendar this week\".",
            tools: ["read_chat", "list_pings", "list_calendar"]),
        GenieAbility(
            topic: "crm",
            words: ["crm", "contacts", "contact", "people", "leads", "lead", "email", "emails", "mail", "sales", "pipeline", "customers", "clients", "deals"],
            summary: "work your CRM and draft emails",
            examples: "Say \"who is Grace in the CRM\", \"add Alan Turing as a contact\", \"move Grace to negotiation\" or \"draft an email to Grace about the demo\". Emails stay drafts for you to send.",
            tools: ["crm_contacts", "create_crm_contact", "update_crm_contact", "delete_crm_contact", "draft_email"]),
        GenieAbility(
            topic: "app",
            words: ["commands", "command", "actions", "action", "app", "timer", "timers", "shortcuts", "shortcut", "palette", "settings"],
            summary: "run the app's commands, like timers",
            examples: "Say \"start a timer\" or name anything from the command palette, and I'll find it and run it.",
            tools: ["find_app_actions", "run_app_action"]),
        GenieAbility(
            topic: "health",
            words: ["health", "healthy", "vitals", "feeling", "mood", "hungry", "velocity", "momentum", "marketing", "pet"],
            summary: "tell you how I'm feeling, which follows your code velocity and marketing",
            examples: "Ask \"how are you feeling\" or \"are you hungry\". Commits, agent runs and merges feed my code side; CRM outreach feeds my marketing side.",
            tools: ["check_vitals"]),
        GenieAbility(
            topic: "tricks",
            words: ["trick", "tricks", "dance", "dancing", "play", "playing", "hands", "hand", "gesture", "gestures", "spider", "camera", "pinch", "fun"],
            summary: "do tricks and play with your hands",
            examples: "Say \"do a backflip\", \"dance\" or \"surprise me\". Turn on Hand play in my menu, then pinch me to pick me up, push your hand toward the screen, wave, or pinch with both hands to turn and resize me.",
            tools: ["perform_trick"]),
        GenieAbility(
            topic: "diffs",
            words: ["diff", "diffs", "review", "reviewing", "changes", "paging", "scroll", "scrolling"],
            summary: "turn diff pages",
            examples: "With a diff open, say \"next page\", \"previous page\", \"next file\" or \"go back a file\".",
            tools: ["turn_page"]),
        GenieAbility(
            topic: "account",
            words: ["account", "profile", "organizations", "organization", "orgs", "username", "signed"],
            summary: "tell you about your account",
            examples: "Ask \"who am I signed in as\" or \"which organizations am I in\".",
            tools: ["get_account"]),
    ]

    public nonisolated static let closing =
        "Anything else, I'll hand to an agent that does it and teaches me for next time. Ask what I can do with agents, search or any of these for examples, or just tell me what you want."

    public nonisolated static func available(_ toolNames: [String]) -> [GenieAbility] {
        let have = Set(toolNames)
        return abilities.filter { $0.tools.contains(where: have.contains) }
    }

    public nonisolated static func ability(_ raw: String) -> GenieAbility? {
        let words = GenieWords.words(raw)
        if let exact = abilities.first(where: { $0.topic == raw.lowercased().trimmingCharacters(in: .whitespacesAndNewlines) }) { return exact }
        for word in words {
            if let hit = abilities.first(where: { $0.words.contains(word) || $0.topic == word }) { return hit }
        }
        return nil
    }

    public nonisolated static func describe(topic: String?, toolNames: [String]) -> String {
        let usable = available(toolNames)
        if let topic, let ability = ability(topic), let shown = usable.first(where: { $0.topic == ability.topic }) {
            return "I can \(shown.summary). \(shown.examples)"
        }
        guard !usable.isEmpty else { return "Right now I can only hand requests to an agent. " + closing }
        return "I can " + joined(usable.map(\.summary)) + ". " + closing
    }

    nonisolated static func joined(_ parts: [String]) -> String {
        guard parts.count > 1 else { return parts.first ?? "" }
        return parts.dropLast().joined(separator: ", ") + ", and " + (parts.last ?? "")
    }
}
