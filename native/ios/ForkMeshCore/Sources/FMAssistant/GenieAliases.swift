import FMBase
import FMLocalAI
import Foundation

public enum GenieAliases {
    public nonisolated static let tools: [String: [String]] = [
        "open_screen": [
            "open", "open_page", "open_view", "open_tab", "open_app", "open_link", "open_repository", "open_repo", "open_issue",
            "open_profile", "go", "go_to", "goto", "go_to_screen", "go_to_page", "navigate", "navigate_to", "navigation", "show",
            "show_screen", "show_page", "show_view", "show_tab", "switch_tab", "switch_to", "select_tab", "change_tab", "take_me_to",
            "visit", "display", "display_screen", "bring_up", "pull_up", "jump_to", "head_to", "view", "screen", "page", "route",
            "open_route", "load_page", "launch_screen",
        ],
        "find_app_actions": [
            "find_actions", "list_actions", "app_actions", "actions", "find_command", "find_commands", "list_commands", "commands",
            "command_palette", "palette", "search_actions", "search_commands", "what_actions", "available_actions", "find_feature",
            "find_features", "lookup_action", "look_up_action", "menu", "shortcuts", "find_shortcut",
        ],
        "run_app_action": [
            "run_action", "do_action", "perform_action", "execute_action", "trigger_action", "run_command", "execute_command",
            "perform_command", "do_command", "invoke_action", "use_action", "press", "tap", "click", "activate", "run_shortcut",
            "app_action",
        ],
        "search_app": [
            "search", "find", "look_up", "lookup", "look_for", "search_for", "global_search", "search_everywhere", "search_everything",
            "app_search", "search_screen", "find_anything", "hunt_for", "locate", "query", "search_up",
        ],
        "open_search": [
            "search_prompt", "open_search_prompt", "open_search_bar", "show_search", "search_bar", "search_box", "empty_search",
            "start_search", "new_search", "search_field", "open_search_screen",
        ],
        "start_agent": [
            "new_agent", "create_agent", "launch_agent", "spawn_agent", "run_agent", "make_agent", "start_coding_agent",
            "kick_off_agent", "kickoff_agent", "send_agent", "dispatch_agent", "agent", "ask_agent", "start_task", "create_task",
            "new_task", "delegate", "delegate_task", "assign_agent", "start_job", "code", "write_code", "fix", "build",
        ],
        "ask_user": [
            "ask", "clarify", "ask_question", "question", "ask_clarifying_question", "clarification", "request_clarification",
            "confirm_with_user", "check_with_user", "follow_up", "followup",
        ],
        "hand_off_to_agent": [
            "hand_off", "handoff", "escalate", "give_up", "cannot_do", "can_t_do", "fallback", "learn", "teach", "teach_genie",
            "unsupported", "pass_to_agent", "hand_to_agent",
        ],
        "list_capabilities": [
            "capabilities", "what_can_you_do", "help", "get_help", "show_help", "abilities", "skills", "features", "list_tools",
            "tools", "list_skills", "list_features", "what_do_you_do", "how_can_you_help", "about", "about_genie", "introduce",
            "introduction", "powers", "what_can_i_say", "what_can_i_ask", "examples", "usage", "guide", "manual", "commands_help",
        ],
        "get_account": [
            "account", "me", "whoami", "who_am_i", "my_account", "profile", "my_profile", "user", "get_user", "current_user",
            "username", "my_email", "organizations", "my_organizations", "orgs",
        ],
        "list_tasks": [
            "tasks", "get_tasks", "show_tasks", "read_tasks", "task_board", "board", "todos", "todo", "to_dos", "my_tasks",
            "assignments", "work_items", "find_task", "read_task",
        ],
        "read_latest": [
            "latest", "newest", "last", "most_recent", "recent", "get_latest", "read_last", "read_newest", "read_recent",
            "whats_new", "what_s_new", "news", "updates", "catch_up", "read_inbox", "inbox", "last_message", "latest_message",
            "last_commit", "latest_commit", "next_event", "next_meeting", "read_last_message", "check_latest", "newest_item",
        ],
        "read_notes": [
            "notes", "get_notes", "list_notes", "show_notes", "read_note", "note", "find_note", "my_notes", "open_note", "memos",
            "notebook",
        ],
        "list_repositories": [
            "repositories", "repos", "list_repos", "get_repos", "my_repos", "my_repositories", "projects", "list_projects",
            "find_repo", "find_repository", "search_repos", "search_repositories", "repository",
        ],
        "list_issues": [
            "issues", "get_issues", "show_issues", "read_issues", "bugs", "list_bugs", "tickets", "list_tickets", "read_issue",
            "get_issue", "find_issue", "issue",
        ],
        "list_pull_requests": [
            "pull_requests", "pulls", "prs", "list_prs", "list_pulls", "get_prs", "get_pull_requests", "merge_requests",
            "list_merge_requests", "read_pr", "read_pull_request", "get_pr", "pull_request", "pr", "reviews", "patches",
        ],
        "read_chat": [
            "chats", "messages", "read_messages", "get_messages", "list_messages", "read_chats", "conversations",
            "channel", "dms", "direct_messages", "show_chat", "check_messages", "read_channel",
        ],
        "list_agents": [
            "agents", "get_agents", "show_agents", "running_agents", "my_agents", "read_agent", "agent_status", "check_agents",
            "agent_transcript", "transcript", "runs", "list_runs", "jobs", "list_jobs",
        ],
        "message_agent": [
            "tell_agent", "send_to_agent", "reply_to_agent", "agent_message", "instruct_agent", "follow_up_agent",
            "continue_agent", "nudge_agent", "talk_to_agent", "chat_with_agent", "steer_agent", "answer_agent",
        ],
        "stop_agent": [
            "kill_agent", "cancel_agent", "halt_agent", "end_agent", "abort_agent", "terminate_agent", "pause_agent", "stop_run",
            "cancel_run", "stop_job", "cancel_job",
        ],
        "list_pings": [
            "pings", "get_pings", "notifications", "list_notifications", "get_notifications", "alerts", "mentions",
            "list_mentions", "unread", "check_notifications", "check_pings", "read_pings",
        ],
        "list_calendar": [
            "calendar", "events", "get_events", "list_events", "schedule", "agenda", "meetings", "list_meetings", "my_schedule",
            "appointments", "check_calendar", "show_calendar", "upcoming",
        ],
        "crm_contacts": [
            "crm", "contacts", "list_contacts", "get_contacts", "find_contact", "search_contacts", "get_contact", "contact",
            "pipeline", "pipelines", "leads", "customers", "clients", "deals", "people", "address_book",
        ],
        "create_crm_contact": [
            "add_contact", "new_contact", "create_contact", "add_lead", "new_lead", "create_lead", "add_person", "save_contact",
            "add_to_crm",
        ],
        "update_crm_contact": [
            "edit_contact", "change_contact", "update_contact", "modify_contact", "move_contact", "add_note_to_contact",
            "contact_note", "log_contact", "mark_contacted", "move_stage", "update_lead",
        ],
        "delete_crm_contact": [
            "remove_contact", "delete_contact", "drop_contact", "erase_contact", "delete_lead", "remove_lead",
        ],
        "draft_email": [
            "email", "write_email", "compose_email", "send_email", "new_email", "mail", "write_mail", "compose", "draft",
            "email_contact", "reply_email", "write_to",
        ],
        "search_web": [
            "web_search", "google", "bing", "duckduckgo", "search_internet", "internet", "browse", "look_up_online",
            "search_online", "research", "web",
        ],
        "read_web_page": [
            "read_page", "fetch_page", "open_url", "read_url", "fetch_url", "get_page", "web_page", "read_website", "browse_url",
            "read_link", "fetch",
        ],
        "check_vitals": [
            "vitals", "health", "check_health", "my_health", "your_health", "how_are_you", "how_are_you_feeling", "feeling", "mood",
            "wellbeing", "well_being", "tamagotchi", "pet", "hunger", "hungry", "velocity", "momentum", "pulse",
        ],
        "perform_trick": [
            "trick", "tricks", "do_trick", "do_a_trick", "show_trick", "spider_trick", "party_trick", "dance", "perform", "play_trick",
            "emote", "animate", "stunt", "do_stunt", "surprise_me", "show_off",
        ],
        "turn_page": [
            "next_page", "previous_page", "page_down", "page_up", "paginate", "flip_page", "scroll_page", "next_file",
            "previous_file", "page_through", "go_to_next_page", "page_diff", "turn",
        ],
        "say": [
            "speak", "say_aloud", "say_words", "say_this", "speak_text", "speak_words", "read_aloud", "repeat", "repeat_after_me",
            "echo", "announce", "pronounce", "voice", "tts", "text_to_speech",
        ],
        "search_forkmesh": [
            "search_data", "search_all", "find_in_forkmesh", "search_my_data", "search_my_stuff", "search_workspace",
            "search_account", "deep_search", "find_anywhere",
        ],
    ]

