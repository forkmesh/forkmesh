import FMBase
import FMLocalAI

extension ForkMeshToolbox {
    public nonisolated static let specs: [LocalToolSpec] = [
        spec("get_account", "The signed-in user's ForkMesh username, email and organizations.", [:]),
        spec("list_tasks", "The organization task board. Default: open tasks assigned to the user. Pass id for one task with its details and replies.", [
            "filter": choice("Which tasks", ["mine", "open", "done", "all"]),
            "query": text("Words to look for in titles and details"),
            "id": text("A task id, for one task in full"),
        ]),
        spec("read_latest", "The newest item of one kind, ready to read aloud: the last inbox ping, commit, chat message, agent, task, note, issue, pull request, or the next calendar event.", [
            "what": choice("Which kind", ForkMeshToolbox.latestKinds),
            "repository": text("owner/name for commit, issue or pull; default: the repository on screen or the most recent one"),
            "count": integer("How many, newest first (1-5, default 1)"),
        ], required: ["what"]),
        spec("read_notes", "The user's notes. Without id or title: the list. With id or title: that note's text.", [
            "id": text("A note id"),
            "title": text("A note title, or part of one"),
            "query": text("Words to look for in note titles"),
        ]),
        spec("list_repositories", "Repositories owned by the user or their organizations; with query, searches every repository on ForkMesh.", [
            "query": text("Words to look for in names and descriptions"),
        ]),
        spec("list_issues", "Issues in one repository, or across the user's most recent repositories. Pass number for one issue in full.", [
            "repository": text("owner/name, or just the name"),
            "state": choice("Which issues", ["open", "closed", "all"]),
            "query": text("Words to look for"),
            "number": integer("An issue number"),
        ]),
        spec("list_pull_requests", "Pull requests in one repository, or across the user's most recent repositories. Pass number for one with its patch.", [
            "repository": text("owner/name, or just the name"),
            "state": choice("Which pull requests", ["open", "closed", "all"]),
            "number": integer("A pull request number"),
        ]),
        spec("read_chat", "Recent ForkMesh chat messages this phone has received, newest last.", [
            "conversation": text("A channel like #general or a person like @ada"),
            "query": text("Words to look for"),
            "limit": integer("How many messages, up to 50"),
        ]),
        spec("list_agents", "The user's coding agents on their desktop and in the cloud. Pass id to read that agent's transcript.", [
            "id": text("An agent id from the list"),
            "running_only": boolean("Only agents still working"),
            "query": text("Words to look for"),
        ]),
        spec("message_agent", "Send a message or follow-up instruction to an agent. The user approves it first.", [
            "id": text("The agent id from list_agents"),
            "text": text("What to tell the agent"),
        ], required: ["id", "text"]),
        spec("stop_agent", "Stop a running agent. The user approves it first.", [
            "id": text("The agent id from list_agents"),
        ], required: ["id"]),
        spec("list_pings", "The user's pings (notifications): mentions, reviews, assignments.", [
            "unread_only": boolean("Only unread pings"),
        ]),
        spec("list_calendar", "Calendar events from today on.", [
            "days_ahead": integer("Days after today to include, default 7"),
            "days_back": integer("Days before today to include, default 0"),
        ]),
        spec("crm_contacts", "The CRM: pipelines, stage counts and contacts. Pass id (or a name or email as contact) for one contact with notes and email history.", [
            "organization": text("CRM organization; default the user's first"),
            "id": text("A contact id"),
            "contact": text("A contact's name or email"),
            "query": text("Words to look for"),
            "flow": text("Pipeline id or name"),
            "stage": text("Stage name"),
            "tag": text("A tag"),
        ]),
        spec("create_crm_contact", "Add a CRM contact. Needs a name or an email.", crmProperties(["notes": text("First note about them")])),
        spec("update_crm_contact", "Change a CRM contact: any field, move its stage, or append_note to its notes. Only pass what changes.", crmProperties([
            "id": text("The contact id"),
            "contact": text("Or the contact's current name or email"),
            "append_note": text("A note to add, dated and signed"),
            "mark_contacted": boolean("Set last contacted to now"),
        ])),
        spec("delete_crm_contact", "Remove a CRM contact. The user approves it first.", [
            "organization": text("CRM organization"),
            "id": text("The contact id"),
            "contact": text("Or the contact's name or email"),
        ]),
        spec("draft_email", "Write an email draft for the user to review and send themselves. Never sends.", [
            "contact": text("A CRM contact's id, name or email, to address it"),
            "to": text("An email address, when not a CRM contact"),
            "subject": text("Subject line"),
            "body": text("The whole email, greeting to signature"),
        ], required: ["subject", "body"]),
        spec("search_web", "Search the web (DuckDuckGo), e.g. to research a person or company.", [
            "query": text("What to search for"),
        ], required: ["query"]),
        spec("read_web_page", "Read a web page's text.", [
            "url": text("The page address"),
        ], required: ["url"]),
        spec("search_forkmesh", "Search tasks, notes, repositories, issues, chat, pings and agent runs at once.", [
            "query": text("Words to look for"),
        ], required: ["query"]),
    ]

    nonisolated static func crmProperties(_ extra: JSONObject) -> JSONObject {
        var properties: JSONObject = [
            "organization": text("CRM organization; default the user's first"),
            "name": text("Full name"),
            "email": text("Email address"),
            "company": text("Company"),
            "title": text("Job title"),
            "phone": text("Phone"),
            "website": text("Website"),
            "location": text("City or country"),
            "linkedin": text("LinkedIn URL"),
            "github": text("GitHub handle"),
            "twitter": text("X/Twitter handle"),
            "mastodon": text("Mastodon handle"),
            "bluesky": text("Bluesky handle"),
            "flow": text("Pipeline id"),
            "stage": text("Pipeline stage"),
            "value": integer("Deal value"),
            "nextStep": text("Next step"),
            "tags": .object(["type": "array", "items": .object(["type": "string"]), "description": "Tags, replacing the current ones"]),
            "owner": text("Owner username"),
            "source": text("Where they came from"),
        ]
        for (key, value) in extra { properties[key] = value }
        return properties
    }

    nonisolated static func spec(_ name: String, _ description: String, _ properties: JSONObject, required: [String] = []) -> LocalToolSpec {
        LocalToolSpec(name: name, description: description, parameters: [
            "type": "object",
            "properties": .object(properties),
            "required": JSONValue(required),
        ])
    }

    nonisolated static func text(_ description: String) -> JSONValue {
        .object(["type": "string", "description": .string(description)])
    }

    nonisolated static func integer(_ description: String) -> JSONValue {
        .object(["type": "integer", "description": .string(description)])
    }

    nonisolated static func boolean(_ description: String) -> JSONValue {
        .object(["type": "boolean", "description": .string(description)])
    }

    nonisolated static func choice(_ description: String, _ values: [String]) -> JSONValue {
        .object(["type": "string", "description": .string(description), "enum": JSONValue(values)])
    }
}
