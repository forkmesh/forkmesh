def F(key, label, req=False, ph="", default=None, where="", options=None,
      slashes=False, kind="string"):
    field = {"key": key, "label": label, "type": kind}
    if req:
        field["required"] = True
    if ph:
        field["placeholder"] = ph
    if default is not None:
        field["default"] = default
    if where:
        field["in"] = where
    if options:
        field["type"] = "select"
        field["options"] = [
            {"value": o, "label": o or "(none)"} for o in options]
    if slashes:
        field["slashes"] = True
    return field


def T(key, label, req=False, ph="", default=None, where=""):
    return F(key, label, req, ph, default, where, kind="text")


def N(key, label, req=False, ph="", default=None, where=""):
    return F(key, label, req, ph, default, where, kind="number")


def B(key, label, default=None, where=""):
    return F(key, label, False, "", default, where, kind="boolean")


def J(key, label, req=False, ph="", default=None, where=""):
    return F(key, label, req, ph, default, where, kind="json")


def OP(key, name, method, path, fields=(), body=None, encoding=None,
       headers=None, query=None, output=None):
    op = {"name": name, "method": method, "path": path, "fields": list(fields)}
    if body is not None:
        op["body"] = body
    if encoding:
        op["encoding"] = encoding
    if headers:
        op["headers"] = headers
    if query:
        op["query"] = query
    if output:
        op["output"] = output
    return key, op


def SECRET(key, label, ph=""):
    return {"key": key, "label": label, "secret": True, "placeholder": ph}


def PLAIN(key, label, ph="", url=False, optional=False):
    field = {"key": key, "label": label, "placeholder": ph}
    if url:
        field["url"] = True
    if optional:
        field["optional"] = True
    return field


def SITE(label="Site URL", ph="https://example.com"):
    return PLAIN("site", label, ph, url=True)


def BEARER(label="Access token", extra=()):
    return {"fields": list(extra) + [SECRET("token", label)],
            "headers": {"Authorization": "Bearer {token}"}}


def HEADER(header, label="API key", prefix="", extra=()):
    return {"fields": list(extra) + [SECRET("apiKey", label)],
            "headers": {header: prefix + "{apiKey}"}}


def QUERY(param, label="API key", extra=()):
    return {"fields": list(extra) + [SECRET("apiKey", label)],
            "query": {param: "{apiKey}"}}


def BASIC(user, user_label, password, password_label, extra=(),
          user_secret=False):
    user_field = (SECRET(user, user_label) if user_secret
                  else PLAIN(user, user_label))
    return {"fields": list(extra) + [user_field, SECRET(password, password_label)],
            "basic": ("{" + user + "}", "{" + password + "}")}


def WEBHOOK(label="Webhook URL", ph="https://"):
    return {"fields": [{"key": "url", "label": label, "secret": True,
                        "url": True, "placeholder": ph}]}


NONE = {"fields": []}

CHAT_MODEL_FIELDS = (
    T("prompt", "Prompt", True, "Summarize: {{ $json.text }}"),
    T("system", "System prompt"),
)


def CHAT(model, path="/chat/completions"):
    return OP("chat", "Chat completion", "POST", path,
              [F("model", "Model", req=True, default=model)]
              + list(CHAT_MODEL_FIELDS),
              body={"model": "{model}", "messages": [
                  {"role": "system", "content": "{system}"},
                  {"role": "user", "content": "{prompt}"}]},
              output="openai")


def APP(key, name, category, color, description, base, auth, ops,
        headers=None, encoding=None):
    app = {"name": name, "category": category, "color": color,
           "description": description, "base": base, "auth": auth,
           "operations": dict(ops)}
    if headers:
        app["headers"] = headers
    if encoding:
        app["encoding"] = encoding
    return key, app


COMMUNICATION = "Communication"
EMAIL = "Email"
PRODUCTIVITY = "Productivity"
FORMS = "Forms and meetings"
DEVELOPER = "Developer tools"
CRM = "CRM and support"
COMMERCE = "Commerce and finance"
SOCIAL = "Social and content"
ANALYTICS = "Analytics"
AI = "AI"
DATA = "Data and storage"
UTILITY = "Utilities"