    public nonisolated static let arguments: [String: [String]] = [
        "screen": ["name", "page", "destination", "view", "tab", "target", "place", "route", "link", "url", "where", "to", "path"],
        "query": ["q", "search", "terms", "keywords", "words", "term", "text", "phrase", "topic", "about", "for", "filter_text"],
        "prompt": ["task", "instructions", "instruction", "request", "job", "description", "text", "goal", "work", "message"],
        "repository": ["repo", "project", "repository_name", "repo_name", "slug"],
        "id": ["identifier", "action", "action_id", "command", "command_id", "agent", "agent_id", "task_id", "note_id", "contact_id"],
        "question": ["text", "prompt", "message", "ask", "query"],
        "reason": ["why", "missing", "request", "task", "message", "text"],
        "what": ["kind", "type", "item", "thing", "which", "category"],
        "text": ["message", "body", "content", "instruction", "instructions", "reply"],
        "count": ["n", "number", "limit", "how_many", "amount"],
        "url": ["link", "address", "page", "href", "website", "site"],
        "conversation": ["channel", "room", "person", "with", "chat"],
        "body": ["text", "content", "message"],
        "subject": ["title", "topic"],
        "to": ["email", "recipient", "address"],
        "contact": ["person", "who", "name"],
        "topic": ["area", "about", "subject", "what", "query"],
        "title": ["name"],
        "words": ["text", "message", "phrase", "content", "speech", "sentence", "utterance", "say"],
        "state": ["status"],
        "trick": ["name", "move", "stunt", "animation", "kind", "which", "dance", "action"],
        "direction": ["way", "to", "where", "which", "move", "turn", "page", "go"],
        "filter": ["which", "scope"],
    ]

    public nonisolated static let noTool: Set<String> = [
        "none", "no", "no_tool", "null", "nil", "nothing", "n_a", "na", "reply", "answer", "respond", "response",
        "talk", "final", "final_answer", "done", "finish",
    ]

    nonisolated static let fillers: Set<String> = ["the", "a", "an", "my", "me", "please", "tool", "function", "app", "genie", "forkmesh"]

    public nonisolated static func normalize(_ raw: String) -> String {
        var out = ""
        var previous: Character?
        for char in raw.trimmingCharacters(in: .whitespacesAndNewlines) {
            if char.isUppercase, let previous, previous.isLowercase || previous.isNumber {
                out.append("_")
            }
            if char.isLetter || char.isNumber {
                out.append(Character(char.lowercased()))
            } else if !out.isEmpty, out.last != "_" {
                out.append("_")
            }
            previous = char
        }
        while out.hasSuffix("_") { out.removeLast() }
        for prefix in ["functions_", "function_", "tool_", "tools_", "genie_", "forkmesh_"] where out.hasPrefix(prefix) && out.count > prefix.count {
            out.removeFirst(prefix.count)
        }
        return out
    }

    public nonisolated static func canonical(_ raw: String, known: [String]) -> String? {
        if known.contains(raw) { return raw }
        let name = normalize(raw)
        guard !name.isEmpty, !noTool.contains(name) else { return nil }
        if known.contains(name) { return name }
        let words = name.split(separator: "_").map(String.init)
        let kept = words.filter { !fillers.contains($0) }
        let trimmed = kept.joined(separator: "_")
        if known.contains(trimmed) { return trimmed }
        for candidate in [name, trimmed] where !candidate.isEmpty {
            if let hit = known.first(where: { (tools[$0] ?? []).contains(candidate) }) { return hit }
        }
        let said = Set(kept)
        guard !said.isEmpty else { return nil }
        var best: (name: String, size: Int)?
        for tool in known {
            for alias in [tool] + (tools[tool] ?? []) {
                let parts = alias.split(separator: "_").map(String.init).filter { !fillers.contains($0) }
                guard parts.count >= 2, Set(parts).isSubset(of: said) else { continue }
                if parts.count > (best?.size ?? 0) { best = (tool, parts.count) }
            }
        }
        if let best { return best.name }
        guard let verb = kept.first else { return nil }
        return known.first { (tools[$0] ?? []).contains(verb) }
    }

    public nonisolated static func arguments(_ args: JSONObject, for spec: LocalToolSpec?) -> JSONObject {
        guard let spec, let properties = spec.parameters["properties"]?.objectValue, !properties.isEmpty else { return args }
        var out = JSONObject()
        var spare = JSONObject()
        for (key, value) in args {
            let name = normalize(key)
            if properties[key] != nil {
                out[key] = value
            } else if properties[name] != nil, out[name] == nil {
                out[name] = value
            } else {
                spare[name] = value
            }
        }
        for key in properties.keys.sorted() where out[key] == nil {
            guard let alias = (arguments[key] ?? []).first(where: { spare[$0] != nil && properties[$0] == nil }) else { continue }
            out[key] = spare.removeValue(forKey: alias)
        }
        let required = spec.parameters.strings("required")
        if required.count == 1, let key = required.first, out[key] == nil, spare.count == 1, let value = spare.values.first {
            out[key] = value
        }
        return out
    }

    public nonisolated static func resolve(_ call: LocalToolCall, specs: [LocalToolSpec]) -> LocalToolCall {
        guard let name = canonical(call.name, known: specs.map(\.name)) else { return call }
        var next = call
        next.name = name
        next.arguments = arguments(call.arguments, for: specs.first { $0.name == name })
        return next
    }
}