INTEGRATIONS = dict((
    APP("slack", "Slack", COMMUNICATION, "#4A154B",
        "Post messages and manage channels in a Slack workspace.",
        "https://slack.com/api", BEARER("Bot token (xoxb-...)"), (
            OP("postMessage", "Send a message", "POST", "/chat.postMessage", (
                F("channel", "Channel", True, "#general or C0123456"),
                T("text", "Text", True),
                F("thread_ts", "Reply in thread (ts)"))),
            OP("createChannel", "Create a channel", "POST",
               "/conversations.create", (
                   F("name", "Name", True), B("is_private", "Private"))),
            OP("listChannels", "List channels", "GET", "/conversations.list", (
                N("limit", "Limit", default="100"),)),
        ), headers={"Content-Type": "application/json; charset=utf-8"}),
    APP("discord", "Discord", COMMUNICATION, "#5865F2",
        "Send messages as a Discord bot.",
        "https://discord.com/api/v10",
        HEADER("Authorization", "Bot token", "Bot "), (
            OP("sendMessage", "Send a message", "POST",
               "/channels/{channelId}/messages", (
                   F("channelId", "Channel ID", True),
                   T("content", "Message", True))),
            OP("listChannels", "List server channels", "GET",
               "/guilds/{guildId}/channels", (F("guildId", "Server ID", True),)),
        )),
    APP("discordWebhook", "Discord Webhook", COMMUNICATION, "#5865F2",
        "Post to a Discord channel through an incoming webhook.",
        "{url}", WEBHOOK("Discord webhook URL",
                         "https://discord.com/api/webhooks/..."), (
            OP("send", "Send a message", "POST", "", (
                T("content", "Message", True), F("username", "Username"),
                F("avatar_url", "Avatar URL"))),
        )),
    APP("teams", "Microsoft Teams", COMMUNICATION, "#4B53BC",
        "Post to a Teams channel through an incoming webhook.",
        "{url}", WEBHOOK("Teams webhook URL"), (
            OP("send", "Send a message", "POST", "", (
                F("title", "Title"), T("text", "Message", True))),
        )),
    APP("telegram", "Telegram", COMMUNICATION, "#26A5E4",
        "Send messages and photos with a Telegram bot.",
        "https://api.telegram.org/bot{token}",
        {"fields": [SECRET("token", "Bot token", "123456:ABC-...")]}, (
            OP("sendMessage", "Send a message", "POST", "/sendMessage", (
                F("chat_id", "Chat ID", True), T("text", "Text", True),
                F("parse_mode", "Formatting",
                  options=("", "Markdown", "MarkdownV2", "HTML")))),
            OP("sendPhoto", "Send a photo", "POST", "/sendPhoto", (
                F("chat_id", "Chat ID", True),
                F("photo", "Photo URL", True), T("caption", "Caption"))),
            OP("getUpdates", "Get updates", "GET", "/getUpdates", (
                N("offset", "Offset"),)),
        )),
    APP("mattermost", "Mattermost", COMMUNICATION, "#0058CC",
        "Post to Mattermost through an incoming webhook.",
        "{url}", WEBHOOK("Mattermost webhook URL"), (
            OP("send", "Send a message", "POST", "", (
                T("text", "Message", True), F("channel", "Channel override"),
                F("username", "Username"))),
        )),
    APP("googleChat", "Google Chat", COMMUNICATION, "#00AC47",
        "Post to a Google Chat space through a webhook.",
        "{url}", WEBHOOK("Google Chat webhook URL"), (
            OP("send", "Send a message", "POST", "", (
                T("text", "Message", True),)),
        )),
    APP("pushover", "Pushover", COMMUNICATION, "#249DF1",
        "Send push notifications to phones and desktops.",
        "https://api.pushover.net/1",
        {"fields": [SECRET("token", "Application token"),
                    SECRET("user", "User key")],
         "body": {"token": "{token}", "user": "{user}"}}, (
            OP("notify", "Send a notification", "POST", "/messages.json", (
                T("message", "Message", True), F("title", "Title"),
                F("url", "URL"),
                N("priority", "Priority (-2 to 2)"))),
        )),
    APP("ntfy", "ntfy", COMMUNICATION, "#317F6F",
        "Publish push notifications to an ntfy topic.",
        "{server}",
        {"fields": [PLAIN("server", "Server URL", "https://ntfy.sh", url=True),
                    dict(SECRET("token", "Access token"), optional=True)],
         "headers": {"Authorization": "Bearer {token}"}}, (
            OP("publish", "Publish a message", "POST", "", (
                F("topic", "Topic", True), T("message", "Message", True),
                F("title", "Title"), N("priority", "Priority (1 to 5)"),
                J("tags", "Tags (JSON list)"))),
        )),
    APP("matrix", "Matrix", COMMUNICATION, "#0DBD8B",
        "Send messages to a Matrix room.",
        "{server}/_matrix/client/v3",
        BEARER("Access token", (PLAIN("server", "Homeserver URL",
                                      "https://matrix.org", url=True),)), (
            OP("sendMessage", "Send a message", "PUT",
               "/rooms/{roomId}/send/m.room.message/{txnId}", (
                   F("roomId", "Room ID", True, "!abc:matrix.org"),
                   T("body", "Message", True),
                   F("txnId", "Transaction ID", True,
                     default="{{ $now }}-{{ $item.index }}")),
               body={"msgtype": "m.text", "body": "{body}"}),
        )),
    APP("twilio", "Twilio", COMMUNICATION, "#F22F46",
        "Send SMS and WhatsApp messages and start calls.",
        "https://api.twilio.com/2010-04-01/Accounts/{accountSid}",
        BASIC("accountSid", "Account SID", "authToken", "Auth token"), (
            OP("sendSms", "Send an SMS", "POST", "/Messages.json", (
                F("To", "To", True, "+15551234567"),
                F("From", "From", True), T("Body", "Message", True))),
            OP("call", "Start a call", "POST", "/Calls.json", (
                F("To", "To", True), F("From", "From", True),
                F("Url", "TwiML URL", True))),
        ), encoding="form"),
    APP("whatsapp", "WhatsApp Business", COMMUNICATION, "#25D366",
        "Send WhatsApp messages through the Cloud API.",
        "https://graph.facebook.com/v19.0", BEARER("Access token"), (
            OP("sendText", "Send a text message", "POST",
               "/{phoneNumberId}/messages", (
                   F("phoneNumberId", "Phone number ID", True),
                   F("to", "To", True, "15551234567"),
                   T("text", "Message", True)),
               body={"messaging_product": "whatsapp", "to": "{to}",
                     "type": "text", "text": {"body": "{text}"}}),
        )),

    APP("sendgrid", "SendGrid", EMAIL, "#1A82E2",
        "Send email and manage marketing contacts.",
        "https://api.sendgrid.com/v3", BEARER("API key"), (
            OP("sendEmail", "Send an email", "POST", "/mail/send", (
                F("from", "From", True), F("to", "To", True),
                F("subject", "Subject", True), T("text", "Text", True)),
               body={"personalizations": [{"to": [{"email": "{to}"}]}],
                     "from": {"email": "{from}"}, "subject": "{subject}",
                     "content": [{"type": "text/plain", "value": "{text}"}]}),
            OP("addContact", "Add or update a contact", "PUT",
               "/marketing/contacts", (
                   F("email", "Email", True), F("firstName", "First name"),
                   F("lastName", "Last name")),
               body={"contacts": [{"email": "{email}",
                                   "first_name": "{firstName}",
                                   "last_name": "{lastName}"}]}),
        )),
    APP("mailgun", "Mailgun", EMAIL, "#F06B66",
        "Send email through a Mailgun domain.",
        "https://api.mailgun.net/v3",
        {"fields": [SECRET("apiKey", "API key")],
         "basic": ("api", "{apiKey}")}, (
            OP("sendEmail", "Send an email", "POST", "/{domain}/messages", (
                F("domain", "Sending domain", True, "mg.example.com"),
                F("from", "From", True), F("to", "To", True),
                F("subject", "Subject", True), T("text", "Text", True),
                T("html", "HTML"))),
        ), encoding="form"),
    APP("postmark", "Postmark", EMAIL, "#FFDE00",
        "Send transactional email.",
        "https://api.postmarkapp.com",
        HEADER("X-Postmark-Server-Token", "Server token"), (
            OP("sendEmail", "Send an email", "POST", "/email", (
                F("From", "From", True), F("To", "To", True),
                F("Subject", "Subject", True), T("TextBody", "Text", True),
                T("HtmlBody", "HTML"))),
        )),
    APP("resend", "Resend", EMAIL, "#000000",
        "Send email with Resend.",
        "https://api.resend.com", BEARER("API key"), (
            OP("sendEmail", "Send an email", "POST", "/emails", (
                F("from", "From", True), F("to", "To", True),
                F("subject", "Subject", True), T("text", "Text", True),
                T("html", "HTML"))),
        )),
    APP("mailchimp", "Mailchimp", EMAIL, "#FFE01B",
        "Add subscribers to Mailchimp audiences.",
        "https://{dc}.api.mailchimp.com/3.0",
        {"fields": [PLAIN("dc", "Data center", "us21"),
                    SECRET("apiKey", "API key")],
         "basic": ("forkmesh", "{apiKey}")}, (
            OP("addMember", "Add a subscriber", "POST",
               "/lists/{listId}/members", (
                   F("listId", "Audience ID", True),
                   F("email_address", "Email", True),
                   F("status", "Status", default="subscribed",
                     options=("subscribed", "pending", "unsubscribed")),
                   J("merge_fields", "Merge fields (JSON)"))),
            OP("listAudiences", "List audiences", "GET", "/lists"),
        )),
    APP("brevo", "Brevo", EMAIL, "#0B996E",
        "Send transactional email and manage contacts.",
        "https://api.brevo.com/v3", HEADER("api-key"), (
            OP("sendEmail", "Send an email", "POST", "/smtp/email", (
                F("from", "From email", True), F("fromName", "From name"),
                F("to", "To", True), F("subject", "Subject", True),
                T("text", "Text", True)),
               body={"sender": {"email": "{from}", "name": "{fromName}"},
                     "to": [{"email": "{to}"}], "subject": "{subject}",
                     "textContent": "{text}"}),
            OP("createContact", "Create a contact", "POST", "/contacts", (
                F("email", "Email", True), J("attributes", "Attributes (JSON)"),
                J("listIds", "List IDs (JSON list)"))),
        )),
    APP("kit", "Kit (ConvertKit)", EMAIL, "#FB6970",
        "Add subscribers and tags in Kit.",
        "https://api.kit.com/v4", HEADER("X-Kit-Api-Key"), (
            OP("createSubscriber", "Create a subscriber", "POST",
               "/subscribers", (
                   F("email_address", "Email", True),
                   F("first_name", "First name"))),
            OP("tagSubscriber", "Tag a subscriber", "POST",
               "/tags/{tagId}/subscribers", (
                   F("tagId", "Tag ID", True),
                   F("email_address", "Email", True))),
        )),
    APP("mailerlite", "MailerLite", EMAIL, "#09C269",
        "Create or update subscribers.",
        "https://connect.mailerlite.com/api", BEARER("API token"), (
            OP("upsertSubscriber", "Create or update a subscriber", "POST",
               "/subscribers", (
                   F("email", "Email", True), J("fields", "Fields (JSON)"),
                   J("groups", "Group IDs (JSON list)"))),
        )),

    APP("notion", "Notion", PRODUCTIVITY, "#000000",
        "Create pages and query databases in Notion.",
        "https://api.notion.com/v1", BEARER("Integration secret"), (
            OP("createPage", "Create a database page", "POST", "/pages", (
                F("databaseId", "Database ID", True),
                J("properties", "Properties (JSON)", True,
                  "{\"Name\": {\"title\": [{\"text\": {\"content\": \"Hello\"}}]}}")),
               body={"parent": {"database_id": "{databaseId}"},
                     "properties": "{properties}"}),
            OP("queryDatabase", "Query a database", "POST",
               "/databases/{databaseId}/query", (
                   F("databaseId", "Database ID", True),
                   J("filter", "Filter (JSON)"))),
            OP("appendText", "Append a paragraph", "PATCH",
               "/blocks/{blockId}/children", (
                   F("blockId", "Page or block ID", True),
                   T("text", "Text", True)),
               body={"children": [{"object": "block", "type": "paragraph",
                                   "paragraph": {"rich_text": [{
                                       "type": "text",
                                       "text": {"content": "{text}"}}]}}]}),
        ), headers={"Notion-Version": "2022-06-28"}),
    APP("airtable", "Airtable", PRODUCTIVITY, "#18BFFF",
        "Create, list and update Airtable records.",
        "https://api.airtable.com/v0", BEARER("Personal access token"), (
            OP("createRecord", "Create a record", "POST", "/{baseId}/{table}", (
                F("baseId", "Base ID", True), F("table", "Table", True),
                J("fields", "Fields (JSON)", True)),
               body={"fields": "{fields}", "typecast": True}),
            OP("listRecords", "List records", "GET", "/{baseId}/{table}", (
                F("baseId", "Base ID", True), F("table", "Table", True),
                F("filterByFormula", "Formula filter"),
                N("maxRecords", "Max records", default="100"))),
            OP("updateRecord", "Update a record", "PATCH",
               "/{baseId}/{table}/{recordId}", (
                   F("baseId", "Base ID", True), F("table", "Table", True),
                   F("recordId", "Record ID", True),
                   J("fields", "Fields (JSON)", True)),
               body={"fields": "{fields}", "typecast": True}),
        )),
    APP("googleSheets", "Google Sheets", PRODUCTIVITY, "#0F9D58",
        "Append and read spreadsheet rows.",
        "https://sheets.googleapis.com/v4/spreadsheets",
        BEARER("OAuth access token"), (
            OP("appendRow", "Append rows", "POST",
               "/{spreadsheetId}/values/{range}:append", (
                   F("spreadsheetId", "Spreadsheet ID", True),
                   F("range", "Range", True, default="Sheet1!A1"),
                   J("values", "Rows (JSON list of lists)", True,
                     "[[\"{{ $json.name }}\", \"{{ $json.email }}\"]]")),
               body={"values": "{values}"},
               query={"valueInputOption": "USER_ENTERED"}),
            OP("readRange", "Read a range", "GET",
               "/{spreadsheetId}/values/{range}", (
                   F("spreadsheetId", "Spreadsheet ID", True),
                   F("range", "Range", True, default="Sheet1!A1:Z100"))),
        )),
    APP("googleCalendar", "Google Calendar", PRODUCTIVITY, "#4285F4",
        "Create and list calendar events.",
        "https://www.googleapis.com/calendar/v3",
        BEARER("OAuth access token"), (
            OP("createEvent", "Create an event", "POST",
               "/calendars/{calendarId}/events", (
                   F("calendarId", "Calendar ID", True, default="primary"),
                   F("summary", "Title", True), T("description", "Description"),
                   F("start", "Start (RFC 3339)", True,
                     "2026-10-09T10:00:00Z"),
                   F("end", "End (RFC 3339)", True, "2026-10-09T11:00:00Z")),
               body={"summary": "{summary}", "description": "{description}",
                     "start": {"dateTime": "{start}"},
                     "end": {"dateTime": "{end}"}}),
            OP("listEvents", "List events", "GET",
               "/calendars/{calendarId}/events", (
                   F("calendarId", "Calendar ID", True, default="primary"),
                   F("timeMin", "From (RFC 3339)"),
                   N("maxResults", "Max results", default="25"))),
        )),
    APP("googleDrive", "Google Drive", PRODUCTIVITY, "#1FA463",
        "List files and create folders in Google Drive.",
        "https://www.googleapis.com/drive/v3", BEARER("OAuth access token"), (
            OP("listFiles", "List files", "GET", "/files", (
                F("q", "Search query", ph="name contains 'report'"),
                N("pageSize", "Page size", default="50"))),
            OP("createFolder", "Create a folder", "POST", "/files", (
                F("name", "Name", True), J("parents", "Parent IDs (JSON list)")),
               body={"name": "{name}",
                     "mimeType": "application/vnd.google-apps.folder",
                     "parents": "{parents}"}),
        )),
    APP("gmail", "Gmail", PRODUCTIVITY, "#EA4335",
        "Search and read Gmail messages.",
        "https://gmail.googleapis.com/gmail/v1/users/me",
        BEARER("OAuth access token"), (
            OP("listMessages", "Search messages", "GET", "/messages", (
                F("q", "Query", ph="from:alerts@example.com is:unread"),
                N("maxResults", "Max results", default="10"))),
            OP("getMessage", "Get a message", "GET", "/messages/{id}", (
                F("id", "Message ID", True),)),
        )),
    APP("microsoft365", "Microsoft 365", PRODUCTIVITY, "#0078D4",
        "Send Outlook mail and add To Do tasks through Microsoft Graph.",
        "https://graph.microsoft.com/v1.0", BEARER("OAuth access token"), (
            OP("sendMail", "Send an Outlook email", "POST", "/me/sendMail", (
                F("to", "To", True), F("subject", "Subject", True),
                T("text", "Text", True)),
               body={"message": {"subject": "{subject}",
                                 "body": {"contentType": "Text",
                                          "content": "{text}"},
                                 "toRecipients": [{"emailAddress": {
                                     "address": "{to}"}}]}}),
            OP("createTodo", "Create a To Do task", "POST",
               "/me/todo/lists/{listId}/tasks", (
                   F("listId", "List ID", True), F("title", "Title", True))),
        )),
    APP("todoist", "Todoist", PRODUCTIVITY, "#E44332",
        "Create, list and close Todoist tasks.",
        "https://api.todoist.com/rest/v2", BEARER("API token"), (
            OP("createTask", "Create a task", "POST", "/tasks", (
                F("content", "Task", True), T("description", "Description"),
                F("due_string", "Due", ph="tomorrow at 9am"),
                N("priority", "Priority (1 to 4)"),
                F("project_id", "Project ID"))),
            OP("listTasks", "List tasks", "GET", "/tasks", (
                F("filter", "Filter", ph="today | overdue"),)),
            OP("closeTask", "Complete a task", "POST", "/tasks/{taskId}/close", (
                F("taskId", "Task ID", True),)),
        )),
    APP("trello", "Trello", PRODUCTIVITY, "#0079BF",
        "Create cards and comments on Trello boards.",
        "https://api.trello.com/1",
        {"fields": [PLAIN("key", "API key"), SECRET("token", "Token")],
         "query": {"key": "{key}", "token": "{token}"}}, (
            OP("createCard", "Create a card", "POST", "/cards", (
                F("idList", "List ID", True), F("name", "Name", True),
                T("desc", "Description"))),
            OP("listCards", "List cards", "GET", "/lists/{listId}/cards", (
                F("listId", "List ID", True),)),
            OP("addComment", "Comment on a card", "POST",
               "/cards/{cardId}/actions/comments", (
                   F("cardId", "Card ID", True), T("text", "Comment", True))),
        )),
    APP("asana", "Asana", PRODUCTIVITY, "#F06A6A",
        "Create and list Asana tasks.",
        "https://app.asana.com/api/1.0", BEARER("Personal access token"), (
            OP("createTask", "Create a task", "POST", "/tasks", (
                F("workspace", "Workspace ID", True),
                F("project", "Project ID"), F("name", "Name", True),
                T("notes", "Notes"), F("dueOn", "Due date", ph="2026-10-31")),
               body={"data": {"workspace": "{workspace}",
                              "projects": ["{project}"], "name": "{name}",
                              "notes": "{notes}", "due_on": "{dueOn}"}}),
            OP("listTasks", "List project tasks", "GET", "/tasks", (
                F("project", "Project ID", True),)),
        )),
    APP("clickup", "ClickUp", PRODUCTIVITY, "#7B68EE",
        "Create and list ClickUp tasks.",
        "https://api.clickup.com/api/v2", HEADER("Authorization", "API token"), (
            OP("createTask", "Create a task", "POST", "/list/{listId}/task", (
                F("listId", "List ID", True), F("name", "Name", True),
                T("description", "Description"), F("status", "Status"),
                N("priority", "Priority (1 to 4)"))),
            OP("listTasks", "List tasks", "GET", "/list/{listId}/task", (
                F("listId", "List ID", True),)),
        )),
    APP("monday", "monday.com", PRODUCTIVITY, "#FF3D57",
        "Create items and run GraphQL queries on monday.com boards.",
        "https://api.monday.com/v2", HEADER("Authorization", "API token"), (
            OP("createItem", "Create an item", "POST", "", (
                F("boardId", "Board ID", True), F("name", "Item name", True),
                F("columnValues", "Column values (JSON text)",
                  ph="{\"status\": \"Done\"}")),
               body={"query": "mutation ($board: ID!, $name: String!, "
                              "$values: JSON) { create_item (board_id: $board, "
                              "item_name: $name, column_values: $values) "
                              "{ id } }",
                     "variables": {"board": "{boardId}", "name": "{name}",
                                   "values": "{columnValues}"}}),
            OP("query", "Run a GraphQL query", "POST", "", (
                T("query", "Query", True, "query { boards (limit: 5) { id name } }"),
                J("variables", "Variables (JSON)"))),
        )),
    APP("baserow", "Baserow", PRODUCTIVITY, "#4891EC",
        "Create and list rows in Baserow tables.",
        "{server}/api",
        HEADER("Authorization", "Database token", "Token ", (
            PLAIN("server", "Server URL", "https://api.baserow.io", url=True),)), (
            OP("createRow", "Create a row", "POST",
               "/database/rows/table/{tableId}/", (
                   F("tableId", "Table ID", True),
                   J("fields", "Fields (JSON)", True)),
               body="{fields}", query={"user_field_names": "true"}),
            OP("listRows", "List rows", "GET",
               "/database/rows/table/{tableId}/", (
                   F("tableId", "Table ID", True), F("search", "Search")),
               query={"user_field_names": "true"}),
        )),

    APP("calendly", "Calendly", FORMS, "#006BFF",
        "Read scheduled Calendly events.",
        "https://api.calendly.com", BEARER("Personal access token"), (
            OP("listEvents", "List scheduled events", "GET",
               "/scheduled_events", (
                   F("user", "User URI", True,
                     "https://api.calendly.com/users/..."),
                   F("status", "Status", options=("", "active", "canceled")),
                   F("min_start_time", "Starting after"))),
            OP("me", "Get the current user", "GET", "/users/me"),
        )),
    APP("typeform", "Typeform", FORMS, "#262627",
        "Read Typeform forms and responses.",
        "https://api.typeform.com", BEARER("Personal access token"), (
            OP("listResponses", "List responses", "GET",
               "/forms/{formId}/responses", (
                   F("formId", "Form ID", True),
                   N("page_size", "Page size", default="25"),
                   F("since", "Since"))),
            OP("listForms", "List forms", "GET", "/forms"),
        )),
    APP("jotform", "Jotform", FORMS, "#FF6100",
        "Read Jotform submissions.",
        "https://api.jotform.com", HEADER("APIKEY"), (
            OP("listSubmissions", "List submissions", "GET",
               "/form/{formId}/submissions", (
                   F("formId", "Form ID", True),
                   N("limit", "Limit", default="20"))),
        )),
    APP("zoom", "Zoom", FORMS, "#0B5CFF",
        "Create and list Zoom meetings.",
        "https://api.zoom.us/v2", BEARER("OAuth access token"), (
            OP("createMeeting", "Create a meeting", "POST",
               "/users/me/meetings", (
                   F("topic", "Topic", True),
                   F("start_time", "Start (ISO 8601)"),
                   N("duration", "Duration (minutes)", default="30"),
                   N("type", "Type", default="2"))),
            OP("listMeetings", "List meetings", "GET", "/users/me/meetings"),
        )),

    APP("github", "GitHub", DEVELOPER, "#181717",
        "Issues, comments, releases and Actions on GitHub.",
        "https://api.github.com", BEARER("Personal access token"), (
            OP("createIssue", "Create an issue", "POST",
               "/repos/{owner}/{repo}/issues", (
                   F("owner", "Owner", True), F("repo", "Repository", True),
                   F("title", "Title", True), T("body", "Body"),
                   J("labels", "Labels (JSON list)"))),
            OP("comment", "Comment on an issue or pull request", "POST",
               "/repos/{owner}/{repo}/issues/{number}/comments", (
                   F("owner", "Owner", True), F("repo", "Repository", True),
                   N("number", "Number", True), T("body", "Comment", True))),
            OP("listIssues", "List issues", "GET",
               "/repos/{owner}/{repo}/issues", (
                   F("owner", "Owner", True), F("repo", "Repository", True),
                   F("state", "State", default="open",
                     options=("open", "closed", "all")))),
            OP("createRelease", "Create a release", "POST",
               "/repos/{owner}/{repo}/releases", (
                   F("owner", "Owner", True), F("repo", "Repository", True),
                   F("tag_name", "Tag", True), F("name", "Name"),
                   T("body", "Notes"))),
            OP("dispatchWorkflow", "Run an Actions workflow", "POST",
               "/repos/{owner}/{repo}/actions/workflows/{workflowId}/dispatches", (
                   F("owner", "Owner", True), F("repo", "Repository", True),
                   F("workflowId", "Workflow file or ID", True, "ci.yml"),
                   F("ref", "Branch", True, default="main"),
                   J("inputs", "Inputs (JSON)"))),
        ), headers={"Accept": "application/vnd.github+json",
                    "X-GitHub-Api-Version": "2022-11-28"}),
    APP("gitlab", "GitLab", DEVELOPER, "#FC6D26",
        "Issues and notes on GitLab projects.",
        "{server}/api/v4",
        HEADER("PRIVATE-TOKEN", "Personal access token", "", (
            PLAIN("server", "Server URL", "https://gitlab.com", url=True),)), (
            OP("createIssue", "Create an issue", "POST",
               "/projects/{projectId}/issues", (
                   F("projectId", "Project ID or path", True, "group/project"),
                   F("title", "Title", True), T("description", "Description"),
                   F("labels", "Labels", ph="bug,urgent"))),
            OP("listIssues", "List issues", "GET",
               "/projects/{projectId}/issues", (
                   F("projectId", "Project ID or path", True),
                   F("state", "State", options=("", "opened", "closed")))),
            OP("addNote", "Comment on an issue", "POST",
               "/projects/{projectId}/issues/{issueIid}/notes", (
                   F("projectId", "Project ID or path", True),
                   N("issueIid", "Issue number", True),
                   T("body", "Comment", True))),
        )),
    APP("bitbucket", "Bitbucket", DEVELOPER, "#0052CC",
        "Issues and pull requests on Bitbucket Cloud.",
        "https://api.bitbucket.org/2.0",
        BASIC("username", "Username", "password", "App password"), (
            OP("createIssue", "Create an issue", "POST",
               "/repositories/{workspace}/{repo}/issues", (
                   F("workspace", "Workspace", True),
                   F("repo", "Repository", True), F("title", "Title", True),
                   T("content", "Description")),
               body={"title": "{title}", "content": {"raw": "{content}"}}),
            OP("listPullRequests", "List pull requests", "GET",
               "/repositories/{workspace}/{repo}/pullrequests", (
                   F("workspace", "Workspace", True),
                   F("repo", "Repository", True))),
        )),
    APP("jira", "Jira", DEVELOPER, "#0052CC",
        "Create, search and comment on Jira issues.",
        "{site}/rest/api/3",
        BASIC("email", "Email", "apiToken", "API token",
              (SITE("Site URL", "https://your-team.atlassian.net"),)), (
            OP("createIssue", "Create an issue", "POST", "/issue", (
                F("projectKey", "Project key", True, "ENG"),
                F("issueType", "Issue type", True, default="Task"),
                F("summary", "Summary", True)),
               body={"fields": {"project": {"key": "{projectKey}"},
                                "issuetype": {"name": "{issueType}"},
                                "summary": "{summary}"}}),
            OP("search", "Search with JQL", "GET", "/search", (
                F("jql", "JQL", True, "project = ENG AND status = \"To Do\""),
                N("maxResults", "Max results", default="25"))),
            OP("comment", "Comment on an issue", "POST",
               "/issue/{issueKey}/comment", (
                   F("issueKey", "Issue key", True, "ENG-42"),
                   T("comment", "Comment", True)),
               body={"body": {"type": "doc", "version": 1, "content": [{
                   "type": "paragraph",
                   "content": [{"type": "text", "text": "{comment}"}]}]}}),
        )),
    APP("linear", "Linear", DEVELOPER, "#5E6AD2",
        "Create Linear issues and run GraphQL queries.",
        "https://api.linear.app/graphql", HEADER("Authorization", "API key"), (
            OP("createIssue", "Create an issue", "POST", "", (
                F("teamId", "Team ID", True), F("title", "Title", True),
                T("description", "Description")),
               body={"query": "mutation IssueCreate($input: IssueCreateInput!) "
                              "{ issueCreate(input: $input) { success issue "
                              "{ id identifier url } } }",
                     "variables": {"input": {"teamId": "{teamId}",
                                             "title": "{title}",
                                             "description": "{description}"}}}),
            OP("query", "Run a GraphQL query", "POST", "", (
                T("query", "Query", True, "query { teams { nodes { id name } } }"),
                J("variables", "Variables (JSON)"))),
        )),
    APP("sentry", "Sentry", DEVELOPER, "#362D59",
        "List and resolve Sentry issues.",
        "https://sentry.io/api/0", BEARER("Auth token"), (
            OP("listIssues", "List project issues", "GET",
               "/projects/{org}/{project}/issues/", (
                   F("org", "Organization slug", True),
                   F("project", "Project slug", True),
                   F("query", "Query", default="is:unresolved"))),
            OP("resolveIssue", "Resolve an issue", "PUT",
               "/organizations/{org}/issues/{issueId}/", (
                   F("org", "Organization slug", True),
                   F("issueId", "Issue ID", True)),
               body={"status": "resolved"}),
        )),
    APP("pagerduty", "PagerDuty", DEVELOPER, "#06AC38",
        "Trigger and resolve PagerDuty incidents.",
        "https://events.pagerduty.com/v2",
        {"fields": [SECRET("routingKey", "Integration (routing) key")],
         "body": {"routing_key": "{routingKey}"}}, (
            OP("trigger", "Trigger an incident", "POST", "/enqueue", (
                F("summary", "Summary", True), F("source", "Source", True,
                                                 default="forkmesh"),
                F("severity", "Severity", default="error",
                  options=("critical", "error", "warning", "info")),
                F("dedupKey", "Dedup key")),
               body={"event_action": "trigger", "dedup_key": "{dedupKey}",
                     "payload": {"summary": "{summary}", "source": "{source}",
                                 "severity": "{severity}"}}),
            OP("resolve", "Resolve an incident", "POST", "/enqueue", (
                F("dedupKey", "Dedup key", True),),
               body={"event_action": "resolve", "dedup_key": "{dedupKey}"}),
        )),
    APP("opsgenie", "Opsgenie", DEVELOPER, "#2684FF",
        "Create and close Opsgenie alerts.",
        "https://api.opsgenie.com/v2",
        HEADER("Authorization", "API key", "GenieKey "), (
            OP("createAlert", "Create an alert", "POST", "/alerts", (
                F("message", "Message", True), T("description", "Description"),
                F("priority", "Priority", default="P3",
                  options=("P1", "P2", "P3", "P4", "P5")),
                F("alias", "Alias"))),
            OP("closeAlert", "Close an alert", "POST",
               "/alerts/{alertId}/close", (
                   F("alertId", "Alert ID", True), T("note", "Note"))),
        )),
    APP("circleci", "CircleCI", DEVELOPER, "#343434",
        "Trigger CircleCI pipelines.",
        "https://circleci.com/api/v2", HEADER("Circle-Token", "API token"), (
            OP("triggerPipeline", "Trigger a pipeline", "POST",
               "/project/{projectSlug}/pipeline", (
                   F("projectSlug", "Project slug", True, "gh/org/repo",
                     slashes=True),
                   F("branch", "Branch", default="main"),
                   J("parameters", "Parameters (JSON)"))),
        )),
    APP("netlify", "Netlify", DEVELOPER, "#00C7B7",
        "List sites and start Netlify builds.",
        "https://api.netlify.com/api/v1", BEARER("Personal access token"), (
            OP("listSites", "List sites", "GET", "/sites"),
            OP("build", "Start a build", "POST", "/sites/{siteId}/builds", (
                F("siteId", "Site ID", True),), body={}),
        )),
    APP("vercel", "Vercel", DEVELOPER, "#000000",
        "List Vercel projects and deployments.",
        "https://api.vercel.com", BEARER("Access token"), (
            OP("listDeployments", "List deployments", "GET", "/v6/deployments", (
                F("projectId", "Project ID"), N("limit", "Limit", default="20"))),
            OP("listProjects", "List projects", "GET", "/v9/projects"),
        )),
    APP("cloudflare", "Cloudflare", DEVELOPER, "#F38020",
        "Purge cache and manage DNS records.",
        "https://api.cloudflare.com/client/v4", BEARER("API token"), (
            OP("purgeCache", "Purge everything", "POST",
               "/zones/{zoneId}/purge_cache", (F("zoneId", "Zone ID", True),),
               body={"purge_everything": True}),
            OP("listDns", "List DNS records", "GET",
               "/zones/{zoneId}/dns_records", (
                   F("zoneId", "Zone ID", True), F("name", "Name"))),
            OP("createDns", "Create a DNS record", "POST",
               "/zones/{zoneId}/dns_records", (
                   F("zoneId", "Zone ID", True),
                   F("type", "Type", True, default="A",
                     options=("A", "AAAA", "CNAME", "TXT", "MX")),
                   F("name", "Name", True), F("content", "Content", True),
                   N("ttl", "TTL", default="1"), B("proxied", "Proxied"))),
        )),
    APP("digitalocean", "DigitalOcean", DEVELOPER, "#0080FF",
        "List and reboot droplets.",
        "https://api.digitalocean.com/v2", BEARER("API token"), (
            OP("listDroplets", "List droplets", "GET", "/droplets"),
            OP("reboot", "Reboot a droplet", "POST",
               "/droplets/{dropletId}/actions", (
                   F("dropletId", "Droplet ID", True),),
               body={"type": "reboot"}),
        )),

    APP("hubspot", "HubSpot", CRM, "#FF7A59",
        "Create and search HubSpot contacts and deals.",
        "https://api.hubapi.com", BEARER("Private app token"), (
            OP("createContact", "Create a contact", "POST",
               "/crm/v3/objects/contacts", (
                   F("email", "Email", True), F("firstName", "First name"),
                   F("lastName", "Last name"), F("phone", "Phone")),
               body={"properties": {"email": "{email}",
                                    "firstname": "{firstName}",
                                    "lastname": "{lastName}",
                                    "phone": "{phone}"}}),
            OP("searchContacts", "Search contacts", "POST",
               "/crm/v3/objects/contacts/search", (
                   F("query", "Search text", True),
                   N("limit", "Limit", default="10")),
               body={"query": "{query}", "limit": "{limit}"}),
            OP("createDeal", "Create a deal", "POST", "/crm/v3/objects/deals", (
                J("properties", "Properties (JSON)", True,
                  "{\"dealname\": \"New deal\", \"amount\": \"1000\"}"),),
               body={"properties": "{properties}"}),
        )),
    APP("salesforce", "Salesforce", CRM, "#00A1E0",
        "Create records and run SOQL queries.",
        "{instance}/services/data/v60.0",
        BEARER("Access token", (PLAIN(
            "instance", "Instance URL", "https://your-org.my.salesforce.com",
            url=True),)), (
            OP("createRecord", "Create a record", "POST", "/sobjects/{object}", (
                F("object", "Object", True, default="Lead"),
                J("fields", "Fields (JSON)", True,
                  "{\"LastName\": \"Lovelace\", \"Company\": \"Acme\"}")),
               body="{fields}"),
            OP("query", "Run a SOQL query", "GET", "/query", (
                F("q", "SOQL", True, "SELECT Id, Name FROM Account LIMIT 10"),)),
        )),
    APP("pipedrive", "Pipedrive", CRM, "#1A1A1A",
        "Create deals and people in Pipedrive.",
        "https://api.pipedrive.com/v1", QUERY("api_token", "API token"), (
            OP("createDeal", "Create a deal", "POST", "/deals", (
                F("title", "Title", True), N("value", "Value"),
                F("currency", "Currency", ph="USD"),
                N("person_id", "Person ID"))),
            OP("createPerson", "Create a person", "POST", "/persons", (
                F("name", "Name", True), F("email", "Email"))),
            OP("listDeals", "List deals", "GET", "/deals", (
                F("status", "Status", options=("", "open", "won", "lost")),)),
        )),
    APP("zohoCrm", "Zoho CRM", CRM, "#E42527",
        "Create leads in Zoho CRM.",
        "{domain}/crm/v5",
        HEADER("Authorization", "OAuth access token", "Zoho-oauthtoken ", (
            PLAIN("domain", "API domain", "https://www.zohoapis.com",
                  url=True),)), (
            OP("createLead", "Create a lead", "POST", "/Leads", (
                F("lastName", "Last name", True), F("firstName", "First name"),
                F("email", "Email"), F("company", "Company")),
               body={"data": [{"Last_Name": "{lastName}",
                               "First_Name": "{firstName}",
                               "Email": "{email}", "Company": "{company}"}]}),
        )),
    APP("freshdesk", "Freshdesk", CRM, "#25C16F",
        "Create and list support tickets.",
        "https://{domain}.freshdesk.com/api/v2",
        {"fields": [PLAIN("domain", "Subdomain", "yourcompany"),
                    SECRET("apiKey", "API key")],
         "basic": ("{apiKey}", "X")}, (
            OP("createTicket", "Create a ticket", "POST", "/tickets", (
                F("email", "Requester email", True),
                F("subject", "Subject", True),
                T("description", "Description", True),
                N("priority", "Priority (1 to 4)", default="1"),
                N("status", "Status (2 open)", default="2"))),
            OP("listTickets", "List tickets", "GET", "/tickets"),
        )),
    APP("zendesk", "Zendesk", CRM, "#03363D",
        "Create and list Zendesk tickets.",
        "https://{subdomain}.zendesk.com/api/v2",
        {"fields": [PLAIN("subdomain", "Subdomain", "yourcompany"),
                    PLAIN("email", "Agent email"),
                    SECRET("apiToken", "API token")],
         "basic": ("{email}/token", "{apiToken}")}, (
            OP("createTicket", "Create a ticket", "POST", "/tickets", (
                F("subject", "Subject", True), T("body", "Description", True),
                F("priority", "Priority",
                  options=("", "low", "normal", "high", "urgent"))),
               body={"ticket": {"subject": "{subject}",
                                "comment": {"body": "{body}"},
                                "priority": "{priority}"}}),
            OP("listTickets", "List tickets", "GET", "/tickets"),
        )),
    APP("intercom", "Intercom", CRM, "#1F8DED",
        "Create Intercom contacts and notes.",
        "https://api.intercom.io", BEARER("Access token"), (
            OP("createContact", "Create a contact", "POST", "/contacts", (
                F("role", "Role", default="user", options=("user", "lead")),
                F("email", "Email", True), F("name", "Name"))),
            OP("addNote", "Add a note to a contact", "POST",
               "/contacts/{contactId}/notes", (
                   F("contactId", "Contact ID", True),
                   T("body", "Note", True), F("admin_id", "Admin ID"))),
        ), headers={"Intercom-Version": "2.11"}),
    APP("helpscout", "Help Scout", CRM, "#1292EE",
        "List Help Scout conversations.",
        "https://api.helpscout.net/v2", BEARER("Access token"), (
            OP("listConversations", "List conversations", "GET",
               "/conversations", (
                   F("mailbox", "Mailbox ID"),
                   F("status", "Status", default="active",
                     options=("active", "pending", "closed", "all")))),
        )),
    APP("attio", "Attio", CRM, "#000000",
        "Create records in Attio.",
        "https://api.attio.com/v2", BEARER("API key"), (
            OP("createRecord", "Create a record", "POST",
               "/objects/{object}/records", (
                   F("object", "Object", True, default="people"),
                   J("values", "Values (JSON)", True)),
               body={"data": {"values": "{values}"}}),
        )),

    APP("stripe", "Stripe", COMMERCE, "#635BFF",
        "Customers, products and charges in Stripe.",
        "https://api.stripe.com/v1", BEARER("Secret key"), (
            OP("createCustomer", "Create a customer", "POST", "/customers", (
                F("email", "Email", True), F("name", "Name"),
                T("description", "Description"))),
            OP("listCustomers", "List customers", "GET", "/customers", (
                F("email", "Email"), N("limit", "Limit", default="10"))),
            OP("createProduct", "Create a product", "POST", "/products", (
                F("name", "Name", True), T("description", "Description"))),
            OP("listCharges", "List charges", "GET", "/charges", (
                N("limit", "Limit", default="10"),)),
        ), encoding="form"),
    APP("paypal", "PayPal", COMMERCE, "#003087",
        "Read PayPal orders and invoices.",
        "https://api-m.paypal.com", BEARER("OAuth access token"), (
            OP("getOrder", "Get an order", "GET",
               "/v2/checkout/orders/{orderId}", (
                   F("orderId", "Order ID", True),)),
            OP("listInvoices", "List invoices", "GET", "/v2/invoicing/invoices"),
        )),
    APP("shopify", "Shopify", COMMERCE, "#95BF47",
        "Orders and products in a Shopify store.",
        "https://{shop}.myshopify.com/admin/api/2024-07",
        HEADER("X-Shopify-Access-Token", "Admin API access token", "", (
            PLAIN("shop", "Store subdomain", "your-store"),)), (
            OP("listOrders", "List orders", "GET", "/orders.json", (
                F("status", "Status", default="any",
                  options=("any", "open", "closed", "cancelled")),
                N("limit", "Limit", default="50"))),
            OP("listProducts", "List products", "GET", "/products.json"),
            OP("createProduct", "Create a product", "POST", "/products.json", (
                F("title", "Title", True), T("description", "Description"),
                F("vendor", "Vendor")),
               body={"product": {"title": "{title}",
                                 "body_html": "{description}",
                                 "vendor": "{vendor}"}}),
        )),
    APP("woocommerce", "WooCommerce", COMMERCE, "#96588A",
        "Orders and products in a WooCommerce store.",
        "{store}/wp-json/wc/v3",
        BASIC("consumerKey", "Consumer key", "consumerSecret",
              "Consumer secret", (PLAIN("store", "Store URL",
                                        "https://shop.example.com",
                                        url=True),)), (
            OP("listOrders", "List orders", "GET", "/orders", (
                F("status", "Status"),)),
            OP("updateOrder", "Update an order status", "PUT",
               "/orders/{orderId}", (
                   N("orderId", "Order ID", True),
                   F("status", "Status", True, default="completed",
                     options=("pending", "processing", "on-hold", "completed",
                              "cancelled", "refunded")))),
            OP("createProduct", "Create a product", "POST", "/products", (
                F("name", "Name", True), F("regular_price", "Price"),
                T("description", "Description"))),
        )),
    APP("square", "Square", COMMERCE, "#3E4348",
        "Payments and customers in Square.",
        "https://connect.squareup.com/v2", BEARER("Access token"), (
            OP("listPayments", "List payments", "GET", "/payments"),
            OP("createCustomer", "Create a customer", "POST", "/customers", (
                F("given_name", "First name"), F("family_name", "Last name"),
                F("email_address", "Email", True))),
        ), headers={"Square-Version": "2024-07-17"}),
    APP("paddle", "Paddle", COMMERCE, "#3B82F6",
        "Transactions and customers in Paddle Billing.",
        "https://api.paddle.com", BEARER("API key"), (
            OP("listTransactions", "List transactions", "GET", "/transactions"),
            OP("createCustomer", "Create a customer", "POST", "/customers", (
                F("email", "Email", True), F("name", "Name"))),
        )),
    APP("lemonsqueezy", "Lemon Squeezy", COMMERCE, "#FFC233",
        "Orders and subscriptions in Lemon Squeezy.",
        "https://api.lemonsqueezy.com/v1", BEARER("API key"), (
            OP("listOrders", "List orders", "GET", "/orders"),
            OP("listSubscriptions", "List subscriptions", "GET",
               "/subscriptions"),
        ), headers={"Accept": "application/vnd.api+json"}),
    APP("quickbooks", "QuickBooks Online", COMMERCE, "#2CA01C",
        "Query and create QuickBooks records.",
        "https://quickbooks.api.intuit.com/v3/company/{realmId}",
        BEARER("OAuth access token", (PLAIN("realmId", "Company (realm) ID"),)), (
            OP("query", "Run a query", "GET", "/query", (
                F("query", "Query", True, "select * from Customer maxresults 10"),)),
            OP("createCustomer", "Create a customer", "POST", "/customer", (
                F("name", "Display name", True), F("email", "Email")),
               body={"DisplayName": "{name}",
                     "PrimaryEmailAddr": {"Address": "{email}"}}),
        )),

    APP("x", "X (Twitter)", SOCIAL, "#000000",
        "Post on X.",
        "https://api.x.com/2", BEARER("OAuth 2.0 user access token"), (
            OP("post", "Create a post", "POST", "/tweets", (
                T("text", "Text", True),)),
        )),
    APP("mastodon", "Mastodon", SOCIAL, "#6364FF",
        "Post statuses on a Mastodon server.",
        "{server}/api/v1",
        BEARER("Access token", (PLAIN("server", "Server URL",
                                      "https://mastodon.social", url=True),)), (
            OP("post", "Post a status", "POST", "/statuses", (
                T("status", "Text", True),
                F("visibility", "Visibility", default="public",
                  options=("public", "unlisted", "private", "direct")))),
        )),
    APP("linkedin", "LinkedIn", SOCIAL, "#0A66C2",
        "Share posts on LinkedIn.",
        "https://api.linkedin.com/rest", BEARER("OAuth access token"), (
            OP("post", "Share a post", "POST", "/posts", (
                F("author", "Author URN", True, "urn:li:person:abc123"),
                T("text", "Text", True)),
               body={"author": "{author}", "commentary": "{text}",
                     "visibility": "PUBLIC",
                     "distribution": {"feedDistribution": "MAIN_FEED",
                                      "targetEntities": [],
                                      "thirdPartyDistributionChannels": []},
                     "lifecycleState": "PUBLISHED",
                     "isReshareDisabledByAuthor": False}),
        ), headers={"LinkedIn-Version": "202406",
                    "X-Restli-Protocol-Version": "2.0.0"}),
    APP("facebookPages", "Facebook Pages", SOCIAL, "#1877F2",
        "Publish posts to a Facebook Page.",
        "https://graph.facebook.com/v19.0",
        QUERY("access_token", "Page access token"), (
            OP("post", "Publish a post", "POST", "/{pageId}/feed", (
                F("pageId", "Page ID", True), T("message", "Message", True),
                F("link", "Link"))),
        )),
    APP("youtube", "YouTube", SOCIAL, "#FF0000",
        "Search videos and read channel statistics.",
        "https://www.googleapis.com/youtube/v3", QUERY("key", "API key"), (
            OP("search", "Search", "GET", "/search", (
                F("q", "Query", True), F("part", "Part", default="snippet"),
                F("type", "Type", default="video",
                  options=("video", "channel", "playlist")),
                N("maxResults", "Max results", default="10"))),
            OP("channel", "Channel statistics", "GET", "/channels", (
                F("id", "Channel ID", True),
                F("part", "Part", default="snippet,statistics"))),
        )),
    APP("reddit", "Reddit", SOCIAL, "#FF4500",
        "Submit posts to a subreddit.",
        "https://oauth.reddit.com", BEARER("OAuth access token"), (
            OP("submit", "Submit a post", "POST", "/api/submit", (
                F("sr", "Subreddit", True), F("title", "Title", True),
                F("kind", "Kind", default="self", options=("self", "link")),
                T("text", "Text"), F("url", "Link URL"))),
        ), encoding="form"),
    APP("wordpress", "WordPress", SOCIAL, "#21759B",
        "Create and list WordPress posts.",
        "{site}/wp-json/wp/v2",
        BASIC("username", "Username", "appPassword", "Application password",
              (SITE("Site URL", "https://blog.example.com"),)), (
            OP("createPost", "Create a post", "POST", "/posts", (
                F("title", "Title", True), T("content", "Content", True),
                F("status", "Status", default="draft",
                  options=("draft", "publish", "pending", "private")))),
            OP("listPosts", "List posts", "GET", "/posts", (
                N("per_page", "Per page", default="10"),)),
        )),
    APP("webflow", "Webflow", SOCIAL, "#4353FF",
        "Create CMS collection items in Webflow.",
        "https://api.webflow.com/v2", BEARER("API token"), (
            OP("createItem", "Create a CMS item", "POST",
               "/collections/{collectionId}/items", (
                   F("collectionId", "Collection ID", True),
                   J("fieldData", "Field data (JSON)", True,
                     "{\"name\": \"Hello\", \"slug\": \"hello\"}"),
                   B("isDraft", "Draft", default="true")),
               body={"fieldData": "{fieldData}", "isDraft": "{isDraft}"}),
        )),

    APP("mixpanel", "Mixpanel", ANALYTICS, "#7856FF",
        "Track events in Mixpanel.",
        "https://api.mixpanel.com", NONE, (
            OP("track", "Track an event", "POST", "/track", (
                F("token", "Project token", True), F("event", "Event", True),
                F("distinctId", "Distinct ID", True)),
               body=[{"event": "{event}",
                      "properties": {"token": "{token}",
                                     "distinct_id": "{distinctId}"}}]),
        )),
    APP("segment", "Segment", ANALYTICS, "#52BD94",
        "Send track and identify calls to Segment.",
        "https://api.segment.io/v1",
        {"fields": [SECRET("writeKey", "Write key")],
         "basic": ("{writeKey}", "")}, (
            OP("track", "Track an event", "POST", "/track", (
                F("userId", "User ID", True), F("event", "Event", True),
                J("properties", "Properties (JSON)"))),
            OP("identify", "Identify a user", "POST", "/identify", (
                F("userId", "User ID", True), J("traits", "Traits (JSON)"))),
        )),
    APP("amplitude", "Amplitude", ANALYTICS, "#1E61F0",
        "Send events to Amplitude.",
        "https://api2.amplitude.com",
        {"fields": [SECRET("apiKey", "Project API key")],
         "body": {"api_key": "{apiKey}"}}, (
            OP("event", "Send an event", "POST", "/2/httpapi", (
                F("userId", "User ID", True), F("eventType", "Event", True),
                J("properties", "Event properties (JSON)")),
               body={"events": [{"user_id": "{userId}",
                                 "event_type": "{eventType}",
                                 "event_properties": "{properties}"}]}),
        )),
    APP("plausible", "Plausible", ANALYTICS, "#5850EC",
        "Read aggregate stats from Plausible.",
        "https://plausible.io/api/v1", BEARER("API key"), (
            OP("aggregate", "Aggregate stats", "GET", "/stats/aggregate", (
                F("site_id", "Site", True, "example.com"),
                F("period", "Period", default="30d",
                  options=("day", "7d", "30d", "month", "6mo", "12mo")),
                F("metrics", "Metrics", default="visitors,pageviews"))),
        )),
    APP("googleAnalytics", "Google Analytics", ANALYTICS, "#E37400",
        "Send events to GA4 with the Measurement Protocol.",
        "https://www.google-analytics.com/mp",
        {"fields": [PLAIN("measurementId", "Measurement ID", "G-XXXXXXX"),
                    SECRET("apiSecret", "API secret")],
         "query": {"measurement_id": "{measurementId}",
                   "api_secret": "{apiSecret}"}}, (
            OP("event", "Send an event", "POST", "/collect", (
                F("clientId", "Client ID", True),
                F("eventName", "Event name", True),
                J("params", "Parameters (JSON)")),
               body={"client_id": "{clientId}",
                     "events": [{"name": "{eventName}",
                                 "params": "{params}"}]}),
        )),

    APP("openai", "OpenAI", AI, "#10A37F",
        "Chat completions, embeddings and images with OpenAI.",
        "https://api.openai.com/v1", BEARER("API key"), (
            CHAT("gpt-4.1-mini"),
            OP("embed", "Create embeddings", "POST", "/embeddings", (
                F("model", "Model", True, default="text-embedding-3-small"),
                T("input", "Text", True))),
            OP("image", "Generate an image", "POST", "/images/generations", (
                F("model", "Model", True, default="gpt-image-1"),
                T("prompt", "Prompt", True),
                F("size", "Size", default="1024x1024",
                  options=("1024x1024", "1024x1536", "1536x1024")))),
        )),
    APP("anthropic", "Anthropic Claude", AI, "#D97757",
        "Send a prompt to Claude with the Messages API.",
        "https://api.anthropic.com/v1", HEADER("x-api-key"), (
            OP("message", "Send a message", "POST", "/messages", (
                F("model", "Model", True, default="claude-opus-5-5"),
                T("prompt", "Prompt", True, "Summarize: {{ $json.text }}"),
                T("system", "System prompt"),
                N("maxTokens", "Max tokens", True, default="4096")),
               body={"model": "{model}", "max_tokens": "{maxTokens}",
                     "system": "{system}",
                     "messages": [{"role": "user", "content": "{prompt}"}],
                     "fallbacks": "default"},
               headers={"anthropic-beta": "server-side-fallback-2026-07-01"},
               output="anthropic"),
        ), headers={"anthropic-version": "2023-06-01"}),
    APP("mistral", "Mistral AI", AI, "#FA520F",
        "Chat completions with Mistral models.",
        "https://api.mistral.ai/v1", BEARER("API key"), (
            CHAT("mistral-large-latest"),
        )),
    APP("groq", "Groq", AI, "#F55036",
        "Fast chat completions on Groq.",
        "https://api.groq.com/openai/v1", BEARER("API key"), (
            CHAT("llama-3.3-70b-versatile"),
        )),
    APP("perplexity", "Perplexity", AI, "#20808D",
        "Answers with web search from Perplexity.",
        "https://api.perplexity.ai", BEARER("API key"), (
            CHAT("sonar"),
        )),
    APP("gemini", "Google Gemini", AI, "#8E75B2",
        "Generate text with Gemini models.",
        "https://generativelanguage.googleapis.com/v1beta",
        HEADER("x-goog-api-key"), (
            OP("generate", "Generate content", "POST",
               "/models/{model}:generateContent", (
                   F("model", "Model", True, default="gemini-2.5-flash"),
                   T("prompt", "Prompt", True)),
               body={"contents": [{"parts": [{"text": "{prompt}"}]}]},
               output="gemini"),
        )),
    APP("cohere", "Cohere", AI, "#39594D",
        "Chat with Cohere Command models.",
        "https://api.cohere.com/v2", BEARER("API key"), (
            OP("chat", "Chat", "POST", "/chat", (
                F("model", "Model", True, default="command-r-plus"),
                T("prompt", "Prompt", True)),
               body={"model": "{model}", "messages": [
                   {"role": "user", "content": "{prompt}"}]},
               output="cohere"),
        )),
    APP("huggingface", "Hugging Face", AI, "#FFD21E",
        "Run models on the Hugging Face Inference API.",
        "https://api-inference.huggingface.co", BEARER("Access token"), (
            OP("run", "Run a model", "POST", "/models/{model}", (
                F("model", "Model", True,
                  "distilbert/distilbert-base-uncased-finetuned-sst-2-english",
                  slashes=True),
                T("inputs", "Inputs", True), J("parameters", "Parameters (JSON)"))),
        )),
    APP("deepl", "DeepL", AI, "#0F2B46",
        "Translate text with DeepL.",
        "{server}/v2",
        HEADER("Authorization", "API key", "DeepL-Auth-Key ", (
            PLAIN("server", "API URL", "https://api-free.deepl.com", url=True),)), (
            OP("translate", "Translate", "POST", "/translate", (
                T("text", "Text", True),
                F("targetLang", "Target language", True, "DE"),
                F("sourceLang", "Source language")),
               body={"text": ["{text}"], "target_lang": "{targetLang}",
                     "source_lang": "{sourceLang}"}),
        )),

    APP("dropbox", "Dropbox", DATA, "#0061FF",
        "List and create Dropbox folders.",
        "https://api.dropboxapi.com/2", BEARER("Access token"), (
            OP("listFolder", "List a folder", "POST", "/files/list_folder", (
                F("path", "Path", True, "/Documents"),)),
            OP("createFolder", "Create a folder", "POST",
               "/files/create_folder_v2", (F("path", "Path", True),)),
        )),
    APP("box", "Box", DATA, "#0061D5",
        "List and create Box folders.",
        "https://api.box.com/2.0", BEARER("Access token"), (
            OP("listFolder", "List folder items", "GET",
               "/folders/{folderId}/items", (
                   F("folderId", "Folder ID", True, default="0"),)),
            OP("createFolder", "Create a folder", "POST", "/folders", (
                F("name", "Name", True),
                F("parentId", "Parent folder ID", True, default="0")),
               body={"name": "{name}", "parent": {"id": "{parentId}"}}),
        )),
    APP("supabase", "Supabase", DATA, "#3ECF8E",
        "Insert and select rows through the Supabase REST API.",
        "{url}/rest/v1",
        {"fields": [PLAIN("url", "Project URL",
                          "https://abc.supabase.co", url=True),
                    SECRET("apiKey", "Service role or anon key")],
         "headers": {"apikey": "{apiKey}",
                     "Authorization": "Bearer {apiKey}"}}, (
            OP("insert", "Insert rows", "POST", "/{table}", (
                F("table", "Table", True), J("row", "Row (JSON)", True)),
               body="{row}", headers={"Prefer": "return=representation"}),
            OP("select", "Select rows", "GET", "/{table}", (
                F("table", "Table", True),
                F("select", "Columns", default="*"),
                J("filters", "Filters (JSON)", ph="{\"id\": \"eq.1\"}",
                  where="query_object"),
                N("limit", "Limit", default="100"))),
        )),
    APP("firebase", "Firebase Realtime Database", DATA, "#FFCA28",
        "Read and write Firebase Realtime Database paths.",
        "{url}",
        {"fields": [PLAIN("url", "Database URL",
                          "https://project-default-rtdb.firebaseio.com",
                          url=True),
                    SECRET("secret", "Database secret or ID token")],
         "query": {"auth": "{secret}"}}, (
            OP("get", "Read a path", "GET", "/{path}.json", (
                F("path", "Path", True, "users/ada", slashes=True),)),
            OP("set", "Write a path", "PUT", "/{path}.json", (
                F("path", "Path", True, slashes=True),
                J("value", "Value (JSON)", True)),
               body="{value}"),
            OP("push", "Push to a list", "POST", "/{path}.json", (
                F("path", "Path", True, slashes=True),
                J("value", "Value (JSON)", True)),
               body="{value}"),
        )),
    APP("algolia", "Algolia", DATA, "#003DFF",
        "Search and index records in Algolia.",
        "https://{appId}-dsn.algolia.net/1",
        {"fields": [PLAIN("appId", "Application ID"),
                    SECRET("apiKey", "API key")],
         "headers": {"X-Algolia-Application-Id": "{appId}",
                     "X-Algolia-API-Key": "{apiKey}"}}, (
            OP("search", "Search an index", "POST", "/indexes/{index}/query", (
                F("index", "Index", True), F("query", "Query", True))),
            OP("addObject", "Add a record", "POST", "/indexes/{index}", (
                F("index", "Index", True), J("object", "Record (JSON)", True)),
               body="{object}"),
        )),
    APP("upstash", "Upstash Redis", DATA, "#00E9A3",
        "Run Redis commands over the Upstash REST API.",
        "{url}",
        BEARER("REST token", (PLAIN("url", "REST URL",
                                    "https://abc.upstash.io", url=True),)), (
            OP("command", "Run a command", "POST", "", (
                J("command", "Command (JSON list)", True,
                  "[\"SET\", \"greeting\", \"hello\"]"),),
               body="{command}"),
        )),

    APP("bitly", "Bitly", UTILITY, "#EE6123",
        "Shorten links with Bitly.",
        "https://api-ssl.bitly.com/v4", BEARER("Access token"), (
            OP("shorten", "Shorten a link", "POST", "/shorten", (
                F("long_url", "URL", True), F("domain", "Domain",
                                              default="bit.ly"))),
        )),
    APP("openweathermap", "OpenWeatherMap", UTILITY, "#EB6E4B",
        "Current weather and forecasts.",
        "https://api.openweathermap.org/data/2.5", QUERY("appid"), (
            OP("current", "Current weather", "GET", "/weather", (
                F("q", "City", True, "Berlin,DE"),
                F("units", "Units", default="metric",
                  options=("metric", "imperial", "standard")))),
            OP("forecast", "Five-day forecast", "GET", "/forecast", (
                F("q", "City", True),
                F("units", "Units", default="metric",
                  options=("metric", "imperial", "standard")))),
        )),
    APP("wikipedia", "Wikipedia", UTILITY, "#636466",
        "Read article summaries from Wikipedia.",
        "https://en.wikipedia.org/api/rest_v1", NONE, (
            OP("summary", "Article summary", "GET", "/page/summary/{title}", (
                F("title", "Title", True, "Ada_Lovelace"),)),
        )),
    APP("homeAssistant", "Home Assistant", UTILITY, "#18BCF2",
        "Call services and read states in Home Assistant.",
        "{server}/api",
        BEARER("Long-lived access token", (PLAIN(
            "server", "Public URL", "https://home.example.com", url=True),)), (
            OP("callService", "Call a service", "POST",
               "/services/{domain}/{service}", (
                   F("domain", "Domain", True, "light"),
                   F("service", "Service", True, "turn_on"),
                   J("data", "Service data (JSON)",
                     ph="{\"entity_id\": \"light.kitchen\"}")),
               body="{data}"),
            OP("getState", "Get an entity state", "GET", "/states/{entityId}", (
                F("entityId", "Entity ID", True, "sensor.temperature"),)),
        )),
    APP("zapier", "Zapier", UTILITY, "#FF4F00",
        "Send data to a Zapier catch hook.",
        "{url}", WEBHOOK("Catch hook URL", "https://hooks.zapier.com/hooks/catch/..."), (
            OP("send", "Send data", "POST", "", (
                J("data", "Data (JSON)", True, "{\"name\": \"{{ $json.name }}\"}"),),
               body="{data}"),
        )),
))
